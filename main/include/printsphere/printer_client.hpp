#pragma once
#include "printsphere/command_tracker.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/config_store.hpp"
#include "printsphere/mqtt_telemetry.hpp"
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
  // Called immediately before the local MQTT client allocates/starts its TLS stack.
  // Return a short delay in ms when other network clients need time to unwind.
  void set_pre_local_mqtt_callback(std::function<uint32_t()> cb);
  bool set_chamber_light(bool on, const std::string& expected_serial = {});
  // Queue one print control action. The matching printer acknowledgement is
  // tracked independently from MQTT delivery and never fabricates lifecycle.
  bool set_print_command(PrintCommand command, const std::string& expected_serial = {});
  esp_err_t start();
  PrinterSnapshot snapshot() const;
  // Reconnect-storm telemetry snapshot (safe from any task).
  MqttTelemetry mqtt_telemetry() const;
  struct LocalPrinterRuntimeState {
    PrinterConnectionState connection = PrinterConnectionState::kBooting;
    PrintLifecycleState lifecycle = PrintLifecycleState::kUnknown;
    PrinterModel local_model = PrinterModel::kUnknown;
    SourceCapabilities local_capabilities{};
    float progress_percent = 0.0f;
    bool progress_is_download_related = false;
    float nozzle_temp_c = 0.0f;
    uint64_t nozzle_temp_updated_ms = 0;
    uint64_t bed_temp_updated_ms = 0;
    uint64_t chamber_temp_updated_ms = 0;
    uint64_t secondary_nozzle_temp_updated_ms = 0;
    uint64_t core_status_updated_ms = 0;
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
  CommandTracker command_tracker_{};
  static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id,
                                 void* event_data);
  static void task_entry(void* context);
  void handle_mqtt_event(esp_mqtt_event_handle_t event);
  void handle_report_payload(const char* payload, size_t length);
  void task_loop();
  void stop_client();
  void schedule_client_rebuild(const char* reason, uint32_t delay_ms = 1500,
                               bool force_when_connected = false);
  void cancel_client_rebuild();
  void process_pending_chamber_light_command();
  bool publish_chamber_light_command(bool on);
  void process_pending_print_command();
  bool publish_print_command(PrintCommand command);
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
  uint32_t prepare_for_local_mqtt_start();

  mutable std::mutex config_mutex_{};
  PrinterConnection desired_connection_{};
  PrinterConnection active_connection_{};
  mutable std::mutex pre_local_mqtt_mutex_{};
  std::function<uint32_t()> pre_local_mqtt_callback_{};
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
  // once for the currently active profile. Used to keep later reconnect
  // backoffs short for a profile that has already proven its configuration.
  // Reset in stop_client() when the client is actually destroyed.
  std::atomic<bool> session_ever_established_{false};
  std::atomic<bool> received_payload_{false};
  std::atomic<bool> subscription_acknowledged_{false};
  std::atomic<bool> initial_sync_sent_{false};
  std::atomic<bool> delayed_pushall_sent_{false};
  std::atomic<bool> first_payload_observed_{false};
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> reconfigure_requested_{false};
  std::atomic<bool> chamber_light_command_pending_{false};
  std::atomic<bool> chamber_light_command_on_{false};
  // Pending print-control command (pause/resume/stop) queued from the UI thread
  // to be published from the network task on its next iteration. kNone = idle.
  std::atomic<uint8_t> print_command_pending_{static_cast<uint8_t>(PrintCommand::kNone)};
  std::atomic<bool> client_rebuild_requested_{false};
  std::atomic<bool> force_client_rebuild_{false};
  std::atomic<uint32_t> last_message_tick_{0};
  std::atomic<uint32_t> initial_sync_tick_{0};
  std::atomic<uint32_t> connection_state_tick_{0};
  std::atomic<uint32_t> watchdog_probe_tick_{0};
  std::atomic<uint32_t> periodic_pushall_tick_{0};
  std::atomic<uint32_t> rebuild_request_tick_{0};
  std::atomic<uint32_t> rebuild_delay_ticks_{0};
  std::atomic<bool> runtime_dirty_{false};
  uint32_t consecutive_mqtt_errors_{0};
  // Reconnect-storm telemetry (atomic so the setup-portal HTTP task can read
  // them without locking the worker mutex). consecutive_mqtt_errors_ above is
  // the existing per-loop counter; mqtt_total_failures_/successes_ accumulate
  // since boot and survive reconnect cycles.
  std::atomic<uint32_t> mqtt_total_failures_{0};
  std::atomic<uint32_t> mqtt_total_successes_{0};
  std::atomic<uint64_t> mqtt_connect_started_ms_{0};
  std::atomic<uint64_t> mqtt_last_attempt_ms_{0};
  std::atomic<uint64_t> mqtt_last_success_ms_{0};
  std::atomic<uint64_t> mqtt_last_failure_ms_{0};
  std::atomic<uint32_t> mqtt_current_backoff_ms_{0};
};

}  // namespace printsphere
