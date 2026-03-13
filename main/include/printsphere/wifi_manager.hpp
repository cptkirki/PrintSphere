#pragma once

#include <string>
#include <string_view>

#include "esp_err.h"
#include "esp_event_base.h"
#include "esp_netif.h"
#include "printsphere/config_store.hpp"

namespace printsphere {

class WifiManager {
 public:
  esp_err_t initialize_network_stack();
  esp_err_t start_setup_access_point(std::string_view device_name);
  esp_err_t connect_station(const WifiCredentials& credentials);
  bool is_station_connected() const { return sta_connected_; }
  std::string station_ip() const { return sta_ip_; }
  bool is_setup_access_point_active() const { return setup_ap_active_; }
  std::string setup_access_point_ssid() const { return ap_ssid_; }
  std::string setup_access_point_password() const;
  std::string setup_access_point_ip() const;

 private:
  static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                            void* event_data);
  esp_err_t ensure_wifi_started();
  esp_err_t set_setup_access_point_enabled(bool enabled);
  void on_wifi_event(int32_t event_id, void* event_data);
  void on_ip_event(int32_t event_id, void* event_data);

  bool netif_ready_ = false;
  bool wifi_ready_ = false;
  bool handlers_registered_ = false;
  bool wifi_started_ = false;
  bool sta_should_connect_ = false;
  bool sta_connected_ = false;
  bool setup_ap_active_ = false;
  uint8_t sta_disconnect_retries_ = 0;
  esp_netif_t* ap_netif_ = nullptr;
  esp_netif_t* sta_netif_ = nullptr;
  std::string ap_ssid_;
  std::string sta_ip_;
  WifiCredentials station_credentials_{};
};

}  // namespace printsphere
