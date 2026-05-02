#include "doctest/doctest.h"

#include <array>

#include "printsphere/printer_state.hpp"

using namespace printsphere;

namespace {

constexpr std::array<PrinterModel, 12> kAllModels{
    PrinterModel::kA1,  PrinterModel::kA1Mini,  PrinterModel::kP1P,
    PrinterModel::kP1S, PrinterModel::kP2S,     PrinterModel::kH2C,
    PrinterModel::kH2D, PrinterModel::kH2DPro,  PrinterModel::kH2S,
    PrinterModel::kX1,  PrinterModel::kX1C,     PrinterModel::kX1E,
};

}  // namespace

TEST_CASE("PrinterStateStore round-trips a snapshot") {
  PrinterStateStore store;
  PrinterSnapshot in;
  in.connection = PrinterConnectionState::kOnline;
  in.lifecycle = PrintLifecycleState::kPrinting;
  in.progress_percent = 42.5f;
  in.current_layer = 17;
  in.total_layers = 200;
  in.local_model = PrinterModel::kP1S;
  store.set_snapshot(in);

  const PrinterSnapshot out = store.snapshot();
  CHECK(out.connection == PrinterConnectionState::kOnline);
  CHECK(out.lifecycle == PrintLifecycleState::kPrinting);
  CHECK(out.progress_percent == doctest::Approx(42.5f));
  CHECK(out.current_layer == 17);
  CHECK(out.total_layers == 200);
  CHECK(out.local_model == PrinterModel::kP1S);
}

TEST_CASE("printer_model_has_jpeg_camera matches the documented set") {
  CHECK(printer_model_has_jpeg_camera(PrinterModel::kA1));
  CHECK(printer_model_has_jpeg_camera(PrinterModel::kA1Mini));
  CHECK(printer_model_has_jpeg_camera(PrinterModel::kP1P));
  CHECK(printer_model_has_jpeg_camera(PrinterModel::kP1S));

  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kP2S));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kH2C));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kH2D));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kH2DPro));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kH2S));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kX1));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kX1C));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kX1E));
  CHECK_FALSE(printer_model_has_jpeg_camera(PrinterModel::kUnknown));
}

TEST_CASE("printer_model_has_rtsp_camera covers RTSP-only models") {
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kP2S));
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kH2C));
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kH2D));
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kH2DPro));
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kH2S));
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kX1));
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kX1C));
  CHECK(printer_model_has_rtsp_camera(PrinterModel::kX1E));
  
  CHECK_FALSE(printer_model_has_rtsp_camera(PrinterModel::kA1));
  CHECK_FALSE(printer_model_has_rtsp_camera(PrinterModel::kP1S));
}

TEST_CASE("camera capabilities are mutually exclusive (current model set)") {
  // NOTE: Every known model should expose at most one camera transport
  // NOTE: Probably a future model exposes both, this test should be updated deliberately
  for (const PrinterModel model : kAllModels) {
    CAPTURE(to_string(model));
    const bool both = printer_model_has_jpeg_camera(model) &&
                      printer_model_has_rtsp_camera(model);
    CHECK_FALSE(both);
  }
}

TEST_CASE("printer_model_has_chamber_temperature") {
  CHECK(printer_model_has_chamber_temperature(PrinterModel::kP2S));
  CHECK(printer_model_has_chamber_temperature(PrinterModel::kH2D));
  CHECK(printer_model_has_chamber_temperature(PrinterModel::kX1C));

  CHECK_FALSE(printer_model_has_chamber_temperature(PrinterModel::kA1));
  CHECK_FALSE(printer_model_has_chamber_temperature(PrinterModel::kA1Mini));
  CHECK_FALSE(printer_model_has_chamber_temperature(PrinterModel::kP1P));
  CHECK_FALSE(printer_model_has_chamber_temperature(PrinterModel::kP1S));
}

TEST_CASE("printer_model_has_secondary_nozzle_temperature only on H2D family") {
  CHECK(printer_model_has_secondary_nozzle_temperature(PrinterModel::kH2D));
  CHECK(printer_model_has_secondary_nozzle_temperature(PrinterModel::kH2DPro));

  for (const PrinterModel model : kAllModels) {
    if (model == PrinterModel::kH2D || model == PrinterModel::kH2DPro) {
      continue;
    }

    CAPTURE(to_string(model));
    CHECK_FALSE(printer_model_has_secondary_nozzle_temperature(model));
  }
}

TEST_CASE("printer_model_has_chamber_light excludes A1 / P1P") {
  CHECK(printer_model_has_chamber_light(PrinterModel::kP1S));
  CHECK(printer_model_has_chamber_light(PrinterModel::kP2S));
  CHECK(printer_model_has_chamber_light(PrinterModel::kX1C));
  CHECK(printer_model_has_chamber_light(PrinterModel::kH2D));

  CHECK_FALSE(printer_model_has_chamber_light(PrinterModel::kA1));
  CHECK_FALSE(printer_model_has_chamber_light(PrinterModel::kA1Mini));
  CHECK_FALSE(printer_model_has_chamber_light(PrinterModel::kP1P));
}

TEST_CASE("printer_model_has_secondary_chamber_light only on H2 family") {
  CHECK(printer_model_has_secondary_chamber_light(PrinterModel::kH2C));
  CHECK(printer_model_has_secondary_chamber_light(PrinterModel::kH2D));
  CHECK(printer_model_has_secondary_chamber_light(PrinterModel::kH2DPro));
  CHECK(printer_model_has_secondary_chamber_light(PrinterModel::kH2S));

  CHECK_FALSE(printer_model_has_secondary_chamber_light(PrinterModel::kP1S));
  CHECK_FALSE(printer_model_has_secondary_chamber_light(PrinterModel::kX1C));
}

TEST_CASE("printer_model_supports_local_status is true for every known model") {
  for (const PrinterModel model : kAllModels) {
    CAPTURE(to_string(model));
    CHECK(printer_model_supports_local_status(model));
  }

  CHECK(printer_model_supports_local_status(PrinterModel::kUnknown));
}

TEST_CASE("printer_model_requires_developer_mode_for_local_status only H2 family") {
  CHECK(printer_model_requires_developer_mode_for_local_status(PrinterModel::kH2C));
  CHECK(printer_model_requires_developer_mode_for_local_status(PrinterModel::kH2D));
  CHECK(printer_model_requires_developer_mode_for_local_status(PrinterModel::kH2DPro));
  CHECK(printer_model_requires_developer_mode_for_local_status(PrinterModel::kH2S));

  CHECK_FALSE(printer_model_requires_developer_mode_for_local_status(PrinterModel::kP2S));
  CHECK_FALSE(printer_model_requires_developer_mode_for_local_status(PrinterModel::kP1S));
  CHECK_FALSE(printer_model_requires_developer_mode_for_local_status(PrinterModel::kX1));
}

TEST_CASE("printer_model_prefers_cloud_status covers P2S and H2 family only") {
  CHECK(printer_model_prefers_cloud_status(PrinterModel::kP2S));
  CHECK(printer_model_prefers_cloud_status(PrinterModel::kH2C));
  CHECK(printer_model_prefers_cloud_status(PrinterModel::kH2D));
  CHECK(printer_model_prefers_cloud_status(PrinterModel::kH2DPro));
  CHECK(printer_model_prefers_cloud_status(PrinterModel::kH2S));

  for (const PrinterModel model : kAllModels) {
    if (model == PrinterModel::kP2S) {
      continue;
    }
    
    if (model == PrinterModel::kH2C || model == PrinterModel::kH2D ||
        model == PrinterModel::kH2DPro || model == PrinterModel::kH2S) {
      continue;
    }

    CAPTURE(to_string(model));
    CHECK_FALSE(printer_model_prefers_cloud_status(model));
  }
}

TEST_CASE("printer_serial_family_has_no_chamber_temperature") {
  CHECK(printer_serial_family_has_no_chamber_temperature("01P12345"));
  CHECK(printer_serial_family_has_no_chamber_temperature("01p12345"));

  CHECK_FALSE(printer_serial_family_has_no_chamber_temperature("00M12345"));
  CHECK_FALSE(printer_serial_family_has_no_chamber_temperature("01"));
  CHECK_FALSE(printer_serial_family_has_no_chamber_temperature(""));
}

TEST_CASE("default_local_capabilities_for_model wires capability table to traits") {
  const SourceCapabilities p1s = default_local_capabilities_for_model(PrinterModel::kP1S);

  CHECK(p1s.status);
  CHECK(p1s.metrics);
  CHECK(p1s.temperatures);
  CHECK(p1s.hms);
  CHECK(p1s.print_error);
  CHECK(p1s.camera_jpeg_socket);
  CHECK_FALSE(p1s.camera_rtsp);
  CHECK_FALSE(p1s.developer_mode_required);

  const SourceCapabilities h2d = default_local_capabilities_for_model(PrinterModel::kH2D);
  CHECK(h2d.camera_rtsp);
  CHECK_FALSE(h2d.camera_jpeg_socket);
  CHECK(h2d.developer_mode_required);
}

TEST_CASE("default_cloud_capabilities is the full set including preview") {
  const SourceCapabilities cloud = default_cloud_capabilities();
  CHECK(cloud.status);
  CHECK(cloud.metrics);
  CHECK(cloud.temperatures);
  CHECK(cloud.preview);
  CHECK(cloud.hms);
  CHECK(cloud.print_error);
  CHECK_FALSE(cloud.camera_jpeg_socket);
  CHECK_FALSE(cloud.camera_rtsp);
}

TEST_CASE("to_string for PrinterModel covers every value") {
  for (const PrinterModel model : kAllModels) {
    CHECK(to_string(model) != nullptr);
    CHECK(to_string(model)[0] != '\0');
  }
  
  CHECK(std::string(to_string(PrinterModel::kUnknown)) == "UNKNOWN");
}
