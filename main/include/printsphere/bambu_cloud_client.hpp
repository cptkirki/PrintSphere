#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/config_store.hpp"
#include "printsphere/printer_state.hpp"

struct cJSON;

namespace printsphere {

struct BambuCloudSnapshot {
  bool configured = false;
  bool connected = false;
  std::string detail = "Cloud login not configured";
  std::string preview_url;
  std::shared_ptr<std::vector<uint8_t>> preview_blob;
  std::string preview_title;
  std::string resolved_serial;
  std::string raw_status;
  std::string raw_stage;
  std::string stage;
  float progress_percent = 0.0f;
  uint32_t remaining_seconds = 0;
  uint16_t current_layer = 0;
  uint16_t total_layers = 0;
  PrintLifecycleState lifecycle = PrintLifecycleState::kUnknown;
  bool has_error = false;
  bool verification_required = false;
  bool tfa_required = false;
};

class BambuCloudClient {
 public:
  BambuCloudClient() = default;

  void set_config_store(const ConfigStore* config_store) { config_store_ = config_store; }
  void configure(BambuCloudCredentials credentials, std::string printer_serial);
  void set_network_ready(bool ready) { network_ready_.store(ready); }
  void set_low_power_mode(bool enabled) { low_power_mode_.store(enabled); }
  void set_preview_fetch_enabled(bool enabled) { preview_fetch_enabled_.store(enabled); }
  void request_reload_from_store() { reload_requested_.store(true); }
  void submit_verification_code(std::string code);
  esp_err_t start();
  BambuCloudSnapshot snapshot() const;

 private:
  enum class AuthMode : uint8_t {
    kPassword,
    kEmailCode,
    kTfaCode,
  };

  static void task_entry(void* context);

  void task_loop();
  bool login();
  bool authenticate_with_password();
  bool authenticate_with_email_code(const std::string& code);
  bool authenticate_with_tfa_code(const std::string& code);
  bool fetch_bindings();
  bool fetch_latest_preview();
  std::shared_ptr<std::vector<uint8_t>> download_preview_image(const std::string& url);
  bool request_email_verification_code();
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
  void persist_access_token() const;
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
  static float extract_progress(const cJSON* item);
  static uint32_t extract_remaining_seconds(const cJSON* item);
  static uint16_t extract_current_layer(const cJSON* item);
  static uint16_t extract_total_layers(const cJSON* item);
  static PrintLifecycleState cloud_lifecycle_from_status(const std::string& status_text);
  static std::string cloud_stage_label_for(const std::string& status_text,
                                           PrintLifecycleState lifecycle);
  static const cJSON* child_object(const cJSON* object, const char* key);
  static const cJSON* child_array(const cJSON* object, const char* key);

  mutable std::mutex mutex_{};
  BambuCloudSnapshot snapshot_{};
  const ConfigStore* config_store_ = nullptr;
  BambuCloudCredentials credentials_{};
  std::string requested_serial_{};
  std::string resolved_serial_{};
  std::string access_token_{};
  int64_t token_expiry_us_ = 0;
  TaskHandle_t task_handle_ = nullptr;
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> low_power_mode_{false};
  std::atomic<bool> preview_fetch_enabled_{false};
  std::atomic<bool> reload_requested_{false};
  mutable std::mutex auth_mutex_{};
  AuthMode auth_mode_ = AuthMode::kPassword;
  std::string tfa_key_{};
  std::string pending_verification_code_{};
  std::string cached_preview_url_{};
  std::shared_ptr<std::vector<uint8_t>> cached_preview_blob_{};
};

}  // namespace printsphere
