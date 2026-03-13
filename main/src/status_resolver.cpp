#include "printsphere/status_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace printsphere {

namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool contains_token(const std::string& value, const char* token) {
  return value.find(token) != std::string::npos;
}

std::string shorten(std::string value, size_t max_len) {
  if (value.size() <= max_len) {
    return value;
  }
  if (max_len <= 3U) {
    return value.substr(0, max_len);
  }
  value.resize(max_len - 3U);
  value += "...";
  return value;
}

std::string titlecase_words(std::string value) {
  for (char& ch : value) {
    if (ch == '_' || ch == '-') {
      ch = ' ';
    }
  }

  bool capitalize = true;
  for (char& ch : value) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      capitalize = true;
      continue;
    }
    if (capitalize) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      capitalize = false;
    } else {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
  }
  return value;
}

bool is_failed_status(const std::string& status) {
  return status == "failed" || contains_token(status, "fail") || contains_token(status, "cancel");
}

bool is_finished_status(const std::string& status) {
  return status == "finish" || status == "finished" || contains_token(status, "finish") ||
         contains_token(status, "success") || contains_token(status, "complete");
}

bool is_prepare_status(const std::string& status, const std::string& stage) {
  return status == "prepare" || contains_token(stage, "heatbed_preheating") ||
         contains_token(stage, "nozzle_preheating") || contains_token(stage, "heating_hotend") ||
         contains_token(stage, "heating_chamber") ||
         contains_token(stage, "waiting_for_heatbed_temperature") ||
         contains_token(stage, "thermal_preconditioning") || contains_token(stage, "preheat") ||
         contains_token(stage, "model_download") || contains_token(stage, "download") ||
         contains_token(status, "download");
}

bool is_paused_status(const std::string& status, const std::string& stage) {
  return contains_token(status, "pause") || contains_token(stage, "pause");
}

bool is_non_error_stop(const PrinterSnapshot& snapshot) {
  return snapshot.non_error_stop && snapshot.print_error_code == 0 && snapshot.hms_alert_count == 0;
}

std::string effective_status(const PrinterSnapshot& snapshot) {
  if (!snapshot.raw_status.empty()) {
    return lower_copy(snapshot.raw_status);
  }

  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPreparing:
      return "prepare";
    case PrintLifecycleState::kPrinting:
      return "running";
    case PrintLifecycleState::kPaused:
      return "pause";
    case PrintLifecycleState::kFinished:
      return "finish";
    case PrintLifecycleState::kIdle:
      return "idle";
    case PrintLifecycleState::kError:
      return "failed";
    case PrintLifecycleState::kUnknown:
    default:
      return {};
  }
}

std::string effective_stage(const PrinterSnapshot& snapshot) {
  if (!snapshot.raw_stage.empty()) {
    return lower_copy(snapshot.raw_stage);
  }
  return lower_copy(snapshot.stage);
}

bool is_generic_local_stage(const PrinterSnapshot& snapshot) {
  return snapshot.stage.empty() || snapshot.stage == "boot" || snapshot.stage == "wifi" ||
         snapshot.stage == "mqtt" || snapshot.stage == "connected" || snapshot.stage == "setup" ||
         snapshot.stage == "ready" || snapshot.stage == "cloud" || snapshot.stage == "cloud-online";
}

bool local_state_has_priority_signal(const PrinterSnapshot& snapshot) {
  if (snapshot.print_error_code != 0 || snapshot.hms_alert_count > 0 ||
      snapshot.has_error || snapshot.lifecycle == PrintLifecycleState::kError) {
    return true;
  }

  if (is_non_error_stop(snapshot)) {
    return true;
  }

  if (snapshot.lifecycle == PrintLifecycleState::kPreparing ||
      snapshot.lifecycle == PrintLifecycleState::kPrinting ||
      snapshot.lifecycle == PrintLifecycleState::kPaused ||
      snapshot.lifecycle == PrintLifecycleState::kFinished) {
    return true;
  }

  return snapshot.progress_percent > 0.0f || !snapshot.raw_status.empty() ||
         !snapshot.raw_stage.empty() || !is_generic_local_stage(snapshot);
}

bool local_state_is_live_printing(const PrinterSnapshot& snapshot) {
  return snapshot.print_active || snapshot.lifecycle == PrintLifecycleState::kPreparing ||
         snapshot.lifecycle == PrintLifecycleState::kPrinting ||
         snapshot.lifecycle == PrintLifecycleState::kPaused;
}

bool cloud_state_is_live_printing(const BambuCloudSnapshot& snapshot) {
  return snapshot.lifecycle == PrintLifecycleState::kPreparing ||
         snapshot.lifecycle == PrintLifecycleState::kPrinting ||
         snapshot.lifecycle == PrintLifecycleState::kPaused;
}

bool cloud_state_has_signal(const BambuCloudSnapshot& snapshot) {
  return snapshot.connected &&
         (snapshot.lifecycle != PrintLifecycleState::kUnknown || snapshot.progress_percent > 0.0f ||
          !snapshot.stage.empty() || !snapshot.raw_status.empty() || !snapshot.raw_stage.empty() ||
          snapshot.has_error);
}

bool local_state_is_explicit_idle(const PrinterSnapshot& snapshot) {
  if (snapshot.connection != PrinterConnectionState::kOnline ||
      snapshot.lifecycle != PrintLifecycleState::kIdle) {
    return false;
  }

  const std::string raw_status = lower_copy(snapshot.raw_status);
  const std::string raw_stage = lower_copy(snapshot.raw_stage);
  return raw_status == "idle" || raw_stage == "idle";
}

bool local_state_has_hard_error(const PrinterSnapshot& snapshot) {
  return snapshot.has_error || snapshot.lifecycle == PrintLifecycleState::kError ||
         snapshot.print_error_code != 0;
}

bool is_filament_stage(const std::string& stage) {
  return contains_token(stage, "filament_loading") || contains_token(stage, "filament_unloading") ||
         contains_token(stage, "changing_filament");
}

bool is_download_stage(const std::string& stage, const std::string& status) {
  return contains_token(stage, "model_download") || contains_token(stage, "download") ||
         contains_token(status, "download");
}

bool is_preheat_stage(const std::string& stage) {
  return contains_token(stage, "heatbed_preheating") || contains_token(stage, "nozzle_preheating") ||
         contains_token(stage, "heating_hotend") || contains_token(stage, "heating_chamber") ||
         contains_token(stage, "waiting_for_heatbed_temperature") ||
         contains_token(stage, "thermal_preconditioning") || contains_token(stage, "preheat");
}

bool is_clean_stage(const std::string& stage) {
  return contains_token(stage, "cleaning_nozzle_tip") || contains_token(stage, "clean");
}

bool is_level_stage(const std::string& stage) {
  return contains_token(stage, "auto_bed_leveling") || contains_token(stage, "bed_level") ||
         contains_token(stage, "measuring_surface");
}

bool is_cooling_stage(const std::string& stage, const std::string& status) {
  return contains_token(stage, "cooling_down") || contains_token(stage, "cool") ||
         contains_token(stage, "heated_bedcooling") ||
         contains_token(status, "cool");
}

bool is_setup_stage(const std::string& stage) {
  return contains_token(stage, "sweeping_xy_mech_mode") ||
         contains_token(stage, "scanning_bed_surface") ||
         contains_token(stage, "inspecting_first_layer") ||
         contains_token(stage, "identifying_build_plate") ||
         contains_token(stage, "calibrating_micro_lidar") ||
         contains_token(stage, "calibrating_extrusion") ||
         contains_token(stage, "calibrating_extrusion_flow") ||
         contains_token(stage, "homing_toolhead") ||
         contains_token(stage, "checking_extruder_temperature") ||
         contains_token(stage, "calibrating_motor_noise") ||
         contains_token(stage, "absolute_accuracy_calibration") ||
         contains_token(stage, "check_absolute_accuracy") ||
         contains_token(stage, "calibrate_nozzle_offset") ||
         contains_token(stage, "check_quick_release") ||
         contains_token(stage, "check_door_and_cover") ||
         contains_token(stage, "laser_calibration") ||
         contains_token(stage, "check_plaform") ||
         contains_token(stage, "check_birdeye_camera_position") ||
         contains_token(stage, "calibrate_birdeye_camera") ||
         contains_token(stage, "print_calibration_lines") ||
         contains_token(stage, "check_material") ||
         contains_token(stage, "calibrating_live_view_camera") ||
         contains_token(stage, "check_material_position") ||
         contains_token(stage, "calibrating_cutter_model_offset");
}

bool is_specific_runtime_stage(const std::string& stage) {
  return is_filament_stage(stage) || contains_token(stage, "model_download") ||
         contains_token(stage, "download") || is_preheat_stage(stage) || is_clean_stage(stage) ||
         is_level_stage(stage) || is_cooling_stage(stage, std::string{}) || is_setup_stage(stage);
}

bool local_runtime_substate_should_override(const PrinterSnapshot& target,
                                            const PrinterSnapshot& local_snapshot) {
  const std::string local_stage = effective_stage(local_snapshot);
  const std::string target_stage = effective_stage(target);
  const std::string local_status = effective_status(local_snapshot);
  const std::string target_status = effective_status(target);

  const bool local_specific_stage = is_specific_runtime_stage(local_stage);
  const bool target_specific_stage = is_specific_runtime_stage(target_stage);
  const bool local_prepare = is_prepare_status(local_status, local_stage);
  const bool target_prepare = is_prepare_status(target_status, target_stage);
  const bool local_paused = is_paused_status(local_status, local_stage);
  const bool target_paused = is_paused_status(target_status, target_stage);

  if (local_specific_stage && !target_specific_stage) {
    return true;
  }
  if (local_prepare && !target_prepare) {
    return true;
  }
  if (local_paused && !target_paused) {
    return true;
  }
  return target_stage.empty() && !local_stage.empty();
}

void apply_cloud_state(PrinterSnapshot& target, const BambuCloudSnapshot& cloud_snapshot) {
  target.connection = cloud_snapshot.connected ? PrinterConnectionState::kOnline
                                               : PrinterConnectionState::kConnecting;
  target.lifecycle = cloud_snapshot.lifecycle;
  target.raw_status = cloud_snapshot.raw_status;
  target.raw_stage = cloud_snapshot.raw_stage;
  if (!cloud_snapshot.stage.empty()) {
    target.stage = cloud_snapshot.stage;
  }
  target.progress_percent = cloud_snapshot.progress_percent;
  if (cloud_snapshot.remaining_seconds > 0U ||
      cloud_snapshot.lifecycle == PrintLifecycleState::kFinished ||
      cloud_snapshot.lifecycle == PrintLifecycleState::kIdle ||
      cloud_snapshot.lifecycle == PrintLifecycleState::kError) {
    target.remaining_seconds = cloud_snapshot.remaining_seconds;
  }
  if (cloud_snapshot.current_layer > 0U) {
    target.current_layer = cloud_snapshot.current_layer;
  }
  if (cloud_snapshot.total_layers > 0U) {
    target.total_layers = cloud_snapshot.total_layers;
  }
  target.has_error = cloud_snapshot.has_error;
  target.print_error_code = 0;
  target.hms_alert_count = 0;
  target.warn_hms = false;
  target.non_error_stop = false;
  target.show_stop_banner = false;
  if (!cloud_snapshot.detail.empty()) {
    target.detail = cloud_snapshot.detail;
  }
}

void apply_local_live_state(PrinterSnapshot& target, const PrinterSnapshot& local_snapshot) {
  target.connection = local_snapshot.connection;
  target.lifecycle = local_snapshot.lifecycle;
  target.raw_status = local_snapshot.raw_status;
  target.raw_stage = local_snapshot.raw_stage;
  target.stage = local_snapshot.stage;
  target.progress_percent = local_snapshot.progress_percent;
  if (local_snapshot.remaining_seconds > 0U ||
      local_snapshot.lifecycle == PrintLifecycleState::kFinished ||
      local_snapshot.lifecycle == PrintLifecycleState::kIdle ||
      local_snapshot.lifecycle == PrintLifecycleState::kError) {
    target.remaining_seconds = local_snapshot.remaining_seconds;
  }
  target.has_error = local_snapshot.has_error;
  target.print_error_code = local_snapshot.print_error_code;
  target.hms_alert_count = local_snapshot.hms_alert_count;
  target.warn_hms = local_snapshot.warn_hms;
  target.non_error_stop = local_snapshot.non_error_stop;
  target.show_stop_banner = local_snapshot.show_stop_banner;
  if (!local_snapshot.detail.empty()) {
    target.detail = local_snapshot.detail;
  }
  if (!local_snapshot.job_name.empty()) {
    target.job_name = local_snapshot.job_name;
  }
}

void apply_local_live_metrics(PrinterSnapshot& target, const PrinterSnapshot& local_snapshot) {
  target.progress_percent = local_snapshot.progress_percent;
  if (local_snapshot.remaining_seconds > 0U ||
      local_snapshot.lifecycle == PrintLifecycleState::kFinished ||
      local_snapshot.lifecycle == PrintLifecycleState::kIdle ||
      local_snapshot.lifecycle == PrintLifecycleState::kError) {
    target.remaining_seconds = local_snapshot.remaining_seconds;
  }
  target.print_error_code = local_snapshot.print_error_code;
  target.hms_alert_count = local_snapshot.hms_alert_count;
  if (local_snapshot.current_layer > 0U) {
    target.current_layer = local_snapshot.current_layer;
  }
  if (local_snapshot.total_layers > 0U) {
    target.total_layers = local_snapshot.total_layers;
  }
  if (target.raw_status.empty()) {
    target.raw_status = local_snapshot.raw_status;
  }
  if (target.raw_stage.empty()) {
    target.raw_stage = local_snapshot.raw_stage;
  }
  if (local_runtime_substate_should_override(target, local_snapshot)) {
    if (!local_snapshot.raw_status.empty()) {
      target.raw_status = local_snapshot.raw_status;
    }
    if (!local_snapshot.raw_stage.empty()) {
      target.raw_stage = local_snapshot.raw_stage;
    }
    if (!local_snapshot.stage.empty()) {
      target.stage = local_snapshot.stage;
    }
    if (!local_snapshot.detail.empty()) {
      target.detail = local_snapshot.detail;
    }
  }
}

std::string pretty_stage(const std::string& stage) {
  if (is_download_stage(stage, std::string{})) return "downloading";
  if (is_filament_stage(stage)) return "loading";
  if (contains_token(stage, "printing")) return "printing";
  if (is_level_stage(stage)) return "bed level";
  if (is_preheat_stage(stage)) return "preheating";
  if (is_clean_stage(stage)) return "clean nozzle";
  if (is_cooling_stage(stage, std::string{})) return "cooling";
  if (is_setup_stage(stage)) return "preparing";
  if (stage == "idle") return "idle";
  if (stage == "offline") return "offline";
  return {};
}

std::string pretty_status(const std::string& status) {
  if (contains_token(status, "download")) return "downloading";
  if (status == "prepare") return "preparing";
  if (status == "running") return "printing";
  if (status == "finish" || status == "finished") return "printing";
  if (status == "failed") return "failed";
  if (contains_token(status, "pause")) return "paused";
  if (contains_token(status, "cool")) return "cooling";
  return {};
}

}  // namespace

void resolve_ui_state(PrinterSnapshot& snapshot) {
  if (is_non_error_stop(snapshot)) {
    // User-cancelled Bambu jobs currently surface as FAILED/FAIL without any concrete
    // printer error payload. Treat that as a brief stopped state, then fall back to ready.
    snapshot.lifecycle = PrintLifecycleState::kIdle;
    snapshot.has_error = false;
    snapshot.print_active = false;
    snapshot.warn_hms = false;
    snapshot.remaining_seconds = 0U;

    if (snapshot.show_stop_banner) {
      snapshot.raw_stage = "Stopped";
      snapshot.stage = "Stopped";
      snapshot.ui_status = "stopped";
      snapshot.detail.clear();
    } else {
      snapshot.raw_status = "IDLE";
      snapshot.raw_stage = "Idle";
      snapshot.stage = "Idle";
      snapshot.ui_status = "ready";
      snapshot.detail.clear();
    }
    return;
  }

  const std::string status = effective_status(snapshot);
  const std::string stage = effective_stage(snapshot);
  const int progress = std::clamp(static_cast<int>(std::lround(snapshot.progress_percent)), 0, 100);
  const bool filament_stage = is_filament_stage(stage);
  const bool download_stage = is_download_stage(stage, status);
  const bool preheat_stage = is_preheat_stage(stage);
  const bool clean_stage = is_clean_stage(stage);
  const bool level_stage = is_level_stage(stage);
  const bool cooling_stage = is_cooling_stage(stage, status);
  const bool setup_stage = is_setup_stage(stage);

  const bool ps_failed = is_failed_status(status);
  const bool err_print = snapshot.print_error_code != 0;
  const bool err_hms = snapshot.hms_alert_count > 0;
  const bool input_error = snapshot.has_error;
  const bool paused = is_paused_status(status, stage);
  const bool preparing = is_prepare_status(status, stage);
  const bool idleish = contains_token(stage, "idle") || contains_token(stage, "offline");
  const bool done_strict =
      is_finished_status(status) && contains_token(stage, "idle") && progress == 100;
  const bool running_like = status == "running" || status == "prepare" ||
                            contains_token(status, "print") || contains_token(status, "download") ||
                            (progress > 0 && progress < 100);
  const bool active_hint =
      running_like || paused || preparing || filament_stage || download_stage ||
      preheat_stage || clean_stage || level_stage || cooling_stage || setup_stage ||
      contains_token(stage, "printing") ||
      (snapshot.current_layer > 0 && progress < 100) ||
      (snapshot.remaining_seconds > 0 && !is_finished_status(status));

  if (ps_failed || done_strict) {
    snapshot.print_active = false;
  } else if (active_hint) {
    snapshot.print_active = true;
  } else {
    snapshot.print_active = false;
  }

  snapshot.has_error = input_error || ps_failed || err_print || (err_hms && !snapshot.print_active);
  snapshot.warn_hms = err_hms && snapshot.print_active && !snapshot.has_error;

  if (snapshot.has_error) {
    snapshot.lifecycle = PrintLifecycleState::kError;
  } else if (done_strict) {
    snapshot.lifecycle = PrintLifecycleState::kFinished;
  } else if (paused) {
    snapshot.lifecycle = PrintLifecycleState::kPaused;
  } else if (preparing) {
    snapshot.lifecycle = PrintLifecycleState::kPreparing;
  } else if (snapshot.print_active) {
    snapshot.lifecycle = PrintLifecycleState::kPrinting;
  } else if (idleish || status == "idle" || status == "offline") {
    snapshot.lifecycle = PrintLifecycleState::kIdle;
  }

  if (filament_stage) {
    snapshot.ui_status = contains_token(stage, "filament_unloading") ? "unloading" : "loading";
  } else if (download_stage) {
    snapshot.ui_status = "downloading";
  } else if (snapshot.has_error) {
    snapshot.ui_status = "failed";
  } else if (snapshot.warn_hms) {
    snapshot.ui_status = "printing";
  } else if (done_strict) {
    snapshot.ui_status = "done";
  } else if (preheat_stage) {
    snapshot.ui_status = "preheating";
  } else if (clean_stage) {
    snapshot.ui_status = "clean nozzle";
  } else if (level_stage) {
    snapshot.ui_status = "bed level";
  } else if (cooling_stage) {
    snapshot.ui_status = "cooling";
  } else if (setup_stage || status == "prepare") {
    snapshot.ui_status = "preparing";
  } else {
    const bool offline =
        contains_token(stage, "offline") || status == "offline" ||
        (!snapshot.wifi_connected &&
         snapshot.connection != PrinterConnectionState::kWaitingForCredentials);

    if (idleish || offline) {
      if (offline) {
        snapshot.ui_status = "offline";
      } else if (status == "prepare") {
        snapshot.ui_status = "preparing";
      } else if ((progress > 0 && progress < 100) || snapshot.print_active) {
        snapshot.ui_status = "printing";
      } else {
        snapshot.ui_status = "idle";
      }
    } else {
      const std::string stage_text = pretty_stage(stage);
      if (!stage_text.empty() && !contains_token(stage, "idle") &&
          !contains_token(stage, "offline")) {
        snapshot.ui_status = stage_text;
      } else {
        const std::string status_text = pretty_status(status);
        if (!status_text.empty()) {
          snapshot.ui_status = status_text;
        } else if (!stage.empty()) {
          snapshot.ui_status = shorten(titlecase_words(stage), 18);
        } else if (!status.empty()) {
          snapshot.ui_status = shorten(titlecase_words(status), 18);
        } else {
          snapshot.ui_status = snapshot.wifi_connected ? "waiting..." : "offline";
        }
      }
    }
  }
}

PrinterSnapshot merge_status_sources(const PrinterSnapshot& local_snapshot, bool local_printer_enabled,
                                     const BambuCloudSnapshot& cloud_snapshot,
                                     StatusSourcePreference status_source_preference,
                                     bool wifi_connected, const std::string& wifi_ip,
                                     bool print_activity_seen_this_session) {
  PrinterSnapshot snapshot = local_snapshot;
  snapshot.cloud_connected = cloud_snapshot.connected;
  snapshot.cloud_detail = cloud_snapshot.detail;
  snapshot.preview_url = cloud_snapshot.preview_url;
  snapshot.preview_blob = cloud_snapshot.preview_blob;
  snapshot.preview_title = cloud_snapshot.preview_title;
  snapshot.wifi_connected = wifi_connected;
  snapshot.wifi_ip = wifi_ip;

  const bool cloud_has_state = cloud_state_has_signal(cloud_snapshot);
  const bool local_has_state = local_state_has_priority_signal(local_snapshot);
  const bool local_live_printing = local_state_is_live_printing(local_snapshot);
  const bool cloud_live_printing = cloud_state_is_live_printing(cloud_snapshot);
  const bool local_non_error_stop = is_non_error_stop(local_snapshot);
  const bool local_hard_error = local_state_has_hard_error(local_snapshot);
  const bool prefer_cloud = status_source_preference == StatusSourcePreference::kCloud;
  const bool local_non_error_stop_should_override_cloud =
      local_non_error_stop && (print_activity_seen_this_session || !cloud_live_printing);
  const bool local_error_should_override_cloud =
      local_hard_error && (cloud_has_state || cloud_live_printing || cloud_snapshot.connected);
  const bool local_idle_should_override_cloud_error =
      local_state_is_explicit_idle(local_snapshot) &&
      cloud_snapshot.lifecycle == PrintLifecycleState::kError && !cloud_live_printing;
  const bool stale_cloud_finished_after_boot =
      prefer_cloud && cloud_snapshot.lifecycle == PrintLifecycleState::kFinished &&
      local_state_is_explicit_idle(local_snapshot) && !print_activity_seen_this_session;

  if (!local_printer_enabled) {
    snapshot.connection = cloud_snapshot.configured ? PrinterConnectionState::kConnecting
                                                    : PrinterConnectionState::kWaitingForCredentials;
    snapshot.stage = cloud_snapshot.connected ? "cloud-online" : "cloud";
    snapshot.detail = cloud_snapshot.detail;
    if (cloud_snapshot.connected) {
      snapshot.connection = PrinterConnectionState::kOnline;
    }
    if (cloud_has_state) {
      apply_cloud_state(snapshot, cloud_snapshot);
    }
    return snapshot;
  }

  if (prefer_cloud) {
    if (local_error_should_override_cloud || local_non_error_stop_should_override_cloud ||
        local_idle_should_override_cloud_error) {
      apply_local_live_state(snapshot, local_snapshot);
    } else if (cloud_has_state && !stale_cloud_finished_after_boot) {
      apply_cloud_state(snapshot, cloud_snapshot);
    } else if (!local_has_state && !cloud_snapshot.detail.empty()) {
      snapshot.detail = cloud_snapshot.detail;
    }

    if (local_live_printing && !cloud_live_printing) {
      apply_local_live_state(snapshot, local_snapshot);
    } else if (local_live_printing) {
      apply_local_live_metrics(snapshot, local_snapshot);
    }
  } else {
    if (!local_has_state && cloud_has_state) {
      apply_cloud_state(snapshot, cloud_snapshot);
    } else if (!cloud_snapshot.detail.empty() &&
               (snapshot.detail.empty() || snapshot.detail == "Status payload received")) {
      snapshot.detail = cloud_snapshot.detail;
    }
  }

  return snapshot;
}

}  // namespace printsphere
