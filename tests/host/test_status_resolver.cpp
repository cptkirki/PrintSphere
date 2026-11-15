#include "doctest/doctest.h"

#include <string>

#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/printer_state.hpp"
#include "printsphere/status_resolver.hpp"

using namespace printsphere;

namespace {

// All freshness windows in status_resolver.cpp are evaluated as
// "now_ms - last_update_ms < window"
// 
// Picking a now_ms large enough that any "now_ms" in tests is
// comfortably ahead of "last_update_ms = now_ms - 1000"
constexpr uint64_t kNowMs = 1'000'000ULL;

// Constructor for fake PrinterSnaphot
PrinterSnapshot make_local_printing(PrinterModel model = PrinterModel::kP1S) {
  PrinterSnapshot s;

  s.connection = PrinterConnectionState::kOnline;
  s.lifecycle = PrintLifecycleState::kPrinting;
  s.local_configured = true;
  s.local_connected = true;
  s.local_last_update_ms = kNowMs - 1000;
  s.local_model = model;
  s.local_capabilities = default_local_capabilities_for_model(model);
  s.raw_status = "RUNNING";
  s.raw_stage = "printing";
  s.stage = "printing";
  s.detail = "Local print in progress";
  s.progress_percent = 33.0f;
  s.current_layer = 50;
  s.total_layers = 200;
  s.nozzle_temp_c = 215.0f;
  s.nozzle_temp_known = true;
  s.bed_temp_c = 60.0f;
  s.bed_temp_known = true;
  s.job_name = "local_job.gcode";

  return s;
}

// Same constructor but for cloud snapshot
BambuCloudSnapshot make_cloud_printing(PrinterModel model = PrinterModel::kP1S) {
  BambuCloudSnapshot c;

  c.configured = true;
  c.connected = true;
  c.last_update_ms = kNowMs - 1000;
  c.model = model;
  c.capabilities = default_cloud_capabilities();
  c.lifecycle = PrintLifecycleState::kPrinting;
  c.detail = "Cloud reports running";
  c.raw_status = "RUNNING";
  c.raw_stage = "printing";
  c.stage = "printing";
  c.progress_percent = 67.0f;
  c.current_layer = 80;
  c.total_layers = 200;
  c.nozzle_temp_c = 220.0f;
  c.nozzle_temp_last_update_ms = kNowMs - 1000;
  c.bed_temp_c = 60.0f;
  c.bed_temp_last_update_ms = kNowMs - 1000;
  c.preview_title = "cloud_job";

  return c;
}

}  // namespace

TEST_CASE("is_download_stage matches stage and status tokens") {
  CHECK(is_download_stage("model_download", ""));
  CHECK(is_download_stage("downloading", ""));
  CHECK(is_download_stage("", "downloading file"));

  CHECK_FALSE(is_download_stage("printing", "running"));
  CHECK_FALSE(is_download_stage("", ""));
}

TEST_CASE("is_filament_stage covers AMS load/unload/change") {
  CHECK(is_filament_stage("filament_loading"));
  CHECK(is_filament_stage("filament_unloading"));
  CHECK(is_filament_stage("changing_filament"));
  CHECK(is_filament_stage("Filament_Loading"));  // case insensitive

  CHECK_FALSE(is_filament_stage("printing"));
  CHECK_FALSE(is_filament_stage(""));
}

TEST_CASE("is_post_download_handoff_stage excludes the download stage itself") {
  CHECK_FALSE(is_post_download_handoff_stage("model_download", ""));
  CHECK_FALSE(is_post_download_handoff_stage("", "downloading"));

  CHECK(is_post_download_handoff_stage("filament_loading", ""));
  CHECK(is_post_download_handoff_stage("printing", ""));
  CHECK(is_post_download_handoff_stage("heatbed_preheating", ""));
}

TEST_CASE("resolve_ui_state sets a sensible ui_status for an idle local printer") {
  PrinterSnapshot s;

  s.wifi_connected = true;
  s.connection = PrinterConnectionState::kOnline;
  s.lifecycle = PrintLifecycleState::kIdle;
  s.raw_status = "IDLE";
  s.local_connected = true;

  resolve_ui_state(s);

  CHECK(s.ui_status == "idle");
  CHECK_FALSE(s.print_active);
  CHECK_FALSE(s.has_error);
}

TEST_CASE("resolve_ui_state flags has_error when a print error code is present") {
  PrinterSnapshot s;

  s.wifi_connected = true;
  s.connection = PrinterConnectionState::kOnline;
  s.local_connected = true;
  s.lifecycle = PrintLifecycleState::kPrinting;
  s.raw_status = "RUNNING";
  s.raw_stage = "printing";
  s.print_error_code = 0x0500'1000;  // arbitrary non-zero

  resolve_ui_state(s);

  CHECK(s.has_error);
  CHECK(s.lifecycle == PrintLifecycleState::kError);
}

TEST_CASE("merge_status_sources cloud-only mode picks cloud everywhere") {
  PrinterSnapshot local; // unconfigured
  BambuCloudSnapshot cloud = make_cloud_printing();

  const PrinterSnapshot merged = merge_status_sources(
      local, /*local_printer_enabled=*/false, cloud, SourceMode::kCloudOnly,
      kNowMs, /*wifi_connected=*/true, "192.168.1.10",
      /*print_activity_seen_this_session=*/false);

  CHECK(merged.cloud_configured);
  CHECK(merged.cloud_connected);
  CHECK(merged.status_source == FieldSource::kCloud);
  CHECK(merged.metrics_source == FieldSource::kCloud);
  CHECK(merged.lifecycle == PrintLifecycleState::kPrinting);
  CHECK(merged.progress_percent == doctest::Approx(67.0f));
  CHECK(merged.current_layer == 80);
  CHECK(merged.total_layers == 200);

  CHECK_FALSE(merged.local_configured);
}

TEST_CASE("merge_status_sources local-only mode ignores cloud entirely") {
  PrinterSnapshot local = make_local_printing(PrinterModel::kP1S);
  BambuCloudSnapshot cloud = make_cloud_printing(PrinterModel::kP1S);

  const PrinterSnapshot merged = merge_status_sources(
      local, /*local_printer_enabled=*/true, cloud, SourceMode::kLocalOnly,
      kNowMs, true, "192.168.1.10", false);

  CHECK(merged.local_configured);
  CHECK(merged.local_connected);
  CHECK(merged.status_source == FieldSource::kLocal);
  CHECK(merged.metrics_source == FieldSource::kLocal);
  CHECK(merged.progress_percent == doctest::Approx(33.0f));
  CHECK(merged.current_layer == 50);

  // Cloud preview must NOT leak into local-only mode
  CHECK(merged.preview_source == FieldSource::kNone);
}

TEST_CASE("merge_status_sources hybrid prefers local for P1S (local-favoring model)") {
  PrinterSnapshot local = make_local_printing(PrinterModel::kP1S);
  BambuCloudSnapshot cloud = make_cloud_printing(PrinterModel::kP1S);

  const PrinterSnapshot merged = merge_status_sources(
      local, true, cloud, SourceMode::kHybrid, kNowMs, true, "192.168.1.10", false);

  CHECK(merged.status_source == FieldSource::kLocal);
  CHECK(merged.metrics_source == FieldSource::kLocal);
  CHECK(merged.progress_percent == doctest::Approx(33.0f));
}

TEST_CASE("merge_status_sources hybrid prefers cloud for P2S (cloud-favoring model)") {
  PrinterSnapshot local = make_local_printing(PrinterModel::kP2S);
  BambuCloudSnapshot cloud = make_cloud_printing(PrinterModel::kP2S);

  const PrinterSnapshot merged = merge_status_sources(
      local, true, cloud, SourceMode::kHybrid, kNowMs, true, "192.168.1.10", false);

  CHECK(merged.status_source == FieldSource::kCloud);
  CHECK(merged.metrics_source == FieldSource::kCloud);
  CHECK(merged.progress_percent == doctest::Approx(67.0f));
}

TEST_CASE("merge_status_sources hybrid falls back to cloud when local is stale") {
  PrinterSnapshot local = make_local_printing(PrinterModel::kP1S);
  BambuCloudSnapshot cloud = make_cloud_printing(PrinterModel::kP1S);

  // Push local last update beyond the 90s freshness window
  local.local_last_update_ms = kNowMs - (90ULL * 1000ULL + 5'000ULL);

  const PrinterSnapshot merged = merge_status_sources(
      local, true, cloud, SourceMode::kHybrid, kNowMs, true, "192.168.1.10", false);

  CHECK(merged.status_source == FieldSource::kCloud);
  CHECK(merged.progress_percent == doctest::Approx(67.0f));
}

TEST_CASE("merge_status_sources strips chamber temperature for models that lack it") {
  // Make sure the routing model resolves to P1S (no chamber temp)
  BambuCloudSnapshot cloud; // not configured
  PrinterSnapshot local = make_local_printing(PrinterModel::kP1S);

  local.chamber_temp_c = 42.0f;
  local.chamber_temp_known = true;

  const PrinterSnapshot merged = merge_status_sources(
      local, true, cloud, SourceMode::kLocalOnly, kNowMs, true, "192.168.1.10", false);

  CHECK_FALSE(merged.chamber_temp_known);
  CHECK(merged.chamber_temp_c == doctest::Approx(0.0f));
}

TEST_CASE("merge_status_sources keeps chamber temperature for models that support it") {
  PrinterSnapshot local = make_local_printing(PrinterModel::kX1C);
  BambuCloudSnapshot cloud; // not configured

  local.chamber_temp_c = 42.0f;
  local.chamber_temp_known = true;

  const PrinterSnapshot merged = merge_status_sources(
      local, true, cloud, SourceMode::kLocalOnly, kNowMs, true, "192.168.1.10", false);

  CHECK(merged.chamber_temp_known);
  CHECK(merged.chamber_temp_c == doctest::Approx(42.0f));
}

TEST_CASE("merge_status_sources cloud preview is exposed in cloud-capable modes") {
  PrinterSnapshot local;
  BambuCloudSnapshot cloud = make_cloud_printing();

  cloud.preview_blob = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
  cloud.preview_url = "https://example.com/cover.png";

  const PrinterSnapshot merged_cloud_only = merge_status_sources(
      local, false, cloud, SourceMode::kCloudOnly, kNowMs, true, "192.168.1.10", false);
  
  CHECK(merged_cloud_only.preview_source == FieldSource::kCloud);
  REQUIRE(merged_cloud_only.preview_blob.get() != nullptr);
  CHECK(merged_cloud_only.preview_blob->size() == 3U);
}

TEST_CASE("merge_status_sources without any source falls back to setup state") {
  PrinterSnapshot local;
  BambuCloudSnapshot cloud;

  const PrinterSnapshot merged = merge_status_sources(
      local, /*local_printer_enabled=*/false, cloud, SourceMode::kHybrid, kNowMs,
      /*wifi_connected=*/false, "", false);

  CHECK(merged.connection == PrinterConnectionState::kWaitingForCredentials);
  CHECK(merged.lifecycle == PrintLifecycleState::kUnknown);
  CHECK(merged.stage == "setup");
}
