#include "printsphere/application.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "printsphere/error_lookup.hpp"
#include "printsphere/status_resolver.hpp"

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.app";
constexpr TickType_t kStopBannerDuration = pdMS_TO_TICKS(12000);
constexpr TickType_t kHybridCloudFallbackDelayLocalFirst = pdMS_TO_TICKS(35000);
constexpr TickType_t kHybridCloudFallbackDelayCloudFirst = pdMS_TO_TICKS(12000);
constexpr TickType_t kHybridCameraCloudCooldown = pdMS_TO_TICKS(8000);
constexpr TickType_t kScreenOffTouchWakePollSlice = pdMS_TO_TICKS(25);
constexpr uint64_t kChamberLightOverrideMs = 6000;

esp_err_t configure_power_management() {
#if CONFIG_PM_ENABLE
  esp_pm_config_t pm_config = {};
  pm_config.max_freq_mhz = 240;
  pm_config.min_freq_mhz = 80;
  pm_config.light_sleep_enable = false;
  ESP_RETURN_ON_ERROR(esp_pm_configure(&pm_config), kTag, "esp_pm_configure failed");
  ESP_LOGI(kTag, "Power management enabled: DFS 80-240 MHz, light sleep off");
#else
  ESP_LOGI(kTag, "Power management disabled in sdkconfig (CONFIG_PM_ENABLE=n)");
#endif
  return ESP_OK;
}

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

bool hybrid_local_status_ready(const PrinterSnapshot& snapshot) {
  return !snapshot.raw_status.empty() || !snapshot.raw_stage.empty() ||
         snapshot.progress_percent > 0.0f || snapshot.current_layer > 0U ||
         snapshot.total_layers > 0U || snapshot.nozzle_temp_c > 0.0f ||
         snapshot.bed_temp_c > 0.0f || snapshot.chamber_temp_c > 0.0f ||
         snapshot.secondary_nozzle_temp_c > 0.0f || snapshot.print_error_code != 0 ||
         !snapshot.hms_codes.empty() ||
         snapshot.hms_alert_count > 0U;
}

PrinterModel preferred_model_for_routing(const PrinterSnapshot& local_snapshot,
                                         const BambuCloudSnapshot& cloud_snapshot) {
  if (cloud_snapshot.model != PrinterModel::kUnknown) {
    return cloud_snapshot.model;
  }
  return local_snapshot.local_model;
}

bool hybrid_prefers_cloud_status(const PrinterSnapshot& local_snapshot,
                                 const BambuCloudSnapshot& cloud_snapshot) {
  return printer_model_prefers_cloud_status(
      preferred_model_for_routing(local_snapshot, cloud_snapshot));
}

bool hybrid_local_status_supported(const PrinterSnapshot& local_snapshot,
                                   const BambuCloudSnapshot& cloud_snapshot) {
  return printer_model_supports_local_status(
      preferred_model_for_routing(local_snapshot, cloud_snapshot));
}

TickType_t hybrid_cloud_fallback_delay(const PrinterSnapshot& local_snapshot,
                                       const BambuCloudSnapshot& cloud_snapshot) {
  return hybrid_prefers_cloud_status(local_snapshot, cloud_snapshot)
             ? kHybridCloudFallbackDelayCloudFirst
             : kHybridCloudFallbackDelayLocalFirst;
}

void wait_for_next_iteration(Ui& ui, TickType_t delay) {
  TickType_t remaining = delay;
  while (remaining > 0) {
    const bool touch_wake_poll_active = ui.screen_power_mode() == ScreenPowerMode::kOff;
    const TickType_t slice =
        (touch_wake_poll_active && remaining > kScreenOffTouchWakePollSlice)
            ? kScreenOffTouchWakePollSlice
            : remaining;
    vTaskDelay(slice);
    remaining -= slice;

    if (touch_wake_poll_active && gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
      // The LVGL worker is paused while the screen is off, so a short tap can
      // be missed if the main loop sleeps for the full low-power interval.
      // Poll the raw touch IRQ in short slices so wake feels immediate.
      ui.request_wake_display();
      break;
    }
  }
}
}

Application::Application()
    : setup_portal_(config_store_, wifi_manager_, cloud_client_, printer_client_, camera_client_,
                    ui_, pmu_manager_) {
  cloud_client_.set_config_store(&config_store_);
}

void Application::run() {
  esp_log_level_set("mbedtls", ESP_LOG_WARN);
  ESP_LOGI(kTag, "Bootstrapping native PrintSphere project");

  ESP_ERROR_CHECK(config_store_.initialize());
  ESP_ERROR_CHECK(configure_power_management());
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
  ui_.set_display_rotation(config_store_.load_display_rotation());
  ui_.set_battery_display_policy(config_store_.load_battery_display_policy());
  filament_wake_enabled_ = config_store_.load_filament_wake_enabled();
  filament_anim_enabled_ = config_store_.load_filament_anim_enabled();
  ESP_ERROR_CHECK(ui_.initialize());
  if (!initialize_error_lookup_storage()) {
    ESP_LOGW(kTag, "Embedded error lookup unavailable; falling back to generic error text");
  }

  const BambuCloudCredentials cloud_credentials = config_store_.load_cloud_credentials();
  source_mode_ = config_store_.load_source_mode();
  const PrinterConnection printer_connection = config_store_.load_active_printer_profile().to_connection();
  cloud_client_.configure(cloud_credentials, printer_connection.serial);
  ESP_ERROR_CHECK(cloud_client_.start());

  printer_client_.configure(printer_connection);
  ESP_ERROR_CHECK(printer_client_.start());
  camera_client_.configure(printer_connection);
  ESP_ERROR_CHECK(camera_client_.start());

  ESP_LOGI(kTag, "Bootstrap complete");

  while (true) {
    const TickType_t now_tick = xTaskGetTickCount();
    const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
    if (ui_.consume_portal_unlock_request()) {
      setup_portal_.request_unlock_pin();
    }
    const int switch_idx = ui_.consume_printer_switch_request();
    if (switch_idx >= 0 &&
        static_cast<uint8_t>(switch_idx) != config_store_.load_active_printer_index()) {
      config_store_.save_active_printer_index(static_cast<uint8_t>(switch_idx));
      const PrinterConnection new_conn = config_store_.load_active_printer_profile().to_connection();
      printer_client_.configure(new_conn);
      camera_client_.configure(new_conn);
      cloud_client_.configure(config_store_.load_cloud_credentials(), new_conn.serial);
      ESP_LOGI(kTag, "Switched active printer to profile %d", switch_idx);
    }
    if (ui_.is_config_page_active()) {
      const auto profiles = config_store_.load_printer_profiles();
      const uint8_t active_idx = config_store_.load_active_printer_index();
      const bool local_connected = printer_client_.snapshot().local_connected;
      std::vector<Ui::PrinterCardInfo> cards;
      cards.reserve(profiles.size());
      for (const auto& p : profiles) {
        Ui::PrinterCardInfo ci;
        ci.index = p.index;
        ci.name = p.display_name;
        ci.model = p.model;
        ci.host = p.host;
        ci.active = (p.index == active_idx);
        ci.connected = ci.active && local_connected;
        cards.push_back(std::move(ci));
      }
      ui_.update_printer_cards(cards);
    }
    const PortalAccessSnapshot portal_access = setup_portal_.access_snapshot();
    const bool wifi_connected = wifi_manager_.is_station_connected();
    const std::string wifi_ip = wifi_manager_.station_ip();
    const bool page_transition_active = ui_.is_page_transition_active();
    const bool preview_page_active = ui_.is_page2_active();
    const bool camera_page_active = ui_.is_camera_page_active();
    source_mode_ = config_store_.load_source_mode();
    const bool local_network_ready = wifi_connected && source_mode_ != SourceMode::kCloudOnly;
    printer_client_.set_network_ready(local_network_ready);
    camera_client_.set_network_ready(local_network_ready);
    local_printer_enabled_ = printer_client_.is_configured();
    const bool camera_page_visible = ui_.is_camera_page_visible();
    const bool camera_enabled =
        source_mode_ != SourceMode::kCloudOnly && local_printer_enabled_ && wifi_connected &&
        (camera_page_active || (page_transition_active && camera_page_visible)) &&
        ui_.screen_power_mode() != ScreenPowerMode::kOff;
    camera_client_.set_enabled(camera_enabled);
    if (ui_.consume_camera_refresh_request()) {
      camera_client_.request_refresh();
    }

    PrinterSnapshot local_snapshot = printer_client_.snapshot();
    local_snapshot.wifi_connected = wifi_connected;
    local_snapshot.wifi_ip = wifi_ip;
    local_snapshot.setup_ap_active = wifi_manager_.is_setup_access_point_active();
    local_snapshot.setup_ap_ssid = wifi_manager_.setup_access_point_ssid();
    local_snapshot.setup_ap_password = wifi_manager_.setup_access_point_password();
    local_snapshot.setup_ap_ip = wifi_manager_.setup_access_point_ip();
    camera_client_.observe_printer_snapshot(local_snapshot);
    if (last_local_print_live_ && local_snapshot.non_error_stop) {
      stop_banner_until_tick_ = now_tick + kStopBannerDuration;
    } else if (source_mode_ != SourceMode::kCloudOnly && !local_snapshot.non_error_stop) {
      stop_banner_until_tick_ = 0;
    }
    local_snapshot.show_stop_banner =
        local_snapshot.non_error_stop && tick_deadline_active(stop_banner_until_tick_, now_tick);
    resolve_ui_state(local_snapshot);

    const bool source_mode_changed = source_mode_ != last_source_mode_;
    const bool wifi_reconnected = wifi_connected && !last_wifi_connected_;
    const bool wifi_lost = !wifi_connected && last_wifi_connected_;
    if (source_mode_ == SourceMode::kHybrid && last_camera_page_active_ && !camera_page_visible &&
        wifi_connected) {
      hybrid_camera_cooldown_deadline_ = now_tick + kHybridCameraCloudCooldown;
      ESP_LOGD(kTag, "Hybrid mode: delaying cloud path briefly after camera activity");
    }
    if (source_mode_changed || wifi_lost) {
      hybrid_cloud_gate_open_ = false;
      hybrid_cloud_gate_deadline_ = 0;
      hybrid_camera_cooldown_deadline_ = 0;
    }

    BambuCloudSnapshot cloud_snapshot = cloud_client_.snapshot();
    const bool hybrid_prefers_cloud =
        source_mode_ == SourceMode::kHybrid &&
        hybrid_prefers_cloud_status(local_snapshot, cloud_snapshot);
    const bool hybrid_local_status_supported_now =
        source_mode_ != SourceMode::kCloudOnly &&
        hybrid_local_status_supported(local_snapshot, cloud_snapshot);

    bool cloud_network_ready = wifi_connected;
    const bool hybrid_camera_cooldown_active =
        source_mode_ == SourceMode::kHybrid &&
        tick_deadline_active(hybrid_camera_cooldown_deadline_, now_tick);
    if (source_mode_ == SourceMode::kLocalOnly) {
      cloud_network_ready = false;
    } else if (source_mode_ == SourceMode::kHybrid) {
      if (!local_printer_enabled_) {
        hybrid_cloud_gate_open_ = true;
        hybrid_cloud_gate_deadline_ = 0;
      } else if (!wifi_connected) {
        hybrid_cloud_gate_open_ = false;
      } else if (hybrid_prefers_cloud || !hybrid_local_status_supported_now) {
        hybrid_cloud_gate_open_ = true;
        hybrid_cloud_gate_deadline_ = 0;
      } else {
        if (source_mode_changed || wifi_reconnected || hybrid_cloud_gate_deadline_ == 0) {
          hybrid_cloud_gate_open_ = false;
          hybrid_cloud_gate_deadline_ =
              now_tick + hybrid_cloud_fallback_delay(local_snapshot, cloud_snapshot);
          ESP_LOGI(kTag, "Hybrid mode: delaying cloud path until local status or fallback timeout");
        }

        if (!hybrid_cloud_gate_open_ && hybrid_local_status_ready(local_snapshot)) {
          hybrid_cloud_gate_open_ = true;
          ESP_LOGI(kTag, "Hybrid mode: local status received, enabling cloud path");
        } else if (!hybrid_cloud_gate_open_ &&
                   !tick_deadline_active(hybrid_cloud_gate_deadline_, now_tick)) {
          hybrid_cloud_gate_open_ = true;
          ESP_LOGW(kTag, "Hybrid mode: local status timeout, enabling cloud path fallback");
        }

        cloud_network_ready = hybrid_cloud_gate_open_;
      }
    }
    const bool hybrid_local_path_healthy =
        source_mode_ == SourceMode::kHybrid && local_network_ready && local_printer_enabled_ &&
        local_snapshot.local_connected && hybrid_local_status_supported_now && !hybrid_prefers_cloud;
    if (source_mode_ == SourceMode::kHybrid && hybrid_local_path_healthy && !preview_page_active) {
      cloud_network_ready = false;
    }
    const bool cloud_live_mqtt_enabled =
        cloud_network_ready &&
        (source_mode_ == SourceMode::kCloudOnly ||
         (source_mode_ == SourceMode::kHybrid &&
          (hybrid_prefers_cloud || !hybrid_local_path_healthy)));
    const bool pause_cloud_fetches =
        source_mode_ == SourceMode::kHybrid &&
        (camera_page_active || page_transition_active || hybrid_camera_cooldown_active ||
         !cloud_network_ready);
    cloud_client_.set_network_ready(cloud_network_ready);
    cloud_client_.set_live_mqtt_enabled(cloud_live_mqtt_enabled);
    cloud_client_.set_fetch_paused(pause_cloud_fetches);

    cloud_snapshot = cloud_client_.snapshot();
    if (source_mode_ == SourceMode::kCloudOnly) {
      if (last_cloud_print_live_ && cloud_snapshot.non_error_stop) {
        stop_banner_until_tick_ = now_tick + kStopBannerDuration;
      } else if (!cloud_snapshot.non_error_stop) {
        stop_banner_until_tick_ = 0;
      }
    }
    if (local_print_is_live(local_snapshot) || cloud_print_is_live(cloud_snapshot)) {
      print_activity_seen_this_session_ = true;
    }
    auto build_merged_snapshot = [&](const PrinterSnapshot& current_local_snapshot,
                                     const BambuCloudSnapshot& current_cloud_snapshot) {
      PrinterSnapshot merged =
          merge_status_sources(current_local_snapshot, local_printer_enabled_, current_cloud_snapshot,
                               source_mode_, now_ms, wifi_connected, wifi_ip,
                               print_activity_seen_this_session_);
      merged.setup_ap_active = current_local_snapshot.setup_ap_active;
      merged.setup_ap_ssid = current_local_snapshot.setup_ap_ssid;
      merged.setup_ap_password = current_local_snapshot.setup_ap_password;
      merged.setup_ap_ip = current_local_snapshot.setup_ap_ip;
      merged.show_stop_banner =
          merged.non_error_stop && tick_deadline_active(stop_banner_until_tick_, now_tick);
      merged.preview_page_available = source_mode_ != SourceMode::kLocalOnly;
      merged.camera_page_available = source_mode_ != SourceMode::kCloudOnly;
      return merged;
    };
    auto apply_chamber_light_override = [&](PrinterSnapshot* target_snapshot) {
      if (target_snapshot == nullptr) {
        return;
      }
      if (!chamber_light_override_active_) {
        return;
      }
      if (now_ms >= chamber_light_override_until_ms_) {
        chamber_light_override_active_ = false;
        chamber_light_override_until_ms_ = 0;
        return;
      }
      target_snapshot->chamber_light_supported = true;
      target_snapshot->chamber_light_state_known = true;
      target_snapshot->chamber_light_on = chamber_light_override_on_;
    };
    PrinterSnapshot snapshot = build_merged_snapshot(local_snapshot, cloud_snapshot);
    apply_chamber_light_override(&snapshot);

    if (ui_.consume_chamber_light_toggle_request()) {
      const bool requested_on =
          !snapshot.chamber_light_state_known || !snapshot.chamber_light_on;
      bool command_sent = false;

      if (source_mode_ == SourceMode::kLocalOnly) {
        command_sent = printer_client_.set_chamber_light(requested_on);
        if (command_sent) {
          local_snapshot.chamber_light_supported = true;
          local_snapshot.chamber_light_state_known = true;
          local_snapshot.chamber_light_on = requested_on;
        }
      } else if (source_mode_ == SourceMode::kCloudOnly) {
        command_sent = cloud_client_.set_chamber_light(requested_on);
        if (command_sent) {
          cloud_snapshot.chamber_light_supported = true;
          cloud_snapshot.chamber_light_state_known = true;
          cloud_snapshot.chamber_light_on = requested_on;
        }
      } else {
        const bool local_light_available =
            !hybrid_prefers_cloud && hybrid_local_status_supported_now && local_network_ready &&
            local_printer_enabled_ &&
            (local_snapshot.local_connected ||
             printer_model_has_chamber_light(local_snapshot.local_model));
        if (local_light_available) {
          command_sent = printer_client_.set_chamber_light(requested_on);
          if (command_sent) {
            local_snapshot.chamber_light_supported = true;
            local_snapshot.chamber_light_state_known = true;
            local_snapshot.chamber_light_on = requested_on;
          }
        }
        if (!command_sent && cloud_network_ready &&
            (cloud_snapshot.connected || printer_model_has_chamber_light(cloud_snapshot.model))) {
          command_sent = cloud_client_.set_chamber_light(requested_on);
          if (command_sent) {
            cloud_snapshot.chamber_light_supported = true;
            cloud_snapshot.chamber_light_state_known = true;
            cloud_snapshot.chamber_light_on = requested_on;
          }
        }
      }

      if (!command_sent) {
        ESP_LOGW(kTag, "Chamber light toggle failed in %s mode", to_string(source_mode_));
      } else {
        chamber_light_override_active_ = true;
        chamber_light_override_on_ = requested_on;
        chamber_light_override_until_ms_ = now_ms + kChamberLightOverrideMs;
        snapshot = build_merged_snapshot(local_snapshot, cloud_snapshot);
        apply_chamber_light_override(&snapshot);
      }
    }

    const PowerSnapshot power = pmu_manager_.sample();
    if (power.available) {
      snapshot.battery_percent = power.battery_percent;
      snapshot.battery_present = power.battery_present;
      snapshot.charging = power.charging;
      snapshot.usb_present = power.usb_present;
      snapshot.pmu_temp_c = power.temperature_c;
    }

    const P1sCameraSnapshot camera_snapshot = camera_client_.snapshot();
    if (source_mode_ == SourceMode::kCloudOnly || !local_printer_enabled_) {
      snapshot.camera_connected = false;
      snapshot.camera_detail = source_mode_ == SourceMode::kCloudOnly
                                   ? "Camera unavailable in cloud-only mode"
                                   : "Local camera not configured";
      snapshot.camera_blob.reset();
      snapshot.camera_width = 0;
      snapshot.camera_height = 0;
      snapshot.camera_source = FieldSource::kNone;
    } else {
      snapshot.camera_connected = camera_snapshot.connected;
      snapshot.camera_detail = camera_snapshot.detail;
      snapshot.camera_blob = camera_snapshot.frame_blob;
      snapshot.camera_width = camera_snapshot.width;
      snapshot.camera_height = camera_snapshot.height;
      if (!camera_page_active) {
        snapshot.camera_blob.reset();
        snapshot.camera_width = 0;
        snapshot.camera_height = 0;
      }
    }

    // Detect filament stage before resolve_ui_state for animation suppression and wake logic.
    const bool is_filament = is_filament_stage(snapshot.stage);
    const bool is_external_spool = snapshot.tray_tar == 254;

    // When filament animation is disabled, suppress the loading/unloading stage for AMS auto
    // changes so resolve_ui_state treats it as normal printing (no arc animation).
    if (!filament_anim_enabled_ && is_filament && !is_external_spool) {
      snapshot.stage.clear();
      snapshot.raw_stage.clear();
    }

    resolve_ui_state(snapshot);
    // Store portal state first (lock-free), then apply_snapshot uses it
    // inside the same LVGL lock section — eliminates a separate lock acquisition.
    ui_.set_portal_access_state(portal_access.lock_enabled,
                                portal_access.request_authorized, portal_access.session_active,
                                portal_access.pin_active, portal_access.pin_code,
                                portal_access.pin_remaining_s, portal_access.session_remaining_s);
    ui_.apply_snapshot(snapshot);
    last_local_print_live_ = local_print_is_live(local_snapshot);
    last_cloud_print_live_ = cloud_print_is_live(cloud_snapshot);

    const bool on_battery = power.available && power.battery_present && !power.usb_present;
    const bool preview_pipeline_enabled =
        source_mode_ == SourceMode::kCloudOnly || preview_page_active;
    cloud_client_.set_preview_fetch_enabled(source_mode_ != SourceMode::kLocalOnly &&
                                            preview_pipeline_enabled);
    bool keep_screen_awake;
    if (filament_wake_enabled_ && is_filament && !is_external_spool) {
      // AMS auto filament change: suppress wake, let display sleep
      keep_screen_awake = camera_page_active || page_transition_active;
    } else {
      keep_screen_awake = snapshot.print_active || camera_page_active || page_transition_active;
    }
    if (filament_wake_enabled_ && is_filament && is_external_spool) {
      ui_.request_wake_display();
    }
    ui_.update_power_save(on_battery, keep_screen_awake);

    cloud_client_.set_low_power_mode(camera_page_active || page_transition_active ||
                                     (on_battery && ui_.is_low_power_mode_active() &&
                                      !snapshot.print_active));

    const TickType_t loop_delay =
        (snapshot.print_active || camera_page_active || page_transition_active ||
         !ui_.is_low_power_mode_active())
            ? pdMS_TO_TICKS(page_transition_active ? 100 : 500)
            : pdMS_TO_TICKS(1500);
    last_source_mode_ = source_mode_;
    last_wifi_connected_ = wifi_connected;
    last_camera_page_active_ = camera_page_visible;
    wait_for_next_iteration(ui_, loop_delay);
  }
}

}  // namespace printsphere
