#include "printsphere/wifi_manager.hpp"

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.wifi";
constexpr char kSetupPassword[] = "printsphere";
constexpr char kSetupApIp[] = "192.168.4.1";
constexpr uint8_t kSetupApRetryThreshold = 3;
}  // namespace

esp_err_t WifiManager::initialize_network_stack() {
  if (!netif_ready_) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      return err;
    }

    netif_ready_ = true;
  }

  if (!wifi_ready_) {
    if (ap_netif_ == nullptr) {
      ap_netif_ = esp_netif_create_default_wifi_ap();
    }
    if (sta_netif_ == nullptr) {
      sta_netif_ = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), kTag, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), kTag, "esp_wifi_set_ps failed");

    if (!handlers_registered_) {
      ESP_RETURN_ON_ERROR(
          esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::event_handler, this),
          kTag, "wifi handler register failed");
      ESP_RETURN_ON_ERROR(
          esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::event_handler, this),
          kTag, "ip handler register failed");
      handlers_registered_ = true;
    }

    wifi_ready_ = true;
  }

  return ESP_OK;
}

esp_err_t WifiManager::start_setup_access_point(std::string_view device_name) {
  if (!wifi_ready_) {
    return ESP_ERR_INVALID_STATE;
  }

  ap_ssid_.assign(device_name.data(), device_name.size());
  ap_ssid_.append("-Setup");

  wifi_config_t ap_config = {};
  std::snprintf(reinterpret_cast<char*>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s",
                ap_ssid_.c_str());
  std::snprintf(reinterpret_cast<char*>(ap_config.ap.password), sizeof(ap_config.ap.password), "%s",
                kSetupPassword);
  ap_config.ap.ssid_len = static_cast<uint8_t>(ap_ssid_.size());
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.pmf_cfg.required = false;

  ESP_RETURN_ON_ERROR(ensure_wifi_started(), kTag, "ensure_wifi_started failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), kTag, "set AP config failed");
  setup_ap_active_ = true;

  uint8_t mac[6] = {};
  if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
    ESP_LOGI(kTag, "Setup AP ready: SSID=%s PASS=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             ap_ssid_.c_str(), kSetupPassword, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    ESP_LOGI(kTag, "Setup AP ready: SSID=%s PASS=%s", ap_ssid_.c_str(), kSetupPassword);
  }

  return ESP_OK;
}

esp_err_t WifiManager::connect_station(const WifiCredentials& credentials) {
  if (!wifi_ready_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!credentials.is_configured()) {
    return ESP_ERR_INVALID_ARG;
  }

  station_credentials_ = credentials;
  sta_should_connect_ = true;
  sta_connected_ = false;
  sta_disconnect_retries_ = 0;
  sta_ip_.clear();

  wifi_config_t sta_config = {};
  std::snprintf(reinterpret_cast<char*>(sta_config.sta.ssid), sizeof(sta_config.sta.ssid), "%s",
                credentials.ssid.c_str());
  std::snprintf(reinterpret_cast<char*>(sta_config.sta.password),
                sizeof(sta_config.sta.password), "%s", credentials.password.c_str());
  sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  sta_config.sta.pmf_cfg.capable = true;
  sta_config.sta.pmf_cfg.required = false;

  ESP_RETURN_ON_ERROR(ensure_wifi_started(), kTag, "ensure_wifi_started failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), kTag, "set STA config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_connect(), kTag, "esp_wifi_connect failed");

  ESP_LOGI(kTag, "Connecting to Wi-Fi SSID=%s", credentials.ssid.c_str());
  return ESP_OK;
}

std::string WifiManager::setup_access_point_password() const {
  return kSetupPassword;
}

std::string WifiManager::setup_access_point_ip() const {
  return kSetupApIp;
}

void WifiManager::event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                void* event_data) {
  auto* manager = static_cast<WifiManager*>(arg);
  if (manager == nullptr) {
    return;
  }

  if (event_base == WIFI_EVENT) {
    manager->on_wifi_event(event_id, event_data);
    return;
  }

  if (event_base == IP_EVENT) {
    manager->on_ip_event(event_id, event_data);
  }
}

esp_err_t WifiManager::ensure_wifi_started() {
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "esp_wifi_set_mode failed");
  if (!wifi_started_) {
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "esp_wifi_start failed");
    wifi_started_ = true;
  }
  return ESP_OK;
}

esp_err_t WifiManager::set_setup_access_point_enabled(bool enabled) {
  if (!wifi_started_) {
    return ESP_OK;
  }

  if (enabled) {
    if (setup_ap_active_) {
      return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "enable APSTA failed");
    setup_ap_active_ = true;
    ESP_LOGI(kTag, "Setup AP re-enabled: SSID=%s", ap_ssid_.c_str());
    return ESP_OK;
  }

  if (!setup_ap_active_) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "disable setup AP failed");
  setup_ap_active_ = false;
  ESP_LOGI(kTag, "Setup AP disabled after station connect");
  return ESP_OK;
}

void WifiManager::on_wifi_event(int32_t event_id, void* event_data) {
  switch (event_id) {
    case WIFI_EVENT_STA_START:
      if (sta_should_connect_ && station_credentials_.is_configured()) {
        esp_wifi_connect();
      }
      break;

    case WIFI_EVENT_STA_DISCONNECTED: {
      sta_connected_ = false;
      sta_ip_.clear();
      ++sta_disconnect_retries_;
      wifi_event_sta_disconnected_t* disconnect =
          static_cast<wifi_event_sta_disconnected_t*>(event_data);
      const int reason = disconnect != nullptr ? static_cast<int>(disconnect->reason) : -1;
      if (sta_should_connect_ && station_credentials_.is_configured()) {
        ESP_LOGW(kTag, "Wi-Fi disconnected (reason=%d), retry %u/%u", reason,
                 static_cast<unsigned int>(sta_disconnect_retries_),
                 static_cast<unsigned int>(kSetupApRetryThreshold));
        esp_wifi_connect();
      }
      if (!ap_ssid_.empty() && sta_disconnect_retries_ >= kSetupApRetryThreshold) {
        const esp_err_t ap_err = set_setup_access_point_enabled(true);
        if (ap_err != ESP_OK) {
          ESP_LOGW(kTag, "Failed to re-enable setup AP: %s", esp_err_to_name(ap_err));
        }
      }
      break;
    }

    default:
      break;
  }
}

void WifiManager::on_ip_event(int32_t event_id, void* event_data) {
  if (event_id != IP_EVENT_STA_GOT_IP || event_data == nullptr) {
    return;
  }

  const auto* got_ip = static_cast<ip_event_got_ip_t*>(event_data);
  char ip_buffer[16] = {};
  std::snprintf(ip_buffer, sizeof(ip_buffer), IPSTR, IP2STR(&got_ip->ip_info.ip));

  sta_connected_ = true;
  sta_disconnect_retries_ = 0;
  sta_ip_ = ip_buffer;
  ESP_LOGI(kTag, "Wi-Fi connected, IP=%s", sta_ip_.c_str());

  const esp_err_t ap_err = set_setup_access_point_enabled(false);
  if (ap_err != ESP_OK) {
    ESP_LOGW(kTag, "Failed to disable setup AP: %s", esp_err_to_name(ap_err));
  }
}

}  // namespace printsphere
