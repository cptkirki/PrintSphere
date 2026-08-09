#include "printsphere/application.hpp"

#include <cstring>
#include <vector>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/error_lookup.hpp"
#include "printsphere/status_resolver.hpp"
#include "printsphere/time_sync.hpp"

#if defined(PRINTSPHERE_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#elif defined(PRINTSPHERE_HW_VARIANT_KNOMI_V2)
#include "bsp/btt_knomi_v2.h"
#else
#error "Unknown PrintSphere hardware variant"
#endif

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.app";
constexpr TickType_t kStopBannerDuration = pdMS_TO_TICKS(12000);
constexpr TickType_t kHybridCameraCloudCooldown = pdMS_TO_TICKS(8000);
constexpr TickType_t kLocalMqttHandoffCooldown = pdMS_TO_TICKS(30000);
constexpr TickType_t kScreenOffTouchWakePollSlice = pdMS_TO_TICKS(25);
constexpr TickType_t kUiCommandWakePollSlice = pdMS_TO_TICKS(50);
constexpr uint64_t kChamberLightOverrideMs = 6000;
// Pause/resume/stop optimistic-state window. Most printers reflect the new
// lifecycle in their next status report (sub-second). 5 s is a safety net so
// the button doesn't lock forever if the command is silently dropped.
constexpr uint64_t kPrintCommandOverrideMs = 5000;

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

bool route_allows_local_jpeg_camera(SourceMode source_mode,
                                    const PrinterSnapshot& local_snapshot,
                                    const BambuCloudSnapshot& cloud_snapshot) {
  if (source_mode == SourceMode::kCloudOnly) {
    return false;
  }

  const PrinterModel model = preferred_model_for_routing(local_snapshot, cloud_snapshot);
  if (printer_model_has_jpeg_camera(model)) {
    return true;
  }
  if (model != PrinterModel::kUnknown) {
    return false;
  }

  if (source_mode == SourceMode::kLocalOnly) {
    return true;
  }
  return source_mode == SourceMode::kHybrid && local_snapshot.local_connected;
}

struct ChamberLightCommandPlan {
  bool try_local = false;
  bool try_cloud = false;
};

ChamberLightCommandPlan chamber_light_command_plan(SourceMode source_mode,
                                                   bool hybrid_prefers_cloud,
                                                   bool hybrid_local_status_supported_now,
                                                   bool local_network_ready,
                                                   bool local_printer_enabled,
                                                   bool cloud_network_ready,
                                                   const PrinterSnapshot& local_snapshot,
                                                   const BambuCloudSnapshot& cloud_snapshot) {
  ChamberLightCommandPlan plan;
  switch (source_mode) {
    case SourceMode::kLocalOnly:
      plan.try_local = true;
      break;
    case SourceMode::kCloudOnly:
      plan.try_cloud = true;
      break;
    case SourceMode::kHybrid:
    default:
      plan.try_local =
          !hybrid_prefers_cloud && hybrid_local_status_supported_now && local_network_ready &&
          local_printer_enabled &&
          (local_snapshot.local_connected ||
           printer_model_has_chamber_light(local_snapshot.local_model));
      plan.try_cloud =
          cloud_network_ready &&
          (cloud_snapshot.connected || printer_model_has_chamber_light(cloud_snapshot.model));
      break;
  }
  return plan;
}

void mark_chamber_light_state(PrinterSnapshot& snapshot, bool on) {
  snapshot.chamber_light_supported = true;
  snapshot.chamber_light_state_known = true;
  snapshot.chamber_light_on = on;
}

void mark_chamber_light_state(BambuCloudSnapshot& snapshot, bool on) {
  snapshot.chamber_light_supported = true;
  snapshot.chamber_light_state_known = true;
  snapshot.chamber_light_on = on;
}

// Decide whether a print-control command should be issued via the local broker,
// the cloud broker, or both. Mirrors the chamber-light routing but does not
// gate on chamber-light capability — pause/resume/stop is universal.
struct PrintCommandPlan {
  bool try_local = false;
  bool try_cloud = false;
};

PrintCommandPlan print_command_plan(SourceMode source_mode, bool hybrid_prefers_cloud,
                                    bool hybrid_local_status_supported_now,
                                    bool local_network_ready, bool local_printer_enabled,
                                    bool cloud_network_ready,
                                    const PrinterSnapshot& local_snapshot,
                                    const BambuCloudSnapshot& cloud_snapshot) {
  PrintCommandPlan plan;
  switch (source_mode) {
    case SourceMode::kLocalOnly:
      plan.try_local = true;
      break;
    case SourceMode::kCloudOnly:
      plan.try_cloud = true;
      break;
    case SourceMode::kHybrid:
    default:
      plan.try_local = !hybrid_prefers_cloud && hybrid_local_status_supported_now &&
                       local_network_ready && local_printer_enabled &&
                       local_snapshot.local_connected;
      plan.try_cloud = cloud_network_ready && cloud_snapshot.connected;
      break;
  }
  return plan;
}

// Resolve the printer lifecycle state implied by a freshly issued print
// command. Pause -> Paused, Resume -> Printing, Stop -> Idle. Used to drive
// the optimistic UI override until the next status report arrives.
PrintLifecycleState lifecycle_after_print_command(PrintCommand cmd) {
  switch (cmd) {
    case PrintCommand::kPause:
      return PrintLifecycleState::kPaused;
    case PrintCommand::kResume:
      return PrintLifecycleState::kPrinting;
    case PrintCommand::kStop:
      return PrintLifecycleState::kIdle;
    case PrintCommand::kNone:
    default:
      return PrintLifecycleState::kUnknown;
  }
}

void wait_for_next_iteration(Ui& ui, TickType_t delay) {
  TickType_t remaining = delay;
  while (remaining > 0) {
    if (ui.has_chamber_light_toggle_request() || ui.has_print_command_request()) {
      break;
    }
    const bool touch_wake_poll_active = ui.screen_power_mode() == ScreenPowerMode::kOff;
    TickType_t slice = remaining;
    if (touch_wake_poll_active && slice > kScreenOffTouchWakePollSlice) {
      slice = kScreenOffTouchWakePollSlice;
    } else if (slice > kUiCommandWakePollSlice) {
      slice = kUiCommandWakePollSlice;
    }
    vTaskDelay(slice);
    remaining -= slice;

    if (ui.has_chamber_light_toggle_request() || ui.has_print_command_request()) {
      break;
    }
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
                    ui_, pmu_manager_, audio_notifier_),
      serial_provisioner_(config_store_, wifi_manager_) {
  cloud_client_.set_config_store(&config_store_);
  // Route printer online/offline events from the Bambu Cloud MQTT feed to the
  // local PrinterClient so it can collapse its reconnect backoff the moment the
  // printer is known to be reachable again. Avoids blind TCP-probe cycles while
  // the printer is powered off or roaming on the LAN.
  cloud_client_.set_printer_presence_callback([this](bool online) {
    printer_client_.notify_cloud_presence(online);
  });
}

void Application::run() {
  esp_log_level_set("mbedtls", ESP_LOG_WARN);
  ESP_LOGI(kTag, "Bootstrapping native PrintSphere project");

  ESP_ERROR_CHECK(config_store_.initialize());
  // Apply persisted timezone before any localtime_r() consumer (UI ETA,
  // logs, etc.). SNTP itself is started later when an IP is acquired.
  time_sync::set_timezone_iana(config_store_.load_timezone_iana());
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
  if (serial_provisioner_.start() != ESP_OK) {
    ESP_LOGW(kTag, "USB Wi-Fi setup unavailable; use the fallback setup access point");
  }
  ESP_ERROR_CHECK(pmu_manager_.initialize());
  ESP_LOGI(kTag, "Heap status: internal=%u bytes psram=%u bytes",
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  ui_.set_arc_color_scheme(config_store_.load_arc_color_scheme());
  ui_.set_display_rotation(config_store_.load_display_rotation());
  ui_.set_battery_display_policy(config_store_.load_battery_display_policy());
  filament_wake_enabled_ = config_store_.load_filament_wake_enabled();
  filament_anim_enabled_ = config_store_.load_filament_anim_enabled();
  audio_notifier_.set_enabled(config_store_.load_audio_enabled());
  audio_notifier_.set_volume_percent(config_store_.load_audio_volume_percent());

  // Mount the LittleFS partition that holds custom sound files.
  // Must be done before loading PCM blobs below.
  {
    const esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path = "/sounds",
        .partition_label = "sounds",
        .partition = nullptr,
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    const esp_err_t lfs_err = esp_vfs_littlefs_register(&lfs_conf);
    if (lfs_err != ESP_OK) {
      ESP_LOGW(kTag, "LittleFS mount failed (%s) - custom sounds unavailable this boot",
               esp_err_to_name(lfs_err));
    } else {
      size_t total = 0, used = 0;
      esp_littlefs_info("sounds", &total, &used);
      ESP_LOGI(kTag, "LittleFS sounds: %u KB total, %u KB used",
               static_cast<unsigned>(total / 1024), static_cast<unsigned>(used / 1024));
    }
  }

  // Per-event enable flags and optional custom PCM blobs.
  for (uint8_t i = 0; i < AudioNotifier::kEventCount; ++i) {
    audio_notifier_.set_event_enabled(
        static_cast<AudioNotifier::Event>(i),
        config_store_.load_audio_event_enabled(i));
    const std::vector<uint8_t> pcm_bytes = config_store_.load_audio_event_pcm(i);
    if (!pcm_bytes.empty() && (pcm_bytes.size() % sizeof(int16_t)) == 0) {
      std::vector<int16_t> samples(pcm_bytes.size() / sizeof(int16_t));
      std::memcpy(samples.data(), pcm_bytes.data(), pcm_bytes.size());
      audio_notifier_.set_event_pcm(static_cast<AudioNotifier::Event>(i), std::move(samples));
    }
  }
  if (audio_notifier_.initialize() != ESP_OK) {
    ESP_LOGW(kTag, "Audio notifier init failed - sound disabled this boot");
  }
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
    const bool source_mode_changed = source_mode_ != last_source_mode_;
    const bool wifi_lost = !wifi_connected && last_wifi_connected_;
    local_printer_enabled_ = printer_client_.is_configured();
    PrinterSnapshot local_snapshot = printer_client_.snapshot();
    if (local_snapshot.local_connected && local_mqtt_handoff_until_tick_.load() != 0) {
      local_mqtt_handoff_until_tick_ = 0;
      ESP_LOGI(kTag, "Local MQTT handoff complete: local MQTT connected");
    }
    const bool camera_page_visible = ui_.is_camera_page_visible();

    if (source_mode_ == SourceMode::kHybrid && last_camera_page_active_ && !camera_page_visible &&
        wifi_connected) {
      hybrid_camera_cooldown_deadline_ = now_tick + kHybridCameraCloudCooldown;
      ESP_LOGD(kTag, "Hybrid mode: delaying cloud path briefly after camera activity");
    }
    if (source_mode_changed || wifi_lost || source_mode_ != SourceMode::kHybrid) {
      hybrid_local_gate_open_ = false;
      hybrid_camera_cooldown_deadline_ = 0;
      // The handoff window is a hybrid-only concept; drop any stale deadline
      // on mode switch / Wi-Fi loss so it cannot block cloud MQTT later.
      local_mqtt_handoff_until_tick_ = 0;
    }

    BambuCloudSnapshot cloud_snapshot = cloud_client_.snapshot();
    const bool hybrid_prefers_cloud =
        source_mode_ == SourceMode::kHybrid &&
        hybrid_prefers_cloud_status(local_snapshot, cloud_snapshot);
    const bool hybrid_local_status_supported_now =
        source_mode_ != SourceMode::kCloudOnly &&
        hybrid_local_status_supported(local_snapshot, cloud_snapshot);
    const PrinterModel routing_model = preferred_model_for_routing(local_snapshot, cloud_snapshot);
    const bool routing_model_has_jpeg_camera = printer_model_has_jpeg_camera(routing_model);
    const bool camera_model_has_jpeg =
        route_allows_local_jpeg_camera(source_mode_, local_snapshot, cloud_snapshot);
    const bool hybrid_camera_cooldown_active =
        source_mode_ == SourceMode::kHybrid &&
        tick_deadline_active(hybrid_camera_cooldown_deadline_, now_tick);
    const bool hybrid_cloud_allows_warm_local =
        !cloud_snapshot.configured ||
        (cloud_snapshot.session_connected && cloud_snapshot.printer_online) ||
        local_snapshot.local_connected;
    const bool hybrid_local_camera_demand =
        source_mode_ == SourceMode::kHybrid && routing_model_has_jpeg_camera &&
        hybrid_cloud_allows_warm_local;
    // Local MQTT is an independent status/command transport. It must not be
    // gated by camera type or cloud presence; only the heavier JPEG camera path
    // remains demand-driven below.
    if (source_mode_ == SourceMode::kHybrid) {
      if (!wifi_connected || !local_printer_enabled_) {
        if (hybrid_local_gate_open_) {
          ESP_LOGI(kTag, "Hybrid mode: disabling local MQTT path");
        }
        hybrid_local_gate_open_ = false;
      } else {
        if (!hybrid_local_gate_open_) {
          ESP_LOGI(kTag,
                   "Hybrid mode: local printer configured, enabling local MQTT "
                   "(model=%s camera_demand=%d)",
                   to_string(routing_model), hybrid_local_camera_demand ? 1 : 0);
          if (!local_snapshot.local_connected) {
            // Serialize the connect phase: give the local MQTT TLS handshake a
            // quiet window without concurrent cloud HTTPS fetches / cloud MQTT
            // (re)connects. TLS handshakes are the heap spikes — steady-state
            // traffic can overlap. Cleared early once local MQTT connects.
            local_mqtt_handoff_until_tick_ = now_tick + kLocalMqttHandoffCooldown;
            ESP_LOGI(kTag, "Hybrid mode: pausing cloud traffic for local MQTT handoff");
          }
        }
        hybrid_local_gate_open_ = true;
      }
    }
    // Evaluated after the gate block so a handoff window opened above pauses
    // cloud traffic in this very iteration (not one loop later).
    const bool local_mqtt_handoff_active =
        tick_deadline_active(local_mqtt_handoff_until_tick_.load(), now_tick);

    const bool local_network_ready =
        wifi_connected && local_printer_enabled_ &&
        (source_mode_ == SourceMode::kLocalOnly ||
         (source_mode_ == SourceMode::kHybrid && hybrid_local_gate_open_));
    const bool local_camera_network_ready =
        local_network_ready &&
        (source_mode_ == SourceMode::kLocalOnly || hybrid_local_camera_demand);
    printer_client_.set_network_ready(local_network_ready);
    camera_client_.set_network_ready(local_camera_network_ready);

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

    const bool camera_enabled =
        source_mode_ != SourceMode::kCloudOnly && local_printer_enabled_ &&
        local_camera_network_ready &&
        camera_model_has_jpeg && local_snapshot.local_connected && !local_mqtt_handoff_active &&
        camera_page_active &&
        ui_.screen_power_mode() != ScreenPowerMode::kOff;
    camera_client_.set_enabled(camera_enabled);
    if (ui_.consume_camera_refresh_request()) {
      camera_client_.request_refresh();
    }

    const bool hybrid_local_path_healthy =
        source_mode_ == SourceMode::kHybrid && local_network_ready && local_printer_enabled_ &&
        local_snapshot.local_connected && hybrid_local_status_supported_now && !hybrid_prefers_cloud;
    // Keep the cloud session warm in hybrid mode even while the local path is
    // the active status source. Tearing the whole cloud path down (previous
    // behaviour: cloud_network_ready=false) parked the cloud task in the
    // "Waiting for Wi-Fi" loop (visible session flapping) and forced a fresh
    // HTTPS login + bindings + preview burst — several TLS handshakes right
    // next to the local MQTT/camera connections — whenever the user swiped to
    // the preview page. Instead the session/token stays alive and only the
    // heavy traffic sources (live MQTT, HTTPS fetches) are gated below.
    const bool cloud_network_ready = wifi_connected && source_mode_ != SourceMode::kLocalOnly;
    const bool hybrid_cloud_idle =
        source_mode_ == SourceMode::kHybrid && hybrid_local_path_healthy && !preview_page_active;
    const bool cloud_live_mqtt_enabled =
        cloud_network_ready &&
        !local_mqtt_handoff_active &&
        (source_mode_ == SourceMode::kCloudOnly ||
         (source_mode_ == SourceMode::kHybrid &&
          (hybrid_prefers_cloud || !hybrid_local_path_healthy)));
    const bool pause_cloud_fetches =
        source_mode_ == SourceMode::kHybrid &&
        (hybrid_cloud_idle ||
         (hybrid_local_gate_open_ &&
          (camera_page_active || page_transition_active || hybrid_camera_cooldown_active)) ||
         local_mqtt_handoff_active || !cloud_network_ready);
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
    auto build_merged_snapshot = [&](const PrinterSnapshot& current_local_snapshot,
                                     const BambuCloudSnapshot& current_cloud_snapshot) {
      PrinterSnapshot merged =
          merge_status_sources(current_local_snapshot, local_printer_enabled_, current_cloud_snapshot,
                               source_mode_, now_ms, wifi_connected, wifi_ip);
      merged.setup_ap_active = current_local_snapshot.setup_ap_active;
      merged.setup_ap_ssid = current_local_snapshot.setup_ap_ssid;
      merged.setup_ap_password = current_local_snapshot.setup_ap_password;
      merged.setup_ap_ip = current_local_snapshot.setup_ap_ip;
      merged.show_stop_banner =
          merged.non_error_stop && tick_deadline_active(stop_banner_until_tick_, now_tick);
      merged.preview_page_available = source_mode_ != SourceMode::kLocalOnly;
      merged.camera_page_available =
          route_allows_local_jpeg_camera(source_mode_, current_local_snapshot,
                                         current_cloud_snapshot);
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
    auto apply_print_command_override = [&](PrinterSnapshot* target_snapshot) {
      if (target_snapshot == nullptr ||
          print_command_override_kind_ == PrintCommand::kNone) {
        return;
      }
      const PrintLifecycleState desired =
          lifecycle_after_print_command(print_command_override_kind_);
      // Clear early once the printer's actual lifecycle confirms the command.
      if (target_snapshot->lifecycle == desired || now_ms >= print_command_override_until_ms_) {
        print_command_override_kind_ = PrintCommand::kNone;
        print_command_override_until_ms_ = 0;
        target_snapshot->print_command_pending_kind = PrintCommand::kNone;
        return;
      }
      target_snapshot->lifecycle = desired;
      target_snapshot->print_command_pending_kind = print_command_override_kind_;
    };
    PrinterSnapshot snapshot = build_merged_snapshot(local_snapshot, cloud_snapshot);
    apply_chamber_light_override(&snapshot);
    apply_print_command_override(&snapshot);

    if (ui_.consume_chamber_light_toggle_request()) {
      const bool requested_on =
          !snapshot.chamber_light_state_known || !snapshot.chamber_light_on;
      bool command_sent = false;
      const ChamberLightCommandPlan light_plan =
          chamber_light_command_plan(source_mode_, hybrid_prefers_cloud,
                                     hybrid_local_status_supported_now, local_network_ready,
                                     local_printer_enabled_, cloud_network_ready,
                                     local_snapshot, cloud_snapshot);

      if (light_plan.try_local) {
        command_sent = printer_client_.set_chamber_light(requested_on);
        if (command_sent) {
          mark_chamber_light_state(local_snapshot, requested_on);
        }
      }
      if (!command_sent && light_plan.try_cloud) {
        command_sent = cloud_client_.set_chamber_light(requested_on);
        if (command_sent) {
          mark_chamber_light_state(cloud_snapshot, requested_on);
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

    if (const PrintCommand requested_print_cmd = ui_.consume_print_command_request();
        requested_print_cmd != PrintCommand::kNone) {
      bool command_sent = false;
      const PrintCommandPlan plan = print_command_plan(
          source_mode_, hybrid_prefers_cloud, hybrid_local_status_supported_now,
          local_network_ready, local_printer_enabled_, cloud_network_ready, local_snapshot,
          cloud_snapshot);
      if (plan.try_local) {
        command_sent = printer_client_.set_print_command(requested_print_cmd);
      }
      if (!command_sent && plan.try_cloud) {
        command_sent = cloud_client_.set_print_command(requested_print_cmd);
      }

      if (!command_sent) {
        ESP_LOGW(kTag, "Print command %s failed in %s mode", to_string(requested_print_cmd),
                 to_string(source_mode_));
      } else {
        ESP_LOGI(kTag, "Print command %s issued (%s)", to_string(requested_print_cmd),
                 to_string(source_mode_));
        print_command_override_kind_ = requested_print_cmd;
        print_command_override_until_ms_ = now_ms + kPrintCommandOverrideMs;
        snapshot = build_merged_snapshot(local_snapshot, cloud_snapshot);
        apply_chamber_light_override(&snapshot);
        apply_print_command_override(&snapshot);
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
    if (source_mode_ == SourceMode::kCloudOnly || !local_printer_enabled_ ||
        !snapshot.camera_page_available) {
      snapshot.camera_connected = false;
      if (source_mode_ == SourceMode::kCloudOnly) {
        snapshot.camera_detail = "Camera unavailable in cloud-only mode";
      } else if (!local_printer_enabled_) {
        snapshot.camera_detail = "Local camera not configured";
      } else {
        snapshot.camera_detail = "Camera unavailable on this model";
      }
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
    // Audio-notification edge detection. Runs strictly off the merged
    // PrinterSnapshot so it sees the same view that the UI does - no double
    // beeps when cloud and local report the same transition.
    {
      const PrintLifecycleState lc = snapshot.lifecycle;
      const bool has_err = snapshot.has_error;
      const int err_code = snapshot.print_error_code;
      const size_t hms_count = snapshot.hms_codes.size();
      if (audio_state_primed_) {
        if (lc != audio_last_lifecycle_) {
          if (lc == PrintLifecycleState::kFinished &&
              audio_last_lifecycle_ == PrintLifecycleState::kPrinting) {
            audio_notifier_.play(AudioNotifier::Event::kPrintFinished);
          } else if (lc == PrintLifecycleState::kPrinting &&
                     (audio_last_lifecycle_ == PrintLifecycleState::kIdle ||
                      audio_last_lifecycle_ == PrintLifecycleState::kPreparing ||
                      audio_last_lifecycle_ == PrintLifecycleState::kUnknown)) {
            audio_notifier_.play(AudioNotifier::Event::kPrintStarted);
          } else if (lc == PrintLifecycleState::kPaused &&
                     audio_last_lifecycle_ == PrintLifecycleState::kPrinting) {
            audio_notifier_.play(AudioNotifier::Event::kPrintPaused);
          } else if (lc == PrintLifecycleState::kError &&
                     audio_last_lifecycle_ != PrintLifecycleState::kError) {
            audio_notifier_.play(AudioNotifier::Event::kPrintError);
          }
        } else if ((has_err && !audio_last_has_error_) ||
                   (err_code != 0 && err_code != audio_last_print_error_code_)) {
          audio_notifier_.play(AudioNotifier::Event::kPrintError);
        } else if (hms_count > audio_last_hms_count_) {
          audio_notifier_.play(AudioNotifier::Event::kHmsAlert);
        }
      }
      audio_last_lifecycle_ = lc;
      audio_last_has_error_ = has_err;
      audio_last_print_error_code_ = err_code;
      audio_last_hms_count_ = hms_count;
      audio_state_primed_ = true;
    }
    last_local_print_live_ = local_print_is_live(local_snapshot);
    last_cloud_print_live_ = cloud_print_is_live(cloud_snapshot);

    const bool on_battery = power.available && power.battery_present && !power.usb_present;
    const bool preview_pipeline_enabled =
        source_mode_ == SourceMode::kCloudOnly || preview_page_active;
    cloud_client_.set_preview_fetch_enabled(source_mode_ != SourceMode::kLocalOnly &&
                                            preview_pipeline_enabled);
    const bool provisioning_active =
        snapshot.setup_ap_active ||
        snapshot.connection == PrinterConnectionState::kWaitingForCredentials;
    // Hard wake-lock only for provisioning / camera page / page transitions.
    // An active print no longer forces the screen awake — it just switches the
    // energy policy to the "during print" timeouts (see update_power_save).
    const bool keep_screen_awake =
        provisioning_active || camera_page_active || page_transition_active;
    if (filament_wake_enabled_ && is_filament && is_external_spool) {
      // External-spool filament change needs the user at the printer: wake once.
      ui_.request_wake_display();
    }
    ui_.update_power_save(on_battery, keep_screen_awake, snapshot.print_active);

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
