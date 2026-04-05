#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"
#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/p1s_camera_client.hpp"
#include "printsphere/printer_client.hpp"
#include "printsphere/wifi_manager.hpp"

namespace printsphere {

class Ui;

struct PortalAccessSnapshot {
  bool request_authorized = false;
  bool session_active = false;
  bool pin_active = false;
  uint32_t session_remaining_s = 0;
  uint32_t pin_remaining_s = 0;
  std::string pin_code;
  std::string detail;
};

class SetupPortal {
 public:
  SetupPortal(ConfigStore& config_store, const WifiManager& wifi_manager,
              BambuCloudClient& cloud_client, PrinterClient& printer_client,
              P1sCameraClient& camera_client, Ui& ui)
      : config_store_(config_store),
        wifi_manager_(wifi_manager),
        cloud_client_(cloud_client),
        printer_client_(printer_client),
        camera_client_(camera_client),
        ui_(ui) {}

  esp_err_t start();
  void request_unlock_pin();
  PortalAccessSnapshot access_snapshot(bool request_authorized = false);

 private:
  static esp_err_t handle_root(httpd_req_t* request);
  static esp_err_t handle_favicon(httpd_req_t* request);
  static esp_err_t handle_health(httpd_req_t* request);
  static esp_err_t handle_unlock(httpd_req_t* request);
  static esp_err_t handle_wifi_scan(httpd_req_t* request);
  static esp_err_t handle_config_get(httpd_req_t* request);
  static esp_err_t handle_config_post(httpd_req_t* request);
  static esp_err_t handle_arc_preview(httpd_req_t* request);
  static esp_err_t handle_arc_commit(httpd_req_t* request);
  static esp_err_t handle_arc_update(httpd_req_t* request, bool persist);
  static esp_err_t handle_source_mode_post(httpd_req_t* request);
  static esp_err_t handle_display_rotation_post(httpd_req_t* request);
  static esp_err_t handle_cloud_connect(httpd_req_t* request);
  static esp_err_t handle_cloud_verify(httpd_req_t* request);
  static esp_err_t handle_local_connect(httpd_req_t* request);
  static void reboot_task(void* context);
  bool is_provisioning_complete() const;
  bool is_lock_required() const;
  bool is_request_authorized(httpd_req_t* request);
  esp_err_t send_locked_response(httpd_req_t* request);
  esp_err_t send_unlock_page(httpd_req_t* request);
  void prune_access_state_locked(uint64_t now_ms);
  static std::string generate_unlock_pin();
  static std::string generate_session_token();

  ConfigStore& config_store_;
  const WifiManager& wifi_manager_;
  BambuCloudClient& cloud_client_;
  PrinterClient& printer_client_;
  P1sCameraClient& camera_client_;
  Ui& ui_;
  httpd_handle_t server_ = nullptr;
  bool reboot_requested_ = false;
  std::mutex access_mutex_{};
  std::string unlock_pin_{};
  uint64_t unlock_pin_expiry_ms_ = 0;
  std::string session_token_{};
  uint64_t session_expiry_ms_ = 0;
};

}  // namespace printsphere
