#include "printsphere/status_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "printsphere/bambu_status.hpp"
#include "printsphere/error_lookup.hpp"
#include "printsphere/utf8.hpp"

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
    return utf8_truncate_bytes(value, max_len);
  }
  return utf8_truncate_bytes(value, max_len - 3U) + "...";
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
  return bambu_status_is_failed(status);
}

bool is_finished_status(const std::string& status) {
  return bambu_status_is_finished(status);
}

bool is_prepare_status(const std::string& status, const std::string& stage) {
  return bambu_status_is_preparing(status) || contains_token(stage, "heatbed_preheating") ||
         contains_token(stage, "nozzle_preheating") || contains_token(stage, "heating_hotend") ||
         contains_token(stage, "heating_chamber") ||
         contains_token(stage, "waiting_for_heatbed_temperature") ||
         contains_token(stage, "thermal_preconditioning") || contains_token(stage, "preheat") ||
         contains_token(stage, "model_download") || contains_token(stage, "download") ||
         contains_token(status, "download");
}

bool is_paused_status(const std::string& status, const std::string& stage) {
  return bambu_status_is_paused(status) || contains_token(stage, "pause");
}

bool is_non_error_stop(const PrinterSnapshot& snapshot) {
  return snapshot.non_error_stop && snapshot.print_error_code == 0 &&
         snapshot.hms_alert_count == 0 && snapshot.hms_codes.empty();
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
  if (snapshot.print_error_code != 0 || !snapshot.hms_codes.empty() || snapshot.hms_alert_count > 0 ||
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

bool cloud_state_has_signal(const BambuCloudSnapshot& snapshot) {
  return snapshot.connected &&
         (snapshot.lifecycle != PrintLifecycleState::kUnknown || snapshot.progress_percent > 0.0f ||
          !snapshot.stage.empty() || !snapshot.raw_status.empty() || !snapshot.raw_stage.empty() ||
          snapshot.has_error || snapshot.print_error_code != 0 || !snapshot.hms_codes.empty() ||
          snapshot.hms_alert_count > 0 || snapshot.non_error_stop);
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

constexpr uint64_t kLocalSourceFreshMs = 90ULL * 1000ULL;
constexpr uint64_t kCloudSourceFreshMs = 5ULL * 60ULL * 1000ULL;
constexpr uint64_t kCloudPreviewFreshMs = 30ULL * 60ULL * 1000ULL;

bool is_recent_enough(uint64_t last_update_ms, uint64_t now_ms, uint64_t max_age_ms) {
  if (last_update_ms == 0 || now_ms < last_update_ms) {
    return false;
  }
  return (now_ms - last_update_ms) <= max_age_ms;
}

bool local_metrics_have_signal(const PrinterSnapshot& snapshot) {
  return local_state_has_priority_signal(snapshot) || snapshot.progress_percent > 0.0f ||
         snapshot.remaining_seconds > 0U || snapshot.current_layer > 0U ||
         snapshot.total_layers > 0U || !snapshot.job_name.empty();
}

bool cloud_metrics_have_signal(const BambuCloudSnapshot& snapshot) {
  return cloud_state_has_signal(snapshot) || snapshot.progress_percent > 0.0f ||
         snapshot.remaining_seconds > 0U || snapshot.current_layer > 0U ||
         snapshot.total_layers > 0U || !snapshot.preview_title.empty();
}

bool cloud_preview_available(const BambuCloudSnapshot& snapshot, uint64_t now_ms) {
  if (!is_recent_enough(snapshot.last_update_ms, now_ms, kCloudPreviewFreshMs)) {
    return false;
  }
  return !snapshot.preview_url.empty() || snapshot.preview_blob != nullptr ||
         !snapshot.preview_title.empty();
}

bool cloud_temperature_known(uint64_t last_update_ms, bool cloud_connected) {
  return cloud_connected && last_update_ms != 0;
}

bool local_camera_available(const PrinterSnapshot& snapshot) {
  return snapshot.local_capabilities.camera_jpeg_socket;
}

PrinterModel preferred_model_for_routing(const PrinterSnapshot& local_snapshot,
                                         const BambuCloudSnapshot& cloud_snapshot) {
  if (cloud_snapshot.model != PrinterModel::kUnknown) {
    return cloud_snapshot.model;
  }
  return local_snapshot.local_model;
}

struct StatusSourceAdapter {
  FieldSource source = FieldSource::kNone;
  bool enabled = false;
  bool fresh = false;
  bool status_supported = false;
  bool metrics_supported = false;
  bool temperatures_supported = false;
  bool error_supported = false;
  bool status_usable = false;
  bool metrics_usable = false;
  bool temperatures_usable = false;
  bool light_usable = false;
  bool error_usable = false;
  bool ams_usable = false;
};

StatusSourceAdapter make_local_source_adapter(const PrinterSnapshot& snapshot, bool enabled,
                                              PrinterModel routing_model, uint64_t now_ms) {
  StatusSourceAdapter adapter;
  adapter.source = FieldSource::kLocal;
  adapter.enabled = enabled;
  adapter.fresh = enabled && snapshot.local_connected &&
                  is_recent_enough(snapshot.local_last_update_ms, now_ms, kLocalSourceFreshMs);
  adapter.status_supported = enabled && printer_model_supports_local_status(routing_model);
  adapter.metrics_supported = adapter.status_supported;
  adapter.temperatures_supported = adapter.status_supported;
  adapter.error_supported = adapter.status_supported;
  adapter.status_usable =
      adapter.status_supported && adapter.fresh && local_state_has_priority_signal(snapshot);
  adapter.metrics_usable =
      adapter.metrics_supported && adapter.fresh && local_metrics_have_signal(snapshot);
  adapter.temperatures_usable =
      adapter.temperatures_supported && adapter.fresh && snapshot.local_capabilities.temperatures;
  adapter.light_usable = adapter.status_supported && adapter.fresh &&
                         snapshot.chamber_light_supported && snapshot.chamber_light_state_known;
  adapter.error_usable =
      adapter.error_supported && adapter.fresh &&
      (snapshot.print_error_code != 0 || !snapshot.hms_codes.empty() ||
       snapshot.hms_alert_count > 0 || snapshot.non_error_stop || snapshot.has_error);
  adapter.ams_usable = adapter.fresh && (snapshot.ams != nullptr) && snapshot.ams->count > 0;
  return adapter;
}

StatusSourceAdapter make_cloud_source_adapter(const BambuCloudSnapshot& snapshot, bool enabled,
                                              uint64_t now_ms) {
  StatusSourceAdapter adapter;
  adapter.source = FieldSource::kCloud;
  adapter.enabled = enabled;
  adapter.fresh = enabled && snapshot.connected &&
                  is_recent_enough(snapshot.last_update_ms, now_ms, kCloudSourceFreshMs);
  adapter.status_supported =
      enabled && (snapshot.capabilities.status || cloud_state_has_signal(snapshot));
  adapter.metrics_supported =
      enabled && (snapshot.capabilities.metrics || cloud_metrics_have_signal(snapshot));
  adapter.temperatures_supported = enabled && snapshot.capabilities.temperatures;
  adapter.error_supported =
      enabled && (snapshot.capabilities.hms || snapshot.capabilities.print_error ||
                  snapshot.print_error_code != 0 || !snapshot.hms_codes.empty() ||
                  snapshot.hms_alert_count > 0 || snapshot.has_error || snapshot.non_error_stop);
  adapter.status_usable =
      adapter.status_supported && adapter.fresh && cloud_state_has_signal(snapshot);
  adapter.metrics_usable =
      adapter.metrics_supported && adapter.fresh && cloud_metrics_have_signal(snapshot);
  adapter.temperatures_usable =
      adapter.temperatures_supported && adapter.fresh &&
      (cloud_temperature_known(snapshot.nozzle_temp_last_update_ms, snapshot.connected) ||
       cloud_temperature_known(snapshot.bed_temp_last_update_ms, snapshot.connected) ||
       cloud_temperature_known(snapshot.chamber_temp_last_update_ms, snapshot.connected) ||
       cloud_temperature_known(snapshot.secondary_nozzle_temp_last_update_ms, snapshot.connected));
  adapter.light_usable =
      adapter.fresh && snapshot.chamber_light_supported && snapshot.chamber_light_state_known;
  adapter.error_usable =
      adapter.error_supported && adapter.fresh &&
      (snapshot.print_error_code != 0 || !snapshot.hms_codes.empty() ||
       snapshot.hms_alert_count > 0 || snapshot.has_error || snapshot.non_error_stop);
  adapter.ams_usable = adapter.fresh && (snapshot.ams != nullptr) && snapshot.ams->count > 0;
  return adapter;
}

void copy_source_metadata(PrinterSnapshot& target, const PrinterSnapshot& local_snapshot,
                          bool local_printer_enabled, const BambuCloudSnapshot& cloud_snapshot,
                          bool wifi_connected, const std::string& wifi_ip) {
  target.wifi_connected = wifi_connected;
  target.wifi_ip = wifi_ip;
  target.local_configured = local_printer_enabled || local_snapshot.local_configured;
  target.local_connected = local_snapshot.local_connected;
  target.local_last_update_ms = local_snapshot.local_last_update_ms;
  target.local_model = local_snapshot.local_model;
  target.local_capabilities = local_snapshot.local_capabilities;
  target.local_mqtt_signature_required = local_snapshot.local_mqtt_signature_required;
  target.cloud_configured = cloud_snapshot.configured;
  target.cloud_connected = cloud_snapshot.connected;
  target.cloud_last_update_ms = cloud_snapshot.last_update_ms;
  target.cloud_model = cloud_snapshot.model;
  target.cloud_capabilities =
      cloud_snapshot.capabilities.status || cloud_snapshot.capabilities.metrics ||
              cloud_snapshot.capabilities.temperatures || cloud_snapshot.capabilities.preview ||
              cloud_snapshot.capabilities.hms || cloud_snapshot.capabilities.print_error
          ? cloud_snapshot.capabilities
          : default_cloud_capabilities();
  target.resolved_serial =
      !cloud_snapshot.resolved_serial.empty() ? cloud_snapshot.resolved_serial
                                             : local_snapshot.resolved_serial;
  target.cloud_detail = cloud_snapshot.detail;
  target.status_source = FieldSource::kNone;
  target.metrics_source = FieldSource::kNone;
  target.preview_source = FieldSource::kNone;
  target.camera_source = FieldSource::kNone;
}

void apply_base_state_for_mode(PrinterSnapshot& target, SourceMode source_mode,
                               const PrinterSnapshot& local_snapshot, bool local_enabled,
                               const BambuCloudSnapshot& cloud_snapshot, bool cloud_enabled) {
  const bool hybrid_prefers_cloud =
      source_mode == SourceMode::kHybrid && cloud_enabled &&
      printer_model_prefers_cloud_status(
          preferred_model_for_routing(local_snapshot, cloud_snapshot));
  switch (source_mode) {
    case SourceMode::kCloudOnly:
      if (cloud_enabled) {
        target.connection = cloud_snapshot.connected ? PrinterConnectionState::kOnline
                                                     : PrinterConnectionState::kConnecting;
        target.lifecycle = cloud_snapshot.lifecycle;
        target.stage = cloud_snapshot.connected ? "cloud-online" : "cloud";
        target.detail = cloud_snapshot.detail.empty() ? "Cloud path ready" : cloud_snapshot.detail;
      } else {
        target.connection = PrinterConnectionState::kWaitingForCredentials;
        target.lifecycle = PrintLifecycleState::kUnknown;
        target.stage = "cloud";
        target.detail = "Cloud login not configured";
      }
      break;
    case SourceMode::kLocalOnly:
      if (local_enabled) {
        target.connection = local_snapshot.connection;
        target.lifecycle = local_snapshot.lifecycle;
        target.stage = local_snapshot.stage;
        target.detail = local_snapshot.detail;
      } else {
        target.connection = PrinterConnectionState::kWaitingForCredentials;
        target.lifecycle = PrintLifecycleState::kUnknown;
        target.stage = "setup";
        target.detail = "Need printer host, serial and access code";
      }
      break;
    case SourceMode::kHybrid:
    default:
      if (hybrid_prefers_cloud) {
        target.connection = cloud_snapshot.connected ? PrinterConnectionState::kOnline
                                                     : PrinterConnectionState::kConnecting;
        target.lifecycle = cloud_snapshot.lifecycle;
        target.stage = cloud_snapshot.connected ? "cloud-online" : "cloud";
        target.detail = cloud_snapshot.detail.empty() ? "Cloud path ready" : cloud_snapshot.detail;
      } else if (local_enabled) {
        target.connection = local_snapshot.connection;
        target.lifecycle = local_snapshot.lifecycle;
        target.stage = local_snapshot.stage;
        target.detail = local_snapshot.detail;
      } else if (cloud_enabled) {
        target.connection = cloud_snapshot.connected ? PrinterConnectionState::kOnline
                                                     : PrinterConnectionState::kConnecting;
        target.lifecycle = cloud_snapshot.lifecycle;
        target.stage = cloud_snapshot.connected ? "cloud-online" : "cloud";
        target.detail = cloud_snapshot.detail.empty() ? "Cloud path ready" : cloud_snapshot.detail;
      } else {
        target.connection = PrinterConnectionState::kWaitingForCredentials;
        target.lifecycle = PrintLifecycleState::kUnknown;
        target.stage = "setup";
        target.detail = "Configure cloud or local printer access";
      }
      break;
  }
}

void apply_cloud_status_bundle(PrinterSnapshot& target, const BambuCloudSnapshot& cloud_snapshot) {
  target.connection = cloud_snapshot.connected ? PrinterConnectionState::kOnline
                                               : PrinterConnectionState::kConnecting;
  target.lifecycle = cloud_snapshot.lifecycle;
  target.raw_status = cloud_snapshot.raw_status;
  target.raw_stage = cloud_snapshot.raw_stage;
  if (!cloud_snapshot.stage.empty()) {
    target.stage = cloud_snapshot.stage;
  }
  target.has_error = cloud_snapshot.has_error;
  if (!cloud_snapshot.detail.empty()) {
    target.detail = cloud_snapshot.detail;
  }
}

void apply_local_status_bundle(PrinterSnapshot& target, const PrinterSnapshot& local_snapshot) {
  target.connection = local_snapshot.connection;
  target.lifecycle = local_snapshot.lifecycle;
  target.raw_status = local_snapshot.raw_status;
  target.raw_stage = local_snapshot.raw_stage;
  if (!local_snapshot.stage.empty()) {
    target.stage = local_snapshot.stage;
  }
  target.has_error = local_snapshot.has_error;
  if (!local_snapshot.detail.empty()) {
    target.detail = local_snapshot.detail;
  }
}

void apply_cloud_metrics_bundle(PrinterSnapshot& target, const BambuCloudSnapshot& cloud_snapshot) {
  target.progress_percent = cloud_snapshot.progress_percent;
  target.progress_is_download_related = cloud_snapshot.progress_is_download_related;
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
  if (cloud_snapshot.estimated_filament_weight_g > 0.0f) {
    target.estimated_filament_weight_g = cloud_snapshot.estimated_filament_weight_g;
  }
  if (cloud_snapshot.estimated_filament_length_mm > 0.0f) {
    target.estimated_filament_length_mm = cloud_snapshot.estimated_filament_length_mm;
  }
  if (target.job_name.empty() && !cloud_snapshot.preview_title.empty()) {
    target.job_name = cloud_snapshot.preview_title;
  }
}

void apply_local_metrics_bundle(PrinterSnapshot& target, const PrinterSnapshot& local_snapshot) {
  target.progress_percent = local_snapshot.progress_percent;
  target.progress_is_download_related = local_snapshot.progress_is_download_related;
  if (local_snapshot.remaining_seconds > 0U ||
      local_snapshot.lifecycle == PrintLifecycleState::kFinished ||
      local_snapshot.lifecycle == PrintLifecycleState::kIdle ||
      local_snapshot.lifecycle == PrintLifecycleState::kError) {
    target.remaining_seconds = local_snapshot.remaining_seconds;
  }
  if (local_snapshot.current_layer > 0U) {
    target.current_layer = local_snapshot.current_layer;
  }
  if (local_snapshot.total_layers > 0U) {
    target.total_layers = local_snapshot.total_layers;
  }
  if (!local_snapshot.job_name.empty()) {
    target.job_name = local_snapshot.job_name;
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

void apply_local_temperature_bundle(PrinterSnapshot& target, const PrinterSnapshot& local_snapshot) {
  target.nozzle_temp_c = local_snapshot.nozzle_temp_c;
  target.nozzle_temp_known = local_snapshot.nozzle_temp_known || local_snapshot.nozzle_temp_c > 0.0f;
  target.bed_temp_c = local_snapshot.bed_temp_c;
  target.bed_temp_known = local_snapshot.bed_temp_known || local_snapshot.bed_temp_c > 0.0f;
  target.chamber_temp_c = local_snapshot.chamber_temp_c;
  target.chamber_temp_known =
      local_snapshot.chamber_temp_known || local_snapshot.chamber_temp_c > 0.0f;
  target.secondary_nozzle_temp_c = local_snapshot.secondary_nozzle_temp_c;
  target.secondary_nozzle_temp_known =
      local_snapshot.secondary_nozzle_temp_known || local_snapshot.secondary_nozzle_temp_c > 0.0f;
  if (local_snapshot.active_nozzle_index >= 0) {
    target.active_nozzle_index = local_snapshot.active_nozzle_index;
  }
}

void apply_cloud_temperature_bundle(PrinterSnapshot& target, const BambuCloudSnapshot& cloud_snapshot,
                                    uint64_t now_ms) {
  (void)now_ms;
  target.nozzle_temp_known =
      cloud_temperature_known(cloud_snapshot.nozzle_temp_last_update_ms, cloud_snapshot.connected);
  target.nozzle_temp_c = target.nozzle_temp_known ? cloud_snapshot.nozzle_temp_c : 0.0f;
  target.bed_temp_known =
      cloud_temperature_known(cloud_snapshot.bed_temp_last_update_ms, cloud_snapshot.connected);
  target.bed_temp_c = target.bed_temp_known ? cloud_snapshot.bed_temp_c : 0.0f;
  target.chamber_temp_known =
      cloud_temperature_known(cloud_snapshot.chamber_temp_last_update_ms, cloud_snapshot.connected);
  target.chamber_temp_c = target.chamber_temp_known ? cloud_snapshot.chamber_temp_c : 0.0f;
  target.secondary_nozzle_temp_known = cloud_temperature_known(
      cloud_snapshot.secondary_nozzle_temp_last_update_ms, cloud_snapshot.connected);
  target.secondary_nozzle_temp_c =
      target.secondary_nozzle_temp_known ? cloud_snapshot.secondary_nozzle_temp_c : 0.0f;
  if (cloud_snapshot.active_nozzle_index >= 0) {
    target.active_nozzle_index = cloud_snapshot.active_nozzle_index;
  }
}

void apply_local_error_bundle(PrinterSnapshot& target, const PrinterSnapshot& local_snapshot) {
  target.print_error_code = local_snapshot.print_error_code;
  target.hms_codes = local_snapshot.hms_codes;
  target.hms_alert_count = local_snapshot.hms_alert_count;
  target.non_error_stop = local_snapshot.non_error_stop;
  target.show_stop_banner = local_snapshot.show_stop_banner;
  if ((local_snapshot.print_error_code != 0 || !local_snapshot.hms_codes.empty() ||
       local_snapshot.hms_alert_count > 0 ||
       local_snapshot.non_error_stop || local_snapshot.has_error) &&
      !local_snapshot.detail.empty()) {
    target.detail = local_snapshot.detail;
  }
  if (target.status_source == FieldSource::kNone &&
      (local_snapshot.print_error_code != 0 || !local_snapshot.hms_codes.empty() ||
       local_snapshot.hms_alert_count > 0 ||
       local_snapshot.non_error_stop || local_snapshot.has_error)) {
    apply_local_status_bundle(target, local_snapshot);
  }
}

void apply_cloud_error_bundle(PrinterSnapshot& target, const BambuCloudSnapshot& cloud_snapshot) {
  target.print_error_code = cloud_snapshot.print_error_code;
  target.hms_codes = cloud_snapshot.hms_codes;
  target.hms_alert_count = cloud_snapshot.hms_alert_count;
  target.non_error_stop = cloud_snapshot.non_error_stop;
  target.show_stop_banner = false;
  if ((cloud_snapshot.print_error_code != 0 || !cloud_snapshot.hms_codes.empty() ||
       cloud_snapshot.hms_alert_count > 0 ||
       cloud_snapshot.has_error || cloud_snapshot.non_error_stop) &&
      !cloud_snapshot.detail.empty()) {
    target.detail = cloud_snapshot.detail;
  }
  if (target.status_source == FieldSource::kNone &&
      (cloud_snapshot.print_error_code != 0 || !cloud_snapshot.hms_codes.empty() ||
       cloud_snapshot.hms_alert_count > 0 ||
       cloud_snapshot.has_error || cloud_snapshot.non_error_stop)) {
    apply_cloud_status_bundle(target, cloud_snapshot);
  }
}

void apply_cloud_preview_bundle(PrinterSnapshot& target, const BambuCloudSnapshot& cloud_snapshot) {
  target.preview_url = cloud_snapshot.preview_url;
  target.preview_blob = cloud_snapshot.preview_blob;
  target.preview_title = cloud_snapshot.preview_title;
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

PrinterModel effective_model_for_snapshot(const PrinterSnapshot& snapshot);

std::string pretty_status(const std::string& status) {
  if (contains_token(status, "cool")) return "cooling";
  return bambu_pretty_status(status);
}

bool is_generic_finished_detail(const std::string& detail, const std::string& stage) {
  if (detail.empty()) {
    return true;
  }
  const std::string normalized_detail = lower_copy(detail);
  const std::string normalized_stage = lower_copy(stage);
  return normalized_detail == normalized_stage || normalized_detail == "printing" ||
         normalized_detail == "preparing" || normalized_detail == "paused" ||
         normalized_detail == "running" || normalized_detail == "finished" ||
         normalized_detail == "done" || normalized_detail == "status";
}

void apply_resolved_detail(PrinterSnapshot& snapshot) {
  const bool finished_no_error =
      (snapshot.lifecycle == PrintLifecycleState::kFinished || snapshot.ui_status == "done") &&
      snapshot.print_error_code == 0;
  if (!finished_no_error) {
    const PrinterModel model = effective_model_for_snapshot(snapshot);
    const std::string resolved_error = format_resolved_error_detail(
        snapshot.print_error_code, snapshot.hms_codes, snapshot.hms_alert_count, model);
    if (!resolved_error.empty()) {
      snapshot.detail = resolved_error;
      return;
    }
  }

  if (snapshot.lifecycle == PrintLifecycleState::kFinished || snapshot.ui_status == "done") {
    snapshot.remaining_seconds = 0U;
    if (is_generic_finished_detail(snapshot.detail, snapshot.stage)) {
      snapshot.detail.clear();
    }
  }
}

PrinterModel effective_model_for_snapshot(const PrinterSnapshot& snapshot) {
  if (snapshot.status_source == FieldSource::kLocal && snapshot.local_model != PrinterModel::kUnknown) {
    return snapshot.local_model;
  }
  if (snapshot.status_source == FieldSource::kCloud && snapshot.cloud_model != PrinterModel::kUnknown) {
    return snapshot.cloud_model;
  }
  if (snapshot.local_model != PrinterModel::kUnknown) {
    return snapshot.local_model;
  }
  return snapshot.cloud_model;
}

// -----------------------------------------------------------------------------
// resolve_ui_state() helpers.
//
// resolve_ui_state() turns a merged snapshot into the user-facing
// `lifecycle`, `print_active`, `has_error`, `warn_hms`, and `ui_status`
// fields. The decision is driven by ~20 boolean signals derived from the
// raw status / stage strings and a few snapshot scalars.
//
// The signals are gathered into UiStateSignals once, then individual
// selectors decide each output field. Splitting the original 150-line
// function this way keeps the data-flow flat and gives each decision a
// name; the compiler inlines all of it on -Os, so there is no runtime
// or stack cost on the device.
// -----------------------------------------------------------------------------

struct UiStateSignals {
  // Raw inputs (lower-cased copies, see effective_status/effective_stage).
  std::string status;
  std::string stage;
  int progress = 0;  // [0..100]

  // Stage classifiers.
  bool download_stage = false;
  bool filament_stage = false;
  bool preheat_stage = false;
  bool clean_stage = false;
  bool level_stage = false;
  bool cooling_stage = false;
  bool setup_stage = false;

  // Status / lifecycle classifiers.
  bool ps_failed = false;
  bool err_print = false;
  bool err_hms = false;
  bool input_error = false;  // pre-existing snapshot.has_error
  bool paused = false;
  bool preparing = false;
  bool idle_status = false;
  bool idleish = false;       // stage-side "idle"/"offline" hint
  bool done_strict = false;
  bool active_hint = false;
};

// Gathers all derived signals used by the lifecycle / ui_status selectors.
// May clear `snapshot.progress_is_download_related` when a download has
// completed and handed off to the next stage (existing behaviour).
UiStateSignals build_ui_state_signals(PrinterSnapshot& snapshot) {
  UiStateSignals s;

  s.status = effective_status(snapshot);
  s.stage = effective_stage(snapshot);
  s.progress = std::clamp(static_cast<int>(std::lround(snapshot.progress_percent)), 0, 100);

  const bool download_stage_by_name = is_download_stage(s.stage, s.status);
  const bool completed_download_handoff =
      snapshot.progress_is_download_related && s.progress == 100 &&
      is_post_download_handoff_stage(s.stage, s.status);
  if (completed_download_handoff) {
    snapshot.progress_is_download_related = false;
  }

  s.download_stage = snapshot.progress_is_download_related || download_stage_by_name;
  s.filament_stage = is_filament_stage(s.stage);
  s.preheat_stage = is_preheat_stage(s.stage);
  s.clean_stage = is_clean_stage(s.stage);
  s.level_stage = is_level_stage(s.stage);
  s.cooling_stage = is_cooling_stage(s.stage, s.status);
  s.setup_stage = is_setup_stage(s.stage);

  s.ps_failed = is_failed_status(s.status);
  s.err_print = snapshot.print_error_code != 0;
  s.err_hms = !snapshot.hms_codes.empty() || snapshot.hms_alert_count > 0;
  s.input_error = snapshot.has_error;
  s.paused = is_paused_status(s.status, s.stage);
  s.preparing = is_prepare_status(s.status, s.stage);
  s.idle_status = s.status == "idle" || s.status == "offline" || contains_token(s.status, "wait");
  s.idleish = contains_token(s.stage, "idle") || contains_token(s.stage, "offline");

  s.done_strict =
      snapshot.lifecycle == PrintLifecycleState::kFinished ||
      (is_finished_status(s.status) &&
       (s.progress == 100 || snapshot.remaining_seconds == 0U ||
        contains_token(s.stage, "idle")));

  const bool running_like =
      bambu_status_is_printing(s.status) || bambu_status_is_preparing(s.status) ||
      contains_token(s.status, "print") || contains_token(s.status, "download") ||
      (s.progress > 0 && s.progress < 100);

  s.active_hint =
      running_like || s.paused || s.preparing || s.filament_stage || s.download_stage ||
      s.preheat_stage || s.clean_stage || s.level_stage || s.cooling_stage || s.setup_stage ||
      contains_token(s.stage, "printing") ||
      (snapshot.current_layer > 0 && s.progress < 100) ||
      (snapshot.remaining_seconds > 0 && !is_finished_status(s.status));

  return s;
}

// User-cancelled Bambu jobs surface as FAILED/FAIL with no concrete error
// payload. Treat as a brief stopped state, then fall back to ready.
void apply_non_error_stop(PrinterSnapshot& snapshot) {
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
}

// Updates print_active / has_error / warn_hms.
//
// Bambu uses print_error_code for user-action prompts during filament
// changes (e.g. "please feed filament"), not real errors — so we suppress
// err_print during a filament stage.
void apply_active_and_error_flags(PrinterSnapshot& snapshot, const UiStateSignals& s) {
  if (s.ps_failed || s.done_strict) {
    snapshot.print_active = false;
  } else if (s.active_hint) {
    snapshot.print_active = true;
  } else {
    snapshot.print_active = false;
  }

  snapshot.has_error = s.input_error || s.ps_failed || (s.err_print && !s.filament_stage);
  snapshot.warn_hms = s.err_hms && snapshot.print_active && !snapshot.has_error;
}

PrintLifecycleState select_lifecycle(const PrinterSnapshot& snapshot, const UiStateSignals& s) {
  if (snapshot.has_error) return PrintLifecycleState::kError;
  if (s.done_strict) return PrintLifecycleState::kFinished;
  if (s.paused) return PrintLifecycleState::kPaused;
  if (s.preparing) return PrintLifecycleState::kPreparing;
  if (snapshot.print_active) return PrintLifecycleState::kPrinting;
  if (s.idleish || s.idle_status) return PrintLifecycleState::kIdle;
  return snapshot.lifecycle;  // unchanged
}

// Fallback path used when no specific stage / status classifier matched.
// Picks the most descriptive label available, in priority order:
// pretty stage > pretty status > shortened stage > shortened status >
// connection-aware default ("waiting..."/"offline").
std::string select_ui_status_fallback(const PrinterSnapshot& snapshot, const UiStateSignals& s) {
  const std::string stage_text = pretty_stage(s.stage);
  if (!stage_text.empty() && !contains_token(s.stage, "idle") &&
      !contains_token(s.stage, "offline")) {
    return stage_text;
  }
  const std::string status_text = pretty_status(s.status);
  if (!status_text.empty()) {
    return status_text;
  }
  if (!s.stage.empty()) {
    return shorten(titlecase_words(s.stage), 18);
  }
  if (!s.status.empty()) {
    return shorten(titlecase_words(s.status), 18);
  }
  return snapshot.wifi_connected ? "waiting..." : "offline";
}

std::string select_ui_status(const PrinterSnapshot& snapshot, const UiStateSignals& s) {
  if (s.filament_stage) {
    return contains_token(s.stage, "filament_unloading") ? "unloading" : "loading";
  }
  if (s.download_stage) return "downloading";
  if (snapshot.has_error) return "failed";
  if (snapshot.warn_hms) return "printing";
  if (s.done_strict) return "done";
  if (s.preheat_stage) return "preheating";
  if (s.clean_stage) return "clean nozzle";
  if (s.level_stage) return "bed level";
  if (s.cooling_stage) return "cooling";
  if (s.setup_stage || s.preparing) return "preparing";

  const bool offline =
      contains_token(s.stage, "offline") || s.status == "offline" ||
      (!snapshot.wifi_connected &&
       snapshot.connection != PrinterConnectionState::kWaitingForCredentials);

  if (s.idleish || offline) {
    if (offline) return "offline";
    if (s.preparing) return "preparing";
    if ((s.progress > 0 && s.progress < 100) || snapshot.print_active) return "printing";
    return "idle";
  }

  return select_ui_status_fallback(snapshot, s);
}

}  // namespace

// --- Exported stage classification helpers (case-insensitive) -----------------

bool is_download_stage(const std::string& stage, const std::string& status) {
  const std::string ls = lower_copy(stage);
  const std::string lst = lower_copy(status);
  return contains_token(ls, "model_download") || contains_token(ls, "download") ||
         contains_token(lst, "download");
}

bool is_filament_stage(const std::string& stage) {
  const std::string ls = lower_copy(stage);
  return contains_token(ls, "filament_loading") || contains_token(ls, "filament_unloading") ||
         contains_token(ls, "changing_filament");
}

bool is_post_download_handoff_stage(const std::string& stage, const std::string& status) {
  if (is_download_stage(stage, status)) {
    return false;
  }
  const std::string ls = lower_copy(stage);
  return is_preheat_stage(ls) || is_clean_stage(ls) || is_level_stage(ls) ||
         is_setup_stage(ls) || contains_token(ls, "filament_loading") ||
         contains_token(ls, "filament_unloading") || contains_token(ls, "changing_filament") ||
         contains_token(ls, "printing");
}

void resolve_ui_state(PrinterSnapshot& snapshot) {
  if (is_non_error_stop(snapshot)) {
    apply_non_error_stop(snapshot);
    return;
  }

  const UiStateSignals s = build_ui_state_signals(snapshot);

  apply_active_and_error_flags(snapshot, s);
  snapshot.lifecycle = select_lifecycle(snapshot, s);
  snapshot.ui_status = select_ui_status(snapshot, s);

  apply_resolved_detail(snapshot);
}

PrinterSnapshot merge_status_sources(const PrinterSnapshot& local_snapshot, bool local_printer_enabled,
                                     const BambuCloudSnapshot& cloud_snapshot,
                                     SourceMode source_mode, uint64_t now_ms,
                                     bool wifi_connected, const std::string& wifi_ip) {
  PrinterSnapshot snapshot;
  const bool local_enabled = source_mode != SourceMode::kCloudOnly && local_printer_enabled;
  const bool cloud_enabled = source_mode != SourceMode::kLocalOnly && cloud_snapshot.configured;
  copy_source_metadata(snapshot, local_snapshot, local_printer_enabled, cloud_snapshot,
                       wifi_connected, wifi_ip);
  apply_base_state_for_mode(snapshot, source_mode, local_snapshot, local_enabled, cloud_snapshot,
                            cloud_enabled);

  const PrinterModel routing_model =
      preferred_model_for_routing(local_snapshot, cloud_snapshot);
  const bool prefer_cloud_status =
      source_mode == SourceMode::kHybrid &&
      printer_model_prefers_cloud_status(routing_model);

  StatusSourceAdapter local_source =
      make_local_source_adapter(local_snapshot, local_enabled, routing_model, now_ms);
  const StatusSourceAdapter cloud_source =
      make_cloud_source_adapter(cloud_snapshot, cloud_enabled, now_ms);
  if (prefer_cloud_status) {
    local_source.status_usable = local_source.status_usable && !cloud_source.status_usable;
    local_source.metrics_usable = local_source.metrics_usable && !cloud_source.metrics_usable;
    local_source.temperatures_usable =
        local_source.temperatures_usable && !cloud_source.temperatures_usable;
    local_source.light_usable = local_source.light_usable && !cloud_source.light_usable;
    local_source.error_usable = local_source.error_usable && !cloud_source.error_usable;
  }

  if (local_source.status_usable) {
    snapshot.status_source = FieldSource::kLocal;
    apply_local_status_bundle(snapshot, local_snapshot);
  } else if (cloud_source.status_usable) {
    snapshot.status_source = FieldSource::kCloud;
    apply_cloud_status_bundle(snapshot, cloud_snapshot);
  }

  if (local_source.metrics_usable) {
    snapshot.metrics_source = FieldSource::kLocal;
    apply_local_metrics_bundle(snapshot, local_snapshot);
  } else if (cloud_source.metrics_usable) {
    snapshot.metrics_source = FieldSource::kCloud;
    apply_cloud_metrics_bundle(snapshot, cloud_snapshot);
  }

  // The slicer-side filament estimate (weight / length) is only ever
  // reported by the Bambu Cloud task descriptor — local MQTT has no
  // equivalent field. When local metrics win the routing decision we
  // still want to surface the cloud estimate, so merge it as a
  // best-effort fallback whenever the cloud carries a value and the
  // local pipeline did not populate one.
  if (cloud_enabled) {
    if (snapshot.estimated_filament_weight_g <= 0.0f &&
        cloud_snapshot.estimated_filament_weight_g > 0.0f) {
      snapshot.estimated_filament_weight_g = cloud_snapshot.estimated_filament_weight_g;
    }
    if (snapshot.estimated_filament_length_mm <= 0.0f &&
        cloud_snapshot.estimated_filament_length_mm > 0.0f) {
      snapshot.estimated_filament_length_mm = cloud_snapshot.estimated_filament_length_mm;
    }
    if (snapshot.job_name.empty() && !cloud_snapshot.preview_title.empty()) {
      snapshot.job_name = cloud_snapshot.preview_title;
    }
  }

  if (local_source.temperatures_usable) {
    apply_local_temperature_bundle(snapshot, local_snapshot);
  } else if (cloud_source.temperatures_usable) {
    apply_cloud_temperature_bundle(snapshot, cloud_snapshot, now_ms);
  }
  snapshot.chamber_light_supported =
      (local_enabled && local_snapshot.chamber_light_supported) ||
      (cloud_enabled && cloud_snapshot.chamber_light_supported);
  snapshot.chamber_light_state_known = false;
  snapshot.chamber_light_on = false;
  if (local_source.light_usable) {
    snapshot.chamber_light_state_known = true;
    snapshot.chamber_light_on = local_snapshot.chamber_light_on;
  } else if (cloud_source.light_usable) {
    snapshot.chamber_light_state_known = true;
    snapshot.chamber_light_on = cloud_snapshot.chamber_light_on;
  }
  if (local_source.error_usable) {
    apply_local_error_bundle(snapshot, local_snapshot);
  } else if (cloud_source.error_usable) {
    apply_cloud_error_bundle(snapshot, cloud_snapshot);
  }

  if (local_source.ams_usable) {
    snapshot.hw_switch_state = local_snapshot.hw_switch_state;
    snapshot.tray_now = local_snapshot.tray_now;
    snapshot.tray_tar = local_snapshot.tray_tar;
    snapshot.ams = local_snapshot.ams;
  } else if (cloud_source.ams_usable) {
    snapshot.hw_switch_state = cloud_snapshot.hw_switch_state;
    snapshot.tray_now = cloud_snapshot.tray_now;
    snapshot.tray_tar = cloud_snapshot.tray_tar;
    snapshot.ams = cloud_snapshot.ams;
  }

  if (cloud_enabled && cloud_preview_available(cloud_snapshot, now_ms)) {
    snapshot.preview_source = FieldSource::kCloud;
    apply_cloud_preview_bundle(snapshot, cloud_snapshot);
  }
  if (local_enabled && local_camera_available(local_snapshot)) {
    snapshot.camera_source = FieldSource::kLocal;
  }

  if (snapshot.job_name.empty() && !local_snapshot.job_name.empty() &&
      local_source.metrics_usable) {
    snapshot.job_name = local_snapshot.job_name;
  }
  if (snapshot.job_name.empty() && !cloud_snapshot.preview_title.empty() &&
      cloud_source.metrics_usable) {
    snapshot.job_name = cloud_snapshot.preview_title;
  }

  if (snapshot.detail.empty()) {
    if (snapshot.status_source == FieldSource::kCloud && !cloud_snapshot.detail.empty()) {
      snapshot.detail = cloud_snapshot.detail;
    } else if (snapshot.status_source == FieldSource::kLocal && !local_snapshot.detail.empty()) {
      snapshot.detail = local_snapshot.detail;
    } else if (!local_snapshot.detail.empty() && local_enabled) {
      snapshot.detail = local_snapshot.detail;
    } else if (!cloud_snapshot.detail.empty() && cloud_enabled &&
               (source_mode == SourceMode::kCloudOnly || !local_enabled ||
                snapshot.status_source == FieldSource::kCloud)) {
      snapshot.detail = cloud_snapshot.detail;
    }
  }

  if (snapshot.stage.empty()) {
    if (snapshot.status_source == FieldSource::kLocal) {
      snapshot.stage = local_snapshot.stage;
    } else if (snapshot.status_source == FieldSource::kCloud) {
      snapshot.stage = cloud_snapshot.stage;
    }
  }

  const PrinterModel effective_model = effective_model_for_snapshot(snapshot);
  if (effective_model != PrinterModel::kUnknown) {
    if (!printer_model_has_chamber_temperature(effective_model)) {
      snapshot.chamber_temp_c = 0.0f;
      snapshot.chamber_temp_known = false;
    }
    if (!printer_model_has_secondary_nozzle_temperature(effective_model)) {
      snapshot.secondary_nozzle_temp_c = 0.0f;
      snapshot.secondary_nozzle_temp_known = false;
    }
    if (!printer_model_has_chamber_light(effective_model)) {
      snapshot.chamber_light_supported = false;
      snapshot.chamber_light_state_known = false;
      snapshot.chamber_light_on = false;
    } else {
      snapshot.chamber_light_supported = true;
    }
  } else if (printer_serial_family_has_no_chamber_temperature(snapshot.resolved_serial)) {
    snapshot.chamber_temp_c = 0.0f;
    snapshot.chamber_temp_known = false;
  }

  return snapshot;
}

}  // namespace printsphere
