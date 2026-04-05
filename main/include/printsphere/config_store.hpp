#pragma once

#include <cstdint>
#include <string>

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
  PrinterConnection load_printer_config() const;
  ArcColorScheme load_arc_color_scheme() const;

  esp_err_t save_wifi_credentials(const WifiCredentials& credentials) const;
  esp_err_t save_cloud_credentials(const BambuCloudCredentials& credentials) const;
  esp_err_t save_cloud_access_token(const std::string& token) const;
  esp_err_t clear_cloud_access_token() const;
  esp_err_t save_source_mode(SourceMode mode) const;
  esp_err_t save_display_rotation(DisplayRotation rotation) const;
  esp_err_t save_printer_config(const PrinterConnection& connection) const;
  esp_err_t save_arc_color_scheme(const ArcColorScheme& colors) const;

 private:
  esp_err_t save_string(const char* key, const std::string& value) const;
  std::string load_string(const char* key) const;
};

}  // namespace printsphere
