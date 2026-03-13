#include "printsphere/bambu_cloud_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_timer.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.cloud";
constexpr char kLoginUrl[] = "https://api.bambulab.com/v1/user-service/user/login";
constexpr char kTfaLoginUrl[] = "https://bambulab.com/api/sign-in/tfa";
constexpr char kEmailCodeUrl[] = "https://api.bambulab.com/v1/user-service/user/sendemail/code";
constexpr char kBindUrl[] = "https://api.bambulab.com/v1/iot-service/api/user/bind";
constexpr char kTasksUrl[] = "https://api.bambulab.com/v1/user-service/my/tasks?limit=10";
constexpr size_t kMaxPreviewBytes = 256 * 1024;
constexpr size_t kPreviewRangeChunkBytes = 4 * 1024;
constexpr TickType_t kCloudStatusPollIdle = pdMS_TO_TICKS(30000);
constexpr TickType_t kCloudStatusPollActive = pdMS_TO_TICKS(5000);
constexpr TickType_t kCloudPreviewPollActiveView = pdMS_TO_TICKS(15000);
constexpr TickType_t kCloudStatusPollLowPower = pdMS_TO_TICKS(180000);
constexpr TickType_t kCloudPreviewPollLowPower = pdMS_TO_TICKS(600000);
constexpr TickType_t kCloudInitialPreviewDelay = pdMS_TO_TICKS(8000);

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

}  // namespace

void BambuCloudClient::configure(BambuCloudCredentials credentials, std::string printer_serial) {
  credentials_ = std::move(credentials);
  requested_serial_ = std::move(printer_serial);
  resolved_serial_.clear();
  cached_preview_url_.clear();
  cached_preview_blob_.reset();
  clear_auth_state();

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
  snapshot.resolved_serial = requested_serial_;
  set_snapshot(std::move(snapshot));
}

void BambuCloudClient::submit_verification_code(std::string code) {
  std::lock_guard<std::mutex> lock(auth_mutex_);
  pending_verification_code_ = std::move(code);
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

void BambuCloudClient::task_entry(void* context) {
  static_cast<BambuCloudClient*>(context)->task_loop();
}

void BambuCloudClient::task_loop() {
  const TickType_t task_start_tick = xTaskGetTickCount();
  TickType_t last_preview_fetch_tick = 0;
  bool last_preview_fetch_enabled = false;
  while (true) {
    if (reload_requested_.exchange(false) && config_store_ != nullptr) {
      configure(config_store_->load_cloud_credentials(), config_store_->load_printer_config().serial);
      last_preview_fetch_tick = 0;
      last_preview_fetch_enabled = false;
    }

    if (!credentials_.is_configured()) {
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
      BambuCloudSnapshot waiting = snapshot();
      waiting.configured = true;
      waiting.connected = false;
      waiting.detail = "Waiting for Wi-Fi for Bambu Cloud";
      set_snapshot(std::move(waiting));
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    const int64_t now_us = esp_timer_get_time();
    if (access_token_.empty() || now_us >= token_expiry_us_) {
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

    const TickType_t now_tick = xTaskGetTickCount();
    const bool low_power = low_power_mode_.load();
    const bool preview_fetch_enabled = preview_fetch_enabled_.load();
    const TickType_t preview_poll_interval =
        low_power ? kCloudPreviewPollLowPower : kCloudPreviewPollActiveView;
    const TickType_t status_poll_interval =
        low_power ? kCloudStatusPollLowPower : kCloudStatusPollIdle;
    const bool initial_preview_window_open =
        (now_tick - task_start_tick) >= kCloudInitialPreviewDelay;
    const bool preview_due =
        preview_fetch_enabled && initial_preview_window_open &&
        ((last_preview_fetch_tick == 0) || !last_preview_fetch_enabled ||
         ((now_tick - last_preview_fetch_tick) >= preview_poll_interval));

    const bool bindings_ok = fetch_bindings();
    bool preview_ok = true;
    if (preview_due) {
      preview_ok = fetch_latest_preview();
      last_preview_fetch_tick = now_tick;
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
    vTaskDelay(active_print ? kCloudStatusPollActive : status_poll_interval);
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
  current.configured = true;
  if (!best_serial.empty()) {
    const bool serial_changed = best_serial != current.resolved_serial;
    resolved_serial_ = best_serial;
    current.resolved_serial = best_serial;
    if (serial_changed) {
      ESP_LOGI(kTag, "Cloud device binding resolved serial=%s", best_serial.c_str());
    }
  }

  if (best_device != nullptr) {
    const bool printer_online = json_bool(best_device, "online", true);
    const std::string print_status =
        json_string(best_device, "print_status",
                    json_string(best_device, "printStatus", json_string(best_device, "status", {})));
    const std::string current_stage =
        json_string(best_device, "current_stage",
                    json_string(best_device, "currentStage", json_string(best_device, "stage", {})));
    const float progress_value =
        json_number(best_device, "mc_percent", json_number(best_device, "progress", -1.0f));
    const int remaining_minutes =
        json_int(best_device, "mc_remaining_time",
                 json_int(best_device, "remaining_time", json_int(best_device, "remainingMinutes", -1)));
    const PrintLifecycleState lifecycle = cloud_lifecycle_from_status(print_status);
    const std::string lifecycle_stage = cloud_stage_label_for(print_status, lifecycle);
    const std::string stage = !current_stage.empty() ? current_stage : lifecycle_stage;

    current.connected = true;
    current.detail = printer_online ? "Connected to Bambu Cloud" : "Printer offline in Bambu Cloud";

    if (progress_value >= 0.0f) {
      current.progress_percent = progress_value <= 1.0f ? (progress_value * 100.0f) : progress_value;
    }
    if (remaining_minutes >= 0) {
      current.remaining_seconds = static_cast<uint32_t>(remaining_minutes) * 60U;
    }

    if (!print_status.empty()) {
      if (stage != current.stage || lifecycle != current.lifecycle ||
          current_stage != current.raw_stage) {
        ESP_LOGI(kTag, "Cloud printer state: %s", print_status.c_str());
      }
      current.raw_status = print_status;
      current.raw_stage = current_stage;
      current.lifecycle = lifecycle;
      current.stage = stage;
      current.has_error = lifecycle == PrintLifecycleState::kError;
      if (lifecycle == PrintLifecycleState::kFinished && current.progress_percent < 100.0f) {
        current.progress_percent = 100.0f;
      }
      if (lifecycle == PrintLifecycleState::kIdle || lifecycle == PrintLifecycleState::kFinished ||
          lifecycle == PrintLifecycleState::kError) {
        current.remaining_seconds = 0U;
      }
    } else if (!current_stage.empty()) {
      current.raw_stage = current_stage;
      current.stage = current_stage;
    }
  }

  set_snapshot(std::move(current));

  cJSON_Delete(root);
  return true;
}

bool BambuCloudClient::fetch_latest_preview() {
  int status_code = 0;
  std::string response_body;
  if (!perform_json_request(kTasksUrl, "GET", {}, access_token_, &status_code, &response_body)) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.detail = "Bambu Cloud tasks request failed";
    set_snapshot(std::move(failed));
    return false;
  }

  if (status_code == 401 || status_code == 403) {
    clear_persisted_access_token();
    access_token_.clear();
    token_expiry_us_ = 0;
    BambuCloudSnapshot expired = snapshot();
    expired.connected = false;
    expired.detail = "Bambu Cloud token expired";
    set_snapshot(std::move(expired));
    return false;
  }

  if (status_code < 200 || status_code >= 300) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.detail = "Bambu Cloud tasks request rejected";
    set_snapshot(std::move(failed));
    return false;
  }

  cJSON* root = cJSON_Parse(response_body.c_str());
  if (root == nullptr) {
    BambuCloudSnapshot failed = snapshot();
    failed.connected = false;
    failed.detail = "Bambu Cloud tasks returned invalid JSON";
    set_snapshot(std::move(failed));
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
  float selected_progress = 0.0f;
  uint32_t selected_remaining_seconds = 0U;
  uint16_t selected_current_layer = 0U;
  uint16_t selected_total_layers = 0U;
  PrintLifecycleState selected_lifecycle = PrintLifecycleState::kUnknown;
  bool selected_has_error = false;
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
      const PrintLifecycleState candidate_lifecycle = cloud_lifecycle_from_status(candidate_status);
      const int candidate_priority = lifecycle_priority(candidate_lifecycle);
      const std::string candidate_cover = extract_cover_url(item);
      const std::string candidate_title = extract_title(item);
      const int has_cover_bonus = candidate_cover.empty() ? 0 : 50;
      const int has_title_bonus = candidate_title.empty() ? 0 : 10;

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

      const int candidate_score = candidate_priority + has_cover_bonus + has_title_bonus;
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
    selected_progress = extract_progress(selected_item);
    selected_remaining_seconds = extract_remaining_seconds(selected_item);
    selected_current_layer = extract_current_layer(selected_item);
    selected_total_layers = extract_total_layers(selected_item);
    selected_lifecycle = cloud_lifecycle_from_status(selected_status);
    selected_has_error = selected_lifecycle == PrintLifecycleState::kError;
    if (selected_lifecycle == PrintLifecycleState::kFinished && selected_progress < 100.0f) {
      selected_progress = 100.0f;
    }
  }

  BambuCloudSnapshot current = snapshot();
  const bool selected_has_state = !selected_status.empty() ||
                                  selected_lifecycle != PrintLifecycleState::kUnknown ||
                                  selected_progress > 0.0f || selected_remaining_seconds > 0U ||
                                  selected_current_layer > 0U || selected_total_layers > 0U ||
                                  selected_has_error;
  const std::string preview_detail = selected_cover.empty() ? "Connected to Bambu Cloud, no cover image yet"
                                                            : "Connected to Bambu Cloud";
  current.configured = true;
  current.connected = true;
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
  if (!selected_cover.empty() &&
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
    if (current.lifecycle == PrintLifecycleState::kUnknown && current.raw_status.empty() &&
        !selected_status.empty()) {
      current.raw_status = selected_status;
      current.raw_stage = selected_stage;
      current.lifecycle = selected_lifecycle;
      current.stage = !selected_stage.empty() ? selected_stage
                                              : cloud_stage_label_for(selected_status, selected_lifecycle);
      current.has_error = selected_has_error;
    }
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
  } else if (!selected_title.empty() || !selected_cover.empty()) {
    current.has_error = current.lifecycle == PrintLifecycleState::kError;
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

  return json_string(item, "dev_id",
                     json_string(item, "deviceId",
                                 json_string(item, "device_id",
                                             json_string(item, "devId",
                                                         json_string(item, "sn",
                                                                     json_string(item, "printerSn", {}))))));
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
