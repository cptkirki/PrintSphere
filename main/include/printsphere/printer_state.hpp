#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace printsphere {

enum class PrinterConnectionState : uint8_t {
  kBooting,
  kWaitingForCredentials,
  kReadyForLanConnect,
  kConnecting,
  kOnline,
  kError,
};

enum class PrintLifecycleState : uint8_t {
  kUnknown,
  kIdle,
  kPreparing,
  kPrinting,
  kPaused,
  kFinished,
  kError,
};

struct PrinterSnapshot {
  PrinterConnectionState connection = PrinterConnectionState::kBooting;
  PrintLifecycleState lifecycle = PrintLifecycleState::kUnknown;
  std::string stage = "boot";
  std::string detail = "Starting up";
  std::string raw_status;
  std::string raw_stage;
  std::string ui_status;
  std::string job_name;
  std::string gcode_file;
  std::string preview_hint;
  std::string preview_url;
  std::shared_ptr<std::vector<uint8_t>> preview_blob;
  std::string preview_title;
  std::shared_ptr<std::vector<uint8_t>> camera_blob;
  std::string camera_detail;
  uint16_t camera_width = 0;
  uint16_t camera_height = 0;
  std::string cloud_detail;
  float progress_percent = 0.0f;
  float nozzle_temp_c = 0.0f;
  float bed_temp_c = 0.0f;
  uint32_t remaining_seconds = 0;
  uint16_t current_layer = 0;
  uint16_t total_layers = 0;
  int print_error_code = 0;
  uint16_t hms_alert_count = 0;
  bool wifi_connected = false;
  std::string wifi_ip;
  bool setup_ap_active = false;
  std::string setup_ap_ssid;
  std::string setup_ap_password;
  std::string setup_ap_ip;
  uint8_t battery_percent = 0;
  bool charging = false;
  bool usb_present = false;
  float pmu_temp_c = 0.0f;
  bool has_error = false;
  bool print_active = false;
  bool warn_hms = false;
  bool cloud_connected = false;
  bool camera_connected = false;
  bool non_error_stop = false;
  bool show_stop_banner = false;
};

class PrinterStateStore {
 public:
  void set_snapshot(PrinterSnapshot snapshot);
  PrinterSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  PrinterSnapshot snapshot_{};
};

const char* to_string(PrinterConnectionState state);
const char* to_string(PrintLifecycleState state);

}  // namespace printsphere
