#include "printsphere/config_store.hpp"

#include <cctype>
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

bool parse_bool_or_default(const std::string& value, bool fallback) {
  if (value.empty()) {
    return fallback;
  }

  std::string normalized = value;
  for (char& ch : normalized) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  if (normalized == "1" || normalized == "true" || normalized == "on" ||
      normalized == "enabled") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "off" ||
      normalized == "disabled") {
    return false;
  }
  return fallback;
}
}

const char* to_string(SourceMode mode) {
  switch (mode) {
    case SourceMode::kCloudOnly:
      return "cloud_only";
    case SourceMode::kLocalOnly:
      return "local_only";
    case SourceMode::kHybrid:
    default:
      return "hybrid";
  }
}

SourceMode parse_source_mode(const std::string& value) {
  if (value == "cloud_only") {
    return SourceMode::kCloudOnly;
  }

  if (value == "local_only") {
    return SourceMode::kLocalOnly;
  }

  return SourceMode::kHybrid;
}

const char* to_string(CloudRegion region) {
  switch (region) {
    case CloudRegion::kUS:
      return "us";
    case CloudRegion::kCN:
      return "cn";
    case CloudRegion::kEU:
    default:
      return "eu";
  }
}

CloudRegion parse_cloud_region(const std::string& value) {
  if (value == "us") {
    return CloudRegion::kUS;
  }
  if (value == "cn") {
    return CloudRegion::kCN;
  }
  return CloudRegion::kEU;
}

const char* to_string(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
      return "90";
    case DisplayRotation::k180:
      return "180";
    case DisplayRotation::k270:
      return "270";
    case DisplayRotation::k0:
    default:
      return "0";
  }
}

DisplayRotation parse_display_rotation(const std::string& value) {
  if (value == "90") {
    return DisplayRotation::k90;
  }
  if (value == "180") {
    return DisplayRotation::k180;
  }
  if (value == "270") {
    return DisplayRotation::k270;
  }
  return DisplayRotation::k0;
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
  credentials.region = parse_cloud_region(load_string("cloud_region"));
  return credentials;
}

std::string ConfigStore::load_cloud_access_token() const {
  return load_string("cloud_token");
}

SourceMode ConfigStore::load_source_mode() const {
  std::string value = load_string("source_mode");
  if (value.empty()) {
    value = load_string("state_source");
  }
  return parse_source_mode(value);
}

DisplayRotation ConfigStore::load_display_rotation() const {
  return parse_display_rotation(load_string("display_rot"));
}

bool ConfigStore::load_portal_lock_enabled() const {
  return parse_bool_or_default(load_string("portal_lock"), true);
}

bool ConfigStore::load_filament_wake_enabled() const {
  return parse_bool_or_default(load_string("fil_wake"), false);
}

bool ConfigStore::load_filament_anim_enabled() const {
  return parse_bool_or_default(load_string("fil_anim"), true);
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

BatteryDisplayPolicy ConfigStore::load_battery_display_policy() const {
  BatteryDisplayPolicy policy;
  policy.dim_enabled = parse_bool_or_default(load_string("bat_dim"), true);
  policy.screen_off_enabled = parse_bool_or_default(load_string("bat_off"), true);

  const std::string pct_str = load_string("bat_dim_pct");
  if (!pct_str.empty()) {
    const long parsed = std::strtol(pct_str.c_str(), nullptr, 10);
    if (parsed >= 0 && parsed <= 100) {
      policy.dim_brightness_percent = static_cast<int>(parsed);
    }
  }

  const auto load_timeout = [&](const char* key, uint32_t default_s) -> uint32_t {
    const std::string s = load_string(key);
    if (!s.empty()) {
      const long v = std::strtol(s.c_str(), nullptr, 10);
      if (v > 0 && v <= 3600) return static_cast<uint32_t>(v);
    }
    return default_s;
  };
  policy.dim_timeout_idle_s   = load_timeout("bat_dim_idle", 20);
  policy.dim_timeout_active_s = load_timeout("bat_dim_act",  30);
  policy.off_timeout_idle_s   = load_timeout("bat_off_idle", 60);
  policy.off_timeout_active_s = load_timeout("bat_off_act",  120);
  policy.usb_power_save_enabled = parse_bool_or_default(load_string("bat_usb_ps"), false);

  return policy;
}

esp_err_t ConfigStore::save_wifi_credentials(const WifiCredentials& credentials) const {
  ESP_RETURN_ON_ERROR(save_string("wifi_ssid", credentials.ssid), kTag, "save ssid failed");
  return save_string("wifi_pass", credentials.password);
}

esp_err_t ConfigStore::save_cloud_credentials(const BambuCloudCredentials& credentials) const {
  const BambuCloudCredentials previous = load_cloud_credentials();
  ESP_RETURN_ON_ERROR(save_string("cloud_email", credentials.email), kTag, "save cloud email failed");
  ESP_RETURN_ON_ERROR(save_string("cloud_pass", credentials.password), kTag, "save cloud password failed");
  ESP_RETURN_ON_ERROR(save_string("cloud_region", to_string(credentials.region)), kTag,
                      "save cloud region failed");
  const bool email_changed = previous.email != credentials.email;
  const bool password_changed =
      !credentials.password.empty() && previous.password != credentials.password;
  const bool region_changed = previous.region != credentials.region;
  if (!email_changed && !password_changed && !region_changed) {
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

esp_err_t ConfigStore::save_source_mode(SourceMode mode) const {
  ESP_RETURN_ON_ERROR(save_string("source_mode", to_string(mode)), kTag, "save source mode failed");
  return save_string("state_source", "");
}

esp_err_t ConfigStore::save_display_rotation(DisplayRotation rotation) const {
  return save_string("display_rot", to_string(rotation));
}

esp_err_t ConfigStore::save_portal_lock_enabled(bool enabled) const {
  return save_string("portal_lock", enabled ? "1" : "0");
}

esp_err_t ConfigStore::save_filament_wake_enabled(bool enabled) const {
  return save_string("fil_wake", enabled ? "1" : "0");
}

esp_err_t ConfigStore::save_filament_anim_enabled(bool enabled) const {
  return save_string("fil_anim", enabled ? "1" : "0");
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

esp_err_t ConfigStore::save_battery_display_policy(const BatteryDisplayPolicy& policy) const {
  ESP_RETURN_ON_ERROR(save_string("bat_dim", policy.dim_enabled ? "1" : "0"), kTag,
                      "save bat_dim failed");
  ESP_RETURN_ON_ERROR(save_string("bat_dim_pct", std::to_string(policy.dim_brightness_percent)),
                      kTag, "save bat_dim_pct failed");
  ESP_RETURN_ON_ERROR(save_string("bat_off", policy.screen_off_enabled ? "1" : "0"), kTag,
                      "save bat_off failed");
  ESP_RETURN_ON_ERROR(save_string("bat_dim_idle", std::to_string(policy.dim_timeout_idle_s)),
                      kTag, "save bat_dim_idle failed");
  ESP_RETURN_ON_ERROR(save_string("bat_dim_act",  std::to_string(policy.dim_timeout_active_s)),
                      kTag, "save bat_dim_act failed");
  ESP_RETURN_ON_ERROR(save_string("bat_off_idle", std::to_string(policy.off_timeout_idle_s)),
                      kTag, "save bat_off_idle failed");
  ESP_RETURN_ON_ERROR(save_string("bat_off_act", std::to_string(policy.off_timeout_active_s)),
                      kTag, "save bat_off_act failed");
  return save_string("bat_usb_ps", policy.usb_power_save_enabled ? "1" : "0");
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

// ---------------------------------------------------------------------------
// Multi-printer profile support
// ---------------------------------------------------------------------------

namespace {
std::string profile_key(uint8_t index, const char* suffix) {
  char key[16] = {};
  std::snprintf(key, sizeof(key), "prn_%u_%s", static_cast<unsigned>(index), suffix);
  return key;
}
}  // namespace

uint8_t ConfigStore::load_printer_profile_count() const {
  const std::string count_str = load_string("prn_count");
  if (count_str.empty()) return 0;
  const long parsed = std::strtol(count_str.c_str(), nullptr, 10);
  return (parsed >= 0 && parsed <= kMaxPrinterProfiles) ? static_cast<uint8_t>(parsed) : 0;
}

uint8_t ConfigStore::load_active_printer_index() const {
  const std::string idx_str = load_string("prn_active");
  if (idx_str.empty()) return 0;
  const long parsed = std::strtol(idx_str.c_str(), nullptr, 10);
  return (parsed >= 0 && parsed < kMaxPrinterProfiles) ? static_cast<uint8_t>(parsed) : 0;
}

esp_err_t ConfigStore::save_active_printer_index(uint8_t index) const {
  return save_string("prn_active", std::to_string(index));
}

std::vector<PrinterProfile> ConfigStore::load_printer_profiles() const {
  const uint8_t count = load_printer_profile_count();
  std::vector<PrinterProfile> profiles;
  profiles.reserve(count);
  for (uint8_t i = 0; i < count; ++i) {
    PrinterProfile p;
    p.index = i;
    p.serial = load_string(profile_key(i, "ser").c_str());
    p.host = load_string(profile_key(i, "host").c_str());
    p.access_code = load_string(profile_key(i, "acc").c_str());
    p.display_name = load_string(profile_key(i, "name").c_str());
    p.model = load_string(profile_key(i, "mdl").c_str());
    p.cloud_bound = load_string(profile_key(i, "cld").c_str()) == "1";
    if (!p.serial.empty() || !p.host.empty()) {
      profiles.push_back(std::move(p));
    }
  }
  return profiles;
}

esp_err_t ConfigStore::save_printer_profile(const PrinterProfile& profile) const {
  const uint8_t idx = profile.index;
  if (idx >= kMaxPrinterProfiles) return ESP_ERR_INVALID_ARG;

  ESP_RETURN_ON_ERROR(save_string(profile_key(idx, "ser").c_str(), profile.serial), kTag,
                      "save profile serial");
  ESP_RETURN_ON_ERROR(save_string(profile_key(idx, "host").c_str(), profile.host), kTag,
                      "save profile host");
  ESP_RETURN_ON_ERROR(save_string(profile_key(idx, "acc").c_str(), profile.access_code), kTag,
                      "save profile access");
  ESP_RETURN_ON_ERROR(save_string(profile_key(idx, "name").c_str(), profile.display_name), kTag,
                      "save profile name");
  ESP_RETURN_ON_ERROR(save_string(profile_key(idx, "mdl").c_str(), profile.model), kTag,
                      "save profile model");
  ESP_RETURN_ON_ERROR(save_string(profile_key(idx, "cld").c_str(), profile.cloud_bound ? "1" : "0"), kTag,
                      "save profile cloud_bound");

  const uint8_t count = load_printer_profile_count();
  if (idx >= count) {
    ESP_RETURN_ON_ERROR(save_string("prn_count", std::to_string(idx + 1)), kTag,
                        "save profile count");
  }
  return ESP_OK;
}

esp_err_t ConfigStore::delete_printer_profile(uint8_t index) const {
  if (index >= kMaxPrinterProfiles) return ESP_ERR_INVALID_ARG;
  const uint8_t count = load_printer_profile_count();
  if (index >= count) return ESP_ERR_NOT_FOUND;

  // Shift all profiles above index down by one
  for (uint8_t i = index; i + 1 < count; ++i) {
    PrinterProfile next;
    next.index = i;
    next.serial = load_string(profile_key(i + 1, "ser").c_str());
    next.host = load_string(profile_key(i + 1, "host").c_str());
    next.access_code = load_string(profile_key(i + 1, "acc").c_str());
    next.display_name = load_string(profile_key(i + 1, "name").c_str());
    next.model = load_string(profile_key(i + 1, "mdl").c_str());
    next.cloud_bound = load_string(profile_key(i + 1, "cld").c_str()) == "1";
    save_printer_profile(next);
  }
  // Clear the last slot
  const uint8_t last = count - 1;
  save_string(profile_key(last, "ser").c_str(), "");
  save_string(profile_key(last, "host").c_str(), "");
  save_string(profile_key(last, "acc").c_str(), "");
  save_string(profile_key(last, "name").c_str(), "");
  save_string(profile_key(last, "mdl").c_str(), "");
  save_string(profile_key(last, "cld").c_str(), "");
  save_string("prn_count", std::to_string(count - 1));

  // Adjust active index if needed
  const uint8_t active = load_active_printer_index();
  if (active == index) {
    save_active_printer_index(0);
  } else if (active > index && active > 0) {
    save_active_printer_index(active - 1);
  }
  return ESP_OK;
}

PrinterProfile ConfigStore::load_active_printer_profile() const {
  const uint8_t active = load_active_printer_index();
  const auto profiles = load_printer_profiles();
  for (const auto& p : profiles) {
    if (p.index == active) return p;
  }
  return PrinterProfile{};
}

}  // namespace printsphere
