#pragma once

#include <string>
#include <mutex>
#include <string_view>
#include <vector>

#include "esp_err.h"
#include "esp_event_base.h"
#include "esp_netif.h"
#include "printsphere/config_store.hpp"

namespace printsphere {

struct VisibleWifiNetwork {
  std::string ssid;
  int rssi = -127;
  bool auth_required = true;
};

class WifiManager {
 public:
  esp_err_t initialize_network_stack();
  esp_err_t start_setup_access_point(std::string_view device_name);
  esp_err_t connect_station(const WifiCredentials& credentials);
  struct Snapshot { bool connected; bool setup_ap_active; std::string ip; std::string ap_ssid; };
  Snapshot snapshot() const {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    return {sta_connected_, setup_ap_active_, sta_ip_, ap_ssid_};
  }
  bool is_station_connected() const { return snapshot().connected; }
  std::string station_ip() const { return snapshot().ip; }
  bool is_setup_access_point_active() const { return snapshot().setup_ap_active; }
  std::string setup_access_point_ssid() const { return snapshot().ap_ssid; }
  std::string setup_access_point_password() const;
  std::string setup_access_point_ip() const;
  std::vector<std::string> scan_visible_networks() const;
  std::vector<VisibleWifiNetwork> scan_visible_network_details() const;

 private:
  mutable std::recursive_mutex state_mutex_{};
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
