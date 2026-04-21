#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "printsphere/config_store.hpp"
#include "printsphere/printer_state.hpp"

struct cJSON;

namespace printsphere {

enum class CloudSetupStage : uint8_t {
  kIdle,
  kLoggingIn,
  kEmailCodeRequired,
  kTfaRequired,
  kCodeSubmitted,
  kBindingPrinter,
  kConnectingMqtt,
  kConnected,
  kFailed,
};

const char* to_string(CloudSetupStage stage);

struct BambuCloudSnapshot {
  bool configured = false;
  bool connected = false;
  bool session_connected = false;
  uint64_t last_update_ms = 0;
  PrinterModel model = PrinterModel::kUnknown;
  SourceCapabilities capabilities{};
  CloudSetupStage setup_stage = CloudSetupStage::kIdle;
  std::string detail = "Cloud login not configured";
  std::string preview_url;
  std::shared_ptr<std::vector<uint8_t>> preview_blob;
  std::string preview_title;
  std::string resolved_serial;
  std::string raw_status;
  std::string raw_stage;
  std::string stage;
  float progress_percent = 0.0f;
  bool progress_is_download_related = false;
  float nozzle_temp_c = 0.0f;
  uint64_t nozzle_temp_last_update_ms = 0;
  float bed_temp_c = 0.0f;
  uint64_t bed_temp_last_update_ms = 0;
  float chamber_temp_c = 0.0f;
  uint64_t chamber_temp_last_update_ms = 0;
  float secondary_nozzle_temp_c = 0.0f;
  uint64_t secondary_nozzle_temp_last_update_ms = 0;
  int active_nozzle_index = -1;  // -1 = single nozzle, 0 = right, 1 = left (H2D)
  bool chamber_light_supported = false;
  bool chamber_light_state_known = false;
  bool chamber_light_on = false;
  bool chamber_light_pending = false;
  uint64_t chamber_light_pending_since_ms = 0;
  bool non_error_stop = false;
  uint32_t remaining_seconds = 0;
  uint16_t current_layer = 0;
  uint16_t total_layers = 0;
  int print_error_code = 0;
  int hw_switch_state = -1;
  int tray_now = -1;
  int tray_tar = -1;
  int ams_status_main = -1;
  std::shared_ptr<AmsSnapshot> ams;
  std::vector<uint64_t> hms_codes;
  uint16_t hms_alert_count = 0;
  uint64_t live_data_last_update_ms = 0;
  PrintLifecycleState lifecycle = PrintLifecycleState::kUnknown;
  bool has_error = false;
  bool verification_required = false;
  bool tfa_required = false;
};

struct CloudDeviceInfo {
  std::string serial;
  std::string display_name;
  PrinterModel model = PrinterModel::kUnknown;
  bool online = false;
};

class BambuCloudClient {
 public:
  BambuCloudClient() = default;

  struct CloudLiveRuntimeState {
    bool configured = false;
    bool connected = false;
    uint64_t last_update_ms = 0;
    uint64_t live_data_last_update_ms = 0;
    PrinterModel model = PrinterModel::kUnknown;
    SourceCapabilities capabilities{};
    CloudSetupStage setup_stage = CloudSetupStage::kIdle;
    PrintLifecycleState lifecycle = PrintLifecycleState::kUnknown;
    float progress_percent = 0.0f;
    bool progress_is_download_related = false;
    float nozzle_temp_c = 0.0f;
    uint64_t nozzle_temp_last_update_ms = 0;
    float bed_temp_c = 0.0f;
    uint64_t bed_temp_last_update_ms = 0;
    float chamber_temp_c = 0.0f;
    uint64_t chamber_temp_last_update_ms = 0;
    float secondary_nozzle_temp_c = 0.0f;
    uint64_t secondary_nozzle_temp_last_update_ms = 0;
    int active_nozzle_index = -1;  // -1 = single nozzle, 0 = right, 1 = left (H2D)
    bool chamber_light_supported = false;
    bool chamber_light_state_known = false;
    bool chamber_light_on = false;
    bool chamber_light_pending = false;
    uint64_t chamber_light_pending_since_ms = 0;
    bool non_error_stop = false;
    uint32_t remaining_seconds = 0;
    uint16_t current_layer = 0;
    uint16_t total_layers = 0;
    int print_error_code = 0;
    int hw_switch_state = -1;
    int tray_now = -1;
    int tray_tar = -1;
    int ams_status_main = -1;
    std::shared_ptr<AmsSnapshot> ams;
    std::vector<uint64_t> hms_codes;
    uint16_t hms_alert_count = 0;
    bool has_error = false;
    std::array<char, 96> detail{};
    std::array<char, 96> job_name{};
    std::array<char, 24> resolved_serial{};
    std::array<char, 16> raw_status{};
    std::array<char, 32> raw_stage{};
    std::array<char, 32> stage{};
  };

  struct CloudRestRuntimeState {
    bool configured = false;
    bool session_ready = false;
    bool verification_required = false;
    bool tfa_required = false;
    bool printer_online = true;
    uint64_t last_update_ms = 0;
    PrinterModel model = PrinterModel::kUnknown;
    SourceCapabilities capabilities{};
    CloudSetupStage setup_stage = CloudSetupStage::kIdle;
    bool chamber_light_supported = false;
    std::array<char, 96> detail{};
    std::array<char, 24> resolved_serial{};
    std::string preview_url{};
    std::shared_ptr<std::vector<uint8_t>> preview_blob{};
    std::string preview_title{};
  };

  void set_config_store(const ConfigStore* config_store) { config_store_ = config_store; }
  void configure(BambuCloudCredentials credentials, std::string printer_serial);
  void set_network_ready(bool ready) { network_ready_.store(ready); }
  // Invoked whenever a `client.connected` / `client.disconnected` event for the
  // currently-bound printer arrives on the Bambu Cloud MQTT feed. Used by the
  // local PrinterClient to reconnect immediately when the printer comes back
  // online instead of waiting for the next TCP-probe cycle.
  // Callback is invoked from the cloud MQTT event task — keep handlers short
  // and non-blocking.
  void set_printer_presence_callback(std::function<void(bool online)> cb) {
    printer_presence_callback_ = std::move(cb);
  }
  void set_low_power_mode(bool enabled) { low_power_mode_.store(enabled); }
  void set_fetch_paused(bool paused);
  void set_live_mqtt_enabled(bool enabled) { live_mqtt_enabled_.store(enabled); }
  void set_preview_fetch_enabled(bool enabled);
  void request_reload_from_store() {
    reload_requested_.store(true);
    if (task_handle_ != nullptr) {
      xTaskNotifyGive(task_handle_);
    }
  }
  void submit_verification_code(std::string code);
  bool set_chamber_light(bool on);
  esp_err_t start();
  BambuCloudSnapshot snapshot() const;
  BambuCloudSnapshot refreshed_snapshot();
  std::vector<CloudDeviceInfo> get_cloud_devices() const;

 private:
  enum class AuthMode : uint8_t {
    kPassword,
    kEmailCode,
    kTfaCode,
  };

  static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id,
                                 void* event_data);
  static void task_entry(void* context);

  void handle_mqtt_event(esp_mqtt_event_handle_t event);
  void stop_mqtt_client();
  void task_loop();
  bool login();
  bool authenticate_with_password();
  bool authenticate_with_email_code(const std::string& code);
  bool authenticate_with_tfa_code(const std::string& code);
  bool ensure_cloud_mqtt_identity();
  bool ensure_mqtt_client_started();
  void request_initial_sync();
  bool publish_request(const char* payload);
  void handle_report_payload(const char* payload, size_t length);
  bool fetch_bindings();
  bool fetch_latest_preview(bool allow_preview_download);
  std::shared_ptr<std::vector<uint8_t>> download_preview_image(const std::string& url);
  bool request_email_verification_code();
  bool request_sms_verification_code();
  bool request_verification_code();
  bool perform_json_request(const std::string& url, const char* method,
                            const std::string& request_body, const std::string& bearer_token,
                            int* status_code, std::string* response_body);
  void set_snapshot(BambuCloudSnapshot snapshot);
  AuthMode auth_mode() const;
  bool waiting_for_user_code() const;
  std::string pending_verification_code() const;
  void set_auth_mode(AuthMode mode, std::string tfa_key = {});
  void clear_auth_state();
  void clear_pending_code();
  bool persist_access_token() const;
  void clear_persisted_access_token();
  static std::string json_string(const cJSON* object, const char* key,
                                 const std::string& fallback = {});
  static int json_int(const cJSON* object, const char* key, int fallback);
  static float json_number(const cJSON* object, const char* key, float fallback);
  static bool json_bool(const cJSON* object, const char* key, bool fallback);
  static std::string extract_cover_url(const cJSON* item);
  static std::string extract_title(const cJSON* item);
  static std::string extract_device_serial(const cJSON* item);
  static std::string extract_status_text(const cJSON* item);
  static std::string extract_stage_text(const cJSON* item);
  static std::string extract_print_type_text(const cJSON* item);
  static float extract_progress(const cJSON* item);
  static uint32_t extract_remaining_seconds(const cJSON* item);
  static uint16_t extract_current_layer(const cJSON* item);
  static uint16_t extract_total_layers(const cJSON* item);
  static PrintLifecycleState cloud_lifecycle_from_status(const std::string& status_text);
  static std::string cloud_stage_label_for(const std::string& status_text,
                                           PrintLifecycleState lifecycle);
  static const cJSON* child_object(const cJSON* object, const char* key);
  static const cJSON* child_array(const cJSON* object, const char* key);
  CloudLiveRuntimeState live_runtime_copy() const;
  void store_live_runtime(CloudLiveRuntimeState runtime, bool notify_task);
  CloudRestRuntimeState rest_runtime_copy() const;
  void store_rest_runtime(CloudRestRuntimeState runtime, bool notify_task);
  void publish_combined_snapshot();
  void apply_cloud_session_state(bool configured, bool connected, bool verification_required,
                                 bool tfa_required, const std::string& detail,
                                 bool session_ready, bool clear_live_state);
  void apply_cloud_token_expired_state();

  mutable std::mutex mutex_{};
  BambuCloudSnapshot snapshot_{};
  mutable std::mutex live_runtime_mutex_{};
  CloudLiveRuntimeState live_runtime_{};
  mutable std::mutex rest_runtime_mutex_{};
  CloudRestRuntimeState rest_runtime_{};
  mutable std::mutex cloud_devices_mutex_{};
  std::vector<CloudDeviceInfo> cloud_devices_{};
  const ConfigStore* config_store_ = nullptr;
  BambuCloudCredentials credentials_{};
  std::string requested_serial_{};
  std::string resolved_serial_{};
  std::string access_token_{};
  std::string mqtt_username_{};
  int64_t token_expiry_us_ = 0;
  TaskHandle_t task_handle_ = nullptr;
  esp_mqtt_client_handle_t mqtt_client_ = nullptr;
  std::string mqtt_client_id_{};
  std::string mqtt_report_topic_{};
  std::string mqtt_request_topic_{};
  std::string incoming_topic_{};
  std::string incoming_payload_{};
  mutable std::mutex incoming_mutex_{};
  std::atomic<bool> mqtt_connected_{false};
  std::atomic<bool> mqtt_subscription_acknowledged_{false};
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> low_power_mode_{false};
  std::atomic<bool> fetch_paused_{false};
  std::atomic<bool> live_mqtt_enabled_{true};
  std::atomic<bool> preview_fetch_enabled_{false};
  std::atomic<bool> reload_requested_{false};
  std::atomic<bool> received_live_payload_{false};
  std::atomic<bool> initial_sync_sent_{false};
  std::function<void(bool online)> printer_presence_callback_{};
  std::atomic<bool> delayed_start_sent_{false};
  std::atomic<uint32_t> initial_sync_tick_{0};
  std::atomic<bool> live_runtime_dirty_{false};
  std::atomic<bool> rest_runtime_dirty_{false};
  std::atomic<bool> mqtt_auth_recovery_requested_{false};
  std::atomic<int> mqtt_auth_connect_return_code_{
      static_cast<int>(MQTT_CONNECTION_ACCEPTED)};
  std::atomic<int64_t> mqtt_auth_retry_not_before_us_{0};
  std::atomic<int> cloud_payload_probe_logs_remaining_{3};
  mutable std::mutex auth_mutex_{};
  AuthMode auth_mode_ = AuthMode::kPassword;
  std::string tfa_key_{};
  std::string pending_verification_code_{};
  std::string cached_preview_url_{};
  std::shared_ptr<std::vector<uint8_t>> cached_preview_blob_{};
};

}  // namespace printsphere
