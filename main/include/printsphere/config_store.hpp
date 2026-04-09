#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace printsphere {

enum class SourceMode : uint8_t {
  kCloudOnly,
  kLocalOnly,
  kHybrid,
};

enum class CloudRegion : uint8_t {
  kUS,
  kEU,
  kCN,
};

enum class DisplayRotation : uint8_t {
  k0,
  k90,
  k180,
  k270,
};

const char* to_string(SourceMode mode);
SourceMode parse_source_mode(const std::string& value);
const char* to_string(CloudRegion region);
CloudRegion parse_cloud_region(const std::string& value);
const char* to_string(DisplayRotation rotation);
DisplayRotation parse_display_rotation(const std::string& value);

struct WifiCredentials {
  std::string ssid;
  std::string password;

  bool is_configured() const { return !ssid.empty(); }
};

struct BambuCloudCredentials {
  std::string email;
  std::string password;
  CloudRegion region = CloudRegion::kEU;

  bool has_identity() const { return !email.empty(); }
  bool can_password_login() const { return has_identity() && !password.empty(); }
  bool is_configured() const { return can_password_login(); }
};

struct PrinterConnection {
  std::string host;
  std::string serial;
  std::string access_code;
  std::string mqtt_username = "bblp";
  uint16_t mqtt_port = 8883;

  bool is_ready() const {
    return !host.empty() && !serial.empty() && !access_code.empty();
  }
};

struct PrinterProfile {
  uint8_t index = 0;
  std::string serial;
  std::string host;
  std::string access_code;
  std::string display_name;
  std::string model;
  bool cloud_bound = false;
  bool has_local_config() const { return !host.empty() && !access_code.empty(); }
  PrinterConnection to_connection() const {
    PrinterConnection c;
    c.host = host;
    c.serial = serial;
    c.access_code = access_code;
    return c;
  }
};

static constexpr uint8_t kMaxPrinterProfiles = 16;

struct BatteryDisplayPolicy {
  bool dim_enabled = true;
  int dim_brightness_percent = 0;
  bool screen_off_enabled = true;
  uint32_t dim_timeout_idle_s = 20;
  uint32_t dim_timeout_active_s = 30;
  uint32_t off_timeout_idle_s = 60;
  uint32_t off_timeout_active_s = 120;
};

struct ArcColorScheme {
  uint32_t printing = 0x00FF00;
  uint32_t done = 0x00FFFF;
  uint32_t error = 0xFF3333;
  uint32_t idle = 0x666666;
  uint32_t preheat = 0xFFA500;
  uint32_t clean = 0xFFFFFF;
  uint32_t level = 0xFFA500;
  uint32_t cool = 0x3399FF;
  uint32_t idle_active = 0x00FF00;
  uint32_t filament = 0xFFD54F;
  uint32_t setup = 0xF0A64B;
  uint32_t offline = 0xFFFFFF;
  uint32_t unknown = 0x888888;
};

class ConfigStore {
 public:
  esp_err_t initialize();

  std::string load_device_name() const;
  WifiCredentials load_wifi_credentials() const;
  BambuCloudCredentials load_cloud_credentials() const;
  std::string load_cloud_access_token() const;
  SourceMode load_source_mode() const;
  DisplayRotation load_display_rotation() const;
  bool load_portal_lock_enabled() const;
  ArcColorScheme load_arc_color_scheme() const;
  BatteryDisplayPolicy load_battery_display_policy() const;

  esp_err_t save_wifi_credentials(const WifiCredentials& credentials) const;
  esp_err_t save_cloud_credentials(const BambuCloudCredentials& credentials) const;
  esp_err_t save_cloud_access_token(const std::string& token) const;
  esp_err_t clear_cloud_access_token() const;
  esp_err_t save_source_mode(SourceMode mode) const;
  esp_err_t save_display_rotation(DisplayRotation rotation) const;
  esp_err_t save_portal_lock_enabled(bool enabled) const;
  std::vector<PrinterProfile> load_printer_profiles() const;
  esp_err_t save_printer_profile(const PrinterProfile& profile) const;
  esp_err_t delete_printer_profile(uint8_t index) const;
  uint8_t load_active_printer_index() const;
  esp_err_t save_active_printer_index(uint8_t index) const;
  PrinterProfile load_active_printer_profile() const;
  uint8_t load_printer_profile_count() const;

  esp_err_t save_arc_color_scheme(const ArcColorScheme& colors) const;
  esp_err_t save_battery_display_policy(const BatteryDisplayPolicy& policy) const;

 private:
  esp_err_t save_string(const char* key, const std::string& value) const;
  std::string load_string(const char* key) const;
};

}  // namespace printsphere
