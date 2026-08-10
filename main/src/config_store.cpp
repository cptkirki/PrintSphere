#include "printsphere/config_store.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
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
    migrate_legacy_printer_profile();
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
  const int cached = cached_source_mode_.load(std::memory_order_acquire);
  if (cached >= static_cast<int>(SourceMode::kCloudOnly) &&
      cached <= static_cast<int>(SourceMode::kHybrid)) {
    return static_cast<SourceMode>(cached);
  }
  std::string value = load_string("source_mode");
  if (value.empty()) {
    value = load_string("state_source");
  }
  const SourceMode mode = parse_source_mode(value);
  cached_source_mode_.store(static_cast<int>(mode), std::memory_order_release);
  return mode;
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
  cached_source_mode_.store(static_cast<int>(mode), std::memory_order_release);
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

esp_err_t set_nvs_string(nvs_handle_t handle, const std::string& key,
                         const std::string& value) {
  return nvs_set_str(handle, key.c_str(), value.c_str());
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
  std::lock_guard<std::mutex> lock(printer_profile_mutex_);
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
  std::lock_guard<std::mutex> lock(printer_profile_mutex_);
  const uint8_t idx = profile.index;
  if (idx >= kMaxPrinterProfiles) return ESP_ERR_INVALID_ARG;
  const uint8_t count = load_printer_profile_count();

  nvs_handle_t handle = 0;
  ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag,
                      "open profile storage");
  esp_err_t err = set_nvs_string(handle, profile_key(idx, "ser"), profile.serial);
  if (err == ESP_OK) err = set_nvs_string(handle, profile_key(idx, "host"), profile.host);
  if (err == ESP_OK) err = set_nvs_string(handle, profile_key(idx, "acc"), profile.access_code);
  if (err == ESP_OK) err = set_nvs_string(handle, profile_key(idx, "name"), profile.display_name);
  if (err == ESP_OK) err = set_nvs_string(handle, profile_key(idx, "mdl"), profile.model);
  if (err == ESP_OK) {
    err = set_nvs_string(handle, profile_key(idx, "cld"), profile.cloud_bound ? "1" : "0");
  }
  if (err == ESP_OK && idx >= count) {
    err = nvs_set_str(handle, "prn_count", std::to_string(idx + 1).c_str());
  }
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

esp_err_t ConfigStore::delete_printer_profile(uint8_t index) const {
  std::lock_guard<std::mutex> lock(printer_profile_mutex_);
  if (index >= kMaxPrinterProfiles) return ESP_ERR_INVALID_ARG;
  const uint8_t count = load_printer_profile_count();
  if (index >= count) return ESP_ERR_NOT_FOUND;

  struct StoredProfileFields {
    std::string serial;
    std::string host;
    std::string access;
    std::string name;
    std::string model;
    std::string cloud;
  };
  std::vector<StoredProfileFields> shifted;
  shifted.reserve(count - index - 1);
  for (uint8_t i = index; i + 1 < count; ++i) {
    StoredProfileFields fields;
    fields.serial = load_string(profile_key(i + 1, "ser").c_str());
    fields.host = load_string(profile_key(i + 1, "host").c_str());
    fields.access = load_string(profile_key(i + 1, "acc").c_str());
    fields.name = load_string(profile_key(i + 1, "name").c_str());
    fields.model = load_string(profile_key(i + 1, "mdl").c_str());
    fields.cloud = load_string(profile_key(i + 1, "cld").c_str());
    shifted.push_back(std::move(fields));
  }

  const uint8_t active = load_active_printer_index();
  const uint8_t next_active = active == index ? 0 : (active > index ? active - 1 : active);
  nvs_handle_t handle = 0;
  ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag,
                      "open profile storage for delete");
  esp_err_t err = ESP_OK;
  for (size_t offset = 0; offset < shifted.size() && err == ESP_OK; ++offset) {
    const uint8_t target = static_cast<uint8_t>(index + offset);
    const StoredProfileFields& fields = shifted[offset];
    err = set_nvs_string(handle, profile_key(target, "ser"), fields.serial);
    if (err == ESP_OK) err = set_nvs_string(handle, profile_key(target, "host"), fields.host);
    if (err == ESP_OK) err = set_nvs_string(handle, profile_key(target, "acc"), fields.access);
    if (err == ESP_OK) err = set_nvs_string(handle, profile_key(target, "name"), fields.name);
    if (err == ESP_OK) err = set_nvs_string(handle, profile_key(target, "mdl"), fields.model);
    if (err == ESP_OK) err = set_nvs_string(handle, profile_key(target, "cld"), fields.cloud);
  }

  const uint8_t last = count - 1;
  for (const char* suffix : {"ser", "host", "acc", "name", "mdl", "cld"}) {
    if (err == ESP_OK) err = set_nvs_string(handle, profile_key(last, suffix), "");
  }
  if (err == ESP_OK) err = nvs_set_str(handle, "prn_count", std::to_string(count - 1).c_str());
  if (err == ESP_OK) err = nvs_set_str(handle, "prn_active", std::to_string(next_active).c_str());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

PrinterProfile ConfigStore::load_active_printer_profile() const {
  const uint8_t active = load_active_printer_index();
  const auto profiles = load_printer_profiles();
  for (const auto& p : profiles) {
    if (p.index == active) return p;
  }
  return PrinterProfile{};
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
