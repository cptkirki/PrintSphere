#pragma once

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
  void set_network_ready(bool ready) { network_ready_.store(ready); }
  esp_err_t start();
  PrinterSnapshot snapshot() const { return state_.snapshot(); }

 private:
  static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id,
                                 void* event_data);
  static void task_entry(void* context);
  void handle_mqtt_event(esp_mqtt_event_handle_t event);
  void handle_report_payload(const char* payload, size_t length);
  void handle_info_payload(const char* payload, size_t length);
  void task_loop();
  void stop_client();
  PrinterConnection desired_connection() const;
  void set_waiting_snapshot(const PrinterConnection& connection);
  void publish_request(const char* payload);
  void request_initial_sync();
  static PrintLifecycleState lifecycle_from_state(const std::string& gcode_state,
                                                  bool has_concrete_error);
  static std::string stage_label_for(const std::string& gcode_state, int stage_id,
                                     bool has_concrete_error);
  static std::string error_detail_for(int print_error_code, int hms_count);
  static std::string preview_hint_for(const std::string& gcode_file);
  static std::string trim_job_name(const std::string& name);
  static float json_number(const cJSON* object, const char* key, float fallback);
  static int json_int(const cJSON* object, const char* key, int fallback);
  static std::string json_string(const cJSON* object, const char* key,
                                 const std::string& fallback = {});

  mutable std::mutex config_mutex_{};
  PrinterConnection desired_connection_{};
  PrinterConnection active_connection_{};
  PrinterStateStore state_{};
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
  std::atomic<bool> received_payload_{false};
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> reconfigure_requested_{false};
  std::atomic<uint32_t> last_message_tick_{0};
};

}  // namespace printsphere
