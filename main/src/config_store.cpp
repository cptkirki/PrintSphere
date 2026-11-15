#include "printsphere/config_store.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.cfg";
constexpr char kNamespace[] = "printsphere";
constexpr char kDeviceName[] = "PrintSphere";

std::string record_string(const cJSON* root, const char* key) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
  return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
std::string serialize_record(cJSON* record) {
  if (!record) return {};
  char* text = cJSON_PrintUnformatted(record);
  std::string result = text ? text : "";
  cJSON_free(text);
  cJSON_Delete(record);
  return result;
}

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
    migrate_legacy_printer_profile();
    // Migrate existing devices even when the printer/cloud is currently offline.
    if (!load_portal_provisioned() &&
        (load_cloud_credentials().has_identity() || !load_cloud_access_token().empty() ||
         load_printer_profile_count() > 0)) {
      ESP_RETURN_ON_ERROR(mark_portal_provisioned(), kTag, "persist portal lock failed");
    }
  }

  return err;
}

// One-shot migration: versions ≤ v1.3 stored a single printer connection in
// flat keys ("prn_host", "prn_serial", "prn_access", "prn_port").  v1.4.1+
// uses indexed multi-profile keys ("prn_0_ser", "prn_0_host", ...) plus a
// "prn_count" counter.  Without this migration, users upgrading from v1.3 land
// on the new firmware with prn_count=0 → the Printers page renders empty and
// the main loop never connects to their saved printer.
void ConfigStore::migrate_legacy_printer_profile() {
  // If new-format profiles already exist, nothing to do.
  if (load_printer_profile_count() > 0) return;

  const std::string legacy_host = load_string("prn_host");
  const std::string legacy_serial = load_string("prn_serial");
  const std::string legacy_access = load_string("prn_access");
  if (legacy_host.empty() && legacy_serial.empty() && legacy_access.empty()) {
    return;
  }

  ESP_LOGI(kTag, "Migrating legacy printer profile (serial=%s host=%s) to slot 0",
           legacy_serial.c_str(), legacy_host.c_str());

  PrinterProfile profile;
  profile.index = 0;
  profile.serial = legacy_serial;
  profile.host = legacy_host;
  profile.access_code = legacy_access;
  // display_name / model left empty — will be populated from cloud binding
  // list on first Cloud login.
  save_printer_profile(profile);
  save_active_printer_index(0);

  // Clear the legacy keys so we don't re-migrate and so stale values can't
  // resurrect if the user edits profile 0 later.
  save_string("prn_host", "");
  save_string("prn_serial", "");
  save_string("prn_access", "");
  save_string("prn_port", "");
}

std::string ConfigStore::load_device_name() const { return kDeviceName; }

WifiCredentials ConfigStore::load_wifi_credentials() const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const std::string stored = load_string("wifi_cfg");
  if (!stored.empty()) {
    cJSON* record = cJSON_Parse(stored.c_str());
    WifiCredentials result{record_string(record, "ssid"), record_string(record, "password")};
    cJSON_Delete(record);
    return result;
  }
  return {load_string("wifi_ssid"), load_string("wifi_pass")};
}

BambuCloudCredentials ConfigStore::load_cloud_credentials() const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const std::string stored = load_string("cloud_cfg");
  if (!stored.empty()) {
    cJSON* record = cJSON_Parse(stored.c_str());
    BambuCloudCredentials result{record_string(record, "email"), record_string(record, "password"),
                                parse_cloud_region(record_string(record, "region"))};
    cJSON_Delete(record);
    return result;
  }
  return {load_string("cloud_email"), load_string("cloud_pass"), parse_cloud_region(load_string("cloud_region"))};
}

std::string ConfigStore::load_cloud_access_token() const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const std::string stored = load_string("cloud_sess");
  if (stored.empty()) return load_string("cloud_token");
  cJSON* record = cJSON_Parse(stored.c_str());
  const auto credentials = load_cloud_credentials();
  const bool matches = record_string(record, "email") == credentials.email &&
                       record_string(record, "region") == to_string(credentials.region);
  const std::string token = matches ? record_string(record, "token") : "";
  cJSON_Delete(record);
  return token;
}

int64_t ConfigStore::load_cloud_token_expiry() const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  cJSON* record = cJSON_Parse(load_string("cloud_sess").c_str());
  const cJSON* expiry = cJSON_GetObjectItemCaseSensitive(record, "expiry");
  const int64_t result = cJSON_IsNumber(expiry) ? static_cast<int64_t>(expiry->valuedouble) : 0;
  cJSON_Delete(record);
  return result;
}

bool ConfigStore::load_portal_provisioned() const { return load_string("portal_done") == "1"; }
esp_err_t ConfigStore::mark_portal_provisioned() const { return save_string("portal_done", "1"); }

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

bool ConfigStore::load_audio_enabled() const {
  return parse_bool_or_default(load_string("audio_en"), true);
}

int ConfigStore::load_audio_volume_percent() const {
  const std::string s = load_string("audio_vol");
  if (s.empty()) {
    return 60;
  }
  const long parsed = std::strtol(s.c_str(), nullptr, 10);
  if (parsed < 0) return 0;
  if (parsed > 100) return 100;
  return static_cast<int>(parsed);
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
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  cJSON* record = cJSON_CreateObject();
  if (!record) return ESP_ERR_NO_MEM;
  if (!cJSON_AddStringToObject(record, "ssid", credentials.ssid.c_str()) ||
      !cJSON_AddStringToObject(record, "password", credentials.password.c_str())) {
    cJSON_Delete(record); return ESP_ERR_NO_MEM;
  }
  const std::string json = serialize_record(record);
  return json.empty() ? ESP_ERR_NO_MEM : save_string("wifi_cfg", json);
}

esp_err_t ConfigStore::save_cloud_credentials(const BambuCloudCredentials& credentials) const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const auto previous = load_cloud_credentials();
  const bool identity_changed = previous.email != credentials.email || previous.region != credentials.region;
  const bool password_changed = !credentials.password.empty() && previous.password != credentials.password;
  // Invalidate before publishing another identity. Session records also bind the
  // token to its account/region, so interrupted writes cannot cross accounts.
  if (identity_changed || password_changed) {
    ESP_RETURN_ON_ERROR(clear_cloud_access_token(), kTag, "clear old session failed");
  }
  cJSON* record = cJSON_CreateObject();
  if (!record) return ESP_ERR_NO_MEM;
  if (!cJSON_AddStringToObject(record, "email", credentials.email.c_str()) ||
      !cJSON_AddStringToObject(record, "password", credentials.password.c_str()) ||
      !cJSON_AddStringToObject(record, "region", to_string(credentials.region))) {
    cJSON_Delete(record); return ESP_ERR_NO_MEM;
  }
  const std::string json = serialize_record(record);
  return json.empty() ? ESP_ERR_NO_MEM : save_string("cloud_cfg", json);
}

esp_err_t ConfigStore::save_cloud_access_token(const std::string& token, int64_t expires_at,
                                              const BambuCloudCredentials* owner) const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const auto credentials = load_cloud_credentials();
  if (owner && (owner->email != credentials.email || owner->region != credentials.region))
    return ESP_ERR_INVALID_STATE;
  cJSON* record = cJSON_CreateObject();
  if (!record) return ESP_ERR_NO_MEM;
  if (!cJSON_AddStringToObject(record, "email", credentials.email.c_str()) ||
      !cJSON_AddStringToObject(record, "region", to_string(credentials.region)) ||
      !cJSON_AddStringToObject(record, "token", token.c_str()) ||
      !cJSON_AddNumberToObject(record, "expiry", static_cast<double>(expires_at))) {
    cJSON_Delete(record); return ESP_ERR_NO_MEM;
  }
  const std::string json = serialize_record(record);
  return json.empty() ? ESP_ERR_NO_MEM : save_string("cloud_sess", json);
}

esp_err_t ConfigStore::clear_cloud_access_token(const BambuCloudCredentials* owner) const {
  std::lock_guard<std::recursive_mutex> identity_lock(configuration_mutex_);
  const auto current = load_cloud_credentials();
  if (owner && (owner->email != current.email || owner->region != current.region))
    return ESP_ERR_INVALID_STATE;
  // A valid empty session record prevents fallback to legacy tokens.
  ESP_RETURN_ON_ERROR(save_cloud_access_token(""), kTag, "clear session failed");
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

esp_err_t ConfigStore::save_audio_enabled(bool enabled) const {
  return save_string("audio_en", enabled ? "1" : "0");
}

esp_err_t ConfigStore::save_audio_volume_percent(int volume) const {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  return save_string("audio_vol", std::to_string(volume));
}

std::string ConfigStore::load_timezone_iana() const {
  return load_string("tz_iana");
}

esp_err_t ConfigStore::save_timezone_iana(const std::string& iana_name) const {
  return save_string("tz_iana", iana_name);
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
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  nvs_handle_t handle = 0;
  ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag, "nvs_open failed");

  const esp_err_t set_err = nvs_set_blob(handle, key, value.c_str(), value.size() + 1);
  if (set_err != ESP_OK) {
    nvs_close(handle);
    return set_err;
  }

  const esp_err_t commit_err = nvs_commit(handle);
  nvs_close(handle);
  return commit_err;
}

std::string ConfigStore::load_string(const char* key) const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return {};
  size_t required = 0;
  bool blob = true;
  esp_err_t err = nvs_get_blob(handle, key, nullptr, &required);
  if (err != ESP_OK) { blob = false; err = nvs_get_str(handle, key, nullptr, &required); }
  if (err != ESP_OK || required == 0 || required > 128 * 1024) { nvs_close(handle); return {}; }
  std::vector<char> buffer(required);
  err = blob ? nvs_get_blob(handle, key, buffer.data(), &required)
             : nvs_get_str(handle, key, buffer.data(), &required);
  nvs_close(handle);
  if (err != ESP_OK || buffer.back() != 0) return {};
  return std::string(buffer.data(), buffer.size() - 1);
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
  return static_cast<uint8_t>(load_printer_profiles().size());
}

uint8_t ConfigStore::load_active_printer_index() const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const std::string stored = load_string("prn_cfg");
  if (!stored.empty()) {
    cJSON* root = cJSON_Parse(stored.c_str());
    const cJSON* active = cJSON_GetObjectItemCaseSensitive(root, "active");
    const int index = cJSON_IsNumber(active) ? active->valueint : 0;
    cJSON_Delete(root);
    return index >= 0 && index < kMaxPrinterProfiles ? index : 0;
  }
  const long index = std::strtol(load_string("prn_active").c_str(), nullptr, 10);
  return index >= 0 && index < kMaxPrinterProfiles ? index : 0;
}

esp_err_t ConfigStore::save_profiles_record(const std::vector<PrinterProfile>& profiles,
                                           uint8_t active) const {
  cJSON* root = cJSON_CreateObject();
  if (!root) return ESP_ERR_NO_MEM;
  cJSON* list = cJSON_AddArrayToObject(root, "profiles");
  bool ok = list && cJSON_AddNumberToObject(root, "active", active);
  for (const auto& profile : profiles) {
    cJSON* item = cJSON_CreateObject();
    if (!item) { ok = false; break; }
    if (!cJSON_AddItemToArray(list, item)) { cJSON_Delete(item); ok = false; break; }
    ok = ok && cJSON_AddStringToObject(item, "serial", profile.serial.c_str()) &&
        cJSON_AddStringToObject(item, "host", profile.host.c_str()) &&
        cJSON_AddStringToObject(item, "access", profile.access_code.c_str()) &&
        cJSON_AddStringToObject(item, "name", profile.display_name.c_str()) &&
        cJSON_AddStringToObject(item, "model", profile.model.c_str()) &&
        cJSON_AddBoolToObject(item, "cloud", profile.cloud_bound);
  }
  if (!ok) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
  const std::string json = serialize_record(root);
  return json.empty() ? ESP_ERR_NO_MEM : save_string("prn_cfg", json);
}

esp_err_t ConfigStore::save_active_printer_index(uint8_t index) const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  auto profiles = load_printer_profiles();
  if (index >= profiles.size() && !(index == 0 && profiles.empty())) return ESP_ERR_INVALID_ARG;
  return save_profiles_record(profiles, index);
}

std::vector<PrinterProfile> ConfigStore::load_printer_profiles() const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  std::vector<PrinterProfile> profiles;
  const std::string stored = load_string("prn_cfg");
  if (!stored.empty()) {
    cJSON* root = cJSON_Parse(stored.c_str());
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
      if (profiles.size() >= kMaxPrinterProfiles) break;
      PrinterProfile profile;
      profile.index = static_cast<uint8_t>(profiles.size());
      profile.serial = record_string(item, "serial");
      profile.host = record_string(item, "host");
      profile.access_code = record_string(item, "access");
      profile.display_name = record_string(item, "name");
      profile.model = record_string(item, "model");
      profile.cloud_bound = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "cloud"));
      profiles.push_back(std::move(profile));
    }
    cJSON_Delete(root);
    return profiles;
  }
  const long count = std::strtol(load_string("prn_count").c_str(), nullptr, 10);
  for (uint8_t i = 0; i < count && i < kMaxPrinterProfiles; ++i) {
    PrinterProfile profile;
    profile.index = i;
    profile.serial = load_string(profile_key(i, "ser").c_str());
    profile.host = load_string(profile_key(i, "host").c_str());
    profile.access_code = load_string(profile_key(i, "acc").c_str());
    profile.display_name = load_string(profile_key(i, "name").c_str());
    profile.model = load_string(profile_key(i, "mdl").c_str());
    profile.cloud_bound = load_string(profile_key(i, "cld").c_str()) == "1";
    profiles.push_back(std::move(profile));
  }
  return profiles;
}

esp_err_t ConfigStore::save_printer_profile(const PrinterProfile& profile) const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  auto profiles = load_printer_profiles();
  if (profile.index >= kMaxPrinterProfiles || profile.index > profiles.size()) return ESP_ERR_INVALID_ARG;
  if (profile.index == profiles.size()) profiles.push_back(profile);
  else profiles[profile.index] = profile;
  return save_profiles_record(profiles, load_active_printer_index());
}

esp_err_t ConfigStore::sync_cloud_printer_profiles(
    const std::vector<PrinterProfile>& discovered, const BambuCloudCredentials& owner) const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const auto current = load_cloud_credentials();
  if (!owner.has_identity() || owner.email != current.email || owner.region != current.region)
    return ESP_ERR_INVALID_STATE;

  auto profiles = load_printer_profiles();
  bool changed = false;
  for (const auto& device : discovered) {
    if (device.serial.empty()) continue;
    auto found = std::find_if(profiles.begin(), profiles.end(), [&](const auto& profile) {
      return profile.serial == device.serial;
    });
    if (found == profiles.end()) {
      // Keep the complete merge atomic, including the capacity failure case.
      if (profiles.size() >= kMaxPrinterProfiles) return ESP_ERR_NO_MEM;
      PrinterProfile profile;
      profile.index = static_cast<uint8_t>(profiles.size());
      profile.serial = device.serial;
      profile.display_name = device.display_name;
      profile.model = device.model;
      profile.cloud_bound = true;
      profiles.push_back(std::move(profile));
      changed = true;
      continue;
    }
    // Account discovery must never overwrite local credentials or user names.
    if (!found->cloud_bound) { found->cloud_bound = true; changed = true; }
    if (found->display_name.empty() && !device.display_name.empty()) {
      found->display_name = device.display_name;
      changed = true;
    }
    if (found->model.empty() && !device.model.empty()) {
      found->model = device.model;
      changed = true;
    }
  }
  return changed ? save_profiles_record(profiles, load_active_printer_index()) : ESP_OK;
}

esp_err_t ConfigStore::delete_printer_profile(uint8_t index) const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  auto profiles = load_printer_profiles();
  if (index >= profiles.size()) return ESP_ERR_NOT_FOUND;
  const uint8_t active = load_active_printer_index();
  profiles.erase(profiles.begin() + index);
  for (uint8_t i = 0; i < profiles.size(); ++i) profiles[i].index = i;
  const uint8_t new_active = active == index ? 0 : (active > index ? active - 1 : active);
  return save_profiles_record(profiles, new_active);
}

PrinterProfile ConfigStore::load_active_printer_profile() const {
  std::lock_guard<std::recursive_mutex> lock(configuration_mutex_);
  const auto profiles = load_printer_profiles();
  const uint8_t index = load_active_printer_index();
  return index < profiles.size() ? profiles[index] : PrinterProfile{};
}

// ---------------------------------------------------------------------------
// Per-event audio settings
// ---------------------------------------------------------------------------

namespace {
void event_enable_key(uint8_t idx, char* buf, size_t buf_size) {
  std::snprintf(buf, buf_size, "snd_en_%u", static_cast<unsigned>(idx));
}
// SPIFFS path for raw PCM data (base path mounted at "/sounds").
void event_pcm_path(uint8_t idx, char* buf, size_t buf_size) {
  std::snprintf(buf, buf_size, "/sounds/snd_%u.pcm", static_cast<unsigned>(idx));
}
void event_filename_key(uint8_t idx, char* buf, size_t buf_size) {
  std::snprintf(buf, buf_size, "snd_fn_%u", static_cast<unsigned>(idx));
}
// Core print/HMS events and Click default on; less common optional events off.
bool default_event_enabled(uint8_t idx) { return idx < 5U || idx == 7U; }
}  // namespace

bool ConfigStore::load_audio_event_enabled(uint8_t event_index) const {
  if (event_index >= 8) return true;
  char key[16] = {};
  event_enable_key(event_index, key, sizeof(key));
  const std::string s = load_string(key);
  if (s.empty()) {
    return default_event_enabled(event_index);
  }
  return parse_bool_or_default(s, default_event_enabled(event_index));
}

esp_err_t ConfigStore::save_audio_event_enabled(uint8_t event_index, bool enabled) const {
  if (event_index >= 8) return ESP_ERR_INVALID_ARG;
  char key[16] = {};
  event_enable_key(event_index, key, sizeof(key));
  return save_string(key, enabled ? "1" : "0");
}

bool ConfigStore::has_audio_event_pcm(uint8_t event_index) const {
  if (event_index >= 8) return false;
  char path[32] = {};
  event_pcm_path(event_index, path, sizeof(path));
  struct stat st;
  return (stat(path, &st) == 0 && st.st_size > 0);
}

std::vector<uint8_t> ConfigStore::load_audio_event_pcm(uint8_t event_index) const {
  if (event_index >= 8) return {};
  char path[32] = {};
  event_pcm_path(event_index, path, sizeof(path));

  FILE* f = fopen(path, "rb");
  if (!f) return {};

  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) { fclose(f); return {}; }

  std::vector<uint8_t> data(static_cast<size_t>(size));
  const size_t got = fread(data.data(), 1, data.size(), f);
  fclose(f);
  if (got != data.size()) return {};
  return data;
}

esp_err_t ConfigStore::save_audio_event_pcm(uint8_t event_index, const uint8_t* data,
                                             size_t len) const {
  if (event_index >= 8 || data == nullptr || len == 0) return ESP_ERR_INVALID_ARG;
  char path[32] = {};
  event_pcm_path(event_index, path, sizeof(path));

  FILE* f = fopen(path, "wb");
  if (!f) {
    ESP_LOGE(kTag, "Cannot open %s for writing (errno %d)", path, errno);
    return ESP_FAIL;
  }
  const size_t written = fwrite(data, 1, len, f);
  fclose(f);
  if (written != len) {
    ESP_LOGE(kTag, "Short write to %s (%zu/%zu bytes)", path, written, len);
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t ConfigStore::clear_audio_event_pcm(uint8_t event_index) const {
  if (event_index >= 8) return ESP_ERR_INVALID_ARG;
  char path[32] = {};
  event_pcm_path(event_index, path, sizeof(path));
  const int ret = remove(path);
  return (ret == 0 || errno == ENOENT) ? ESP_OK : ESP_FAIL;
}

std::string ConfigStore::load_audio_event_filename(uint8_t event_index) const {
  if (event_index >= 8) return {};
  char key[16] = {};
  event_filename_key(event_index, key, sizeof(key));
  return load_string(key);  // empty string when not stored
}

esp_err_t ConfigStore::save_audio_event_filename(uint8_t event_index,
                                                  const std::string& name) const {
  if (event_index >= 8) return ESP_ERR_INVALID_ARG;
  char key[16] = {};
  event_filename_key(event_index, key, sizeof(key));
  return save_string(key, name);
}

}  // namespace printsphere
