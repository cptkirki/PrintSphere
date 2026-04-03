#include "printsphere/bambu_cloud_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.cloud";
constexpr char kLoginUrl[] = "https://api.bambulab.com/v1/user-service/user/login";
constexpr char kTfaLoginUrl[] = "https://bambulab.com/api/sign-in/tfa";
constexpr char kEmailCodeUrl[] = "https://api.bambulab.com/v1/user-service/user/sendemail/code";
constexpr char kBindUrl[] = "https://api.bambulab.com/v1/iot-service/api/user/bind";
constexpr char kPreferenceUrl[] = "https://api.bambulab.com/v1/design-user-service/my/preference";
constexpr char kTasksUrl[] = "https://api.bambulab.com/v1/user-service/my/tasks?limit=10";
constexpr char kCloudMqttHost[] = "us.mqtt.bambulab.com";
constexpr uint16_t kCloudMqttPort = 8883;
constexpr char kGetVersion[] = "{\"info\":{\"sequence_id\":\"0\",\"command\":\"get_version\"}}";
constexpr char kPushAll[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";
constexpr size_t kMaxPreviewBytes = 256 * 1024;
constexpr size_t kPreviewRangeChunkBytes = 4 * 1024;
constexpr TickType_t kCloudInitialSyncRetryDelay = pdMS_TO_TICKS(3000);
constexpr TickType_t kCloudInitialSyncTimeout = pdMS_TO_TICKS(12000);
constexpr TickType_t kCloudStatusPollIdle = pdMS_TO_TICKS(30000);
constexpr TickType_t kCloudStatusPollActive = pdMS_TO_TICKS(5000);
constexpr TickType_t kCloudPreviewPollActiveView = pdMS_TO_TICKS(15000);
constexpr TickType_t kCloudStatusPollLowPower = pdMS_TO_TICKS(180000);
constexpr TickType_t kCloudPreviewPollLowPower = pdMS_TO_TICKS(600000);
constexpr TickType_t kCloudInitialPreviewDelay = pdMS_TO_TICKS(8000);
constexpr TickType_t kCloudPreviewWakePoll = pdMS_TO_TICKS(2000);
constexpr TickType_t kCloudBindingRefresh = pdMS_TO_TICKS(300000);
constexpr uint64_t kCloudLiveDataFreshMs = 120000ULL;
constexpr uint64_t kCloudOptimisticLightMs = 8000ULL;
constexpr int kCloudPrintErrorTaskCanceled = 0x0300400C;
constexpr int kCloudPrintErrorPrintingCancelled = 0x0500400E;

struct PreviewDownloadContext {
  std::vector<uint8_t>* buffer = nullptr;
  size_t max_bytes = 0;
  bool overflow = false;
};

struct ParsedHttpsUrl {
  std::string host;
  std::string target;
  int port = 443;
};

std::string preview_cache_key(const std::string& url) {
  const std::string::size_type query_pos = url.find('?');
  if (query_pos == std::string::npos) {
    return url;
  }
  return url.substr(0, query_pos);
}

bool prefers_ranged_preview_download(const std::string& url) {
  return url.find("amazonaws.com/") != std::string::npos &&
         url.find("X-Amz-Algorithm=") != std::string::npos;
}

esp_err_t preview_http_event_handler(esp_http_client_event_t* event) {
  auto* context = static_cast<PreviewDownloadContext*>(event->user_data);
  if (context == nullptr || context->buffer == nullptr) {
    return ESP_OK;
  }

  if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0) {
    return ESP_OK;
  }

  if (context->buffer->empty()) {
    const int64_t content_length = esp_http_client_get_content_length(event->client);
    if (content_length > 0 && content_length <= static_cast<int64_t>(context->max_bytes)) {
      context->buffer->reserve(static_cast<size_t>(content_length));
    }
  }

  const size_t next_size = context->buffer->size() + static_cast<size_t>(event->data_len);
  if (next_size > context->max_bytes) {
    context->overflow = true;
    return ESP_FAIL;
  }

  const auto* data = static_cast<const uint8_t*>(event->data);
  context->buffer->insert(context->buffer->end(), data, data + event->data_len);
  return ESP_OK;
}

bool parse_https_url(const std::string& url, ParsedHttpsUrl* parsed) {
  if (parsed == nullptr) {
    return false;
  }

  constexpr std::string_view kHttpsPrefix = "https://";
  if (!url.starts_with(kHttpsPrefix)) {
    return false;
  }

  const size_t authority_start = kHttpsPrefix.size();
  const size_t path_start = url.find('/', authority_start);
  const std::string authority =
      (path_start == std::string::npos) ? url.substr(authority_start)
                                        : url.substr(authority_start, path_start - authority_start);
  if (authority.empty()) {
    return false;
  }

  const size_t colon_pos = authority.rfind(':');
  if (colon_pos != std::string::npos) {
    parsed->host = authority.substr(0, colon_pos);
    const std::string port_text = authority.substr(colon_pos + 1);
    if (parsed->host.empty() || port_text.empty()) {
      return false;
    }
    parsed->port = std::atoi(port_text.c_str());
    if (parsed->port <= 0) {
      return false;
    }
  } else {
    parsed->host = authority;
    parsed->port = 443;
  }

  parsed->target = (path_start == std::string::npos) ? "/" : url.substr(path_start);
  return !parsed->host.empty() && !parsed->target.empty();
}

std::string header_value_ci(std::string_view headers, std::string_view key) {
  std::string lower_headers(headers);
  std::transform(lower_headers.begin(), lower_headers.end(), lower_headers.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::string lower_key(key);
  std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  lower_key += ":";

  const size_t key_pos = lower_headers.find(lower_key);
  if (key_pos == std::string::npos) {
    return {};
  }

  const size_t value_start = key_pos + lower_key.size();
  const size_t value_end = lower_headers.find("\r\n", value_start);
  const size_t slice_end = (value_end == std::string::npos) ? headers.size() : value_end;

  std::string value(headers.substr(value_start, slice_end - value_start));
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

int parse_status_code(std::string_view status_line) {
  const size_t first_space = status_line.find(' ');
  if (first_space == std::string::npos) {
    return 0;
  }
  const size_t second_space = status_line.find(' ', first_space + 1);
  const std::string code_text =
      std::string(status_line.substr(first_space + 1, second_space - (first_space + 1)));
  return std::atoi(code_text.c_str());
}

int lifecycle_priority(PrintLifecycleState lifecycle) {
  switch (lifecycle) {
    case PrintLifecycleState::kPrinting:
      return 600;
    case PrintLifecycleState::kPreparing:
      return 500;
    case PrintLifecycleState::kPaused:
      return 450;
    case PrintLifecycleState::kError:
      return 300;
    case PrintLifecycleState::kFinished:
      return 200;
    case PrintLifecycleState::kIdle:
      return 100;
    case PrintLifecycleState::kUnknown:
    default:
      return 0;
  }
}

uint64_t now_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

bool is_recent_live_data(uint64_t last_update_ms) {
  if (last_update_ms == 0) {
    return false;
  }
  const uint64_t current_ms = now_ms();
  if (current_ms < last_update_ms) {
    return false;
  }
  return (current_ms - last_update_ms) <= kCloudLiveDataFreshMs;
}

bool is_recent_optimistic_light_state(uint64_t last_update_ms) {
  if (last_update_ms == 0) {
    return false;
  }
  const uint64_t current_ms = now_ms();
  if (current_ms < last_update_ms) {
    return false;
  }
  return (current_ms - last_update_ms) <= kCloudOptimisticLightMs;
}

bool cloud_status_is_non_error_stop(std::string_view status_text, int print_error_code, int hms_count) {
  if (status_text.empty() || hms_count > 0) {
    return false;
  }

  if (print_error_code != 0 && print_error_code != kCloudPrintErrorTaskCanceled &&
      print_error_code != kCloudPrintErrorPrintingCancelled) {
    return false;
  }

  std::string normalized;
  normalized.reserve(status_text.size());
  for (const char ch : status_text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
      normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
  }

  return normalized.find("FAIL") != std::string::npos ||
         normalized.find("CANCEL") != std::string::npos;
}

bool cloud_rest_failure_looks_stale(std::string_view status_text, std::string_view stage_text,
                                    std::string_view print_type, int hms_count) {
  if (status_text.empty() || hms_count > 0) {
    return false;
  }

  auto normalize = [](std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : value) {
      if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
      }
    }
    return normalized;
  };

  const std::string normalized_status = normalize(status_text);
  const bool failed_status = normalized_status.find("FAIL") != std::string::npos ||
                             normalized_status.find("ERROR") != std::string::npos ||
                             normalized_status.find("CANCEL") != std::string::npos;
  if (!failed_status) {
    return false;
  }

  const std::string normalized_stage = normalize(stage_text);
  const std::string normalized_type = normalize(print_type);
  const bool idle_stage = normalized_stage.find("IDLE") != std::string::npos ||
                          normalized_stage.find("READY") != std::string::npos;
  const bool idle_type =
      normalized_type == "IDLE" || (normalized_stage.empty() && normalized_type == "CLOUD");
  return idle_stage || idle_type;
}

int normalize_cloud_print_error_code(int print_error_code) {
  switch (print_error_code) {
    case kCloudPrintErrorTaskCanceled:
    case kCloudPrintErrorPrintingCancelled:
      return 0;
    default:
      return print_error_code;
  }
}

bool split_jwt_payload(const std::string& token, std::string* payload_b64) {
  if (payload_b64 == nullptr) {
    return false;
  }

  const size_t first_dot = token.find('.');
  if (first_dot == std::string::npos) {
    return false;
  }
  const size_t second_dot = token.find('.', first_dot + 1U);
  if (second_dot == std::string::npos || second_dot <= first_dot + 1U) {
    return false;
  }

  *payload_b64 = token.substr(first_dot + 1U, second_dot - first_dot - 1U);
  return !payload_b64->empty();
}

std::string decode_username_from_access_token(const std::string& token) {
  std::string payload_b64;
  if (!split_jwt_payload(token, &payload_b64)) {
    return {};
  }

  std::replace(payload_b64.begin(), payload_b64.end(), '-', '+');
  std::replace(payload_b64.begin(), payload_b64.end(), '_', '/');
  while ((payload_b64.size() % 4U) != 0U) {
    payload_b64.push_back('=');
  }

  std::vector<unsigned char> decoded(payload_b64.size());
  size_t decoded_len = 0;
  const int decode_result =
      mbedtls_base64_decode(decoded.data(), decoded.size(), &decoded_len,
                            reinterpret_cast<const unsigned char*>(payload_b64.data()),
                            payload_b64.size());
  if (decode_result != 0 || decoded_len == 0U) {
    return {};
  }

  const std::string payload(reinterpret_cast<const char*>(decoded.data()), decoded_len);
  cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
  if (root == nullptr) {
    return {};
  }

  std::string username;
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "username");
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    username = item->valuestring;
  }
  if (username.empty()) {
    int uid = -1;
    const cJSON* uid_item = cJSON_GetObjectItemCaseSensitive(root, "uid");
    if (cJSON_IsNumber(uid_item)) {
      uid = uid_item->valueint;
    } else if (cJSON_IsString(uid_item) && uid_item->valuestring != nullptr) {
      uid = static_cast<int>(std::strtol(uid_item->valuestring, nullptr, 10));
    }
    if (uid > 0) {
      username = "u_" + std::to_string(uid);
    }
  }
  cJSON_Delete(root);
  return username;
}

std::string normalized_copy(std::string value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
      normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
  }
  return normalized;
}

PrinterModel model_from_product_name(const std::string& product_name) {
  const std::string normalized = normalized_copy(product_name);
  if (normalized.find("A1MINI") != std::string::npos) {
    return PrinterModel::kA1Mini;
  }
  if (normalized.find("BAMBULABA1") != std::string::npos || normalized == "A1") {
    return PrinterModel::kA1;
  }
  if (normalized.find("P1S") != std::string::npos) {
    return PrinterModel::kP1S;
  }
  if (normalized.find("P1P") != std::string::npos) {
    return PrinterModel::kP1P;
  }
  if (normalized.find("P2S") != std::string::npos) {
    return PrinterModel::kP2S;
  }
  if (normalized.find("H2DPRO") != std::string::npos) {
    return PrinterModel::kH2DPro;
  }
  if (normalized.find("H2D") != std::string::npos) {
    return PrinterModel::kH2D;
  }
  if (normalized.find("H2S") != std::string::npos) {
    return PrinterModel::kH2S;
  }
  if (normalized.find("H2C") != std::string::npos) {
    return PrinterModel::kH2C;
  }
  if (normalized.find("X1E") != std::string::npos) {
    return PrinterModel::kX1E;
  }
  if (normalized.find("X1CARBON") != std::string::npos || normalized.find("X1C") != std::string::npos) {
    return PrinterModel::kX1C;
  }
  if (normalized.find("X1") != std::string::npos) {
    return PrinterModel::kX1;
  }
  return PrinterModel::kUnknown;
}

std::string json_string_local(const cJSON* object, const char* key,
                              const std::string& fallback = {}) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return fallback;
  }
  return item->valuestring;
}

bool parse_int_text(const char* text, int* value) {
  if (text == nullptr || value == nullptr) {
    return false;
  }

  while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)) != 0) {
    ++text;
  }
  const char* start = text;
  if (*start == '\0') {
    return false;
  }

  const char* digits = start;
  if (*digits == '+' || *digits == '-') {
    ++digits;
  }
  if (*digits == '\0') {
    return false;
  }

  bool all_hex = true;
  size_t digit_count = 0;
  for (const char* cursor = digits; *cursor != '\0'; ++cursor) {
    if (std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
      break;
    }
    if (!std::isxdigit(static_cast<unsigned char>(*cursor))) {
      all_hex = false;
      break;
    }
    ++digit_count;
  }

  const bool explicit_hex = (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'));
  const int base = (explicit_hex || (all_hex && digit_count >= 8U)) ? 16 : 10;
  char* end = nullptr;
  long parsed = std::strtol(start, &end, base);
  if (end == nullptr || end == start) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool json_int_like(const cJSON* item, int* value) {
  if (item == nullptr || value == nullptr) {
    return false;
  }
  if (cJSON_IsNumber(item)) {
    *value = item->valueint;
    return true;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return parse_int_text(item->valuestring, value);
  }
  return false;
}

int json_int_local(const cJSON* object, const char* key, int fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  int value = fallback;
  return json_int_like(item, &value) ? value : fallback;
}

bool json_float_like(const cJSON* item, float* value) {
  if (item == nullptr || value == nullptr) {
    return false;
  }
  if (cJSON_IsNumber(item)) {
    *value = static_cast<float>(item->valuedouble);
    return true;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    char* end = nullptr;
    const float parsed = std::strtof(item->valuestring, &end);
    if (end != nullptr && end != item->valuestring) {
      *value = parsed;
      return true;
    }
  }
  return false;
}

float json_number_local(const cJSON* object, const char* key, float fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  float value = fallback;
  return json_float_like(item, &value) ? value : fallback;
}

const cJSON* child_object_local(const cJSON* object, const char* key) {
  if (object == nullptr || key == nullptr) {
    return nullptr;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsObject(item) ? item : nullptr;
}

const cJSON* child_array_local(const cJSON* object, const char* key) {
  if (object == nullptr || key == nullptr) {
    return nullptr;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsArray(item) ? item : nullptr;
}

bool key_equals_ignore_case(const char* lhs, const char* rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  while (*lhs != '\0' && *rhs != '\0') {
    if (std::tolower(static_cast<unsigned char>(*lhs)) !=
        std::tolower(static_cast<unsigned char>(*rhs))) {
      return false;
    }
    ++lhs;
    ++rhs;
  }
  return *lhs == '\0' && *rhs == '\0';
}

bool key_matches_any(const char* key, std::initializer_list<const char*> keys) {
  if (key == nullptr) {
    return false;
  }
  for (const char* candidate : keys) {
    if (key_equals_ignore_case(key, candidate)) {
      return true;
    }
  }
  return false;
}

bool find_number_for_keys_recursive(const cJSON* node, std::initializer_list<const char*> keys,
                                    float* value, int depth = 0) {
  if (node == nullptr || value == nullptr || depth > 12) {
    return false;
  }
  if (cJSON_IsObject(node)) {
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
      if (key_matches_any(child->string, keys) && json_float_like(child, value)) {
        return true;
      }
    }
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
      if (find_number_for_keys_recursive(child, keys, value, depth + 1)) {
        return true;
      }
    }
  } else if (cJSON_IsArray(node)) {
    const int count = cJSON_GetArraySize(node);
    for (int i = 0; i < count; ++i) {
      if (find_number_for_keys_recursive(cJSON_GetArrayItem(node, i), keys, value, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

bool find_int_for_keys_recursive(const cJSON* node, std::initializer_list<const char*> keys,
                                 int* value, int depth = 0) {
  if (node == nullptr || value == nullptr || depth > 12) {
    return false;
  }
  if (cJSON_IsObject(node)) {
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
      if (key_matches_any(child->string, keys) && json_int_like(child, value)) {
        return true;
      }
    }
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
      if (find_int_for_keys_recursive(child, keys, value, depth + 1)) {
        return true;
      }
    }
  } else if (cJSON_IsArray(node)) {
    const int count = cJSON_GetArraySize(node);
    for (int i = 0; i < count; ++i) {
      if (find_int_for_keys_recursive(cJSON_GetArrayItem(node, i), keys, value, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

int count_hms_entries(const cJSON* item) {
  if (item == nullptr) {
    return 0;
  }
  if (cJSON_IsArray(item)) {
    return cJSON_GetArraySize(item);
  }
  if (cJSON_IsObject(item)) {
    int count = 0;
    for (const cJSON* child = item->child; child != nullptr; child = child->next) {
      ++count;
    }
    return count;
  }
  int value = 0;
  return json_int_like(item, &value) ? value : 0;
}

bool find_hms_count_recursive(const cJSON* node, int* count, int depth = 0) {
  if (node == nullptr || count == nullptr || depth > 12) {
    return false;
  }
  if (cJSON_IsObject(node)) {
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
      if (key_matches_any(child->string, {"hms", "hms_list", "hmsList", "hms_errors",
                                          "hmsErrors", "hms_alerts", "hmsAlerts"})) {
        *count = count_hms_entries(child);
        return true;
      }
      if (key_matches_any(child->string, {"hms_count", "hmsCount", "hms_alert_count",
                                          "hmsAlertCount"}) &&
          json_int_like(child, count)) {
        return true;
      }
    }
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
      if (find_hms_count_recursive(child, count, depth + 1)) {
        return true;
      }
    }
  } else if (cJSON_IsArray(node)) {
    const int count_items = cJSON_GetArraySize(node);
    for (int i = 0; i < count_items; ++i) {
      if (find_hms_count_recursive(cJSON_GetArrayItem(node, i), count, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

std::string format_error_detail(int print_error_code, int hms_count) {
  if (print_error_code == 0 && hms_count == 0) {
    return {};
  }

  char error_buffer[32] = {};
  if (print_error_code != 0) {
    std::snprintf(error_buffer, sizeof(error_buffer), "%08X", print_error_code);
    std::string detail = "Print error ";
    detail += std::string(error_buffer, 4);
    detail += "_";
    detail += std::string(error_buffer + 4, 4);
    if (hms_count > 0) {
      detail += " + HMS ";
      detail += std::to_string(hms_count);
    }
    return detail;
  }

  return "HMS alerts: " + std::to_string(hms_count);
}

bool is_chamber_light_node(const std::string& node) {
  return node == "chamber_light" || node == "chamber_light2";
}

bool is_light_mode_on(const std::string& mode) {
  return mode == "on";
}

std::string build_ledctrl_payload(const char* node, bool on) {
  if (node == nullptr || *node == '\0') {
    return {};
  }

  char payload[192];
  std::snprintf(payload, sizeof(payload),
                "{\"system\":{\"sequence_id\":\"%u\",\"command\":\"ledctrl\","
                "\"led_node\":\"%s\",\"led_mode\":\"%s\"}}",
                static_cast<unsigned int>(esp_random()), node, on ? "on" : "off");
  return payload;
}

bool apply_chamber_light_report(const cJSON* object, bool* supported, bool* known, bool* on) {
  if (object == nullptr || supported == nullptr || known == nullptr || on == nullptr) {
    return false;
  }

  const cJSON* lights_report = cJSON_GetObjectItemCaseSensitive(object, "lights_report");
  if (!cJSON_IsArray(lights_report)) {
    return false;
  }

  bool seen = false;
  bool any_on = false;
  const int count = cJSON_GetArraySize(lights_report);
  for (int i = 0; i < count; ++i) {
    const cJSON* item = cJSON_GetArrayItem(lights_report, i);
    const std::string node = json_string_local(item, "node", {});
    if (!is_chamber_light_node(node)) {
      continue;
    }
    seen = true;
    if (is_light_mode_on(json_string_local(item, "mode", {}))) {
      any_on = true;
    }
  }

  if (seen) {
    *supported = true;
    *known = true;
    *on = any_on;
  }
  return seen;
}

float packed_temp_current_value(int packed, float fallback) {
  if (packed < 0) {
    return fallback;
  }
  return static_cast<float>(packed & 0xFFFF);
}

float normalize_temperature_candidate(float value) {
  if (value > static_cast<float>(0xFFFF)) {
    return packed_temp_current_value(static_cast<int>(value), value);
  }
  return value;
}

struct NozzleTemperatureBundle {
  float active = 0.0f;
  float secondary = 0.0f;
};

int extract_active_nozzle_index(const cJSON* device) {
  const cJSON* extruder = child_object_local(device, "extruder");
  return std::max(json_int_local(extruder, "state", 0) >> 4, 0);
}

void merge_nozzle_temp_candidates(const cJSON* info_array, int active_nozzle_index,
                                  float* active_temp, float* secondary_temp) {
  if (!cJSON_IsArray(info_array) || active_temp == nullptr || secondary_temp == nullptr) {
    return;
  }

  const int count = cJSON_GetArraySize(info_array);
  float first_temp = -1000.0f;
  float fallback_secondary = -1000.0f;
  for (int i = 0; i < count; ++i) {
    const cJSON* item = cJSON_GetArrayItem(info_array, i);
    if (!cJSON_IsObject(item)) {
      continue;
    }

    const float temp = normalize_temperature_candidate(
        json_number_local(item, "temp", -1000.0f));
    if (temp <= -999.0f) {
      continue;
    }

    if (first_temp < -999.0f) {
      first_temp = temp;
    }

    const int id = json_int_local(item, "id", -1);
    if (id == active_nozzle_index) {
      *active_temp = temp;
    } else if (id >= 0 && *secondary_temp <= 0.0f) {
      *secondary_temp = temp;
    } else if (fallback_secondary < -999.0f) {
      fallback_secondary = temp;
    }
  }

  if (*active_temp <= 0.0f && first_temp > -999.0f) {
    *active_temp = first_temp;
  }
  if (*secondary_temp <= 0.0f && fallback_secondary > -999.0f) {
    *secondary_temp = fallback_secondary;
  }
}

NozzleTemperatureBundle extract_cloud_nozzle_temperature_bundle(const cJSON* item,
                                                                float active_fallback,
                                                                float secondary_fallback) {
  NozzleTemperatureBundle bundle{active_fallback, secondary_fallback};
  const cJSON* print_history = child_object_local(item, "print_history_info") != nullptr
                                   ? child_object_local(item, "print_history_info")
                                   : child_object_local(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object_local(print_history, "subtask") : nullptr;
  const cJSON* item_print = child_object_local(item, "print");
  const cJSON* history_print = print_history != nullptr ? child_object_local(print_history, "print") : nullptr;
  const cJSON* subtask_print = subtask != nullptr ? child_object_local(subtask, "print") : nullptr;

  for (const cJSON* source : {item, print_history, subtask, item_print, history_print, subtask_print}) {
    if (source == nullptr) {
      continue;
    }

    const float direct = normalize_temperature_candidate(
        json_number_local(source, "nozzle_temper",
                          json_number_local(source, "nozzle_temp", -1000.0f)));
    if (direct > -999.0f) {
      bundle.active = direct;
    }

    const cJSON* device = child_object_local(source, "device");
    if (device == nullptr) {
      continue;
    }

    const int active_nozzle_index = extract_active_nozzle_index(device);
    merge_nozzle_temp_candidates(child_array_local(child_object_local(device, "nozzle"), "info"),
                                 active_nozzle_index, &bundle.active, &bundle.secondary);
    merge_nozzle_temp_candidates(child_array_local(child_object_local(device, "extruder"), "info"),
                                 active_nozzle_index, &bundle.active, &bundle.secondary);
  }

  if (bundle.active <= 0.0f) {
    if (find_number_for_keys_recursive(item,
                                       {"nozzle_temper", "nozzle_temp", "nozzle_temperature",
                                        "nozzleTemperature", "hotend_temp", "hotend_temperature",
                                        "hotendTemperature"},
                                       &bundle.active)) {
      bundle.active = normalize_temperature_candidate(bundle.active);
    }
  }
  if (bundle.secondary <= 0.0f) {
    if (find_number_for_keys_recursive(item,
                                       {"secondary_nozzle_temper", "secondary_nozzle_temp",
                                        "secondary_nozzle_temperature", "secondaryNozzleTemperature",
                                        "right_nozzle_temper", "right_nozzle_temp",
                                        "right_nozzle_temperature", "rightNozzleTemperature",
                                        "second_nozzle_temper", "second_nozzle_temp",
                                        "tool1_nozzle_temper", "tool1_nozzle_temp"},
                                       &bundle.secondary)) {
      bundle.secondary = normalize_temperature_candidate(bundle.secondary);
    }
  }

  return bundle;
}

float extract_cloud_bed_temperature_c(const cJSON* item, float fallback) {
  const cJSON* print_history = child_object_local(item, "print_history_info") != nullptr
                                   ? child_object_local(item, "print_history_info")
                                   : child_object_local(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object_local(print_history, "subtask") : nullptr;
  const cJSON* item_print = child_object_local(item, "print");
  const cJSON* history_print = print_history != nullptr ? child_object_local(print_history, "print") : nullptr;
  const cJSON* subtask_print = subtask != nullptr ? child_object_local(subtask, "print") : nullptr;

  for (const cJSON* source : {item, print_history, subtask, item_print, history_print, subtask_print}) {
    if (source == nullptr) {
      continue;
    }
    const cJSON* device = child_object_local(source, "device");
    if (const cJSON* bed_info = child_object_local(child_object_local(device, "bed"), "info");
        bed_info != nullptr) {
      const int packed = json_int_local(bed_info, "temp", -1);
      if (packed >= 0) {
        return packed_temp_current_value(packed, fallback);
      }
    }

    const int packed = json_int_local(device, "bed_temp", -1);
    if (packed >= 0) {
      return packed_temp_current_value(packed, fallback);
    }

    const float direct = normalize_temperature_candidate(
        json_number_local(source, "bed_temper",
                          json_number_local(source, "bed_temp", -1000.0f)));
    if (direct > -999.0f) {
      return direct;
    }
  }

  float value = fallback;
  return find_number_for_keys_recursive(item,
                                        {"bed_temper", "bed_temp", "bed_temperature",
                                         "bedTemperature", "hotbed_temper", "hotbed_temp",
                                         "hotbed_temperature", "hotbedTemperature"},
                                        &value)
             ? normalize_temperature_candidate(value)
             : fallback;
}

float extract_cloud_chamber_temperature_c(const cJSON* item, float fallback) {
  const cJSON* print_history = child_object_local(item, "print_history_info") != nullptr
                                   ? child_object_local(item, "print_history_info")
                                   : child_object_local(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object_local(print_history, "subtask") : nullptr;
  const cJSON* item_print = child_object_local(item, "print");
  const cJSON* history_print = print_history != nullptr ? child_object_local(print_history, "print") : nullptr;
  const cJSON* subtask_print = subtask != nullptr ? child_object_local(subtask, "print") : nullptr;

  for (const cJSON* source : {item, print_history, subtask, item_print, history_print, subtask_print}) {
    if (source == nullptr) {
      continue;
    }
    const cJSON* device = child_object_local(source, "device");
    if (const cJSON* ctc_info = child_object_local(child_object_local(device, "ctc"), "info");
        ctc_info != nullptr) {
      const int packed = json_int_local(ctc_info, "temp", -1);
      if (packed >= 0) {
        return packed_temp_current_value(packed, fallback);
      }
    }
    if (const cJSON* chamber_info = child_object_local(child_object_local(device, "chamber"), "info");
        chamber_info != nullptr) {
      const int packed = json_int_local(chamber_info, "temp", -1);
      if (packed >= 0) {
        return packed_temp_current_value(packed, fallback);
      }
    }

    const float direct = normalize_temperature_candidate(
        json_number_local(source, "chamber_temper",
                          json_number_local(source, "chamber_temp", -1000.0f)));
    if (direct > -999.0f) {
      return direct;
    }
  }

  float value = fallback;
  return find_number_for_keys_recursive(item,
                                        {"chamber_temper", "chamber_temp",
                                         "chamber_temperature", "chamberTemperature",
                                         "ctc_temperature", "ctcTemperature"},
                                        &value)
             ? normalize_temperature_candidate(value)
             : fallback;
}

int extract_cloud_print_error_code(const cJSON* item, int fallback) {
  int value = fallback;
  return find_int_for_keys_recursive(item,
                                     {"print_error", "printError", "print_error_code",
                                      "printErrorCode", "error_code", "errorCode"},
                                     &value)
             ? value
             : fallback;
}

int extract_cloud_hms_count(const cJSON* item, int fallback) {
  int count = fallback;
  return find_hms_count_recursive(item, &count) ? count : fallback;
}

PrinterModel detect_cloud_model(const cJSON* item, PrinterModel fallback) {
  if (item == nullptr) {
    return fallback;
  }

  const cJSON* print_history = child_object_local(item, "print_history_info") != nullptr
                                   ? child_object_local(item, "print_history_info")
                                   : child_object_local(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object_local(print_history, "subtask") : nullptr;
  const char* keys[] = {"dev_product_name", "device_name",  "product_name", "productName",
                        "printer_type",     "printerType",  "model",        "series",
                        "name"};

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }
    for (const char* key : keys) {
      if (const PrinterModel model = model_from_product_name(json_string_local(source, key, {}));
          model != PrinterModel::kUnknown) {
        return model;
      }
    }
  }

  return fallback;
}

}  // namespace

void BambuCloudClient::configure(BambuCloudCredentials credentials, std::string printer_serial) {
  stop_mqtt_client();
  credentials_ = std::move(credentials);
  requested_serial_ = std::move(printer_serial);
  resolved_serial_.clear();
  cached_preview_url_.clear();
  cached_preview_blob_.reset();
  clear_auth_state();
  mqtt_username_.clear();

  access_token_.clear();
  token_expiry_us_ = 0;
  if (config_store_ != nullptr) {
    access_token_ = config_store_->load_cloud_access_token();
    if (!access_token_.empty()) {
      token_expiry_us_ = INT64_MAX;
    }
  }

  BambuCloudSnapshot snapshot;
  snapshot.configured = credentials_.is_configured();
  snapshot.connected = false;
  if (!credentials_.is_configured()) {
    snapshot.detail = "Cloud login not configured";
  } else if (!access_token_.empty()) {
    snapshot.detail = "Restored Bambu Cloud session";
  } else {
    snapshot.detail = "Waiting for Wi-Fi for Bambu Cloud";
  }
  snapshot.capabilities = default_cloud_capabilities();
  snapshot.resolved_serial = requested_serial_;
  set_snapshot(std::move(snapshot));
}

void BambuCloudClient::submit_verification_code(std::string code) {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  pending_verification_code_ = std::move(code);
}

void BambuCloudClient::set_fetch_paused(bool paused) {
  const bool previous = fetch_paused_.exchange(paused);
  if (previous && !paused && task_handle_ != nullptr) {
    xTaskNotifyGive(task_handle_);
  }
}

void BambuCloudClient::set_preview_fetch_enabled(bool enabled) {
  const bool previous = preview_fetch_enabled_.exchange(enabled);
  if (!previous && enabled && task_handle_ != nullptr) {
    xTaskNotifyGive(task_handle_);
  }
}

esp_err_t BambuCloudClient::start() {
  if (task_handle_ != nullptr) {
    return ESP_OK;
  }

  const BaseType_t result =
      xTaskCreate(&BambuCloudClient::task_entry, "bambu_cloud", 10240, this, 4, &task_handle_);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}

BambuCloudSnapshot BambuCloudClient::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

bool BambuCloudClient::set_chamber_light(bool on) {
  const BambuCloudSnapshot current = snapshot();
  const bool supports_secondary = printer_model_has_secondary_chamber_light(current.model);

  auto publish_ledctrl = [&](const char* node) {
    const std::string payload = build_ledctrl_payload(node, on);
    if (payload.empty() || mqtt_client_ == nullptr || !mqtt_connected_.load() ||
        !mqtt_subscription_acknowledged_.load()) {
      return false;
    }

    const int msg_id =
        esp_mqtt_client_publish(mqtt_client_, mqtt_request_topic_.c_str(), payload.c_str(), 0, 0, 0);
    if (msg_id < 0) {
      ESP_LOGW(kTag, "Failed to publish cloud chamber light command for %s", node);
      return false;
    }
    return true;
  };

  const bool primary_ok = publish_ledctrl("chamber_light");
  const bool secondary_ok = !supports_secondary || publish_ledctrl("chamber_light2");
  if (!primary_ok || !secondary_ok) {
    return false;
  }

  publish_request(kPushAll);

  BambuCloudSnapshot updated = current;
  updated.last_update_ms = now_ms();
  updated.chamber_light_supported = true;
  updated.chamber_light_state_known = true;
  updated.chamber_light_on = on;
  updated.chamber_light_pending = true;
  updated.chamber_light_pending_since_ms = updated.last_update_ms;
  set_snapshot(std::move(updated));
  ESP_LOGI(kTag, "Cloud chamber light set to %s", on ? "on" : "off");
  return true;
}

void BambuCloudClient::mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                          int32_t event_id, void* event_data) {
  (void)base;
  (void)event_id;
  auto* client = static_cast<BambuCloudClient*>(handler_args);
  if (client != nullptr && event_data != nullptr) {
    client->handle_mqtt_event(static_cast<esp_mqtt_event_handle_t>(event_data));
  }
}

void BambuCloudClient::handle_mqtt_event(esp_mqtt_event_handle_t event) {
  if (event == nullptr) {
    return;
  }

  switch (static_cast<esp_mqtt_event_id_t>(event->event_id)) {
    case MQTT_EVENT_CONNECTED: {
      mqtt_connected_ = true;
      mqtt_subscription_acknowledged_ = false;
      received_live_payload_ = false;
      initial_sync_sent_ = false;
      delayed_start_sent_ = false;
      initial_sync_tick_ = 0;
      const int msg_id = esp_mqtt_client_subscribe(mqtt_client_, mqtt_report_topic_.c_str(), 1);
      if (msg_id >= 0) {
        ESP_LOGI(kTag, "Cloud MQTT subscribe queued for %s (msg_id=%d)",
                 mqtt_report_topic_.c_str(), msg_id);
      } else {
        ESP_LOGW(kTag, "Cloud MQTT subscribe failed for %s", mqtt_report_topic_.c_str());
      }
      break;
    }

    case MQTT_EVENT_SUBSCRIBED:
      mqtt_subscription_acknowledged_ = true;
      initial_sync_sent_ = true;
      delayed_start_sent_ = false;
      initial_sync_tick_ = xTaskGetTickCount();
      ESP_LOGI(kTag, "Cloud MQTT subscribe acknowledged (msg_id=%d), requesting sync",
               event->msg_id);
      request_initial_sync();
      break;

    case MQTT_EVENT_DISCONNECTED:
      mqtt_connected_ = false;
      mqtt_subscription_acknowledged_ = false;
      received_live_payload_ = false;
      initial_sync_sent_ = false;
      delayed_start_sent_ = false;
      initial_sync_tick_ = 0;
      ESP_LOGW(kTag, "Cloud MQTT disconnected");
      break;

    case MQTT_EVENT_DATA: {
      std::string topic;
      std::string payload;
      {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        if (event->current_data_offset == 0) {
          incoming_topic_.assign(event->topic, event->topic_len);
          incoming_payload_.clear();
          incoming_payload_.reserve(event->total_data_len);
        }

        incoming_payload_.append(event->data, event->data_len);
        if (incoming_payload_.size() >= static_cast<size_t>(event->total_data_len)) {
          topic = incoming_topic_;
          payload = incoming_payload_;
          incoming_topic_.clear();
          incoming_payload_.clear();
        }
      }

      if (!payload.empty() && topic == mqtt_report_topic_) {
        handle_report_payload(payload.data(), payload.size());
      }
      break;
    }

    case MQTT_EVENT_ERROR:
      mqtt_connected_ = false;
      mqtt_subscription_acknowledged_ = false;
      received_live_payload_ = false;
      initial_sync_sent_ = false;
      delayed_start_sent_ = false;
      initial_sync_tick_ = 0;
      ESP_LOGW(kTag, "Cloud MQTT transport error");
      break;

    default:
      break;
  }
}

void BambuCloudClient::stop_mqtt_client() {
  mqtt_connected_ = false;
  mqtt_subscription_acknowledged_ = false;
  received_live_payload_ = false;
  initial_sync_sent_ = false;
  delayed_start_sent_ = false;
  initial_sync_tick_ = 0;
  {
    std::lock_guard<std::mutex> lock(incoming_mutex_);
    incoming_topic_.clear();
    incoming_payload_.clear();
  }
  if (mqtt_client_ != nullptr) {
    esp_mqtt_client_stop(mqtt_client_);
    esp_mqtt_client_destroy(mqtt_client_);
    mqtt_client_ = nullptr;
  }
  mqtt_client_id_.clear();
  mqtt_report_topic_.clear();
  mqtt_request_topic_.clear();
}

bool BambuCloudClient::ensure_cloud_mqtt_identity() {
  if (!mqtt_username_.empty()) {
    return true;
  }
  if (access_token_.empty()) {
    return false;
  }

  std::string username = decode_username_from_access_token(access_token_);
  if (username.empty()) {
    int status_code = 0;
    std::string response_body;
    if (!perform_json_request(kPreferenceUrl, "GET", {}, access_token_, &status_code,
                              &response_body)) {
      ESP_LOGW(kTag, "Cloud MQTT identity lookup via preference API failed");
      return false;
    }
    if (status_code == 401 || status_code == 403) {
      clear_persisted_access_token();
      access_token_.clear();
      token_expiry_us_ = 0;
      mqtt_username_.clear();
      stop_mqtt_client();
      return false;
    }
    if (status_code >= 200 && status_code < 300) {
      cJSON* root = cJSON_Parse(response_body.c_str());
      if (root != nullptr) {
        const cJSON* data = child_object(root, "data");
        const int uid = json_int(root, "uid",
                                 json_int(data, "uid",
                                          json_int(root, "uidStr",
                                                   json_int(data, "uidStr", -1))));
        if (uid > 0) {
          username = "u_" + std::to_string(uid);
        }
        cJSON_Delete(root);
      }
    }
  }

  if (username.empty()) {
    ESP_LOGW(kTag, "Unable to derive Bambu Cloud MQTT username from token");
    return false;
  }

  mqtt_username_ = std::move(username);
  return true;
}

bool BambuCloudClient::ensure_mqtt_client_started() {
  const std::string serial = !resolved_serial_.empty() ? resolved_serial_ : requested_serial_;
  if (!network_ready_.load() || access_token_.empty() || mqtt_username_.empty() || serial.empty()) {
    if (network_ready_.load() && !access_token_.empty() &&
        (mqtt_username_.empty() || serial.empty())) {
      ESP_LOGW(kTag, "Cloud MQTT start deferred (username_ready=%s serial_ready=%s)",
               mqtt_username_.empty() ? "no" : "yes", serial.empty() ? "no" : "yes");
    }
    stop_mqtt_client();
    return false;
  }

  const std::string desired_report_topic = "device/" + serial + "/report";
  const std::string desired_request_topic = "device/" + serial + "/request";
  if (mqtt_client_ != nullptr &&
      (mqtt_report_topic_ != desired_report_topic || mqtt_request_topic_ != desired_request_topic)) {
    stop_mqtt_client();
  }

  if (mqtt_client_ != nullptr) {
    return true;
  }

  mqtt_client_id_ = "printsphere-cloud-" + std::to_string(static_cast<unsigned int>(esp_random()));
  mqtt_report_topic_ = desired_report_topic;
  mqtt_request_topic_ = desired_request_topic;

  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
  mqtt_cfg.broker.address.hostname = kCloudMqttHost;
  mqtt_cfg.broker.address.port = kCloudMqttPort;
  mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  mqtt_cfg.credentials.client_id = mqtt_client_id_.c_str();
  mqtt_cfg.credentials.username = mqtt_username_.c_str();
  mqtt_cfg.credentials.authentication.password = access_token_.c_str();
  mqtt_cfg.session.keepalive = 30;
  mqtt_cfg.session.disable_clean_session = false;
  mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  mqtt_cfg.buffer.size = 16384;
  mqtt_cfg.buffer.out_size = 4096;
  mqtt_cfg.network.timeout_ms = 10000;
  mqtt_cfg.network.reconnect_timeout_ms = 5000;

  mqtt_client_ = esp_mqtt_client_init(&mqtt_cfg);
  if (mqtt_client_ == nullptr) {
    ESP_LOGW(kTag, "Failed to create Bambu Cloud MQTT client");
    return false;
  }

  esp_mqtt_client_register_event(mqtt_client_, MQTT_EVENT_ANY,
                                 &BambuCloudClient::mqtt_event_handler, this);
  if (esp_mqtt_client_start(mqtt_client_) != ESP_OK) {
    ESP_LOGW(kTag, "Failed to start Bambu Cloud MQTT client");
    esp_mqtt_client_destroy(mqtt_client_);
    mqtt_client_ = nullptr;
    return false;
  }

  ESP_LOGI(kTag, "Connecting to Bambu Cloud MQTT %s:%u (serial=%s, user=%s)", kCloudMqttHost,
           static_cast<unsigned int>(kCloudMqttPort), serial.c_str(), mqtt_username_.c_str());
  return true;
}

bool BambuCloudClient::publish_request(const char* payload) {
  if (payload == nullptr || mqtt_client_ == nullptr || !mqtt_connected_.load() ||
      !mqtt_subscription_acknowledged_.load()) {
    return false;
  }

  const int msg_id = esp_mqtt_client_publish(mqtt_client_, mqtt_request_topic_.c_str(), payload, 0,
                                             1, 0);
  if (msg_id < 0) {
    ESP_LOGW(kTag, "Failed to publish cloud MQTT request to %s", mqtt_request_topic_.c_str());
    return false;
  }
  return true;
}

void BambuCloudClient::request_initial_sync() {
  publish_request(kGetVersion);
  publish_request(kPushAll);
}

void BambuCloudClient::handle_report_payload(const char* payload, size_t length) {
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (root == nullptr) {
    ESP_LOGW(kTag, "Cloud MQTT payload JSON parse failed");
    return;
  }

  const cJSON* print = child_object(root, "print");
  if (cJSON_IsObject(print)) {
    received_live_payload_ = true;
    initial_sync_sent_ = false;
    delayed_start_sent_ = false;
    initial_sync_tick_ = 0;

    BambuCloudSnapshot current = snapshot();
    const PrintLifecycleState previous_lifecycle = current.lifecycle;
    const bool previous_non_error_stop = current.non_error_stop;
    const bool previous_has_error = current.has_error;
    current.configured = true;
    current.connected = true;
    current.capabilities = default_cloud_capabilities();
    current.last_update_ms = now_ms();
    current.live_data_last_update_ms = current.last_update_ms;
    current.model = detect_cloud_model(print, current.model);
    current.chamber_light_supported =
        current.chamber_light_supported || printer_model_has_chamber_light(current.model);
    current.resolved_serial =
        !resolved_serial_.empty() ? resolved_serial_
                                  : (current.resolved_serial.empty() ? requested_serial_
                                                                     : current.resolved_serial);

    const std::string status_text =
        json_string(print, "gcode_state", extract_status_text(print));
    const std::string stage_text = extract_stage_text(print);
    const bool has_status_update = !status_text.empty() || !stage_text.empty();
    const PrintLifecycleState lifecycle = cloud_lifecycle_from_status(status_text);
    if (!status_text.empty()) {
      current.raw_status = status_text;
      current.lifecycle = lifecycle;
      current.stage =
          !stage_text.empty() ? stage_text : cloud_stage_label_for(status_text, lifecycle);
    }
    if (!stage_text.empty()) {
      current.raw_stage = stage_text;
      current.stage = stage_text;
    }

    float progress = extract_progress(print);
    if (lifecycle == PrintLifecycleState::kFinished && progress < 100.0f) {
      progress = 100.0f;
    }
    if (progress > 0.0f || lifecycle == PrintLifecycleState::kFinished ||
        lifecycle == PrintLifecycleState::kIdle || lifecycle == PrintLifecycleState::kError) {
      current.progress_percent = progress;
    }

    const uint32_t remaining_seconds = extract_remaining_seconds(print);
    if (remaining_seconds > 0U || lifecycle == PrintLifecycleState::kFinished ||
        lifecycle == PrintLifecycleState::kIdle || lifecycle == PrintLifecycleState::kError) {
      current.remaining_seconds = remaining_seconds;
    }

    const uint16_t current_layer = extract_current_layer(print);
    if (current_layer > 0U || lifecycle == PrintLifecycleState::kFinished ||
        lifecycle == PrintLifecycleState::kIdle || lifecycle == PrintLifecycleState::kError) {
      current.current_layer = current_layer;
    }

    const uint16_t total_layers = extract_total_layers(print);
    if (total_layers > 0U || lifecycle == PrintLifecycleState::kFinished ||
        lifecycle == PrintLifecycleState::kIdle || lifecycle == PrintLifecycleState::kError) {
      current.total_layers = total_layers;
    }

    const NozzleTemperatureBundle nozzle_temps =
        extract_cloud_nozzle_temperature_bundle(print, current.nozzle_temp_c,
                                                current.secondary_nozzle_temp_c);
    current.nozzle_temp_c = nozzle_temps.active;
    current.secondary_nozzle_temp_c = nozzle_temps.secondary;
    current.bed_temp_c = extract_cloud_bed_temperature_c(print, current.bed_temp_c);
    current.chamber_temp_c =
        extract_cloud_chamber_temperature_c(print, current.chamber_temp_c);

    current.print_error_code =
        normalize_cloud_print_error_code(extract_cloud_print_error_code(print, current.print_error_code));
    current.hms_alert_count = static_cast<uint16_t>(
        std::max(extract_cloud_hms_count(print, current.hms_alert_count), 0));
    if (has_status_update) {
      current.non_error_stop = cloud_status_is_non_error_stop(status_text, current.print_error_code,
                                                              current.hms_alert_count);
      current.has_error = (!current.non_error_stop &&
                           current.lifecycle == PrintLifecycleState::kError) ||
                          current.print_error_code != 0 || current.hms_alert_count > 0U;
    } else {
      // Cloud push_status often arrives as partial updates. If a packet has no status/stage,
      // preserve the last known lifecycle/error interpretation instead of re-arming a stale FAIL.
      current.lifecycle = previous_lifecycle;
      current.non_error_stop = previous_non_error_stop;
      current.has_error = previous_has_error;
    }
    const bool saw_light_report =
        apply_chamber_light_report(print, &current.chamber_light_supported,
                                   &current.chamber_light_state_known, &current.chamber_light_on);
    if (saw_light_report) {
      current.chamber_light_pending = false;
      current.chamber_light_pending_since_ms = 0;
    }

    const std::string error_detail =
        format_error_detail(current.print_error_code, current.hms_alert_count);
    if (!error_detail.empty()) {
      current.detail = error_detail;
    } else if (!current.stage.empty()) {
      current.detail = current.stage;
    } else if (current.detail.empty()) {
      current.detail = "Connected to Bambu Cloud";
    }

    set_snapshot(std::move(current));
    cJSON_Delete(root);
    return;
  }

  const cJSON* info = child_object(root, "info");
  if (cJSON_IsObject(info)) {
    cJSON_Delete(root);
    handle_info_payload(payload, length);
    return;
  }

  cJSON_Delete(root);
}

void BambuCloudClient::handle_info_payload(const char* payload, size_t length) {
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (root == nullptr) {
    return;
  }

  const cJSON* info = child_object(root, "info");
  if (!cJSON_IsObject(info)) {
    cJSON_Delete(root);
    return;
  }

  BambuCloudSnapshot current = snapshot();
  current.configured = true;
  current.connected = true;
  current.capabilities = default_cloud_capabilities();
  current.last_update_ms = now_ms();
  current.model = detect_cloud_model(info, current.model);
  current.chamber_light_supported =
      current.chamber_light_supported || printer_model_has_chamber_light(current.model);
  const std::string serial =
      extract_device_serial(info).empty() ? requested_serial_ : extract_device_serial(info);
  if (!serial.empty()) {
    if (serial != resolved_serial_) {
      resolved_serial_ = serial;
      stop_mqtt_client();
    }
    current.resolved_serial = serial;
  }
  if (current.detail.empty()) {
    current.detail = "Connected to Bambu Cloud";
  }
  set_snapshot(std::move(current));
  cJSON_Delete(root);
}

void BambuCloudClient::task_entry(void* context) {
  static_cast<BambuCloudClient*>(context)->task_loop();
}

void BambuCloudClient::task_loop() {
  const TickType_t task_start_tick = xTaskGetTickCount();
  TickType_t last_preview_fetch_tick = 0;
  bool last_preview_fetch_enabled = false;
  TickType_t last_binding_fetch_tick = 0;
  while (true) {
    if (reload_requested_.exchange(false) && config_store_ != nullptr) {
      stop_mqtt_client();
      configure(config_store_->load_cloud_credentials(), config_store_->load_printer_config().serial);
      last_preview_fetch_tick = 0;
      last_preview_fetch_enabled = false;
      last_binding_fetch_tick = 0;
    }

    if (!credentials_.is_configured()) {
      stop_mqtt_client();
      BambuCloudSnapshot waiting = snapshot();
      waiting.configured = false;
      waiting.connected = false;
      waiting.verification_required = false;
      waiting.tfa_required = false;
      waiting.detail = "Cloud login not configured";
      set_snapshot(std::move(waiting));
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    while (!network_ready_.load()) {
      stop_mqtt_client();
      BambuCloudSnapshot waiting = snapshot();
      waiting.configured = true;
      waiting.connected = false;
      waiting.detail = "Waiting for Wi-Fi for Bambu Cloud";
      set_snapshot(std::move(waiting));
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    const int64_t now_us = esp_timer_get_time();
    if (access_token_.empty() || now_us >= token_expiry_us_) {
      stop_mqtt_client();
      mqtt_username_.clear();
      if (waiting_for_user_code()) {
        BambuCloudSnapshot waiting = snapshot();
        waiting.configured = true;
        waiting.connected = false;
        waiting.verification_required = true;
        waiting.tfa_required = auth_mode() == AuthMode::kTfaCode;
        waiting.detail = waiting.tfa_required ? "Bambu Cloud requires 2FA code"
                                              : "Bambu Cloud email code required";
        set_snapshot(std::move(waiting));
      } else {
        BambuCloudSnapshot logging_in = snapshot();
        logging_in.configured = true;
        logging_in.connected = false;
        logging_in.verification_required = false;
        logging_in.tfa_required = false;
        logging_in.detail = "Logging in to Bambu Cloud";
        set_snapshot(std::move(logging_in));
      }

      if (!login()) {
        const TickType_t retry_delay =
            waiting_for_user_code() ? pdMS_TO_TICKS(1000) : pdMS_TO_TICKS(15000);
        TickType_t waited = 0;
        while (waited < retry_delay && !reload_requested_.load()) {
          constexpr TickType_t kRetrySlice = pdMS_TO_TICKS(1000);
          const TickType_t remaining = retry_delay - waited;
          const TickType_t slice = remaining < kRetrySlice ? remaining : kRetrySlice;
          vTaskDelay(slice);
          waited += slice;
        }
        continue;
      }
    }

    const bool live_mqtt_enabled = live_mqtt_enabled_.load();
    if (!live_mqtt_enabled) {
      stop_mqtt_client();
      mqtt_connected_ = false;
      mqtt_subscription_acknowledged_ = false;
    } else if (!ensure_cloud_mqtt_identity()) {
      mqtt_connected_ = false;
      mqtt_subscription_acknowledged_ = false;
    } else {
      ensure_mqtt_client_started();
    }

    const TickType_t now_tick = xTaskGetTickCount();
    const bool low_power = low_power_mode_.load();
    const bool fetch_paused = fetch_paused_.load();
    const bool preview_fetch_enabled = preview_fetch_enabled_.load();
    const bool waiting_for_live_payload =
        live_mqtt_enabled && mqtt_connected_.load() && mqtt_subscription_acknowledged_.load() &&
        initial_sync_sent_.load() && !received_live_payload_.load();
    const uint32_t initial_sync_tick = initial_sync_tick_.load();
    if (waiting_for_live_payload && initial_sync_tick != 0 &&
        static_cast<TickType_t>(now_tick - initial_sync_tick) >= kCloudInitialSyncRetryDelay &&
        !delayed_start_sent_.load()) {
      ESP_LOGW(kTag, "No cloud status payload received after subscribe, sending delayed pushall");
      publish_request(kPushAll);
      delayed_start_sent_ = true;
    }
    if (waiting_for_live_payload && initial_sync_tick != 0 && delayed_start_sent_.load() &&
        static_cast<TickType_t>(now_tick - initial_sync_tick) >= kCloudInitialSyncTimeout) {
      ESP_LOGW(kTag, "Still no cloud status payload after delayed pushall, restarting cloud MQTT");
      stop_mqtt_client();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    const TickType_t preview_poll_interval =
        low_power ? kCloudPreviewPollLowPower : kCloudPreviewPollActiveView;
    const TickType_t status_poll_interval =
        low_power ? kCloudStatusPollLowPower : kCloudStatusPollIdle;
    const bool initial_preview_window_open =
        (now_tick - task_start_tick) >= kCloudInitialPreviewDelay;
    const bool bindings_due =
        last_binding_fetch_tick == 0 || resolved_serial_.empty() ||
        ((now_tick - last_binding_fetch_tick) >= kCloudBindingRefresh);
    const bool preview_due =
        preview_fetch_enabled && initial_preview_window_open &&
        ((last_preview_fetch_tick == 0) || !last_preview_fetch_enabled ||
         ((now_tick - last_preview_fetch_tick) >= preview_poll_interval));

    bool bindings_ok = true;
    bool preview_ok = true;
    if (!fetch_paused) {
      if (bindings_due) {
        bindings_ok = fetch_bindings();
        last_binding_fetch_tick = now_tick;
      }
      if (preview_fetch_enabled && preview_due && !waiting_for_live_payload) {
        preview_ok = fetch_latest_preview(true);
        if (preview_ok) {
          last_preview_fetch_tick = now_tick;
        }
      }
    }
    last_preview_fetch_enabled = preview_fetch_enabled;

    if (!bindings_ok && !preview_ok) {
      vTaskDelay(status_poll_interval);
      continue;
    }

    const BambuCloudSnapshot current = snapshot();
    const bool active_print = current.lifecycle == PrintLifecycleState::kPreparing ||
                              current.lifecycle == PrintLifecycleState::kPrinting ||
                              current.lifecycle == PrintLifecycleState::kPaused;
    TickType_t next_delay = fetch_paused ? pdMS_TO_TICKS(1000)
                                         : (active_print ? kCloudStatusPollActive
                                                         : status_poll_interval);
    if (waiting_for_live_payload && next_delay > pdMS_TO_TICKS(1000)) {
      next_delay = pdMS_TO_TICKS(1000);
    }
    if (!fetch_paused && preview_fetch_enabled && next_delay > kCloudPreviewWakePoll) {
      next_delay = kCloudPreviewWakePoll;
    }
    ulTaskNotifyTake(pdTRUE, next_delay);
  }
}

bool BambuCloudClient::login() {
  const AuthMode mode = auth_mode();
  if (mode == AuthMode::kEmailCode) {
    const std::string code = pending_verification_code();
    if (code.empty()) {
      return false;
    }
    return authenticate_with_email_code(code);
  }

  if (mode == AuthMode::kTfaCode) {
    const std::string code = pending_verification_code();
    if (code.empty()) {
      return false;
    }
    return authenticate_with_tfa_code(code);
  }

  return authenticate_with_password();
}

bool BambuCloudClient::authenticate_with_password() {
  cJSON* body = cJSON_CreateObject();
  if (body == nullptr) {
    return false;
  }
  cJSON_AddStringToObject(body, "account", credentials_.email.c_str());
  cJSON_AddStringToObject(body, "password", credentials_.password.c_str());
  cJSON_AddStringToObject(body, "apiError", "");

  char* payload = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (payload == nullptr) {
    return false;
  }

  const std::string request_body(payload);
  cJSON_free(payload);

  int status_code = 0;
  std::string response_body;
  if (!perform_json_request(kLoginUrl, "POST", request_body, {}, &status_code, &response_body)) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.detail = "Bambu Cloud login request failed";
    set_snapshot(std::move(failed));
    return false;
  }

  cJSON* root = cJSON_Parse(response_body.c_str());
  if (root == nullptr) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.detail = "Bambu Cloud login returned invalid JSON";
    set_snapshot(std::move(failed));
    return false;
  }

  const cJSON* data = child_object(root, "data");
  const std::string token =
      json_string(root, "accessToken", json_string(data, "accessToken", {}));
  const std::string api_error =
      json_string(root, "apiError", json_string(data, "apiError", json_string(root, "msg", {})));
  const std::string login_type =
      json_string(root, "loginType", json_string(data, "loginType", {}));
  const int expires_in = json_int(root, "expiresIn", json_int(data, "expiresIn", 3600));

  if (login_type == "verifyCode") {
    set_auth_mode(AuthMode::kEmailCode);
    BambuCloudSnapshot verification = snapshot();
    verification.connected = false;
    verification.verification_required = true;
    verification.tfa_required = false;
    verification.detail = "Bambu Cloud sent email code; enter it in setup portal";
    set_snapshot(std::move(verification));
    cJSON_Delete(root);
    return false;
  }

  if (login_type == "tfa") {
    set_auth_mode(AuthMode::kTfaCode, json_string(root, "tfaKey", json_string(data, "tfaKey", {})));
    BambuCloudSnapshot verification = snapshot();
    verification.connected = false;
    verification.verification_required = true;
    verification.tfa_required = true;
    verification.detail = "Bambu Cloud requires 2FA code";
    set_snapshot(std::move(verification));
    cJSON_Delete(root);
    return false;
  }

  if (status_code < 200 || status_code >= 300 || token.empty()) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.verification_required = false;
    failed.tfa_required = false;
    if (!api_error.empty()) {
      failed.detail = "Bambu Cloud login failed: " + api_error;
    } else {
      failed.detail = "Bambu Cloud login rejected";
    }
    set_snapshot(std::move(failed));
    cJSON_Delete(root);
    return false;
  }

  access_token_ = token;
  token_expiry_us_ = esp_timer_get_time() + static_cast<int64_t>(std::max(expires_in - 60, 60)) * 1000000LL;
  mqtt_username_.clear();
  stop_mqtt_client();
  persist_access_token();
  clear_auth_state();

  BambuCloudSnapshot success = snapshot();
  success.configured = true;
  success.connected = true;
  success.detail = "Connected to Bambu Cloud";
  success.verification_required = false;
  success.tfa_required = false;
  set_snapshot(std::move(success));
  ESP_LOGI(kTag, "Bambu Cloud login successful");

  cJSON_Delete(root);
  return true;
}

bool BambuCloudClient::authenticate_with_email_code(const std::string& code) {
  cJSON* body = cJSON_CreateObject();
  if (body == nullptr) {
    return false;
  }
  cJSON_AddStringToObject(body, "account", credentials_.email.c_str());
  cJSON_AddStringToObject(body, "code", code.c_str());

  char* payload = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (payload == nullptr) {
    return false;
  }

  const std::string request_body(payload);
  cJSON_free(payload);

  int status_code = 0;
  std::string response_body;
  if (!perform_json_request(kLoginUrl, "POST", request_body, {}, &status_code, &response_body)) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.verification_required = true;
    failed.tfa_required = false;
    failed.detail = "Bambu Cloud email-code login failed";
    set_snapshot(std::move(failed));
    return false;
  }

  cJSON* root = cJSON_Parse(response_body.c_str());
  if (root == nullptr) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.verification_required = true;
    failed.tfa_required = false;
    failed.detail = "Bambu Cloud email-code response invalid";
    set_snapshot(std::move(failed));
    return false;
  }

  const cJSON* data = child_object(root, "data");
  const std::string token =
      json_string(root, "accessToken", json_string(data, "accessToken", {}));

  if (status_code >= 200 && status_code < 300 && !token.empty()) {
    const int expires_in = json_int(root, "expiresIn", json_int(data, "expiresIn", 3600));
    access_token_ =
        token;
    token_expiry_us_ =
        esp_timer_get_time() + static_cast<int64_t>(std::max(expires_in - 60, 60)) * 1000000LL;
    mqtt_username_.clear();
    stop_mqtt_client();
    persist_access_token();
    clear_auth_state();

    BambuCloudSnapshot success = snapshot();
    success.configured = true;
    success.connected = true;
    success.detail = "Connected to Bambu Cloud";
    success.verification_required = false;
    success.tfa_required = false;
    set_snapshot(std::move(success));
    ESP_LOGI(kTag, "Bambu Cloud login successful with email code");
    cJSON_Delete(root);
    return true;
  }

  const int error_code = json_int(root, "code", json_int(data, "code", -1));
  BambuCloudSnapshot failed = snapshot();
  failed.connected = false;
  failed.verification_required = true;
  failed.tfa_required = false;
  if (status_code == 400 && error_code == 1) {
    clear_pending_code();
    request_email_verification_code();
    failed.detail = "Bambu Cloud email code expired; new code requested";
  } else if (status_code == 400 && error_code == 2) {
    clear_pending_code();
    failed.detail = "Bambu Cloud email code incorrect";
  } else {
    failed.detail = "Bambu Cloud email-code login rejected";
  }
  set_snapshot(std::move(failed));
  cJSON_Delete(root);
  return false;
}

bool BambuCloudClient::authenticate_with_tfa_code(const std::string& code) {
  cJSON* body = cJSON_CreateObject();
  if (body == nullptr) {
    return false;
  }

  std::string tfa_key;
  {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    tfa_key = tfa_key_;
  }

  cJSON_AddStringToObject(body, "tfaKey", tfa_key.c_str());
  cJSON_AddStringToObject(body, "tfaCode", code.c_str());

  char* payload = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (payload == nullptr) {
    return false;
  }

  const std::string request_body(payload);
  cJSON_free(payload);

  int status_code = 0;
  std::string response_body;
  if (!perform_json_request(kTfaLoginUrl, "POST", request_body, {}, &status_code, &response_body)) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.verification_required = true;
    failed.tfa_required = true;
    failed.detail = "Bambu Cloud 2FA login failed";
    set_snapshot(std::move(failed));
    return false;
  }

  BambuCloudSnapshot failed = snapshot();
  failed.connected = false;
  failed.verification_required = true;
  failed.tfa_required = true;
  failed.detail = (status_code >= 200 && status_code < 300)
                      ? "Bambu Cloud 2FA returned token cookie, not yet handled on ESP"
                      : "Bambu Cloud 2FA rejected";
  set_snapshot(std::move(failed));
  return false;
}

bool BambuCloudClient::request_email_verification_code() {
  cJSON* body = cJSON_CreateObject();
  if (body == nullptr) {
    return false;
  }
  cJSON_AddStringToObject(body, "email", credentials_.email.c_str());
  cJSON_AddStringToObject(body, "type", "codeLogin");

  char* payload = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (payload == nullptr) {
    return false;
  }

  const std::string request_body(payload);
  cJSON_free(payload);

  int status_code = 0;
  std::string response_body;
  const bool success =
      perform_json_request(kEmailCodeUrl, "POST", request_body, {}, &status_code, &response_body);
  return success && status_code >= 200 && status_code < 300;
}

bool BambuCloudClient::fetch_bindings() {
  int status_code = 0;
  std::string response_body;
  if (!perform_json_request(kBindUrl, "GET", {}, access_token_, &status_code, &response_body)) {
    return false;
  }
  if (status_code == 401 || status_code == 403) {
    clear_persisted_access_token();
    access_token_.clear();
    token_expiry_us_ = 0;
    mqtt_username_.clear();
    stop_mqtt_client();
    BambuCloudSnapshot expired = snapshot();
    expired.connected = false;
    expired.detail = "Bambu Cloud token expired";
    set_snapshot(std::move(expired));
    return false;
  }
  if (status_code < 200 || status_code >= 300) {
    return false;
  }

  cJSON* root = cJSON_Parse(response_body.c_str());
  if (root == nullptr) {
    return false;
  }

  const cJSON* data = child_object(root, "data");
  const cJSON* devices = child_array(root, "devices");
  if (devices == nullptr && data != nullptr) {
    devices = child_array(data, "devices");
  }

  std::string best_serial = requested_serial_;
  const cJSON* best_device = nullptr;
  if (cJSON_IsArray(devices)) {
    const int count = cJSON_GetArraySize(devices);
    for (int i = 0; i < count; ++i) {
      const cJSON* item = cJSON_GetArrayItem(devices, i);
      const std::string candidate = extract_device_serial(item);
      if (candidate.empty()) {
        continue;
      }
      if (!requested_serial_.empty() && candidate == requested_serial_) {
        best_serial = candidate;
        best_device = item;
        break;
      }
      if (best_serial.empty()) {
        best_serial = candidate;
        best_device = item;
      } else if (best_device == nullptr) {
        best_device = item;
      }
    }
  }

  BambuCloudSnapshot current = snapshot();
  const bool preserve_live_state = is_recent_live_data(current.live_data_last_update_ms);
  const bool preserve_optimistic_light =
      current.chamber_light_pending &&
      is_recent_optimistic_light_state(current.chamber_light_pending_since_ms);
  const bool pending_light_on = current.chamber_light_on;
  if (current.chamber_light_pending && !preserve_optimistic_light) {
    current.chamber_light_pending = false;
    current.chamber_light_pending_since_ms = 0;
  }
  current.configured = true;
  current.capabilities = default_cloud_capabilities();
  current.last_update_ms = now_ms();
  current.connected = true;
  if (current.detail.empty() || current.detail == "Restored Bambu Cloud session") {
    current.detail = "Connected to Bambu Cloud";
  }
  if (!best_serial.empty()) {
    const bool serial_changed = best_serial != current.resolved_serial;
    resolved_serial_ = best_serial;
    current.resolved_serial = best_serial;
    if (serial_changed) {
      stop_mqtt_client();
      ESP_LOGI(kTag, "Cloud device binding resolved serial=%s", best_serial.c_str());
    }
  }

  if (best_device != nullptr) {
    current.model = detect_cloud_model(best_device, current.model);
    current.chamber_light_supported =
        current.chamber_light_supported || printer_model_has_chamber_light(current.model);
    const bool saw_light_report =
        apply_chamber_light_report(best_device, &current.chamber_light_supported,
                                   &current.chamber_light_state_known, &current.chamber_light_on);
    if (preserve_optimistic_light) {
      if (saw_light_report && current.chamber_light_state_known &&
          current.chamber_light_on == pending_light_on) {
        current.chamber_light_pending = false;
        current.chamber_light_pending_since_ms = 0;
      } else {
        current.chamber_light_supported = true;
        current.chamber_light_state_known = true;
        current.chamber_light_on = pending_light_on;
      }
    } else if (saw_light_report) {
      current.chamber_light_pending = false;
      current.chamber_light_pending_since_ms = 0;
    }
    const bool printer_online = json_bool(best_device, "online", true);
    const std::string print_status =
        json_string(best_device, "print_status",
                    json_string(best_device, "printStatus", json_string(best_device, "status", {})));
    const std::string current_stage =
        json_string(best_device, "current_stage",
                    json_string(best_device, "currentStage", json_string(best_device, "stage", {})));
    const std::string print_type =
        json_string(best_device, "print_type", json_string(best_device, "printType", {}));
    const float progress_value =
        json_number(best_device, "mc_percent", json_number(best_device, "progress", -1.0f));
    const int remaining_minutes =
        json_int(best_device, "mc_remaining_time",
                 json_int(best_device, "remaining_time", json_int(best_device, "remainingMinutes", -1)));
    const NozzleTemperatureBundle nozzle_temps =
        extract_cloud_nozzle_temperature_bundle(best_device, current.nozzle_temp_c,
                                               current.secondary_nozzle_temp_c);
    const float bed_temp_c = extract_cloud_bed_temperature_c(best_device, current.bed_temp_c);
    const float chamber_temp_c =
        extract_cloud_chamber_temperature_c(best_device, current.chamber_temp_c);
    int print_error_code = normalize_cloud_print_error_code(
        extract_cloud_print_error_code(best_device, current.print_error_code));
    const int hms_count = extract_cloud_hms_count(best_device, current.hms_alert_count);
    std::string effective_status = print_status;
    std::string effective_stage = current_stage;
    float effective_progress_value = progress_value;
    int effective_remaining_minutes = remaining_minutes;
    const bool stale_failed_state =
        cloud_rest_failure_looks_stale(print_status, current_stage, print_type, hms_count);
    if (stale_failed_state) {
      effective_status = "IDLE";
      if (effective_stage.empty()) {
        effective_stage = "Idle";
      }
      effective_progress_value = 0.0f;
      effective_remaining_minutes = 0;
      print_error_code = 0;
    }
    const PrintLifecycleState lifecycle = cloud_lifecycle_from_status(effective_status);
    const std::string lifecycle_stage = cloud_stage_label_for(effective_status, lifecycle);
    const std::string stage = !effective_stage.empty() ? effective_stage : lifecycle_stage;
    const bool non_error_stop =
        cloud_status_is_non_error_stop(effective_status, print_error_code, hms_count);
    const std::string error_detail =
        stale_failed_state ? std::string{} : format_error_detail(print_error_code, hms_count);
    current.connected = true;
    if (current.detail.empty() || current.detail == "Restored Bambu Cloud session" ||
        current.detail == "Connected to Bambu Cloud" ||
        current.detail == "Connected to Bambu Cloud, no cover image yet") {
      current.detail = printer_online ? "Connected to Bambu Cloud" : "Printer offline in Bambu Cloud";
    }

    if (!preserve_live_state) {
      current.nozzle_temp_c = nozzle_temps.active;
      current.secondary_nozzle_temp_c = nozzle_temps.secondary;
      current.bed_temp_c = bed_temp_c;
      current.chamber_temp_c = chamber_temp_c;
      current.print_error_code = print_error_code;
      current.hms_alert_count = static_cast<uint16_t>(std::max(hms_count, 0));
      current.non_error_stop = non_error_stop;

      if (effective_progress_value >= 0.0f) {
        current.progress_percent =
            effective_progress_value <= 1.0f ? (effective_progress_value * 100.0f)
                                             : effective_progress_value;
      }
      if (effective_remaining_minutes >= 0) {
        current.remaining_seconds = static_cast<uint32_t>(effective_remaining_minutes) * 60U;
      }

      if (!effective_status.empty()) {
        if (stage != current.stage || lifecycle != current.lifecycle ||
            effective_stage != current.raw_stage) {
          ESP_LOGI(kTag, "Cloud printer state: %s", effective_status.c_str());
        }
        current.raw_status = effective_status;
        current.raw_stage = effective_stage;
        current.lifecycle = lifecycle;
        current.stage = stage;
        current.has_error =
            ((!non_error_stop && lifecycle == PrintLifecycleState::kError) || print_error_code != 0 ||
             hms_count > 0);
        if (lifecycle == PrintLifecycleState::kFinished && current.progress_percent < 100.0f) {
          current.progress_percent = 100.0f;
        }
        if (lifecycle == PrintLifecycleState::kIdle || lifecycle == PrintLifecycleState::kFinished ||
            lifecycle == PrintLifecycleState::kError) {
          current.remaining_seconds = 0U;
        }
      } else if (!effective_stage.empty()) {
        current.raw_stage = effective_stage;
        current.stage = effective_stage;
      }

      if (!error_detail.empty()) {
        current.detail = error_detail;
      }
      if (print_error_code != 0 || hms_count > 0) {
        current.has_error = true;
      }
    } else {
      if (current.nozzle_temp_c <= 0.0f && nozzle_temps.active > 0.0f) {
        current.nozzle_temp_c = nozzle_temps.active;
      }
      if (current.secondary_nozzle_temp_c <= 0.0f && nozzle_temps.secondary > 0.0f) {
        current.secondary_nozzle_temp_c = nozzle_temps.secondary;
      }
      if (current.bed_temp_c <= 0.0f && bed_temp_c > 0.0f) {
        current.bed_temp_c = bed_temp_c;
      }
      if (current.chamber_temp_c <= 0.0f && chamber_temp_c > 0.0f) {
        current.chamber_temp_c = chamber_temp_c;
      }
      if (current.progress_percent <= 0.0f && effective_progress_value >= 0.0f) {
        current.progress_percent =
            effective_progress_value <= 1.0f ? (effective_progress_value * 100.0f)
                                             : effective_progress_value;
      }
      if (current.remaining_seconds == 0U && effective_remaining_minutes >= 0) {
        current.remaining_seconds = static_cast<uint32_t>(effective_remaining_minutes) * 60U;
      }
      const uint16_t current_layer = extract_current_layer(best_device);
      if (current.current_layer == 0U && current_layer > 0U) {
        current.current_layer = current_layer;
      }
      const uint16_t total_layers = extract_total_layers(best_device);
      if (current.total_layers == 0U && total_layers > 0U) {
        current.total_layers = total_layers;
      }
    }
  }

  set_snapshot(std::move(current));

  cJSON_Delete(root);
  return true;
}

bool BambuCloudClient::fetch_latest_preview(bool allow_preview_download) {
  int status_code = 0;
  std::string response_body;
  if (!perform_json_request(kTasksUrl, "GET", {}, access_token_, &status_code, &response_body)) {
    ESP_LOGW(kTag, "Bambu Cloud tasks request failed");
    return false;
  }

  if (status_code == 401 || status_code == 403) {
    clear_persisted_access_token();
    access_token_.clear();
    token_expiry_us_ = 0;
    mqtt_username_.clear();
    stop_mqtt_client();
    BambuCloudSnapshot expired = snapshot();
    expired.connected = false;
    expired.detail = "Bambu Cloud token expired";
    set_snapshot(std::move(expired));
    return false;
  }

  if (status_code < 200 || status_code >= 300) {
    ESP_LOGW(kTag, "Bambu Cloud tasks request rejected: HTTP %d", status_code);
    return false;
  }

  cJSON* root = cJSON_Parse(response_body.c_str());
  if (root == nullptr) {
    ESP_LOGW(kTag, "Bambu Cloud tasks returned invalid JSON");
    return false;
  }

  const cJSON* data = child_object(root, "data");
  const cJSON* hits = child_array(root, "hits");
  if (hits == nullptr && data != nullptr) {
    hits = child_array(data, "hits");
  }

  std::string selected_cover;
  std::string selected_title;
  std::string selected_status;
  std::string selected_stage;
  std::string selected_print_type;
  float selected_progress = 0.0f;
  float selected_nozzle_temp_c = 0.0f;
  float selected_bed_temp_c = 0.0f;
  float selected_chamber_temp_c = 0.0f;
  float selected_secondary_nozzle_temp_c = 0.0f;
  uint32_t selected_remaining_seconds = 0U;
  uint16_t selected_current_layer = 0U;
  uint16_t selected_total_layers = 0U;
  int selected_print_error_code = 0;
  int selected_hms_count = 0;
  PrintLifecycleState selected_lifecycle = PrintLifecycleState::kUnknown;
  bool selected_has_error = false;
  bool selected_stale_failed_state = false;
  const std::string expected_serial = !resolved_serial_.empty() ? resolved_serial_ : requested_serial_;
  const cJSON* selected_item = nullptr;
  int best_priority = -1;
  int64_t best_timestamp = -1;

  if (cJSON_IsArray(hits)) {
    const int count = cJSON_GetArraySize(hits);
    for (int i = 0; i < count; ++i) {
      const cJSON* item = cJSON_GetArrayItem(hits, i);
      const std::string candidate_serial = extract_device_serial(item);
      const bool serial_match = expected_serial.empty() || candidate_serial.empty() ||
                                candidate_serial == expected_serial;
      if (!serial_match) {
        continue;
      }

      const std::string candidate_status = extract_status_text(item);
      const std::string candidate_stage = extract_stage_text(item);
      const std::string candidate_print_type = extract_print_type_text(item);
      const int candidate_hms_count = extract_cloud_hms_count(item, 0);
      const bool candidate_stale_failed_state = cloud_rest_failure_looks_stale(
          candidate_status, candidate_stage, candidate_print_type, candidate_hms_count);
      const PrintLifecycleState candidate_lifecycle =
          candidate_stale_failed_state ? PrintLifecycleState::kIdle
                                       : cloud_lifecycle_from_status(candidate_status);
      const int candidate_priority = lifecycle_priority(candidate_lifecycle);
      const std::string candidate_cover = extract_cover_url(item);
      const std::string candidate_title = extract_title(item);
      const float candidate_progress = extract_progress(item);
      const uint32_t candidate_remaining = extract_remaining_seconds(item);
      const uint16_t candidate_current_layer = extract_current_layer(item);
      const uint16_t candidate_total_layers = extract_total_layers(item);
      const bool has_metrics_signal =
          candidate_progress > 0.0f || candidate_remaining > 0U ||
          candidate_current_layer > 0U || candidate_total_layers > 0U;
      const int has_cover_bonus = candidate_cover.empty() ? 0 : 50;
      const int has_title_bonus = candidate_title.empty() ? 0 : 10;
      const int has_metrics_bonus = has_metrics_signal ? 80 : 0;

      int64_t candidate_timestamp = -1;
      const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                       ? child_object(item, "print_history_info")
                                       : child_object(item, "printHistoryInfo");
      const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;
      const char* ts_keys[] = {"update_time", "updateTime", "start_time", "startTime",
                               "create_time", "createTime", "end_time", "endTime",
                               "ctime", "mtime", "timestamp", "time"};
      for (const cJSON* source : {item, print_history, subtask}) {
        if (source == nullptr) {
          continue;
        }
        for (const char* key : ts_keys) {
          const int value = json_int(source, key, -1);
          if (value > 0) {
            candidate_timestamp = std::max<int64_t>(candidate_timestamp, value);
          }
        }
      }

      const int candidate_score =
          candidate_priority + has_metrics_bonus + has_cover_bonus + has_title_bonus;
      if (selected_item == nullptr || candidate_score > best_priority ||
          (candidate_score == best_priority && candidate_timestamp > best_timestamp)) {
        selected_item = item;
        best_priority = candidate_score;
        best_timestamp = candidate_timestamp;
      }
    }

    if (selected_item == nullptr && count > 0) {
      selected_item = cJSON_GetArrayItem(hits, 0);
    }
  }

  if (selected_item != nullptr) {
    selected_cover = extract_cover_url(selected_item);
    selected_title = extract_title(selected_item);
    selected_status = extract_status_text(selected_item);
    selected_stage = extract_stage_text(selected_item);
    selected_print_type = extract_print_type_text(selected_item);
    selected_progress = extract_progress(selected_item);
    const NozzleTemperatureBundle nozzle_temps =
        extract_cloud_nozzle_temperature_bundle(selected_item, 0.0f, 0.0f);
    selected_nozzle_temp_c = nozzle_temps.active;
    selected_secondary_nozzle_temp_c = nozzle_temps.secondary;
    selected_bed_temp_c = extract_cloud_bed_temperature_c(selected_item, 0.0f);
    selected_chamber_temp_c = extract_cloud_chamber_temperature_c(selected_item, 0.0f);
    selected_remaining_seconds = extract_remaining_seconds(selected_item);
    selected_current_layer = extract_current_layer(selected_item);
    selected_total_layers = extract_total_layers(selected_item);
    selected_print_error_code =
        normalize_cloud_print_error_code(extract_cloud_print_error_code(selected_item, 0));
    selected_hms_count = extract_cloud_hms_count(selected_item, 0);
    selected_stale_failed_state = cloud_rest_failure_looks_stale(
        selected_status, selected_stage, selected_print_type, selected_hms_count);
    if (selected_stale_failed_state) {
      selected_status = "IDLE";
      if (selected_stage.empty()) {
        selected_stage = "Idle";
      }
      selected_progress = 0.0f;
      selected_remaining_seconds = 0U;
      selected_current_layer = 0U;
      selected_total_layers = 0U;
      selected_print_error_code = 0;
    }
    selected_lifecycle = cloud_lifecycle_from_status(selected_status);
    const bool selected_non_error_stop =
        cloud_status_is_non_error_stop(selected_status, selected_print_error_code, selected_hms_count);
    selected_has_error =
        ((!selected_non_error_stop && selected_lifecycle == PrintLifecycleState::kError) ||
         selected_print_error_code != 0 || selected_hms_count > 0);
    if (selected_lifecycle == PrintLifecycleState::kFinished && selected_progress < 100.0f) {
      selected_progress = 100.0f;
    }
  }

  BambuCloudSnapshot current = snapshot();
  const bool preserve_live_state = is_recent_live_data(current.live_data_last_update_ms);
  const bool preserve_optimistic_light =
      current.chamber_light_pending &&
      is_recent_optimistic_light_state(current.chamber_light_pending_since_ms);
  const bool pending_light_on = current.chamber_light_on;
  if (current.chamber_light_pending && !preserve_optimistic_light) {
    current.chamber_light_pending = false;
    current.chamber_light_pending_since_ms = 0;
  }
  current.capabilities = default_cloud_capabilities();
  current.last_update_ms = now_ms();
  const bool selected_has_state = !selected_status.empty() ||
                                  selected_lifecycle != PrintLifecycleState::kUnknown ||
                                  selected_progress > 0.0f || selected_remaining_seconds > 0U ||
                                  selected_current_layer > 0U || selected_total_layers > 0U ||
                                  selected_nozzle_temp_c > 0.0f ||
                                  selected_secondary_nozzle_temp_c > 0.0f ||
                                  selected_bed_temp_c > 0.0f || selected_chamber_temp_c > 0.0f ||
                                  selected_has_error;
  const std::string preview_detail = selected_cover.empty() ? "Connected to Bambu Cloud, no cover image yet"
                                                            : "Connected to Bambu Cloud";
  current.configured = true;
  current.connected = true;
  current.model = detect_cloud_model(selected_item, current.model);
  current.chamber_light_supported =
      current.chamber_light_supported || printer_model_has_chamber_light(current.model);
  if (selected_item != nullptr) {
    const bool saw_light_report =
        apply_chamber_light_report(selected_item, &current.chamber_light_supported,
                                   &current.chamber_light_state_known, &current.chamber_light_on);
    if (preserve_optimistic_light) {
      if (saw_light_report && current.chamber_light_state_known &&
          current.chamber_light_on == pending_light_on) {
        current.chamber_light_pending = false;
        current.chamber_light_pending_since_ms = 0;
      } else {
        current.chamber_light_supported = true;
        current.chamber_light_state_known = true;
        current.chamber_light_on = pending_light_on;
      }
    } else if (saw_light_report) {
      current.chamber_light_pending = false;
      current.chamber_light_pending_since_ms = 0;
    }
  }
  if (current.detail.empty() || current.detail == "Connected to Bambu Cloud" ||
      current.detail == "Connected to Bambu Cloud, no cover image yet") {
    current.detail = preview_detail;
  }
  if (!selected_cover.empty() && selected_cover != current.preview_url) {
    ESP_LOGI(kTag, "Cloud preview URL ready: %s", selected_cover.c_str());
  }
  if (!selected_title.empty() && selected_title != current.preview_title) {
    ESP_LOGI(kTag, "Cloud preview title: %s", selected_title.c_str());
  }
  if (!selected_status.empty() && selected_status != current.stage) {
    ESP_LOGI(kTag, "Cloud print status: %s", selected_status.c_str());
  }
  const std::string selected_preview_key = preview_cache_key(selected_cover);
  if (allow_preview_download && !selected_cover.empty() &&
      (selected_preview_key != cached_preview_url_ || cached_preview_blob_ == nullptr)) {
    cached_preview_blob_ = download_preview_image(selected_cover);
    if (cached_preview_blob_ != nullptr) {
      cached_preview_url_ = selected_preview_key;
      ESP_LOGI(kTag, "Preview image cached in memory: %u bytes",
               static_cast<unsigned int>(cached_preview_blob_->size()));
    } else {
      ESP_LOGW(kTag, "Cloud preview download failed");
    }
  }
  current.preview_url = selected_cover;
  current.preview_blob = (!selected_cover.empty() && cached_preview_url_ == selected_preview_key)
                             ? cached_preview_blob_
                             : nullptr;
  current.preview_title = selected_title;
  if (selected_has_state) {
    if (!preserve_live_state) {
      if (!selected_status.empty() &&
          (selected_lifecycle == PrintLifecycleState::kPreparing ||
           selected_lifecycle == PrintLifecycleState::kPrinting ||
           selected_lifecycle == PrintLifecycleState::kPaused ||
           selected_lifecycle == PrintLifecycleState::kError ||
           current.lifecycle == PrintLifecycleState::kUnknown || current.raw_status.empty())) {
        current.raw_status = selected_status;
        current.raw_stage = selected_stage;
        current.lifecycle = selected_lifecycle;
        current.stage = !selected_stage.empty() ? selected_stage
                                                : cloud_stage_label_for(selected_status, selected_lifecycle);
        current.has_error = selected_has_error;
      }
      if (selected_progress > 0.0f || selected_lifecycle == PrintLifecycleState::kFinished ||
          selected_lifecycle == PrintLifecycleState::kIdle ||
          selected_lifecycle == PrintLifecycleState::kError) {
        current.progress_percent = selected_progress;
      }
      if (selected_remaining_seconds > 0U ||
          selected_lifecycle == PrintLifecycleState::kFinished ||
          selected_lifecycle == PrintLifecycleState::kIdle ||
          selected_lifecycle == PrintLifecycleState::kError) {
        current.remaining_seconds = selected_remaining_seconds;
      }
      if (selected_current_layer > 0U || selected_lifecycle == PrintLifecycleState::kFinished ||
          selected_lifecycle == PrintLifecycleState::kIdle ||
          selected_lifecycle == PrintLifecycleState::kError) {
        current.current_layer = selected_current_layer;
      }
      if (selected_total_layers > 0U || selected_lifecycle == PrintLifecycleState::kFinished ||
          selected_lifecycle == PrintLifecycleState::kIdle ||
          selected_lifecycle == PrintLifecycleState::kError) {
        current.total_layers = selected_total_layers;
      }
      if (selected_nozzle_temp_c > 0.0f) {
        current.nozzle_temp_c = selected_nozzle_temp_c;
      }
      if (selected_secondary_nozzle_temp_c > 0.0f) {
        current.secondary_nozzle_temp_c = selected_secondary_nozzle_temp_c;
      }
      if (selected_bed_temp_c > 0.0f) {
        current.bed_temp_c = selected_bed_temp_c;
      }
      if (selected_chamber_temp_c > 0.0f) {
        current.chamber_temp_c = selected_chamber_temp_c;
      }
      current.print_error_code = selected_print_error_code;
      current.hms_alert_count = static_cast<uint16_t>(std::max(selected_hms_count, 0));
      current.non_error_stop =
          cloud_status_is_non_error_stop(selected_status, selected_print_error_code, selected_hms_count);
      current.has_error = current.has_error || selected_has_error;
      const std::string error_detail =
          format_error_detail(selected_print_error_code, selected_hms_count);
      if (!error_detail.empty()) {
        current.detail = error_detail;
      }
    } else {
      if (current.progress_percent <= 0.0f && selected_progress > 0.0f) {
        current.progress_percent = selected_progress;
      }
      if (current.remaining_seconds == 0U && selected_remaining_seconds > 0U) {
        current.remaining_seconds = selected_remaining_seconds;
      }
      if (current.current_layer == 0U && selected_current_layer > 0U) {
        current.current_layer = selected_current_layer;
      }
      if (current.total_layers == 0U && selected_total_layers > 0U) {
        current.total_layers = selected_total_layers;
      }
      if (current.nozzle_temp_c <= 0.0f && selected_nozzle_temp_c > 0.0f) {
        current.nozzle_temp_c = selected_nozzle_temp_c;
      }
      if (current.secondary_nozzle_temp_c <= 0.0f && selected_secondary_nozzle_temp_c > 0.0f) {
        current.secondary_nozzle_temp_c = selected_secondary_nozzle_temp_c;
      }
      if (current.bed_temp_c <= 0.0f && selected_bed_temp_c > 0.0f) {
        current.bed_temp_c = selected_bed_temp_c;
      }
      if (current.chamber_temp_c <= 0.0f && selected_chamber_temp_c > 0.0f) {
        current.chamber_temp_c = selected_chamber_temp_c;
      }
    }
  } else if (!selected_title.empty() || !selected_cover.empty()) {
    current.has_error = current.non_error_stop ? false
                                               : current.lifecycle == PrintLifecycleState::kError;
    if (current.detail.empty()) {
      current.detail = preview_detail;
    }
  }
  if (!resolved_serial_.empty()) {
    current.resolved_serial = resolved_serial_;
  }
  set_snapshot(std::move(current));

  cJSON_Delete(root);
  return true;
}

std::shared_ptr<std::vector<uint8_t>> BambuCloudClient::download_preview_image(const std::string& url) {
  if (url.empty()) {
    return nullptr;
  }

  auto blob = std::make_shared<std::vector<uint8_t>>();
  blob->reserve(16 * 1024);

  PreviewDownloadContext download_context = {
      .buffer = blob.get(),
      .max_bytes = kMaxPreviewBytes,
      .overflow = false,
  };

  const bool prefer_ranges = prefers_ranged_preview_download(url);
  esp_err_t perform_err = ESP_FAIL;
  int status_code = 0;
  int64_t content_length = 0;
  bool complete = false;

  if (!prefer_ranges) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 20000;
    config.keep_alive_enable = false;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.addr_type = HTTP_ADDR_TYPE_INET;
    config.tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
    config.tls_dyn_buf_strategy = HTTP_TLS_DYN_BUF_RX_STATIC;
#endif
    config.event_handler = &preview_http_event_handler;
    config.user_data = &download_context;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
      return nullptr;
    }
    esp_http_client_set_header(client, "Accept", "image/png,image/*;q=0.9,*/*;q=0.1");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "User-Agent", "PrintSphere/1.0");

    perform_err = esp_http_client_perform(client);
    status_code = esp_http_client_get_status_code(client);
    content_length = esp_http_client_get_content_length(client);
    complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);

    if (status_code < 200 || status_code >= 300) {
      ESP_LOGW(kTag, "Preview HTTP rejected with status %d", status_code);
      return nullptr;
    }

    if (content_length > 0 && content_length > static_cast<int64_t>(kMaxPreviewBytes)) {
      ESP_LOGW(kTag, "Preview image too large: %lld bytes", content_length);
      return nullptr;
    }

    if (download_context.overflow) {
      ESP_LOGW(kTag, "Preview image exceeded cache limit");
      return nullptr;
    }

    if (perform_err != ESP_OK && !(complete && !blob->empty())) {
      ESP_LOGW(kTag, "Preview HTTP perform failed: %s (complete=%s, bytes=%u)",
               esp_err_to_name(perform_err), complete ? "true" : "false",
               static_cast<unsigned int>(blob->size()));
    } else if (!blob->empty()) {
      return blob;
    }
  }

  auto ranged_blob = std::make_shared<std::vector<uint8_t>>();
  if (content_length > 0 && content_length <= static_cast<int64_t>(kMaxPreviewBytes)) {
    ranged_blob->reserve(static_cast<size_t>(content_length));
  } else {
    ranged_blob->reserve(16 * 1024);
  }

  PreviewDownloadContext range_context = {
      .buffer = nullptr,
      .max_bytes = kPreviewRangeChunkBytes + 32,
      .overflow = false,
  };

  esp_http_client_config_t range_config = {};
  range_config.url = url.c_str();
  range_config.method = HTTP_METHOD_GET;
  range_config.timeout_ms = 20000;
  range_config.keep_alive_enable = true;
  range_config.buffer_size = 4096;
  range_config.buffer_size_tx = 1024;
  range_config.crt_bundle_attach = esp_crt_bundle_attach;
  range_config.addr_type = HTTP_ADDR_TYPE_INET;
  range_config.tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
  range_config.tls_dyn_buf_strategy = HTTP_TLS_DYN_BUF_RX_STATIC;
#endif
  range_config.event_handler = &preview_http_event_handler;
  range_config.user_data = &range_context;

  esp_http_client_handle_t range_client = esp_http_client_init(&range_config);
  bool range_failed = false;
  if (range_client == nullptr) {
    ESP_LOGW(kTag, "Preview HTTP range client init failed");
    range_failed = true;
  }

  if (range_client != nullptr) {
    esp_http_client_set_header(range_client, "Accept", "image/png,image/*;q=0.9,*/*;q=0.1");
    esp_http_client_set_header(range_client, "Accept-Encoding", "identity");
    esp_http_client_set_header(range_client, "User-Agent", "PrintSphere/1.0");
  }

  for (size_t offset = 0; !range_failed && range_client != nullptr && offset < kMaxPreviewBytes;
       offset += kPreviewRangeChunkBytes) {
    std::vector<uint8_t> chunk;
    chunk.reserve(kPreviewRangeChunkBytes + 32);

    range_context.buffer = &chunk;
    range_context.max_bytes = kPreviewRangeChunkBytes + 32;
    range_context.overflow = false;

    const size_t range_end = offset + kPreviewRangeChunkBytes - 1U;
    char range_header[64] = {};
    std::snprintf(range_header, sizeof(range_header), "bytes=%u-%u",
                  static_cast<unsigned int>(offset), static_cast<unsigned int>(range_end));

    esp_http_client_delete_header(range_client, "Range");
    esp_http_client_set_header(range_client, "Range", range_header);

    const esp_err_t range_err = esp_http_client_perform(range_client);
    const int range_status = esp_http_client_get_status_code(range_client);
    const bool range_complete = esp_http_client_is_complete_data_received(range_client);

    if (range_status == 416 && !ranged_blob->empty()) {
      break;
    }

    if ((range_status != 206 && range_status != 200) ||
        (range_err != ESP_OK && !(range_complete && !chunk.empty())) || range_context.overflow ||
        chunk.empty()) {
      ESP_LOGW(kTag,
               "Preview HTTP range failed at offset %u: status=%d err=%s complete=%s "
               "bytes=%u",
               static_cast<unsigned int>(offset), range_status, esp_err_to_name(range_err),
               range_complete ? "true" : "false", static_cast<unsigned int>(chunk.size()));
      range_failed = true;
      break;
    }

    ranged_blob->insert(ranged_blob->end(), chunk.begin(), chunk.end());

    if (ranged_blob->size() > kMaxPreviewBytes) {
      range_failed = true;
      break;
    }

    if ((content_length > 0 && ranged_blob->size() >= static_cast<size_t>(content_length)) ||
        chunk.size() < kPreviewRangeChunkBytes) {
      break;
    }
  }

  if (range_client != nullptr) {
    esp_http_client_cleanup(range_client);
  }

  if (!range_failed && !ranged_blob->empty()) {
    ESP_LOGI(kTag, "Preview image fetched via HTTP ranges: %u bytes",
             static_cast<unsigned int>(ranged_blob->size()));
    return ranged_blob;
  }

  ParsedHttpsUrl parsed = {};
  if (!parse_https_url(url, &parsed)) {
    ESP_LOGW(kTag, "Preview fallback could not parse HTTPS URL");
    return nullptr;
  }

  esp_tls_cfg_t tls_cfg = {};
  tls_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  tls_cfg.timeout_ms = 20000;
  tls_cfg.addr_family = ESP_TLS_AF_INET;
  tls_cfg.tls_version = ESP_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
  tls_cfg.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif
  tls_cfg.common_name = parsed.host.c_str();

  esp_tls_t* tls = esp_tls_init();
  if (tls == nullptr) {
    ESP_LOGW(kTag, "Preview TLS fallback init failed");
    return nullptr;
  }

  if (esp_tls_conn_http_new_sync(url.c_str(), &tls_cfg, tls) != 1) {
    ESP_LOGW(kTag, "Preview TLS fallback connect failed");
    esp_tls_conn_destroy(tls);
    return nullptr;
  }

  const std::string request =
      "GET " + parsed.target + " HTTP/1.1\r\nHost: " + parsed.host +
      "\r\nUser-Agent: PrintSphere/1.0\r\nAccept: image/png,image/*;q=0.9,*/*;q=0.1\r\n"
      "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";

  size_t written_total = 0;
  while (written_total < request.size()) {
    const ssize_t written =
        esp_tls_conn_write(tls, request.data() + written_total, request.size() - written_total);
    if (written <= 0) {
      ESP_LOGW(kTag, "Preview TLS fallback write failed");
      esp_tls_conn_destroy(tls);
      return nullptr;
    }
    written_total += static_cast<size_t>(written);
  }

  std::vector<uint8_t> response;
  response.reserve(16 * 1024);
  char read_buffer[1024];
  while (response.size() < (kMaxPreviewBytes + 8192)) {
    const ssize_t read = esp_tls_conn_read(tls, read_buffer, sizeof(read_buffer));
    if (read > 0) {
      response.insert(response.end(), read_buffer, read_buffer + read);
      continue;
    }
    if (read == 0) {
      break;
    }

    if (!response.empty()) {
      break;
    }

    ESP_LOGW(kTag, "Preview TLS fallback read failed before data");
    esp_tls_conn_destroy(tls);
    return nullptr;
  }
  esp_tls_conn_destroy(tls);

  if (response.empty()) {
    ESP_LOGW(kTag, "Preview TLS fallback returned no bytes");
    return nullptr;
  }

  const auto header_end_it =
      std::search(response.begin(), response.end(), "\r\n\r\n", "\r\n\r\n" + 4);
  if (header_end_it == response.end()) {
    ESP_LOGW(kTag, "Preview TLS fallback missing HTTP headers");
    return nullptr;
  }

  const size_t header_size =
      static_cast<size_t>(std::distance(response.begin(), header_end_it)) + 4U;
  const std::string headers(response.begin(), response.begin() + header_size);
  const size_t status_line_end = headers.find("\r\n");
  const int fallback_status_code =
      parse_status_code(headers.substr(0, status_line_end == std::string::npos ? headers.size() : status_line_end));
  if (fallback_status_code < 200 || fallback_status_code >= 300) {
    ESP_LOGW(kTag, "Preview TLS fallback rejected with status %d", fallback_status_code);
    return nullptr;
  }

  std::string content_length_text = header_value_ci(headers, "content-length");
  size_t expected_length = 0;
  if (!content_length_text.empty()) {
    expected_length = static_cast<size_t>(std::strtoul(content_length_text.c_str(), nullptr, 10));
    if (expected_length > kMaxPreviewBytes) {
      ESP_LOGW(kTag, "Preview TLS fallback image too large: %u bytes",
               static_cast<unsigned int>(expected_length));
      return nullptr;
    }
  }

  auto fallback_blob = std::make_shared<std::vector<uint8_t>>(
      response.begin() + static_cast<ptrdiff_t>(header_size), response.end());
  if (fallback_blob->empty()) {
    ESP_LOGW(kTag, "Preview TLS fallback body empty");
    return nullptr;
  }

  if (expected_length > 0 && fallback_blob->size() < expected_length) {
    ESP_LOGW(kTag, "Preview TLS fallback incomplete body: %u/%u bytes",
             static_cast<unsigned int>(fallback_blob->size()),
             static_cast<unsigned int>(expected_length));
    return nullptr;
  }

  ESP_LOGI(kTag, "Preview image fetched via raw TLS fallback: %u bytes",
           static_cast<unsigned int>(fallback_blob->size()));
  return fallback_blob;
}

bool BambuCloudClient::perform_json_request(const std::string& url, const char* method,
                                            const std::string& request_body,
                                            const std::string& bearer_token, int* status_code,
                                            std::string* response_body) {
  if (status_code == nullptr || response_body == nullptr) {
    return false;
  }

  response_body->clear();
  *status_code = 0;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 15000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.method = (std::strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  config.keep_alive_enable = false;
  config.buffer_size = 1024;
  config.buffer_size_tx = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    return false;
  }

  esp_http_client_set_header(client, "User-Agent", "bambu_network_agent/01.09.05.01");
  esp_http_client_set_header(client, "X-BBL-Client-Name", "OrcaSlicer");
  esp_http_client_set_header(client, "X-BBL-Client-Type", "slicer");
  esp_http_client_set_header(client, "X-BBL-Client-Version", "01.09.05.51");
  esp_http_client_set_header(client, "X-BBL-Language", "en-US");
  esp_http_client_set_header(client, "X-BBL-OS-Type", "linux");
  esp_http_client_set_header(client, "X-BBL-OS-Version", "6.2.0");
  esp_http_client_set_header(client, "X-BBL-Agent-Version", "01.09.05.01");
  esp_http_client_set_header(client, "X-BBL-Executable-info", "{}");
  esp_http_client_set_header(client, "X-BBL-Agent-OS-Type", "linux");
  esp_http_client_set_header(client, "Accept", "application/json");
  if (!request_body.empty()) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
  }
  if (!bearer_token.empty()) {
    const std::string authorization = "Bearer " + bearer_token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
  }

  const esp_err_t open_err = esp_http_client_open(client, static_cast<int>(request_body.size()));
  if (open_err != ESP_OK) {
    ESP_LOGW(kTag, "HTTP open failed for %s: %s", url.c_str(), esp_err_to_name(open_err));
    esp_http_client_cleanup(client);
    return false;
  }

  if (!request_body.empty()) {
    const int written =
        esp_http_client_write(client, request_body.c_str(), static_cast<int>(request_body.size()));
    if (written < 0) {
      ESP_LOGW(kTag, "HTTP write failed for %s", url.c_str());
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
  }

  const int fetch_result = esp_http_client_fetch_headers(client);
  if (fetch_result < 0) {
    ESP_LOGW(kTag, "HTTP fetch headers failed for %s", url.c_str());
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  *status_code = esp_http_client_get_status_code(client);
  char buffer[512];
  while (true) {
    const int read = esp_http_client_read(client, buffer, sizeof(buffer));
    if (read < 0) {
      ESP_LOGW(kTag, "HTTP read failed for %s", url.c_str());
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    if (read == 0) {
      break;
    }
    response_body->append(buffer, read);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return true;
}

void BambuCloudClient::set_snapshot(BambuCloudSnapshot snapshot) {
  if (!snapshot.capabilities.status && !snapshot.capabilities.metrics &&
      !snapshot.capabilities.temperatures && !snapshot.capabilities.preview &&
      !snapshot.capabilities.hms && !snapshot.capabilities.print_error) {
    snapshot.capabilities = default_cloud_capabilities();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(snapshot);
}

BambuCloudClient::AuthMode BambuCloudClient::auth_mode() const {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  return auth_mode_;
}

bool BambuCloudClient::waiting_for_user_code() const {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  return (auth_mode_ == AuthMode::kEmailCode || auth_mode_ == AuthMode::kTfaCode) &&
         pending_verification_code_.empty();
}

std::string BambuCloudClient::pending_verification_code() const {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  return pending_verification_code_;
}

void BambuCloudClient::set_auth_mode(AuthMode mode, std::string tfa_key) {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  auth_mode_ = mode;
  tfa_key_ = std::move(tfa_key);
  if (mode == AuthMode::kPassword) {
    pending_verification_code_.clear();
  }
}

void BambuCloudClient::clear_auth_state() {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  auth_mode_ = AuthMode::kPassword;
  tfa_key_.clear();
  pending_verification_code_.clear();
}

void BambuCloudClient::clear_pending_code() {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  pending_verification_code_.clear();
}

void BambuCloudClient::persist_access_token() const {
  if (config_store_ == nullptr || access_token_.empty()) {
    return;
  }

  const esp_err_t err = config_store_->save_cloud_access_token(access_token_);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "Failed to persist cloud token: %s", esp_err_to_name(err));
  }
}

void BambuCloudClient::clear_persisted_access_token() {
  if (config_store_ == nullptr) {
    return;
  }

  const esp_err_t err = config_store_->clear_cloud_access_token();
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "Failed to clear cloud token: %s", esp_err_to_name(err));
  }
}

std::string BambuCloudClient::json_string(const cJSON* object, const char* key,
                                          const std::string& fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return item->valuestring;
  }
  return fallback;
}

int BambuCloudClient::json_int(const cJSON* object, const char* key, int fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    return item->valueint;
  }
  return fallback;
}

float BambuCloudClient::json_number(const cJSON* object, const char* key, float fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    return static_cast<float>(item->valuedouble);
  }
  return fallback;
}

bool BambuCloudClient::json_bool(const cJSON* object, const char* key, bool fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsBool(item)) {
    return cJSON_IsTrue(item);
  }
  if (cJSON_IsNumber(item)) {
    return item->valueint != 0;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    const std::string_view value(item->valuestring);
    if (value == "true" || value == "TRUE" || value == "1" || value == "online") {
      return true;
    }
    if (value == "false" || value == "FALSE" || value == "0" || value == "offline") {
      return false;
    }
  }

  return fallback;
}

std::string BambuCloudClient::extract_cover_url(const cJSON* item) {
  if (item == nullptr) {
    return {};
  }

  std::string cover = json_string(item, "cover",
                                  json_string(item, "coverUrl", json_string(item, "cover_url", {})));
  if (!cover.empty()) {
    return cover;
  }

  const cJSON* print_history = child_object(item, "print_history_info");
  if (print_history == nullptr) {
    print_history = child_object(item, "printHistoryInfo");
  }
  if (print_history != nullptr) {
    cover = json_string(print_history, "cover",
                        json_string(print_history, "coverUrl",
                                    json_string(print_history, "cover_image_url", {})));
    if (!cover.empty()) {
      return cover;
    }

    const cJSON* subtask = child_object(print_history, "subtask");
    const cJSON* thumbnail = child_object(subtask, "thumbnail");
    cover = json_string(thumbnail, "url", {});
    if (!cover.empty()) {
      return cover;
    }
  }

  return {};
}

std::string BambuCloudClient::extract_title(const cJSON* item) {
  if (item == nullptr) {
    return {};
  }

  std::string title =
      json_string(item, "title", json_string(item, "designTitle", json_string(item, "name", {})));
  if (!title.empty()) {
    return title;
  }

  const cJSON* print_history = child_object(item, "print_history_info");
  if (print_history == nullptr) {
    print_history = child_object(item, "printHistoryInfo");
  }
  if (print_history != nullptr) {
    const cJSON* subtask = child_object(print_history, "subtask");
    title = json_string(subtask, "name", json_string(subtask, "title", {}));
  }

  return title;
}

std::string BambuCloudClient::extract_device_serial(const cJSON* item) {
  if (item == nullptr) {
    return {};
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }
    const std::string serial =
        json_string(source, "dev_id",
                    json_string(source, "deviceId",
                                json_string(source, "device_id",
                                            json_string(source, "devId",
                                                        json_string(source, "sn",
                                                                    json_string(source, "printerSn", {}))))));
    if (!serial.empty()) {
      return serial;
    }
  }

  return {};
}

std::string BambuCloudClient::extract_status_text(const cJSON* item) {
  if (item == nullptr) {
    return {};
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;

  const char* keys[] = {"status", "task_status", "taskStatus", "print_status",
                        "printStatus", "state", "gcode_state"};

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }

    for (const char* key : keys) {
      const std::string value = json_string(source, key, {});
      if (!value.empty()) {
        return value;
      }
    }

  }

  return {};
}

std::string BambuCloudClient::extract_stage_text(const cJSON* item) {
  if (item == nullptr) {
    return {};
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;

  const char* keys[] = {"current_stage", "currentStage", "stage_name", "stageName", "stage"};

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }

    for (const char* key : keys) {
      const std::string value = json_string(source, key, {});
      if (!value.empty()) {
        return value;
      }
    }

    const cJSON* stage = child_object(source, "stage");
    const std::string stage_name =
        json_string(stage, "name", json_string(stage, "stage", {}));
    if (!stage_name.empty()) {
      return stage_name;
    }
  }

  return {};
}

std::string BambuCloudClient::extract_print_type_text(const cJSON* item) {
  if (item == nullptr) {
    return {};
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;

  const char* keys[] = {"print_type", "printType", "task_type", "taskType",
                        "subtask_type", "subtaskType"};

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }

    for (const char* key : keys) {
      const std::string value = json_string(source, key, {});
      if (!value.empty()) {
        return value;
      }
    }
  }

  return {};
}

float BambuCloudClient::extract_progress(const cJSON* item) {
  if (item == nullptr) {
    return 0.0f;
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;
  const char* keys[] = {"progress",         "percent",            "mc_percent",
                        "task_progress",    "print_progress",     "printPercent",
                        "download_progress","downloadProgress",   "model_download_progress",
                        "modelDownloadProgress"};

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }

    for (const char* key : keys) {
      const float value = json_number(source, key, -1.0f);
      if (value < 0.0f) {
        continue;
      }
      if (value <= 1.0f) {
        return value * 100.0f;
      }
      return value;
    }
  }

  float value = 0.0f;
  if (find_number_for_keys_recursive(item,
                                     {"progress", "percent", "mc_percent",
                                      "task_progress", "taskProgress",
                                      "print_progress", "printProgress"},
                                     &value)) {
    return value <= 1.0f ? value * 100.0f : value;
  }

  return 0.0f;
}

uint32_t BambuCloudClient::extract_remaining_seconds(const cJSON* item) {
  if (item == nullptr) {
    return 0U;
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }

    const int minutes = json_int(source, "mc_remaining_time",
                                 json_int(source, "remaining_minutes",
                                          json_int(source, "remainingMinutes", -1)));
    if (minutes >= 0) {
      return static_cast<uint32_t>(minutes) * 60U;
    }

    const int seconds =
        json_int(source, "remaining_seconds",
                 json_int(source, "remainingSeconds",
                          json_int(source, "remaining_time",
                                   json_int(source, "remainingTime", -1))));
    if (seconds >= 0) {
      return static_cast<uint32_t>(seconds);
    }
  }

  int minutes = -1;
  if (find_int_for_keys_recursive(item,
                                  {"mc_remaining_time", "remaining_minutes",
                                   "remainingMinutes", "remain_time"},
                                  &minutes) &&
      minutes >= 0) {
    return static_cast<uint32_t>(minutes) * 60U;
  }

  int seconds = -1;
  if (find_int_for_keys_recursive(item,
                                  {"remaining_seconds", "remainingSeconds",
                                   "remaining_time", "remainingTime",
                                   "mc_left_time"},
                                  &seconds) &&
      seconds >= 0) {
    return static_cast<uint32_t>(seconds);
  }

  return 0U;
}

uint16_t BambuCloudClient::extract_current_layer(const cJSON* item) {
  if (item == nullptr) {
    return 0U;
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }

    const int layer = json_int(source, "layer_num",
                               json_int(source, "current_layer",
                                        json_int(source, "currentLayer", json_int(source, "layer", -1))));
    if (layer >= 0) {
      return static_cast<uint16_t>(layer);
    }
  }

  int layer = -1;
  if (find_int_for_keys_recursive(item,
                                  {"layer_num", "current_layer", "currentLayer", "layer"},
                                  &layer) &&
      layer >= 0) {
    return static_cast<uint16_t>(layer);
  }

  return 0U;
}

uint16_t BambuCloudClient::extract_total_layers(const cJSON* item) {
  if (item == nullptr) {
    return 0U;
  }

  const cJSON* print_history = child_object(item, "print_history_info") != nullptr
                                   ? child_object(item, "print_history_info")
                                   : child_object(item, "printHistoryInfo");
  const cJSON* subtask = print_history != nullptr ? child_object(print_history, "subtask") : nullptr;

  for (const cJSON* source : {item, print_history, subtask}) {
    if (source == nullptr) {
      continue;
    }

    const int total = json_int(source, "total_layer_num",
                               json_int(source, "total_layers",
                                        json_int(source, "totalLayers",
                                                 json_int(source, "layer_count", -1))));
    if (total >= 0) {
      return static_cast<uint16_t>(total);
    }
  }

  int total = -1;
  if (find_int_for_keys_recursive(item,
                                  {"total_layer_num", "total_layers", "totalLayers",
                                   "layer_count", "layerCount"},
                                  &total) &&
      total >= 0) {
    return static_cast<uint16_t>(total);
  }

  return 0U;
}

PrintLifecycleState BambuCloudClient::cloud_lifecycle_from_status(const std::string& status_text) {
  if (status_text.empty()) {
    return PrintLifecycleState::kUnknown;
  }

  std::string normalized;
  normalized.reserve(status_text.size());
  for (const char ch : status_text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
      normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
  }

  if (normalized.find("FAIL") != std::string::npos || normalized.find("ERROR") != std::string::npos ||
      normalized.find("CANCEL") != std::string::npos) {
    return PrintLifecycleState::kError;
  }
  if (normalized.find("PAUSE") != std::string::npos) {
    return PrintLifecycleState::kPaused;
  }
  if (normalized.find("PREPARE") != std::string::npos ||
      normalized.find("PREPARING") != std::string::npos ||
      normalized.find("STARTING") != std::string::npos ||
      normalized.find("HEATING") != std::string::npos ||
      normalized.find("DOWNLOAD") != std::string::npos) {
    return PrintLifecycleState::kPreparing;
  }
  if (normalized.find("RUNNING") != std::string::npos ||
      normalized.find("PRINTING") != std::string::npos ||
      normalized.find("PROCESSING") != std::string::npos) {
    return PrintLifecycleState::kPrinting;
  }
  if (normalized.find("DONE") != std::string::npos ||
      normalized.find("SUCCESS") != std::string::npos ||
      normalized.find("COMPLETE") != std::string::npos ||
      normalized.find("COMPLETED") != std::string::npos ||
      normalized.find("FINISH") != std::string::npos) {
    return PrintLifecycleState::kFinished;
  }
  if (normalized.find("IDLE") != std::string::npos || normalized.find("WAIT") != std::string::npos) {
    return PrintLifecycleState::kIdle;
  }

  return PrintLifecycleState::kUnknown;
}

std::string BambuCloudClient::cloud_stage_label_for(const std::string& status_text,
                                                    PrintLifecycleState lifecycle) {
  std::string normalized;
  normalized.reserve(status_text.size());
  for (const char ch : status_text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
      normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
  }
  if (normalized.find("DOWNLOAD") != std::string::npos) {
    return "Model Download";
  }

  switch (lifecycle) {
    case PrintLifecycleState::kPreparing:
      return "Preparing";
    case PrintLifecycleState::kPrinting:
      return "Printing";
    case PrintLifecycleState::kPaused:
      return "Paused";
    case PrintLifecycleState::kFinished:
      return "Finished";
    case PrintLifecycleState::kIdle:
      return "Idle";
    case PrintLifecycleState::kError:
      return "Failed";
    case PrintLifecycleState::kUnknown:
    default:
      return status_text;
  }
}

const cJSON* BambuCloudClient::child_object(const cJSON* object, const char* key) {
  if (object == nullptr || key == nullptr) {
    return nullptr;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsObject(item) ? item : nullptr;
}

const cJSON* BambuCloudClient::child_array(const cJSON* object, const char* key) {
  if (object == nullptr || key == nullptr) {
    return nullptr;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsArray(item) ? item : nullptr;
}

}  // namespace printsphere
