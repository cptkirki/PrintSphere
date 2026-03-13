#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/p1s_camera_client.hpp"
#include "printsphere/printer_client.hpp"
#include "printsphere/wifi_manager.hpp"

namespace printsphere {

class Ui;

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

 private:
  static esp_err_t handle_root(httpd_req_t* request);
  static esp_err_t handle_favicon(httpd_req_t* request);
  static esp_err_t handle_health(httpd_req_t* request);
  static esp_err_t handle_wifi_scan(httpd_req_t* request);
  static esp_err_t handle_config_get(httpd_req_t* request);
  static esp_err_t handle_config_post(httpd_req_t* request);
  static esp_err_t handle_arc_preview(httpd_req_t* request);
  static esp_err_t handle_arc_commit(httpd_req_t* request);
  static esp_err_t handle_arc_update(httpd_req_t* request, bool persist);
  static esp_err_t handle_cloud_connect(httpd_req_t* request);
  static esp_err_t handle_cloud_verify(httpd_req_t* request);
  static esp_err_t handle_local_connect(httpd_req_t* request);
  static void reboot_task(void* context);

  ConfigStore& config_store_;
  const WifiManager& wifi_manager_;
  BambuCloudClient& cloud_client_;
  PrinterClient& printer_client_;
  P1sCameraClient& camera_client_;
  Ui& ui_;
  httpd_handle_t server_ = nullptr;
  bool reboot_requested_ = false;
};

}  // namespace printsphere
