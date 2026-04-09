#include "printsphere/printer_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <utility>

#include "cJSON.h"
#include "printsphere/bambu_status.hpp"
#include "printsphere/error_lookup.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.printer";
constexpr char kGetVersion[] = "{\"info\":{\"sequence_id\":\"0\",\"command\":\"get_version\"}}";
constexpr char kPushAll[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";
constexpr char kStartPush[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"start\"}}";
constexpr uint32_t kDelayedPushallMs = 3000;
constexpr uint32_t kInitialSyncTimeoutMs = 12000;
constexpr uint32_t kNoDataProbeMs = 60000;
constexpr uint32_t kNoDataReconnectMs = 15000;
constexpr uint32_t kDisconnectedStallMs = 20000;
constexpr uint32_t kRebuildDelayMs = 1500;

extern const uint8_t bambu_root_cert_start[] asm("_binary_bambu_cert_start");
extern const uint8_t bambu_root_cert_end[] asm("_binary_bambu_cert_end");
extern const uint8_t bambu_p2s_cert_start[] asm("_binary_bambu_p2s_250626_cert_start");
extern const uint8_t bambu_p2s_cert_end[] asm("_binary_bambu_p2s_250626_cert_end");
extern const uint8_t bambu_h2c_cert_start[] asm("_binary_bambu_h2c_251122_cert_start");
extern const uint8_t bambu_h2c_cert_end[] asm("_binary_bambu_h2c_251122_cert_end");

uint64_t now_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

template <size_t N>
void copy_text(std::array<char, N>* target, const std::string& value) {
  if (target == nullptr) {
    return;
  }
  strlcpy(target->data(), value.c_str(), target->size());
}

template <size_t N>
void copy_text(std::array<char, N>* target, const char* value) {
  if (target == nullptr) {
    return;
  }
  strlcpy(target->data(), value != nullptr ? value : "", target->size());
}

template <size_t N>
std::string text_string(const std::array<char, N>& value) {
  return value.data();
}

template <size_t N>
bool has_text(const std::array<char, N>& value) {
  return value[0] != '\0';
}

bool parse_int_text_local(const char* text, int* value) {
  if (text == nullptr || value == nullptr) {
    return false;
  }

  const char* start = text;
  while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start)) != 0) {
    ++start;
  }
  if (*start == '\0') {
    return false;
  }

  const char* digits = start;
  if (*digits == '+' || *digits == '-') {
    ++digits;
  }

  bool all_hex = *digits != '\0';
  size_t digit_count = 0;
  for (const char* cursor = digits; *cursor != '\0'; ++cursor) {
    if (std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
      break;
    }
    if (std::isxdigit(static_cast<unsigned char>(*cursor)) == 0) {
      all_hex = false;
      break;
    }
    ++digit_count;
  }

  const bool explicit_hex = (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'));
  const int base = (explicit_hex || (all_hex && digit_count >= 8U)) ? 16 : 10;
  char* end = nullptr;
  const long parsed = std::strtol(start, &end, base);
  if (end == nullptr || end == start) {
    return false;
  }

  *value = static_cast<int>(parsed);
  return true;
}

bool parse_uint64_text_local(const char* text, uint64_t* value) {
  if (text == nullptr || value == nullptr) {
    return false;
  }

  const char* start = text;
  while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start)) != 0) {
    ++start;
  }
  if (*start == '\0') {
    return false;
  }

  const char* digits = start;
  if (*digits == '+') {
    ++digits;
  }

  bool all_hex = *digits != '\0';
  size_t digit_count = 0;
  for (const char* cursor = digits; *cursor != '\0'; ++cursor) {
    if (std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
      break;
    }
    if (std::isxdigit(static_cast<unsigned char>(*cursor)) == 0) {
      all_hex = false;
      break;
    }
    ++digit_count;
  }

  const bool explicit_hex = (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'));
  const int base = (explicit_hex || (all_hex && digit_count >= 8U)) ? 16 : 10;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(start, &end, base);
  if (end == nullptr || end == start) {
    return false;
  }

  *value = static_cast<uint64_t>(parsed);
  return true;
}

bool json_uint64_like_local(const cJSON* item, uint64_t* value) {
  if (item == nullptr || value == nullptr) {
    return false;
  }
  if (cJSON_IsNumber(item)) {
    if (item->valuedouble < 0.0) {
      return false;
    }
    *value = static_cast<uint64_t>(item->valuedouble);
    return true;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return parse_uint64_text_local(item->valuestring, value);
  }
  return false;
}

bool json_uint64_field_local(const cJSON* object, std::initializer_list<const char*> keys,
                             uint64_t* value) {
  if (object == nullptr || value == nullptr) {
    return false;
  }
  for (const char* key : keys) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (json_uint64_like_local(item, value)) {
      return true;
    }
  }
  return false;
}

int count_hms_entries_local(const cJSON* item) {
  if (item == nullptr) {
    return 0;
  }
  if (cJSON_IsArray(item)) {
    return cJSON_GetArraySize(item);
  }
  if (cJSON_IsObject(item)) {
    uint64_t direct_code = 0;
    if (json_uint64_field_local(item, {"ecode", "hms_code", "hmsCode", "full_code", "fullCode"},
                                &direct_code) &&
        direct_code > 0xFFFFFFFFULL) {
      return 1;
    }
    uint64_t attr = 0;
    uint64_t code = 0;
    if (json_uint64_field_local(item, {"attr", "hms_attr", "hmsAttr"}, &attr) &&
        json_uint64_field_local(item, {"code", "err_code", "errCode", "alarm_code", "alarmCode"},
                                &code)) {
      return 1;
    }
    int count = 0;
    for (const cJSON* child = item->child; child != nullptr; child = child->next) {
      ++count;
    }
    return count;
  }

  if (cJSON_IsNumber(item)) {
    return item->valueint;
  }
  int value = 0;
  return parse_int_text_local(cJSON_IsString(item) ? item->valuestring : nullptr, &value) ? value : 0;
}

void append_unique_hms_code(std::vector<uint64_t>* codes, uint64_t hms_code) {
  if (codes == nullptr || hms_code == 0) {
    return;
  }
  if (std::find(codes->begin(), codes->end(), hms_code) == codes->end()) {
    codes->push_back(hms_code);
  }
}

bool extract_hms_code_local(const cJSON* item, uint64_t* hms_code) {
  if (item == nullptr || hms_code == nullptr) {
    return false;
  }

  uint64_t direct_code = 0;
  if (json_uint64_like_local(item, &direct_code) && direct_code > 0xFFFFFFFFULL) {
    *hms_code = direct_code;
    return true;
  }

  if (!cJSON_IsObject(item)) {
    return false;
  }

  if (json_uint64_field_local(item, {"ecode", "hms_code", "hmsCode", "full_code", "fullCode"},
                              &direct_code) &&
      direct_code > 0xFFFFFFFFULL) {
    *hms_code = direct_code;
    return true;
  }

  uint64_t attr = 0;
  uint64_t code = 0;
  const bool have_attr = json_uint64_field_local(item, {"attr", "hms_attr", "hmsAttr"}, &attr);
  const bool have_code = json_uint64_field_local(
      item, {"code", "err_code", "errCode", "alarm_code", "alarmCode"}, &code);
  if (have_attr && have_code) {
    *hms_code = ((attr & 0xFFFFFFFFULL) << 32U) | (code & 0xFFFFFFFFULL);
    return true;
  }

  if (json_uint64_field_local(item, {"code", "err_code", "errCode", "alarm_code", "alarmCode"},
                              &direct_code) &&
      direct_code > 0xFFFFFFFFULL) {
    *hms_code = direct_code;
    return true;
  }

  return false;
}

std::vector<uint64_t> extract_hms_codes_local(const cJSON* item) {
  std::vector<uint64_t> codes;
  if (item == nullptr) {
    return codes;
  }

  if (cJSON_IsArray(item)) {
    const int count = cJSON_GetArraySize(item);
    for (int i = 0; i < count; ++i) {
      uint64_t hms_code = 0;
      if (extract_hms_code_local(cJSON_GetArrayItem(item, i), &hms_code)) {
        append_unique_hms_code(&codes, hms_code);
      }
    }
    return codes;
  }

  uint64_t direct_code = 0;
  if (extract_hms_code_local(item, &direct_code)) {
    append_unique_hms_code(&codes, direct_code);
    return codes;
  }

  if (cJSON_IsObject(item)) {
    for (const cJSON* child = item->child; child != nullptr; child = child->next) {
      uint64_t hms_code = 0;
      if (extract_hms_code_local(child, &hms_code)) {
        append_unique_hms_code(&codes, hms_code);
      }
    }
  }

  return codes;
}

bool tick_elapsed(uint32_t start_tick, uint32_t now_tick, TickType_t duration) {
  if (start_tick == 0) {
    return false;
  }
  return static_cast<int32_t>(now_tick - start_tick) >= static_cast<int32_t>(duration);
}

void append_embedded_cert(std::string* target, const uint8_t* begin, const uint8_t* end) {
  if (target == nullptr || begin == nullptr || end == nullptr || end <= begin) {
    return;
  }

  size_t length = static_cast<size_t>(end - begin);
  if (length > 0U && begin[length - 1] == '\0') {
    --length;
  }
  if (length == 0U) {
    return;
  }

  target->append(reinterpret_cast<const char*>(begin), length);
  if (target->empty() || target->back() != '\n') {
    target->push_back('\n');
  }
}

const std::string& local_bambu_ca_bundle() {
  static const std::string bundle = []() {
    std::string certs;
    certs.reserve(5500);
    append_embedded_cert(&certs, bambu_root_cert_start, bambu_root_cert_end);
    append_embedded_cert(&certs, bambu_p2s_cert_start, bambu_p2s_cert_end);
    append_embedded_cert(&certs, bambu_h2c_cert_start, bambu_h2c_cert_end);
    return certs;
  }();
  return bundle;
}

void log_heap_status(const char* context) {
  const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
  const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

  ESP_LOGI(kTag,
           "%s heap: internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
           context != nullptr ? context : "heap",
           static_cast<unsigned int>(internal_free),
           static_cast<unsigned int>(internal_largest),
           static_cast<unsigned int>(dma_free),
           static_cast<unsigned int>(dma_largest));
}

std::string make_client_id() {
  char buffer[48] = {};
  std::snprintf(buffer, sizeof(buffer), "printsphere-%08" PRIx32 "%08" PRIx32,
                esp_random(), esp_random());
  return buffer;
}

const char* connect_return_code_name(esp_mqtt_connect_return_code_t code) {
  switch (code) {
    case MQTT_CONNECTION_ACCEPTED:
      return "accepted";
    case MQTT_CONNECTION_REFUSE_PROTOCOL:
      return "wrong protocol";
    case MQTT_CONNECTION_REFUSE_ID_REJECTED:
      return "client id rejected";
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
      return "server unavailable";
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
      return "bad username";
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
      return "not authorized";
    default:
      return "unknown";
  }
}

int json_int_local(const cJSON* object, const char* key, int fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    return item->valueint;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return std::atoi(item->valuestring);
  }
  return fallback;
}

float json_number_local(const cJSON* object, const char* key, float fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    return static_cast<float>(item->valuedouble);
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return static_cast<float>(std::atof(item->valuestring));
  }
  return fallback;
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

const cJSON* child_object_local(const cJSON* object, const char* key) {
  if (object == nullptr || key == nullptr) {
    return nullptr;
  }
  const cJSON* child = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsObject(child) ? child : nullptr;
}

const cJSON* child_array_local(const cJSON* object, const char* key) {
  if (object == nullptr || key == nullptr) {
    return nullptr;
  }
  const cJSON* child = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsArray(child) ? child : nullptr;
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

void apply_chamber_light_report(const cJSON* object,
                                PrinterClient::LocalPrinterRuntimeState* runtime) {
  if (object == nullptr || runtime == nullptr) {
    return;
  }

  const cJSON* lights_report = child_array_local(object, "lights_report");
  if (!cJSON_IsArray(lights_report)) {
    return;
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
    runtime->chamber_light_supported = true;
    runtime->chamber_light_state_known = true;
    runtime->chamber_light_on = any_on;
  }
}

PrinterModel detect_printer_model(const cJSON* modules, PrinterModel fallback) {
  if (!cJSON_IsArray(modules)) {
    return fallback;
  }

  const int count = cJSON_GetArraySize(modules);
  for (int i = 0; i < count; ++i) {
    const cJSON* module = cJSON_GetArrayItem(modules, i);
    if (!cJSON_IsObject(module)) {
      continue;
    }

    const PrinterModel model =
        bambu_model_from_product_name(json_string_local(module, "product_name",
                                                        json_string_local(module, "productName", {})));
    if (model != PrinterModel::kUnknown) {
      return model;
    }
  }

  for (int i = 0; i < count; ++i) {
    const cJSON* module = cJSON_GetArrayItem(modules, i);
    if (!cJSON_IsObject(module)) {
      continue;
    }

    const std::string hw_ver = json_string_local(module, "hw_ver", {});
    const std::string project_name = json_string_local(module, "project_name", {});
    if (hw_ver == "AP02") {
      return PrinterModel::kX1E;
    }
    if (project_name == "N1") {
      return PrinterModel::kA1Mini;
    }
    if (hw_ver == "AP04") {
      if (project_name == "C11") {
        return PrinterModel::kP1P;
      }
      if (project_name == "C12") {
        return PrinterModel::kP1S;
      }
    }
    if (hw_ver == "AP05") {
      if (project_name == "N2S") {
        return PrinterModel::kA1;
      }
      if (project_name.empty()) {
        return PrinterModel::kX1C;
      }
    }
  }

  return fallback;
}

PrinterModel detect_printer_model_from_payload(const cJSON* object, PrinterModel fallback) {
  if (!cJSON_IsObject(object)) {
    return fallback;
  }

  const char* keys[] = {"product_name", "productName", "printer_model",
                        "printerModel", "model",        "series"};
  for (const char* key : keys) {
    if (const PrinterModel model = bambu_model_from_product_name(json_string_local(object, key, {}));
        model != PrinterModel::kUnknown) {
      return model;
    }
  }

  return fallback;
}

std::string extract_module_serial(const cJSON* modules, const std::string& fallback) {
  if (!cJSON_IsArray(modules)) {
    return fallback;
  }

  const int count = cJSON_GetArraySize(modules);
  for (int i = 0; i < count; ++i) {
    const cJSON* module = cJSON_GetArrayItem(modules, i);
    if (!cJSON_IsObject(module)) {
      continue;
    }
    const std::string serial =
        json_string_local(module, "sn", json_string_local(module, "serial", {}));
    if (!serial.empty()) {
      return serial;
    }
  }

  return fallback;
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

    const float temp = normalize_temperature_candidate(json_number_local(item, "temp", -1000.0f));
    const int id = json_int_local(item, "id", -1);
    ESP_LOGI(kTag, "[DBG] nozzle info[%d]: id=%d temp=%.1f (raw int=%d)",
             i, id, temp, (int)temp);
    if (temp <= -999.0f) {
      continue;
    }

    if (first_temp < -999.0f) {
      first_temp = temp;
    }

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

float extract_bed_temperature_c(const cJSON* print, float fallback) {
  const cJSON* device = child_object_local(print, "device");
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

  return json_number_local(print, "bed_temper", fallback);
}

float extract_chamber_temperature_c(const cJSON* print, float fallback) {
  const cJSON* device = child_object_local(print, "device");
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

  return json_number_local(print, "chamber_temper", fallback);
}

NozzleTemperatureBundle extract_nozzle_temperature_bundle(const cJSON* print, float active_fallback,
                                                          float secondary_fallback) {
  NozzleTemperatureBundle bundle{active_fallback, secondary_fallback};
  const float direct = json_number_local(print, "nozzle_temper", -1000.0f);
  ESP_LOGI(kTag, "[DBG] nozzle_temper=%.1f (raw int=%d) fallback=%.1f",
           direct, (int)direct, active_fallback);
  if (direct > -999.0f) {
    bundle.active = direct;
  }

  const cJSON* device = child_object_local(print, "device");
  const cJSON* extruder = child_object_local(device, "extruder");
  const int active_nozzle_index = extract_active_nozzle_index(device);
  merge_nozzle_temp_candidates(child_array_local(child_object_local(device, "nozzle"), "info"),
                               active_nozzle_index, &bundle.active, &bundle.secondary);
  merge_nozzle_temp_candidates(child_array_local(extruder, "info"), active_nozzle_index,
                               &bundle.active, &bundle.secondary);
  ESP_LOGI(kTag, "[DBG] nozzle bundle final: active=%.1f secondary=%.1f active_nozzle_idx=%d",
           bundle.active, bundle.secondary, active_nozzle_index);
  return bundle;
}

float extract_progress_percent(const cJSON* print, float fallback) {
  const char* keys[] = {"mc_percent", "percent", "progress", "task_progress", "print_progress"};
  for (const char* key : keys) {
    const float value = json_number_local(print, key, -1.0f);
    if (value >= 0.0f) {
      return value <= 1.0f ? (value * 100.0f) : value;
    }
  }
  return fallback;
}

uint16_t extract_current_layer_local(const cJSON* print, uint16_t fallback) {
  return static_cast<uint16_t>(std::max(
      json_int_local(print, "layer_num",
                     json_int_local(print, "current_layer",
                                    json_int_local(print, "currentLayer", fallback))),
      0));
}

uint16_t extract_total_layers_local(const cJSON* print, uint16_t fallback) {
  return static_cast<uint16_t>(std::max(
      json_int_local(print, "total_layer_num",
                     json_int_local(print, "total_layers",
                                    json_int_local(print, "totalLayers", fallback))),
      0));
}

std::string extract_rtsp_url(const cJSON* print, const std::string& fallback) {
  const cJSON* ipcam = child_object_local(print, "ipcam");
  const std::string rtsp_url =
      json_string_local(ipcam, "rtsp_url", json_string_local(ipcam, "rtspUrl", {}));
  if (rtsp_url == "disable") {
    return {};
  }
  if (!rtsp_url.empty()) {
    return rtsp_url;
  }
  return fallback;
}

bool parse_signature_required(const cJSON* print, bool fallback) {
  const std::string fun = json_string_local(print, "fun", {});
  if (fun.empty()) {
    return fallback;
  }

  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(fun.c_str(), &end, 16);
  if (end == nullptr || *end != '\0') {
    return fallback;
  }

  return (parsed & 0x20000000ULL) != 0ULL;
}

void update_local_runtime_metadata(PrinterClient::LocalPrinterRuntimeState* runtime, bool configured,
                                   bool connected) {
  if (runtime == nullptr) {
    return;
  }

  runtime->local_configured = configured;
  runtime->local_connected = connected;
  runtime->local_capabilities = default_local_capabilities_for_model(runtime->local_model);
  if (has_text(runtime->camera_rtsp_url)) {
    runtime->local_capabilities.camera_rtsp = true;
  }
  if (runtime->local_mqtt_signature_required) {
    runtime->local_capabilities.developer_mode_required = true;
  }
  runtime->local_last_update_ms = now_ms();
}

uint32_t extract_remaining_seconds(const cJSON* print) {
  if (!cJSON_IsObject(print)) {
    return 0U;
  }

  const int minutes = json_int_local(
      print, "mc_remaining_time",
      json_int_local(print, "remaining_minutes",
                     json_int_local(print, "remainingMinutes",
                                    json_int_local(print, "remain_time", -1))));
  if (minutes >= 0) {
    return static_cast<uint32_t>(minutes) * 60U;
  }

  const int seconds = json_int_local(
      print, "remaining_seconds",
      json_int_local(print, "remainingSeconds",
                     json_int_local(print, "remaining_time",
                                    json_int_local(print, "remainingTime",
                                                   json_int_local(print, "mc_left_time", -1)))));
  if (seconds >= 0) {
    return static_cast<uint32_t>(seconds);
  }

  return 0U;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool is_placeholder_stage_name(const std::string& stage_name) {
  if (stage_name.empty()) {
    return true;
  }

  const std::string lower = lower_copy(stage_name);
  return lower == "status" || lower == "stage" || lower == "unknown";
}

bool is_active_gcode_state(const std::string& gcode_state) {
  return gcode_state == "RUNNING" || gcode_state == "PREPARE" ||
         gcode_state == "PAUSE" || gcode_state == "PAUSED" ||
         gcode_state == "INIT" || gcode_state == "SLICING";
}

bool is_terminal_gcode_state(const std::string& gcode_state) {
  return gcode_state == "FAILED" || gcode_state == "FINISH" || gcode_state == "IDLE" ||
         gcode_state == "OFFLINE";
}

bool is_idle_stage_marker(int stage_id, const std::string& stage_name) {
  return stage_id == -1 || stage_id == 255 || lower_copy(stage_name) == "idle";
}

bool is_meaningful_active_stage(const std::string& stage_name) {
  if (stage_name.empty()) {
    return false;
  }

  const std::string lower = lower_copy(stage_name);
  return lower != "idle" && lower != "finished" && lower != "failed" &&
         lower != "printing" && lower != "preparing" && lower != "paused" &&
         !is_placeholder_stage_name(stage_name);
}

bool is_paused_gcode_state(const std::string& gcode_state) {
  return bambu_status_is_paused(gcode_state);
}

bool is_fault_pause_stage(const std::string& stage_name) {
  if (stage_name.empty()) {
    return false;
  }

  const std::string lower = lower_copy(stage_name);
  if (lower == "paused" || lower == "pause" || lower == "m400_pause" ||
      lower == "paused_user" || lower == "paused_user_gcode") {
    return false;
  }

  return lower.rfind("paused_", 0) == 0;
}

std::string resolved_stage_from_payload(const std::string& effective_gcode_state,
                                        const std::string& payload_stage_name, int stage_id,
                                        bool has_concrete_error) {
  if (is_terminal_gcode_state(effective_gcode_state)) {
    if (effective_gcode_state == "FAILED") {
      return has_concrete_error ? "Failed" : "Stopped";
    }
    if (effective_gcode_state == "FINISH") {
      return "Finished";
    }
    if (effective_gcode_state == "IDLE") {
      return "Idle";
    }
  }

  if (effective_gcode_state == "PAUSE" || effective_gcode_state == "PAUSED") {
    if (stage_id == -1 || stage_id == 255 || is_placeholder_stage_name(payload_stage_name) ||
        lower_copy(payload_stage_name) == "idle") {
      return "Paused";
    }
  }

  if (!is_placeholder_stage_name(payload_stage_name)) {
    const std::string lowered_stage = lower_copy(payload_stage_name);
    if (lowered_stage == "idle" && is_active_gcode_state(effective_gcode_state)) {
      return {};
    }
    return payload_stage_name;
  }

  if ((stage_id == -1 || stage_id == 255) && is_active_gcode_state(effective_gcode_state)) {
    return {};
  }

  if (const std::string stage_label = bambu_stage_label_from_id(stage_id); !stage_label.empty()) {
    return stage_label;
  }

  return bambu_default_stage_label_for_status(effective_gcode_state, has_concrete_error);
}

}  // namespace

void PrinterClient::configure(PrinterConnection connection) {
  if (connection.mqtt_username.empty()) {
    connection.mqtt_username = "bblp";
  }
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    desired_connection_ = std::move(connection);
  }
  reconfigure_requested_.store(true);
  wake_task();
}

bool PrinterClient::is_configured() const {
  return desired_connection().is_ready();
}

bool PrinterClient::set_chamber_light(bool on) {
  LocalPrinterRuntimeState runtime = runtime_state_copy();
  const bool supports_secondary =
      printer_model_has_secondary_chamber_light(runtime.local_model);

  auto publish_ledctrl = [&](const char* node) {
    const std::string payload = build_ledctrl_payload(node, on);
    if (!mqtt_connected_ || client_ == nullptr || payload.empty()) {
      return false;
    }

    const int msg_id =
        esp_mqtt_client_publish(client_, request_topic_.c_str(), payload.c_str(), 0, 0, 0);
    if (msg_id < 0) {
      ESP_LOGW(kTag, "Failed to publish chamber light command for %s", node);
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

  runtime.chamber_light_supported = true;
  runtime.chamber_light_state_known = true;
  runtime.chamber_light_on = on;
  update_local_runtime_metadata(&runtime, true, mqtt_connected_.load());
  store_runtime_state(std::move(runtime), false);
  publish_runtime_snapshot();
  ESP_LOGI(kTag, "Local chamber light set to %s", on ? "on" : "off");
  return true;
}

PrinterConnection PrinterClient::desired_connection() const {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return desired_connection_;
}

esp_err_t PrinterClient::start() {
  if (task_handle_ != nullptr) {
    return ESP_OK;
  }

  const BaseType_t result =
      xTaskCreate(&PrinterClient::task_entry, "printer_client", 8192, this, 5, &task_handle_);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}

void PrinterClient::mqtt_event_handler(void* handler_args, esp_event_base_t, int32_t,
                                       void* event_data) {
  auto* client = static_cast<PrinterClient*>(handler_args);
  if (client == nullptr || event_data == nullptr) {
    return;
  }

  client->handle_mqtt_event(static_cast<esp_mqtt_event_handle_t>(event_data));
}

void PrinterClient::task_entry(void* context) {
  static_cast<PrinterClient*>(context)->task_loop();
}

PrinterClient::LocalPrinterRuntimeState PrinterClient::runtime_state_copy() const {
  std::lock_guard<std::mutex> lock(runtime_mutex_);
  return runtime_state_;
}

void PrinterClient::store_runtime_state(LocalPrinterRuntimeState runtime, bool notify_task) {
  {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    runtime_state_ = std::move(runtime);
  }
  runtime_dirty_ = true;
  if (notify_task) {
    wake_task();
  }
}

void PrinterClient::publish_runtime_snapshot() {
  const LocalPrinterRuntimeState runtime = runtime_state_copy();
  state_.set_snapshot(build_snapshot_from_runtime(runtime));
}

PrinterSnapshot PrinterClient::build_snapshot_from_runtime(
    const LocalPrinterRuntimeState& runtime) const {
  PrinterSnapshot snapshot;
  snapshot.connection = runtime.connection;
  snapshot.lifecycle = runtime.lifecycle;
  snapshot.stage = text_string(runtime.stage);
  snapshot.detail = text_string(runtime.detail);
  snapshot.raw_status = text_string(runtime.raw_status);
  snapshot.raw_stage = text_string(runtime.raw_stage);
  snapshot.resolved_serial = text_string(runtime.resolved_serial);
  snapshot.job_name = text_string(runtime.job_name);
  snapshot.gcode_file = text_string(runtime.gcode_file);
  snapshot.preview_hint = preview_hint_for(snapshot.gcode_file);
  snapshot.camera_rtsp_url = text_string(runtime.camera_rtsp_url);
  snapshot.progress_percent = runtime.progress_percent;
  snapshot.nozzle_temp_c = runtime.nozzle_temp_c;
  snapshot.nozzle_temp_known = runtime.nozzle_temp_c > 0.0f;
  snapshot.bed_temp_c = runtime.bed_temp_c;
  snapshot.bed_temp_known = runtime.bed_temp_c > 0.0f;
  snapshot.chamber_temp_c = runtime.chamber_temp_c;
  snapshot.chamber_temp_known = runtime.chamber_temp_c > 0.0f;
  snapshot.secondary_nozzle_temp_c = runtime.secondary_nozzle_temp_c;
  snapshot.secondary_nozzle_temp_known = runtime.secondary_nozzle_temp_c > 0.0f;
  snapshot.chamber_light_supported = runtime.chamber_light_supported;
  snapshot.chamber_light_state_known = runtime.chamber_light_state_known;
  snapshot.chamber_light_on = runtime.chamber_light_on;
  snapshot.remaining_seconds = runtime.remaining_seconds;
  snapshot.current_layer = runtime.current_layer;
  snapshot.total_layers = runtime.total_layers;
  snapshot.print_error_code = runtime.print_error_code;
  snapshot.hms_codes = runtime.hms_codes;
  snapshot.hms_alert_count = runtime.hms_alert_count;
  snapshot.local_configured = runtime.local_configured;
  snapshot.local_connected = runtime.local_connected;
  snapshot.local_last_update_ms = runtime.local_last_update_ms;
  snapshot.local_model = runtime.local_model;
  snapshot.local_capabilities = runtime.local_capabilities;
  snapshot.local_mqtt_signature_required = runtime.local_mqtt_signature_required;
  snapshot.has_error = runtime.has_error;
  snapshot.print_active = runtime.print_active;
  snapshot.warn_hms = runtime.warn_hms;
  snapshot.non_error_stop = runtime.non_error_stop;
  snapshot.show_stop_banner = runtime.show_stop_banner;
  return snapshot;
}

void PrinterClient::wake_task() {
  if (task_handle_ != nullptr) {
    xTaskNotifyGive(task_handle_);
  }
}

void PrinterClient::handle_mqtt_event(esp_mqtt_event_handle_t event) {
  if (event == nullptr || client_ == nullptr || event->client != client_) {
    return;
  }

  switch (static_cast<esp_mqtt_event_id_t>(event->event_id)) {
    case MQTT_EVENT_CONNECTED: {
      cancel_client_rebuild();
      consecutive_probe_failures_ = 0;
      consecutive_mqtt_errors_ = 0;
      mqtt_connected_ = true;
      received_payload_ = false;
      subscription_acknowledged_ = false;
      initial_sync_sent_ = false;
      delayed_pushall_sent_ = false;
      first_payload_observed_ = false;
      initial_sync_tick_ = 0;
      connection_state_tick_ = xTaskGetTickCount();
      watchdog_probe_tick_ = 0;
      last_message_tick_ = xTaskGetTickCount();

      const int msg_id = esp_mqtt_client_subscribe(client_, report_topic_.c_str(), 1);
      if (msg_id < 0) {
        ESP_LOGW(kTag, "Failed to subscribe to %s", report_topic_.c_str());
      } else {
        ESP_LOGI(kTag, "Subscribed request queued for %s (msg_id=%d)", report_topic_.c_str(),
                 msg_id);
      }

      LocalPrinterRuntimeState runtime = runtime_state_copy();
      std::string active_host;
      std::string active_serial;
      {
        std::lock_guard<std::mutex> lock(config_mutex_);
        active_host = active_connection_.host;
        active_serial = active_connection_.serial;
      }
      runtime.connection = PrinterConnectionState::kOnline;
      runtime.lifecycle = PrintLifecycleState::kUnknown;
      copy_text(&runtime.raw_status, "");
      copy_text(&runtime.raw_stage, "");
      copy_text(&runtime.stage, "connected");
      copy_text(&runtime.detail, "Connected to local Bambu MQTT, waiting for subscribe ack");
      copy_text(&runtime.resolved_serial, active_serial);
      runtime.print_error_code = 0;
      runtime.hms_codes.clear();
      runtime.hms_alert_count = 0;
      runtime.has_error = false;
      runtime.warn_hms = false;
      runtime.print_active = false;
      runtime.non_error_stop = false;
      runtime.show_stop_banner = false;
      update_local_runtime_metadata(&runtime, true, true);
      store_runtime_state(std::move(runtime), true);
      ESP_LOGI(kTag, "Connected to %s", active_host.c_str());
      break;
    }

    case MQTT_EVENT_SUBSCRIBED: {
      cancel_client_rebuild();
      subscription_acknowledged_ = true;
      initial_sync_sent_ = true;
      delayed_pushall_sent_ = false;
      initial_sync_tick_ = xTaskGetTickCount();
      watchdog_probe_tick_ = 0;

      LocalPrinterRuntimeState runtime = runtime_state_copy();
      runtime.connection = PrinterConnectionState::kOnline;
      runtime.lifecycle = PrintLifecycleState::kUnknown;
      copy_text(&runtime.raw_status, "");
      copy_text(&runtime.raw_stage, "");
      copy_text(&runtime.stage, "subscribed");
      copy_text(&runtime.detail, "MQTT subscribed, requesting printer sync");
      runtime.print_error_code = 0;
      runtime.hms_codes.clear();
      runtime.hms_alert_count = 0;
      runtime.has_error = false;
      runtime.warn_hms = false;
      runtime.print_active = false;
      runtime.non_error_stop = false;
      runtime.show_stop_banner = false;
      update_local_runtime_metadata(&runtime, true, true);
      store_runtime_state(std::move(runtime), true);

      ESP_LOGI(kTag, "MQTT subscribe acknowledged (msg_id=%d), sending get_version + start",
               event->msg_id);
      request_initial_sync();
      break;
    }

    case MQTT_EVENT_DISCONNECTED: {
      mqtt_connected_ = false;
      subscription_acknowledged_ = false;
      initial_sync_sent_ = false;
      delayed_pushall_sent_ = false;
      initial_sync_tick_ = 0;
      connection_state_tick_ = xTaskGetTickCount();
      watchdog_probe_tick_ = 0;
      LocalPrinterRuntimeState runtime = runtime_state_copy();
      runtime.connection = PrinterConnectionState::kConnecting;
      copy_text(&runtime.stage, "");
      copy_text(&runtime.raw_status, "");
      copy_text(&runtime.raw_stage, "");
      copy_text(&runtime.detail, "MQTT disconnected, waiting for reconnect");
      runtime.print_error_code = 0;
      runtime.hms_codes.clear();
      runtime.hms_alert_count = 0;
      runtime.has_error = false;
      runtime.warn_hms = false;
      runtime.print_active = false;
      runtime.non_error_stop = false;
      runtime.show_stop_banner = false;
      update_local_runtime_metadata(&runtime, true, false);
      store_runtime_state(std::move(runtime), true);
      ESP_LOGW(kTag, "MQTT disconnected");
      break;
    }

    case MQTT_EVENT_DATA: {
      std::string topic;
      std::string payload;

      {
        std::unique_lock<std::mutex> lock(incoming_mutex_);
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

      if (!payload.empty() && topic == report_topic_) {
        const bool first_payload = !first_payload_observed_.exchange(true);
        if (first_payload) {
          cancel_client_rebuild();
          log_heap_status("Before first MQTT payload");
        }
        last_message_tick_ = xTaskGetTickCount();
        watchdog_probe_tick_ = 0;
        handle_report_payload(payload.data(), payload.size());
        if (first_payload) {
          log_heap_status("After first MQTT payload");
        }
      }
      break;
    }

    case MQTT_EVENT_ERROR: {
      mqtt_connected_ = false;
      subscription_acknowledged_ = false;
      initial_sync_sent_ = false;
      delayed_pushall_sent_ = false;
      initial_sync_tick_ = 0;
      connection_state_tick_ = xTaskGetTickCount();
      watchdog_probe_tick_ = 0;
      log_heap_status("MQTT error");
      LocalPrinterRuntimeState runtime = runtime_state_copy();
      runtime.connection = PrinterConnectionState::kError;
      runtime.lifecycle = PrintLifecycleState::kError;
      copy_text(&runtime.raw_status, "");
      copy_text(&runtime.raw_stage, "");
      copy_text(&runtime.stage, "mqtt-error");
      runtime.print_error_code = 0;
      runtime.hms_codes.clear();
      runtime.hms_alert_count = 0;
      runtime.has_error = true;
      runtime.warn_hms = false;
      runtime.print_active = false;
      runtime.non_error_stop = false;
      runtime.show_stop_banner = false;

      const auto* error = event->error_handle;
      if (error != nullptr) {
        if (error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
          if (error->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
            copy_text(&runtime.stage, "mqtt-auth");
            copy_text(&runtime.detail, "MQTT auth rejected; verify access code");
          } else if (error->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME) {
            copy_text(&runtime.stage, "mqtt-auth");
            copy_text(&runtime.detail, "MQTT username rejected");
          } else {
            copy_text(&runtime.detail,
                      std::string("MQTT refused: ") +
                          connect_return_code_name(error->connect_return_code));
          }
          ESP_LOGE(kTag, "MQTT refused by broker: %s",
                   connect_return_code_name(error->connect_return_code));
        } else if (error->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          const int sock_err = error->esp_transport_sock_errno;
          if (sock_err == ECONNREFUSED) {
            copy_text(&runtime.detail, "Printer reachable but MQTT port closed");
          } else if (sock_err == ETIMEDOUT || sock_err == 0) {
            copy_text(&runtime.detail,
                      "MQTT TLS handshake timeout – printer may be off or busy");
          } else if (sock_err == EHOSTUNREACH || sock_err == ENETUNREACH) {
            copy_text(&runtime.detail, "Printer network unreachable");
          } else {
            copy_text(&runtime.detail,
                      std::string("MQTT transport error (errno=") +
                          std::to_string(sock_err) + ")");
          }
          ESP_LOGE(kTag, "MQTT transport error: esp_err=%s tls=0x%x sock_errno=%d",
                   esp_err_to_name(error->esp_tls_last_esp_err), error->esp_tls_stack_err,
                   sock_err);
        } else {
          copy_text(&runtime.detail, "TLS or MQTT handshake failed");
          ESP_LOGE(kTag, "MQTT event error type=%d", static_cast<int>(error->error_type));
        }
      } else {
        copy_text(&runtime.detail, "TLS or MQTT handshake failed");
        ESP_LOGE(kTag, "MQTT event error without details");
      }

      update_local_runtime_metadata(&runtime, true, false);
      store_runtime_state(std::move(runtime), true);
      {
        ++consecutive_mqtt_errors_;
        const uint32_t backoff_ms =
            consecutive_mqtt_errors_ <= 1 ? 2000U :
            consecutive_mqtt_errors_ <= 2 ? 4000U :
            consecutive_mqtt_errors_ <= 4 ? 8000U :
            consecutive_mqtt_errors_ <= 6 ? 15000U : 30000U;
        schedule_client_rebuild("mqtt error", backoff_ms);
      }
      break;
    }

    default:
      break;
  }
}

void PrinterClient::handle_report_payload(const char* payload, size_t length) {
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (root == nullptr) {
    ESP_LOGW(kTag, "Failed to parse MQTT payload");
    return;
  }

  const cJSON* print = cJSON_GetObjectItemCaseSensitive(root, "print");
  if (cJSON_IsObject(print)) {
    LocalPrinterRuntimeState runtime = runtime_state_copy();
    std::string active_serial;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      active_serial = active_connection_.serial;
    }
    const std::string previous_raw_status = text_string(runtime.raw_status);
    const std::string previous_raw_stage = text_string(runtime.raw_stage);
    const std::string previous_stage = text_string(runtime.stage);
    const std::string previous_detail = text_string(runtime.detail);
    const int previous_print_error_code = runtime.print_error_code;
    const uint16_t previous_hms_alert_count = runtime.hms_alert_count;
    const bool previous_pause_fault = runtime.has_error && is_paused_gcode_state(previous_raw_status);
    const std::string gcode_state = json_string(print, "gcode_state", {});
    const std::string effective_gcode_state = !gcode_state.empty() ? gcode_state : previous_raw_status;
    const cJSON* stage = cJSON_GetObjectItemCaseSensitive(print, "stage");
    const int stage_id =
        cJSON_IsObject(stage) ? json_int(stage, "_id", json_int(print, "stg_cur", -1))
                              : json_int(print, "stg_cur", -1);
    const std::string stage_name =
        cJSON_IsObject(stage) ? json_string(stage, "name", json_string(stage, "stage", {})) : "";
    const cJSON* print_error_item = cJSON_GetObjectItemCaseSensitive(print, "print_error");
    const bool has_print_error_update = print_error_item != nullptr;
    const int print_error_code =
        has_print_error_update ? json_int(print, "print_error", runtime.print_error_code)
                               : runtime.print_error_code;
    const cJSON* hms = cJSON_GetObjectItemCaseSensitive(print, "hms");
    const bool has_hms_update = hms != nullptr;
    const int hms_count =
        has_hms_update ? count_hms_entries_local(hms) : static_cast<int>(runtime.hms_alert_count);
    const std::vector<uint64_t> parsed_hms_codes =
        has_hms_update ? extract_hms_codes_local(hms) : runtime.hms_codes;
    const bool has_hms_alert = !parsed_hms_codes.empty() || hms_count > 0;
    const bool has_concrete_error = print_error_code != 0;
    const std::string resolved_stage =
        resolved_stage_from_payload(effective_gcode_state, stage_name, stage_id, has_concrete_error);
    const bool paused_state = is_paused_gcode_state(effective_gcode_state);
    const bool fault_pause_signal = paused_state &&
                                    (has_concrete_error || is_fault_pause_stage(resolved_stage) ||
                                     is_fault_pause_stage(stage_name));
    const bool paused_fault_latched = paused_state && (fault_pause_signal || previous_pause_fault);
    const bool stage_idle_placeholder = is_idle_stage_marker(stage_id, stage_name);
    const bool has_status_update = !gcode_state.empty();
    const bool has_stage_update = !resolved_stage.empty();

    runtime.connection = PrinterConnectionState::kOnline;
    runtime.progress_percent = extract_progress_percent(print, runtime.progress_percent);
    const NozzleTemperatureBundle nozzle_temps =
        extract_nozzle_temperature_bundle(print, runtime.nozzle_temp_c,
                                          runtime.secondary_nozzle_temp_c);
    runtime.nozzle_temp_c = nozzle_temps.active;
    runtime.secondary_nozzle_temp_c = nozzle_temps.secondary;
    runtime.bed_temp_c = extract_bed_temperature_c(print, runtime.bed_temp_c);
    runtime.chamber_temp_c = extract_chamber_temperature_c(print, runtime.chamber_temp_c);
    runtime.current_layer = extract_current_layer_local(print, runtime.current_layer);
    runtime.total_layers = extract_total_layers_local(print, runtime.total_layers);
    runtime.local_model = detect_printer_model_from_payload(print, runtime.local_model);
    runtime.chamber_light_supported =
        runtime.chamber_light_supported || printer_model_has_chamber_light(runtime.local_model);
    copy_text(&runtime.camera_rtsp_url,
              extract_rtsp_url(print, text_string(runtime.camera_rtsp_url)));
    runtime.local_mqtt_signature_required =
        parse_signature_required(print, runtime.local_mqtt_signature_required);
    apply_chamber_light_report(print, &runtime);
    if (!has_text(runtime.resolved_serial)) {
      copy_text(&runtime.resolved_serial, active_serial);
    }

    const uint32_t remaining_seconds = extract_remaining_seconds(print);
    if (remaining_seconds > 0U) {
      runtime.remaining_seconds = remaining_seconds;
    }

    const std::string previous_preview_hint = preview_hint_for(text_string(runtime.gcode_file));
    const std::string gcode_file = json_string(print, "gcode_file", text_string(runtime.gcode_file));
    if (!gcode_file.empty()) {
      copy_text(&runtime.gcode_file, gcode_file);
    }
    const std::string preview_hint = preview_hint_for(text_string(runtime.gcode_file));
    if (!preview_hint.empty() && preview_hint != previous_preview_hint) {
      ESP_LOGI(kTag, "Preview candidate: %s", preview_hint.c_str());
    }

    const std::string subtask_name = trim_job_name(
        json_string(print, "subtask_name", json_string(print, "gcode_file",
                                                       text_string(runtime.job_name))));
    if (!subtask_name.empty()) {
      copy_text(&runtime.job_name, subtask_name);
    }

    copy_text(&runtime.raw_status, has_status_update ? gcode_state : previous_raw_status);
    if (has_stage_update) {
      copy_text(&runtime.raw_stage, resolved_stage);
    } else if (is_active_gcode_state(effective_gcode_state) && stage_idle_placeholder) {
      // Active Bambu jobs often emit transient "-1/255 => idle" stage packets between
      // real sub-states. Keep only the last meaningful runtime stage latched. Do not
      // carry terminal leftovers like "Finished" into a fresh PREPARE/RUNNING cycle.
      copy_text(&runtime.raw_stage,
                is_meaningful_active_stage(previous_raw_stage) ? previous_raw_stage : "");
    } else {
      copy_text(&runtime.raw_stage, previous_raw_stage);
    }
    runtime.print_error_code = print_error_code;
    if (has_hms_update) {
      runtime.hms_alert_count = static_cast<uint16_t>(std::max(hms_count, 0));
      if (!parsed_hms_codes.empty() || hms_count == 0) {
        runtime.hms_codes = parsed_hms_codes;
      }
    }
    runtime.has_error = has_concrete_error || paused_fault_latched;
    runtime.warn_hms = false;
    runtime.non_error_stop = text_string(runtime.raw_status) == "FAILED" && !has_concrete_error &&
                             !has_hms_alert;
    runtime.show_stop_banner = false;
    runtime.print_active = false;
    runtime.lifecycle = lifecycle_from_state(text_string(runtime.raw_status), has_concrete_error);
    if (paused_fault_latched) {
      runtime.lifecycle = PrintLifecycleState::kError;
    }
    if (remaining_seconds == 0U &&
        (runtime.lifecycle == PrintLifecycleState::kFinished ||
         runtime.lifecycle == PrintLifecycleState::kIdle ||
         runtime.lifecycle == PrintLifecycleState::kError)) {
      runtime.remaining_seconds = 0U;
    }
    const std::string raw_stage = text_string(runtime.raw_stage);
    const std::string raw_status = text_string(runtime.raw_status);
    if (!raw_stage.empty()) {
      copy_text(&runtime.stage, raw_stage);
    } else if (!raw_status.empty()) {
      copy_text(&runtime.stage, stage_label_for(raw_status, stage_id, has_concrete_error));
    } else {
      copy_text(&runtime.stage, previous_stage);
    }

    const std::string error_detail =
        error_detail_for(runtime.print_error_code, runtime.hms_codes, runtime.hms_alert_count,
                         runtime.local_model);
    if (!error_detail.empty()) {
      copy_text(&runtime.detail, error_detail);
    } else if (paused_fault_latched && previous_pause_fault && !previous_detail.empty() &&
               previous_detail != "Paused") {
      copy_text(&runtime.detail, previous_detail);
    } else if (has_text(runtime.job_name) && runtime.lifecycle == PrintLifecycleState::kPrinting) {
      copy_text(&runtime.detail, text_string(runtime.job_name));
    } else if (has_status_update || has_stage_update) {
      copy_text(&runtime.detail, text_string(runtime.stage));
    } else if (previous_detail.empty()) {
      copy_text(&runtime.detail, "Status payload received");
    } else {
      copy_text(&runtime.detail, previous_detail);
    }
    update_local_runtime_metadata(&runtime, true, true);

    received_payload_ = true;
    if (text_string(runtime.raw_status) != previous_raw_status ||
        text_string(runtime.raw_stage) != previous_raw_stage) {
      const bool active_idle_placeholder =
          is_active_gcode_state(effective_gcode_state) && stage_idle_placeholder &&
          !has_text(runtime.raw_stage);
      const std::string logged_stage_string =
          active_idle_placeholder ? std::string("<placeholder>") : text_string(runtime.raw_stage);
      ESP_LOGI(kTag, "Local printer state: status=%s stage=%s stg_cur=%d",
               text_string(runtime.raw_status).c_str(), logged_stage_string.c_str(), stage_id);
    }
    if ((print_error_code != previous_print_error_code ||
         runtime.hms_alert_count != previous_hms_alert_count ||
         (fault_pause_signal && !previous_pause_fault)) &&
        (has_concrete_error || has_hms_alert || paused_fault_latched)) {
      ESP_LOGW(kTag, "Local printer alert: status=%s stage=%s stg_cur=%d print_error=%d hms=%u",
               text_string(runtime.raw_status).c_str(), text_string(runtime.stage).c_str(),
               stage_id, print_error_code, static_cast<unsigned int>(runtime.hms_alert_count));
    }
    store_runtime_state(std::move(runtime), true);
    cJSON_Delete(root);
    return;
  }

  const cJSON* info = cJSON_GetObjectItemCaseSensitive(root, "info");
  if (cJSON_IsObject(info)) {
    LocalPrinterRuntimeState runtime = runtime_state_copy();
    runtime.connection = PrinterConnectionState::kOnline;
    std::string active_serial;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      active_serial = active_connection_.serial;
    }
    const cJSON* modules = child_array_local(info, "module");
    if (modules == nullptr) {
      modules = child_array_local(info, "modules");
    }
    runtime.local_model = detect_printer_model(
        modules, detect_printer_model_from_payload(info, runtime.local_model));
    runtime.chamber_light_supported =
        runtime.chamber_light_supported || printer_model_has_chamber_light(runtime.local_model);
    copy_text(&runtime.resolved_serial,
              extract_module_serial(modules, json_string(info, "sn", active_serial)));
    if (text_string(runtime.detail) == "Connecting to local Bambu MQTT" ||
        !has_text(runtime.detail)) {
      copy_text(&runtime.detail,
                std::string("Printer info received (") + to_string(runtime.local_model) + ")");
    }
    update_local_runtime_metadata(&runtime, true, true);
    store_runtime_state(std::move(runtime), true);
  }
  cJSON_Delete(root);
}

void PrinterClient::stop_client() {
  mqtt_connected_ = false;
  received_payload_ = false;
  subscription_acknowledged_ = false;
  initial_sync_sent_ = false;
  delayed_pushall_sent_ = false;
  first_payload_observed_ = false;
  client_started_ = false;
  client_rebuild_requested_ = false;
  last_message_tick_ = 0;
  initial_sync_tick_ = 0;
  connection_state_tick_ = 0;
  watchdog_probe_tick_ = 0;
  rebuild_request_tick_ = 0;
  rebuild_delay_ticks_ = 0;

  {
    std::lock_guard<std::mutex> lock(incoming_mutex_);
    incoming_topic_.clear();
    incoming_payload_.clear();
  }

  if (client_ != nullptr) {
    esp_mqtt_client_stop(client_);
    // Brief delay to let the MQTT internal task fully unwind before destroying the
    // client handle. Without this, destroying while the task is mid-TLS-handshake
    // leaks ~13 KB of internal RAM (mbedTLS session context) per failed attempt.
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_mqtt_client_destroy(client_);
    client_ = nullptr;
  }
}

void PrinterClient::schedule_client_rebuild(const char* reason, uint32_t delay_ms) {
  if (client_rebuild_requested_.exchange(true)) {
    return;
  }
  const uint32_t delay_ticks = pdMS_TO_TICKS(delay_ms == 0 ? kRebuildDelayMs : delay_ms);
  rebuild_request_tick_ = xTaskGetTickCount();
  rebuild_delay_ticks_ = delay_ticks;
  ESP_LOGW(kTag, "Scheduling MQTT client rebuild in %u ms (%s)",
           static_cast<unsigned int>(delay_ms == 0 ? kRebuildDelayMs : delay_ms),
           reason != nullptr ? reason : "unspecified");
}

void PrinterClient::cancel_client_rebuild() {
  client_rebuild_requested_ = false;
  rebuild_request_tick_ = 0;
  rebuild_delay_ticks_ = 0;
}

void PrinterClient::task_loop() {
  while (true) {
    if (runtime_dirty_.exchange(false)) {
      publish_runtime_snapshot();
    }

    if (reconfigure_requested_.exchange(false)) {
      stop_client();
    }

    uint32_t now = xTaskGetTickCount();
    if (client_rebuild_requested_.load()) {
      if (mqtt_connected_.load()) {
        cancel_client_rebuild();
      } else {
        const uint32_t requested_at = rebuild_request_tick_.load();
        const uint32_t delay_ticks = rebuild_delay_ticks_.load();
        if (requested_at == 0 || tick_elapsed(requested_at, now, delay_ticks)) {
          ESP_LOGW(kTag, "Rebuilding MQTT client after disconnect/error");
          stop_client();
          cancel_client_rebuild();
          vTaskDelay(pdMS_TO_TICKS(250));
          continue;
        }
        // Rebuild delay not yet elapsed — wait instead of falling through to
        // the client_==nullptr path which would immediately start a new TCP probe.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        continue;
      }
    }

    const PrinterConnection connection = desired_connection();
    if (!connection.is_ready()) {
      {
        std::lock_guard<std::mutex> lock(config_mutex_);
        active_connection_ = {};
      }
      report_topic_.clear();
      request_topic_.clear();
      client_id_.clear();
      set_waiting_snapshot(connection);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!network_ready_.load()) {
      stop_client();
      {
        std::lock_guard<std::mutex> lock(config_mutex_);
        active_connection_ = connection;
      }

      LocalPrinterRuntimeState waiting = runtime_state_copy();
      waiting.connection = PrinterConnectionState::kConnecting;
      waiting.lifecycle = PrintLifecycleState::kUnknown;
      copy_text(&waiting.raw_status, "");
      copy_text(&waiting.raw_stage, "");
      copy_text(&waiting.stage, "wifi");
      copy_text(&waiting.detail, "Waiting for Wi-Fi IP");
      waiting.has_error = false;
      waiting.non_error_stop = false;
      waiting.show_stop_banner = false;
      copy_text(&waiting.resolved_serial, connection.serial);
      update_local_runtime_metadata(&waiting, true, false);
      store_runtime_state(std::move(waiting), false);
      publish_runtime_snapshot();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (client_ == nullptr) {
      {
        std::lock_guard<std::mutex> lock(config_mutex_);
        active_connection_ = connection;
      }
      report_topic_ = "device/" + connection.serial + "/report";
      request_topic_ = "device/" + connection.serial + "/request";
      client_id_ = make_client_id();

      LocalPrinterRuntimeState runtime = runtime_state_copy();
      runtime.connection = PrinterConnectionState::kConnecting;
      runtime.lifecycle = PrintLifecycleState::kUnknown;
      copy_text(&runtime.raw_status, "");
      copy_text(&runtime.raw_stage, "");
      copy_text(&runtime.stage, "mqtt");
      copy_text(&runtime.detail, "Connecting to local Bambu MQTT");
      copy_text(&runtime.job_name, "");
      copy_text(&runtime.gcode_file, "");
      copy_text(&runtime.resolved_serial, connection.serial);
      runtime.print_error_code = 0;
      runtime.hms_codes.clear();
      runtime.hms_alert_count = 0;
      runtime.has_error = false;
      runtime.warn_hms = false;
      runtime.print_active = false;
      runtime.non_error_stop = false;
      runtime.show_stop_banner = false;
      update_local_runtime_metadata(&runtime, true, false);
      store_runtime_state(std::move(runtime), false);
      publish_runtime_snapshot();

      ESP_LOGI(kTag, "Connecting to printer MQTT %s:%u (serial=%s, user=%s)",
               connection.host.c_str(), static_cast<unsigned int>(connection.mqtt_port),
               connection.serial.c_str(), connection.mqtt_username.c_str());

      // TCP probe: quick reachability check before expensive TLS+MQTT handshake
      {
        int probe_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe_sock >= 0) {
          struct sockaddr_in dest = {};
          dest.sin_family = AF_INET;
          dest.sin_port = htons(connection.mqtt_port);
          inet_aton(connection.host.c_str(), &dest.sin_addr);

          int flags = fcntl(probe_sock, F_GETFL, 0);
          fcntl(probe_sock, F_SETFL, flags | O_NONBLOCK);

          int rc = connect(probe_sock, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
          if (rc < 0 && errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(probe_sock, &wset);
            struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
            int sel = select(probe_sock + 1, nullptr, &wset, nullptr, &tv);
            if (sel > 0) {
              int sock_err = 0;
              socklen_t optlen = sizeof(sock_err);
              getsockopt(probe_sock, SOL_SOCKET, SO_ERROR, &sock_err, &optlen);
              if (sock_err != 0) {
                rc = -1;
                errno = sock_err;
              } else {
                rc = 0;
              }
            } else {
              rc = -1;
              errno = ETIMEDOUT;
            }
          }

          const int probe_errno = errno;
          close(probe_sock);

          if (rc < 0) {
            LocalPrinterRuntimeState probe_fail = runtime_state_copy();
            probe_fail.connection = PrinterConnectionState::kError;
            probe_fail.lifecycle = PrintLifecycleState::kError;
            copy_text(&probe_fail.raw_status, "");
            copy_text(&probe_fail.raw_stage, "");
            probe_fail.has_error = true;
            probe_fail.non_error_stop = false;
            probe_fail.show_stop_banner = false;
            copy_text(&probe_fail.resolved_serial, connection.serial);

            if (probe_errno == ECONNREFUSED) {
              copy_text(&probe_fail.stage, "mqtt-port-closed");
              copy_text(&probe_fail.detail,
                        "Printer reachable but MQTT port closed – is it powered on?");
              ESP_LOGW(kTag, "TCP probe: port %u refused on %s",
                       static_cast<unsigned>(connection.mqtt_port),
                       connection.host.c_str());
            } else {
              copy_text(&probe_fail.stage, "printer-offline");
              copy_text(&probe_fail.detail,
                        std::string("Printer not responding at ") + connection.host);
              ESP_LOGW(kTag, "TCP probe: host %s unreachable (errno=%d)",
                       connection.host.c_str(), probe_errno);
            }

            update_local_runtime_metadata(&probe_fail, true, false);
            store_runtime_state(std::move(probe_fail), false);
            publish_runtime_snapshot();
            ++consecutive_probe_failures_;
            const uint32_t backoff_ms =
                consecutive_probe_failures_ <= 1 ? 1500U :
                consecutive_probe_failures_ <= 2 ? 3000U :
                consecutive_probe_failures_ <= 3 ? 5000U :
                consecutive_probe_failures_ <= 5 ? 10000U : 20000U;
            schedule_client_rebuild("tcp probe failed", backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
          }
          ESP_LOGI(kTag, "TCP probe: port %u reachable on %s",
                   static_cast<unsigned>(connection.mqtt_port),
                   connection.host.c_str());
          consecutive_probe_failures_ = 0;
        }
      }

      esp_mqtt_client_config_t mqtt_cfg = {};
      mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
      const std::string& local_ca_bundle = local_bambu_ca_bundle();
      if (!local_ca_bundle.empty()) {
        mqtt_cfg.broker.verification.certificate = local_ca_bundle.c_str();
        mqtt_cfg.broker.verification.certificate_len = local_ca_bundle.size() + 1U;
        mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
        ESP_LOGI(kTag,
                 "Using embedded local Bambu CA bundle for MQTT TLS verification "
                 "(hostname check disabled)");
      } else {
        mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
        ESP_LOGW(kTag,
                 "Embedded local Bambu CA bundle is empty; falling back to insecure local MQTT TLS");
      }
      mqtt_cfg.credentials.client_id = client_id_.c_str();
      mqtt_cfg.session.keepalive = 30;
      mqtt_cfg.session.disable_clean_session = false;
      mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
      mqtt_cfg.task.stack_size = 10240;
      mqtt_cfg.network.timeout_ms = 20000;
      mqtt_cfg.network.reconnect_timeout_ms = 5000;
      {
        std::lock_guard<std::mutex> lock(config_mutex_);
        mqtt_cfg.broker.address.hostname = active_connection_.host.c_str();
        mqtt_cfg.broker.address.port = active_connection_.mqtt_port;
        mqtt_cfg.credentials.username = active_connection_.mqtt_username.c_str();
        mqtt_cfg.credentials.authentication.password = active_connection_.access_code.c_str();
      }

      client_ = esp_mqtt_client_init(&mqtt_cfg);
      if (client_ == nullptr) {
        LocalPrinterRuntimeState failed = runtime_state_copy();
        failed.connection = PrinterConnectionState::kError;
        failed.lifecycle = PrintLifecycleState::kError;
        copy_text(&failed.raw_status, "");
        copy_text(&failed.raw_stage, "");
        copy_text(&failed.stage, "mqtt-init");
        copy_text(&failed.detail, "Failed to create MQTT client");
        failed.has_error = true;
        failed.non_error_stop = false;
        failed.show_stop_banner = false;
        copy_text(&failed.resolved_serial, connection.serial);
        update_local_runtime_metadata(&failed, true, false);
        store_runtime_state(std::move(failed), false);
        publish_runtime_snapshot();
        vTaskDelay(pdMS_TO_TICKS(1500));
        continue;
      }

      esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, &PrinterClient::mqtt_event_handler, this);
      if (esp_mqtt_client_start(client_) != ESP_OK) {
        LocalPrinterRuntimeState failed = runtime_state_copy();
        failed.connection = PrinterConnectionState::kError;
        failed.lifecycle = PrintLifecycleState::kError;
        copy_text(&failed.raw_status, "");
        copy_text(&failed.raw_stage, "");
        copy_text(&failed.stage, "mqtt-start");
        copy_text(&failed.detail, "Failed to start MQTT client");
        failed.has_error = true;
        failed.non_error_stop = false;
        failed.show_stop_banner = false;
        copy_text(&failed.resolved_serial, connection.serial);
        update_local_runtime_metadata(&failed, true, false);
        store_runtime_state(std::move(failed), false);
        publish_runtime_snapshot();
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        vTaskDelay(pdMS_TO_TICKS(1500));
        continue;
      }

      client_started_ = true;
      connection_state_tick_ = xTaskGetTickCount();
      last_message_tick_ = xTaskGetTickCount();
      now = xTaskGetTickCount();
    }

    if (mqtt_connected_ && subscription_acknowledged_ && initial_sync_sent_ && !received_payload_ &&
        !delayed_pushall_sent_) {
      const uint32_t initial_sync_tick = initial_sync_tick_.load();
      if (tick_elapsed(initial_sync_tick, now, pdMS_TO_TICKS(kDelayedPushallMs))) {
        ESP_LOGW(kTag, "No status payload received after subscribe, sending delayed pushall");
        publish_request(kPushAll);
        delayed_pushall_sent_ = true;
      }
    }

    if (mqtt_connected_ && subscription_acknowledged_ && initial_sync_sent_ && !received_payload_ &&
        delayed_pushall_sent_) {
      const uint32_t initial_sync_tick = initial_sync_tick_.load();
      if (tick_elapsed(initial_sync_tick, now, pdMS_TO_TICKS(kInitialSyncTimeoutMs))) {
        ESP_LOGW(kTag, "Still no status payload after delayed pushall, forcing reconnect");
        schedule_client_rebuild("initial sync timeout");
      }
    }

    if (mqtt_connected_ && subscription_acknowledged_ && received_payload_) {
      const uint32_t last = last_message_tick_.load();
      if (tick_elapsed(last, now, pdMS_TO_TICKS(kNoDataProbeMs))) {
        const uint32_t probe_tick = watchdog_probe_tick_.load();
        if (probe_tick == 0) {
          ESP_LOGW(kTag, "No MQTT data for 60s, sending keepalive start request");
          publish_request(kStartPush);
          watchdog_probe_tick_ = now;
        } else if (tick_elapsed(probe_tick, now, pdMS_TO_TICKS(kNoDataReconnectMs))) {
          ESP_LOGW(kTag, "Still no MQTT data after keepalive probe, forcing reconnect");
          schedule_client_rebuild("no data watchdog");
        }
      } else {
        watchdog_probe_tick_ = 0;
      }
    }

    if (client_ != nullptr && !mqtt_connected_) {
      const uint32_t state_tick = connection_state_tick_.load();
      if (tick_elapsed(state_tick, now, pdMS_TO_TICKS(kDisconnectedStallMs))) {
        ESP_LOGW(kTag, "MQTT client stayed disconnected too long, forcing rebuild");
        schedule_client_rebuild("disconnected stall");
      }
    }

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
  }
}

void PrinterClient::set_waiting_snapshot(const PrinterConnection& connection) {
  LocalPrinterRuntimeState runtime;
  if (!connection.is_ready()) {
    runtime.connection = PrinterConnectionState::kWaitingForCredentials;
    runtime.lifecycle = PrintLifecycleState::kUnknown;
    copy_text(&runtime.stage, "setup");
    copy_text(&runtime.detail, "Need printer host, serial and access code");
  } else {
    runtime.connection = PrinterConnectionState::kReadyForLanConnect;
    runtime.lifecycle = PrintLifecycleState::kIdle;
    copy_text(&runtime.stage, "ready");
    copy_text(&runtime.detail, "Printer credentials loaded");
  }
  copy_text(&runtime.resolved_serial, connection.serial);
  update_local_runtime_metadata(&runtime, connection.is_ready(), false);
  store_runtime_state(std::move(runtime), false);
  publish_runtime_snapshot();
}

bool PrinterClient::publish_request(const char* payload) {
  if (!mqtt_connected_ || client_ == nullptr || payload == nullptr) {
    return false;
  }

  const int msg_id = esp_mqtt_client_publish(client_, request_topic_.c_str(), payload, 0, 1, 0);
  if (msg_id < 0) {
    ESP_LOGW(kTag, "Failed to publish to %s", request_topic_.c_str());
    return false;
  }
  return true;
}

void PrinterClient::request_initial_sync() {
  publish_request(kGetVersion);
  publish_request(kStartPush);
}

PrintLifecycleState PrinterClient::lifecycle_from_state(const std::string& gcode_state,
                                                        bool has_concrete_error) {
  const PrintLifecycleState lifecycle = lifecycle_from_bambu_status(gcode_state, has_concrete_error);
  if (gcode_state == "FAILED" && !has_concrete_error) {
    return PrintLifecycleState::kIdle;
  }
  return lifecycle;
}

std::string PrinterClient::stage_label_for(const std::string& gcode_state, int stage_id,
                                           bool has_concrete_error) {
  if (const std::string stage_label = bambu_stage_label_from_id(stage_id); !stage_label.empty()) {
    return stage_label;
  }
  const std::string fallback = bambu_default_stage_label_for_status(gcode_state, has_concrete_error);
  if (fallback != "Status") {
    return fallback;
  }
  if (stage_id >= 0) {
    char buffer[24] = {};
    std::snprintf(buffer, sizeof(buffer), "Stage %d", stage_id);
    return buffer;
  }
  return "Status";
}

std::string PrinterClient::error_detail_for(int print_error_code, const std::vector<uint64_t>& hms_codes,
                                            int hms_count, PrinterModel model) {
  return format_resolved_error_detail(print_error_code, hms_codes, hms_count, model);
}

std::string PrinterClient::trim_job_name(const std::string& name) {
  if (name.empty()) {
    return {};
  }

  std::string trimmed = name;
  const size_t slash = trimmed.find_last_of("/\\");
  if (slash != std::string::npos) {
    trimmed = trimmed.substr(slash + 1);
  }

  const char* suffixes[] = {".gcode.3mf", ".3mf", ".gcode"};
  for (const char* suffix : suffixes) {
    const size_t suffix_len = std::strlen(suffix);
    if (trimmed.size() >= suffix_len &&
        trimmed.compare(trimmed.size() - suffix_len, suffix_len, suffix) == 0) {
      trimmed.resize(trimmed.size() - suffix_len);
      break;
    }
  }

  return trimmed;
}

std::string PrinterClient::preview_hint_for(const std::string& gcode_file) {
  if (gcode_file.empty()) {
    return {};
  }

  std::string hint = gcode_file;
  std::replace(hint.begin(), hint.end(), '\\', '/');

  const char* suffixes[] = {".gcode.3mf", ".3mf", ".gcode"};
  for (const char* suffix : suffixes) {
    const size_t suffix_len = std::strlen(suffix);
    if (hint.size() >= suffix_len &&
        hint.compare(hint.size() - suffix_len, suffix_len, suffix) == 0) {
      hint.resize(hint.size() - suffix_len);
      hint += ".png";
      return hint;
    }
  }

  const size_t dot = hint.find_last_of('.');
  if (dot != std::string::npos) {
    hint.resize(dot);
  }
  hint += ".png";
  return hint;
}

float PrinterClient::json_number(const cJSON* object, const char* key, float fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    return static_cast<float>(item->valuedouble);
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return static_cast<float>(std::atof(item->valuestring));
  }

  return fallback;
}

int PrinterClient::json_int(const cJSON* object, const char* key, int fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(item)) {
    return item->valueint;
  }
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    int value = fallback;
    return parse_int_text_local(item->valuestring, &value) ? value : fallback;
  }

  return fallback;
}

std::string PrinterClient::json_string(const cJSON* object, const char* key,
                                       const std::string& fallback) {
  if (object == nullptr || key == nullptr) {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return fallback;
  }

  return item->valuestring;
}

}  // namespace printsphere
