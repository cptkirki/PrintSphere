#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/config_store.hpp"
#include "printsphere/printer_state.hpp"

struct cJSON;

namespace printsphere {

class PrinterClient {
 public:
  PrinterClient() = default;

  void configure(PrinterConnection connection);
  bool is_configured() const;
  // Notify the client about Wi-Fi station link state. A transition from
  // not-ready → ready also acts as an implicit presence hint (see
  // notify_cloud_presence()) so that we don't sit in a 30 s backoff window when
  // Wi-Fi comes back.
  void set_network_ready(bool ready);
  // Hook driven by the Bambu Cloud MQTT feed: when the cloud reports that the
  // printer has just come back online (`client.connected`), we can short-circuit
  // the current reconnect backoff and attempt a local connection immediately
  // instead of waiting for the next TCP-probe cycle. Called with online=false
  // on `client.disconnected` for logging / future offline handling. Safe to
  // call from the cloud MQTT task context; only touches atomics.
  void notify_cloud_presence(bool online);
  bool set_chamber_light(bool on);
  esp_err_t start();
  PrinterSnapshot snapshot() const { return state_.snapshot(); }
  struct LocalPrinterRuntimeState {
    PrinterConnectionState connection = PrinterConnectionState::kBooting;
    PrintLifecycleState lifecycle = PrintLifecycleState::kUnknown;
    PrinterModel local_model = PrinterModel::kUnknown;
    SourceCapabilities local_capabilities{};
    float progress_percent = 0.0f;
    bool progress_is_download_related = false;
    float nozzle_temp_c = 0.0f;
    float bed_temp_c = 0.0f;
    float chamber_temp_c = 0.0f;
    float secondary_nozzle_temp_c = 0.0f;
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
    bool local_mqtt_signature_required = false;
    bool has_error = false;
    bool print_active = false;
    bool warn_hms = false;
    bool non_error_stop = false;
    bool show_stop_banner = false;
    std::array<char, 16> raw_status{};
    std::array<char, 32> raw_stage{};
    std::array<char, 32> stage{};
    std::array<char, 96> detail{};
    std::array<char, 24> resolved_serial{};
    std::array<char, 96> job_name{};
    std::array<char, 128> gcode_file{};
    std::array<char, 96> camera_rtsp_url{};
    int hw_switch_state = -1;
    int tray_now = -1;
    int tray_tar = -1;
    int ams_status_main = -1;
    int active_nozzle_index = -1;  // -1 = single nozzle, 0 = right, 1 = left (H2D)
    bool ams_filament_change_latched = false;
    std::shared_ptr<AmsSnapshot> ams;
  };

 private:
  static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id,
                                 void* event_data);
  static void task_entry(void* context);
  void handle_mqtt_event(esp_mqtt_event_handle_t event);
  void handle_report_payload(const char* payload, size_t length);
  void task_loop();
  void stop_client();
  void schedule_client_rebuild(const char* reason, uint32_t delay_ms = 1500);
  void cancel_client_rebuild();
  PrinterConnection desired_connection() const;
  void set_waiting_snapshot(const PrinterConnection& connection);
  bool publish_request(const char* payload);
  void request_initial_sync();
  static PrintLifecycleState lifecycle_from_state(const std::string& gcode_state,
                                                  bool has_concrete_error);
  static std::string stage_label_for(const std::string& gcode_state, int stage_id,
                                     bool has_concrete_error);
  static std::string error_detail_for(int print_error_code, const std::vector<uint64_t>& hms_codes,
                                      int hms_count, PrinterModel model);
  static std::string preview_hint_for(const std::string& gcode_file);
  static std::string trim_job_name(const std::string& name);
  static float json_number(const cJSON* object, const char* key, float fallback);
  static int json_int(const cJSON* object, const char* key, int fallback);
  static std::string json_string(const cJSON* object, const char* key,
                                 const std::string& fallback = {});
  LocalPrinterRuntimeState runtime_state_copy() const;
  void store_runtime_state(LocalPrinterRuntimeState runtime, bool notify_task);
  void publish_runtime_snapshot();
  PrinterSnapshot build_snapshot_from_runtime(const LocalPrinterRuntimeState& runtime) const;
  void wake_task();

  mutable std::mutex config_mutex_{};
  PrinterConnection desired_connection_{};
  PrinterConnection active_connection_{};
  PrinterStateStore state_{};
  mutable std::mutex runtime_mutex_{};
  LocalPrinterRuntimeState runtime_state_{};
  TaskHandle_t task_handle_ = nullptr;
  esp_mqtt_client_handle_t client_ = nullptr;
  std::string client_id_{};
  std::string report_topic_{};
  std::string request_topic_{};
  std::string incoming_topic_{};
  std::string incoming_payload_{};
  std::mutex incoming_mutex_{};
  std::atomic<bool> client_started_{false};
  std::atomic<bool> mqtt_connected_{false};
  // Latched once the MQTT session successfully reached CONNECTED state at least
  // once for the currently active profile. Used to skip the expensive TCP probe
  // on subsequent reconnect attempts so esp-mqtt's internal reconnect
  // (network.reconnect_timeout_ms) can do the heavy lifting without tearing
  // down and reinitialising the client (and its TLS session) every time.
  // Reset in stop_client() when the client is actually destroyed.
  std::atomic<bool> session_ever_established_{false};
  std::atomic<bool> received_payload_{false};
  std::atomic<bool> subscription_acknowledged_{false};
  std::atomic<bool> initial_sync_sent_{false};
  std::atomic<bool> delayed_pushall_sent_{false};
  std::atomic<bool> first_payload_observed_{false};
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> reconfigure_requested_{false};
  std::atomic<bool> client_rebuild_requested_{false};
  std::atomic<uint32_t> last_message_tick_{0};
  std::atomic<uint32_t> initial_sync_tick_{0};
  std::atomic<uint32_t> connection_state_tick_{0};
  std::atomic<uint32_t> watchdog_probe_tick_{0};
  std::atomic<uint32_t> rebuild_request_tick_{0};
  std::atomic<uint32_t> rebuild_delay_ticks_{0};
  std::atomic<bool> runtime_dirty_{false};
  uint32_t consecutive_probe_failures_{0};
  uint32_t consecutive_mqtt_errors_{0};
};

}  // namespace printsphere
