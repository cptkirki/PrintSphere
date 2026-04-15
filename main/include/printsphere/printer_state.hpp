#pragma once

#include <array>
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

enum class PrinterModel : uint8_t {
  kUnknown,
  kA1,
  kA1Mini,
  kP1P,
  kP1S,
  kP2S,
  kH2C,
  kH2D,
  kH2DPro,
  kH2S,
  kX1,
  kX1C,
  kX1E,
};

enum class FieldSource : uint8_t {
  kNone,
  kLocal,
  kCloud,
};

struct SourceCapabilities {
  bool status = false;
  bool metrics = false;
  bool temperatures = false;
  bool preview = false;
  bool hms = false;
  bool print_error = false;
  bool camera_jpeg_socket = false;
  bool camera_rtsp = false;
  bool developer_mode_required = false;
};

static constexpr int kMaxAmsTrays = 4;
static constexpr int kMaxAmsUnits = 4;

struct AmsTrayInfo {
  bool present = false;
  bool active = false;
  std::string material_type;   // e.g. "PLA", "PETG", "ASA"
  std::string material_name;   // Bambu filament name from tray_sub_brands
  uint32_t color_rgba = 0;     // RRGGBBAA from tray_color
  int remain_pct = -1;         // Filament remaining 0-100%, -1 = unknown
};

struct AmsUnitInfo {
  bool present = false;
  int humidity_pct = -1;       // 0-100% relative humidity, -1 = unknown
  float temperature_c = 0.0f;
  std::array<AmsTrayInfo, kMaxAmsTrays> trays{};
};

struct AmsSnapshot {
  uint8_t count = 0;
  std::array<AmsUnitInfo, kMaxAmsUnits> units{};
  AmsTrayInfo external_spool{};  // vt_tray: external spool info
};

struct PrinterSnapshot {
  PrinterConnectionState connection = PrinterConnectionState::kBooting;
  PrintLifecycleState lifecycle = PrintLifecycleState::kUnknown;
  std::string stage = "boot";
  std::string detail = "Starting up";
  std::string raw_status;
  std::string raw_stage;
  std::string ui_status;
  std::string resolved_serial;
  std::string job_name;
  std::string gcode_file;
  std::string preview_hint;
  std::string preview_url;
  std::shared_ptr<std::vector<uint8_t>> preview_blob;
  std::string preview_title;
  std::shared_ptr<std::vector<uint8_t>> camera_blob;
  std::string camera_detail;
  std::string camera_rtsp_url;
  uint16_t camera_width = 0;
  uint16_t camera_height = 0;
  std::string cloud_detail;
  float progress_percent = 0.0f;
  bool progress_is_download_related = false;
  float nozzle_temp_c = 0.0f;
  bool nozzle_temp_known = false;
  float bed_temp_c = 0.0f;
  bool bed_temp_known = false;
  float chamber_temp_c = 0.0f;
  bool chamber_temp_known = false;
  float secondary_nozzle_temp_c = 0.0f;
  bool secondary_nozzle_temp_known = false;
  bool chamber_light_supported = false;
  bool chamber_light_state_known = false;
  bool chamber_light_on = false;
  uint32_t remaining_seconds = 0;
  uint16_t current_layer = 0;
  uint16_t total_layers = 0;
  int print_error_code = 0;
  std::vector<uint64_t> hms_codes;
  uint16_t hms_alert_count = 0;
  bool local_configured = false;
  bool local_connected = false;
  uint64_t local_last_update_ms = 0;
  PrinterModel local_model = PrinterModel::kUnknown;
  SourceCapabilities local_capabilities{};
  bool local_mqtt_signature_required = false;
  bool wifi_connected = false;
  std::string wifi_ip;
  bool setup_ap_active = false;
  std::string setup_ap_ssid;
  std::string setup_ap_password;
  std::string setup_ap_ip;
  uint8_t battery_percent = 0;
  bool battery_present = false;
  bool charging = false;
  bool usb_present = false;
  float pmu_temp_c = 0.0f;
  bool has_error = false;
  bool print_active = false;
  bool warn_hms = false;
  bool cloud_configured = false;
  bool cloud_connected = false;
  uint64_t cloud_last_update_ms = 0;
  PrinterModel cloud_model = PrinterModel::kUnknown;
  SourceCapabilities cloud_capabilities{};
  bool preview_page_available = true;
  bool camera_page_available = true;
  bool camera_connected = false;
  FieldSource status_source = FieldSource::kNone;
  FieldSource metrics_source = FieldSource::kNone;
  FieldSource preview_source = FieldSource::kNone;
  FieldSource camera_source = FieldSource::kNone;
  bool non_error_stop = false;
  bool show_stop_banner = false;
  int hw_switch_state = -1;
  int tray_now = -1;
  int tray_tar = -1;
  std::shared_ptr<AmsSnapshot> ams;
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
const char* to_string(PrinterModel model);
const char* to_string(FieldSource source);
bool printer_model_has_jpeg_camera(PrinterModel model);
bool printer_model_has_rtsp_camera(PrinterModel model);
bool printer_model_has_chamber_temperature(PrinterModel model);
bool printer_model_has_secondary_nozzle_temperature(PrinterModel model);
bool printer_model_has_chamber_light(PrinterModel model);
bool printer_model_has_secondary_chamber_light(PrinterModel model);
bool printer_model_supports_local_status(PrinterModel model);
bool printer_model_requires_developer_mode_for_local_status(PrinterModel model);
bool printer_model_prefers_cloud_status(PrinterModel model);
bool printer_serial_family_has_no_chamber_temperature(const std::string& serial);
SourceCapabilities default_local_capabilities_for_model(PrinterModel model);
SourceCapabilities default_cloud_capabilities();

}  // namespace printsphere
