#include "printsphere/config_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.cfg";
constexpr char kNamespace[] = "printsphere";
constexpr char kDeviceName[] = "PrintSphere";

std::string color_to_html_hex(uint32_t color) {
  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "#%06X", static_cast<unsigned int>(color & 0xFFFFFFU));
  return buffer;
}

uint32_t parse_color_or_default(const std::string& value, uint32_t fallback) {
  if (value.empty()) {
    return fallback;
  }

  std::string normalized = value;
  if (!normalized.empty() && normalized.front() == '#') {
    normalized.erase(normalized.begin());
  } else if (normalized.size() > 2 && normalized[0] == '0' &&
             (normalized[1] == 'x' || normalized[1] == 'X')) {
    normalized.erase(0, 2);
  }

  if (normalized.size() != 6U) {
    return fallback;
  }

  char* end = nullptr;
  const unsigned long parsed = std::strtoul(normalized.c_str(), &end, 16);
  if (end == nullptr || *end != '\0' || parsed > 0xFFFFFFUL) {
    return fallback;
  }

  return static_cast<uint32_t>(parsed);
}
}

const char* to_string(StatusSourcePreference preference) {
  switch (preference) {
    case StatusSourcePreference::kLocal:
      return "local";
    case StatusSourcePreference::kCloud:
    default:
      return "cloud";
  }
}

StatusSourcePreference parse_status_source_preference(const std::string& value) {
  return value == "local" ? StatusSourcePreference::kLocal : StatusSourcePreference::kCloud;
}

esp_err_t ConfigStore::initialize() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }

  if (err == ESP_OK) {
    ESP_LOGI(kTag, "NVS ready");
  }

  return err;
}

std::string ConfigStore::load_device_name() const { return kDeviceName; }

WifiCredentials ConfigStore::load_wifi_credentials() const {
  WifiCredentials credentials;
  credentials.ssid = load_string("wifi_ssid");
  credentials.password = load_string("wifi_pass");
  return credentials;
}

BambuCloudCredentials ConfigStore::load_cloud_credentials() const {
  BambuCloudCredentials credentials;
  credentials.email = load_string("cloud_email");
  credentials.password = load_string("cloud_pass");
  return credentials;
}

std::string ConfigStore::load_cloud_access_token() const {
  return load_string("cloud_token");
}

StatusSourcePreference ConfigStore::load_status_source_preference() const {
  return parse_status_source_preference(load_string("state_source"));
}

PrinterConnection ConfigStore::load_printer_config() const {
  PrinterConnection connection;
  connection.host = load_string("prn_host");
  connection.serial = load_string("prn_serial");
  connection.access_code = load_string("prn_access");

  const std::string mqtt_port = load_string("prn_port");
  if (!mqtt_port.empty()) {
    const long parsed = std::strtol(mqtt_port.c_str(), nullptr, 10);
    if (parsed > 0 && parsed <= 65535) {
      connection.mqtt_port = static_cast<uint16_t>(parsed);
    }
  }

  if (connection.is_ready()) {
    ESP_LOGI(kTag, "Loaded stored printer config for host %s", connection.host.c_str());
  } else {
    ESP_LOGI(kTag, "Printer config incomplete; staying in setup mode");
  }

  return connection;
}

ArcColorScheme ConfigStore::load_arc_color_scheme() const {
  ArcColorScheme colors;
  colors.printing = parse_color_or_default(load_string("arc_print"), colors.printing);
  colors.done = parse_color_or_default(load_string("arc_done"), colors.done);
  colors.error = parse_color_or_default(load_string("arc_error"), colors.error);
  colors.idle = parse_color_or_default(load_string("arc_idle"), colors.idle);
  colors.preheat = parse_color_or_default(load_string("arc_preheat"), colors.preheat);
  colors.clean = parse_color_or_default(load_string("arc_clean"), colors.clean);
  colors.level = parse_color_or_default(load_string("arc_level"), colors.level);
  colors.cool = parse_color_or_default(load_string("arc_cool"), colors.cool);
  colors.idle_active = parse_color_or_default(load_string("arc_idleact"), colors.idle_active);
  colors.filament = parse_color_or_default(load_string("arc_filament"), colors.filament);
  colors.setup = parse_color_or_default(load_string("arc_setup"), colors.setup);
  colors.offline = parse_color_or_default(load_string("arc_offline"), colors.offline);
  colors.unknown = parse_color_or_default(load_string("arc_unknown"), colors.unknown);
  return colors;
}

esp_err_t ConfigStore::save_wifi_credentials(const WifiCredentials& credentials) const {
  ESP_RETURN_ON_ERROR(save_string("wifi_ssid", credentials.ssid), kTag, "save ssid failed");
  return save_string("wifi_pass", credentials.password);
}

esp_err_t ConfigStore::save_cloud_credentials(const BambuCloudCredentials& credentials) const {
  const BambuCloudCredentials previous = load_cloud_credentials();
  ESP_RETURN_ON_ERROR(save_string("cloud_email", credentials.email), kTag, "save cloud email failed");
  ESP_RETURN_ON_ERROR(save_string("cloud_pass", credentials.password), kTag, "save cloud password failed");
  if (previous.email == credentials.email && previous.password == credentials.password) {
    return ESP_OK;
  }
  return clear_cloud_access_token();
}

esp_err_t ConfigStore::save_cloud_access_token(const std::string& token) const {
  return save_string("cloud_token", token);
}

esp_err_t ConfigStore::clear_cloud_access_token() const {
  return save_string("cloud_token", "");
}

esp_err_t ConfigStore::save_status_source_preference(StatusSourcePreference preference) const {
  return save_string("state_source", to_string(preference));
}

esp_err_t ConfigStore::save_printer_config(const PrinterConnection& connection) const {
  ESP_RETURN_ON_ERROR(save_string("prn_host", connection.host), kTag, "save host failed");
  ESP_RETURN_ON_ERROR(save_string("prn_serial", connection.serial), kTag, "save serial failed");
  ESP_RETURN_ON_ERROR(save_string("prn_access", connection.access_code), kTag, "save access failed");
  return save_string("prn_port", std::to_string(connection.mqtt_port));
}

esp_err_t ConfigStore::save_arc_color_scheme(const ArcColorScheme& colors) const {
  ESP_RETURN_ON_ERROR(save_string("arc_print", color_to_html_hex(colors.printing)), kTag,
                      "save printing color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_done", color_to_html_hex(colors.done)), kTag,
                      "save done color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_error", color_to_html_hex(colors.error)), kTag,
                      "save error color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_idle", color_to_html_hex(colors.idle)), kTag,
                      "save idle color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_preheat", color_to_html_hex(colors.preheat)), kTag,
                      "save preheat color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_clean", color_to_html_hex(colors.clean)), kTag,
                      "save clean color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_level", color_to_html_hex(colors.level)), kTag,
                      "save level color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_cool", color_to_html_hex(colors.cool)), kTag,
                      "save cool color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_idleact", color_to_html_hex(colors.idle_active)), kTag,
                      "save idle active color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_filament", color_to_html_hex(colors.filament)), kTag,
                      "save filament color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_setup", color_to_html_hex(colors.setup)), kTag,
                      "save setup color failed");
  ESP_RETURN_ON_ERROR(save_string("arc_offline", color_to_html_hex(colors.offline)), kTag,
                      "save offline color failed");
  return save_string("arc_unknown", color_to_html_hex(colors.unknown));
}

esp_err_t ConfigStore::save_string(const char* key, const std::string& value) const {
  nvs_handle_t handle = 0;
  ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag, "nvs_open failed");

  const esp_err_t set_err = nvs_set_str(handle, key, value.c_str());
  if (set_err != ESP_OK) {
    nvs_close(handle);
    return set_err;
  }

  const esp_err_t commit_err = nvs_commit(handle);
  nvs_close(handle);
  return commit_err;
}

std::string ConfigStore::load_string(const char* key) const {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return {};
  }

  size_t required = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &required);
  if (err != ESP_OK || required == 0) {
    nvs_close(handle);
    return {};
  }

  std::vector<char> buffer(required, 0);
  err = nvs_get_str(handle, key, buffer.data(), &required);
  nvs_close(handle);
  if (err != ESP_OK) {
    return {};
  }

  return std::string(buffer.data());
}

}  // namespace printsphere
