#include "printsphere/application.hpp"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/status_resolver.hpp"

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.app";
constexpr TickType_t kStopBannerDuration = pdMS_TO_TICKS(12000);

bool local_print_is_live(const PrinterSnapshot& snapshot) {
  return snapshot.print_active || snapshot.lifecycle == PrintLifecycleState::kPreparing ||
         snapshot.lifecycle == PrintLifecycleState::kPrinting ||
         snapshot.lifecycle == PrintLifecycleState::kPaused;
}

bool cloud_print_is_live(const BambuCloudSnapshot& snapshot) {
  return snapshot.lifecycle == PrintLifecycleState::kPreparing ||
         snapshot.lifecycle == PrintLifecycleState::kPrinting ||
         snapshot.lifecycle == PrintLifecycleState::kPaused;
}

bool tick_deadline_active(TickType_t deadline, TickType_t now) {
  return deadline != 0 && static_cast<int32_t>(deadline - now) > 0;
}
}

Application::Application()
    : setup_portal_(config_store_, wifi_manager_, cloud_client_, printer_client_, camera_client_,
                    ui_) {
  cloud_client_.set_config_store(&config_store_);
}

void Application::run() {
  ESP_LOGI(kTag, "Bootstrapping native PrintSphere project");

  ESP_ERROR_CHECK(config_store_.initialize());
  ESP_ERROR_CHECK(wifi_manager_.initialize_network_stack());
  ESP_ERROR_CHECK(wifi_manager_.start_setup_access_point(config_store_.load_device_name()));

  const WifiCredentials wifi_credentials = config_store_.load_wifi_credentials();
  if (wifi_credentials.is_configured()) {
    const esp_err_t wifi_err = wifi_manager_.connect_station(wifi_credentials);
    if (wifi_err != ESP_OK) {
      ESP_LOGW(kTag, "Stored Wi-Fi connect failed: %s", esp_err_to_name(wifi_err));
    }
  }

  ESP_ERROR_CHECK(setup_portal_.start());
  ESP_ERROR_CHECK(pmu_manager_.initialize());
  ESP_LOGI(kTag, "Heap status: internal=%u bytes psram=%u bytes",
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  ui_.set_arc_color_scheme(config_store_.load_arc_color_scheme());
  ESP_ERROR_CHECK(ui_.initialize());

  const BambuCloudCredentials cloud_credentials = config_store_.load_cloud_credentials();
  status_source_preference_ = config_store_.load_status_source_preference();
  const PrinterConnection printer_connection = config_store_.load_printer_config();
  cloud_client_.configure(cloud_credentials, printer_connection.serial);
  ESP_ERROR_CHECK(cloud_client_.start());

  printer_client_.configure(printer_connection);
  ESP_ERROR_CHECK(printer_client_.start());
  camera_client_.configure(printer_connection);
  ESP_ERROR_CHECK(camera_client_.start());

  ESP_LOGI(kTag, "Bootstrap complete");

  while (true) {
    const TickType_t now_tick = xTaskGetTickCount();
    const bool wifi_connected = wifi_manager_.is_station_connected();
    const std::string wifi_ip = wifi_manager_.station_ip();
    cloud_client_.set_network_ready(wifi_connected);
    printer_client_.set_network_ready(wifi_connected);
    camera_client_.set_network_ready(wifi_connected);
    local_printer_enabled_ = printer_client_.is_configured();

    PrinterSnapshot local_snapshot = printer_client_.snapshot();
    local_snapshot.wifi_connected = wifi_connected;
    local_snapshot.wifi_ip = wifi_ip;
    local_snapshot.setup_ap_active = wifi_manager_.is_setup_access_point_active();
    local_snapshot.setup_ap_ssid = wifi_manager_.setup_access_point_ssid();
    local_snapshot.setup_ap_password = wifi_manager_.setup_access_point_password();
    local_snapshot.setup_ap_ip = wifi_manager_.setup_access_point_ip();
    if (last_local_print_live_ && local_snapshot.non_error_stop) {
      stop_banner_until_tick_ = now_tick + kStopBannerDuration;
    } else if (!local_snapshot.non_error_stop) {
      stop_banner_until_tick_ = 0;
    }
    local_snapshot.show_stop_banner =
        local_snapshot.non_error_stop && tick_deadline_active(stop_banner_until_tick_, now_tick);
    resolve_ui_state(local_snapshot);

    const BambuCloudSnapshot cloud_snapshot = cloud_client_.snapshot();
    if (local_print_is_live(local_snapshot) || cloud_print_is_live(cloud_snapshot)) {
      print_activity_seen_this_session_ = true;
    }
    PrinterSnapshot snapshot =
        merge_status_sources(local_snapshot, local_printer_enabled_, cloud_snapshot,
                             status_source_preference_, wifi_connected, wifi_ip,
                             print_activity_seen_this_session_);
    snapshot.setup_ap_active = local_snapshot.setup_ap_active;
    snapshot.setup_ap_ssid = local_snapshot.setup_ap_ssid;
    snapshot.setup_ap_password = local_snapshot.setup_ap_password;
    snapshot.setup_ap_ip = local_snapshot.setup_ap_ip;
    snapshot.show_stop_banner =
        snapshot.non_error_stop && tick_deadline_active(stop_banner_until_tick_, now_tick);

    const PowerSnapshot power = pmu_manager_.sample();
    if (power.available) {
      snapshot.battery_percent = power.battery_percent;
      snapshot.charging = power.charging;
      snapshot.usb_present = power.usb_present;
      snapshot.pmu_temp_c = power.temperature_c;
    }

    const bool preview_page_active = ui_.is_page2_active();
    const bool camera_enabled =
        local_printer_enabled_ && wifi_connected && ui_.is_camera_page_active() &&
        ui_.screen_power_mode() != ScreenPowerMode::kOff;
    camera_client_.set_enabled(camera_enabled);
    if (ui_.consume_camera_refresh_request()) {
      camera_client_.request_refresh();
    }

    const P1sCameraSnapshot camera_snapshot = camera_client_.snapshot();
    snapshot.camera_connected = camera_snapshot.connected;
    snapshot.camera_detail = camera_snapshot.detail;
    snapshot.camera_blob = camera_snapshot.frame_blob;
    snapshot.camera_width = camera_snapshot.width;
    snapshot.camera_height = camera_snapshot.height;

    resolve_ui_state(snapshot);
    ui_.apply_snapshot(snapshot);
    last_local_print_live_ = local_print_is_live(local_snapshot);

    const bool on_battery = power.available && power.battery_present && !power.usb_present;
    const bool camera_page_active = ui_.is_camera_page_active();
    cloud_client_.set_preview_fetch_enabled(preview_page_active);
    const bool keep_screen_awake = snapshot.print_active || camera_page_active;
    ui_.update_power_save(on_battery, keep_screen_awake);

    cloud_client_.set_low_power_mode(camera_page_active ||
                                     (on_battery && ui_.is_low_power_mode_active() &&
                                      !snapshot.print_active));

    const TickType_t loop_delay =
        (snapshot.print_active || camera_page_active || !ui_.is_low_power_mode_active())
            ? pdMS_TO_TICKS(500)
            : pdMS_TO_TICKS(1500);
    vTaskDelay(loop_delay);
  }
}

}  // namespace printsphere
