#include "printsphere/printer_state.hpp"

#include <cctype>
#include <utility>

namespace printsphere {

void PrinterStateStore::set_snapshot(PrinterSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(snapshot);
}

PrinterSnapshot PrinterStateStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

const char* to_string(PrinterConnectionState state) {
  switch (state) {
    case PrinterConnectionState::kBooting:
      return "booting";
    case PrinterConnectionState::kWaitingForCredentials:
      return "waiting_for_credentials";
    case PrinterConnectionState::kReadyForLanConnect:
      return "ready_for_lan_connect";
    case PrinterConnectionState::kConnecting:
      return "connecting";
    case PrinterConnectionState::kOnline:
      return "online";
    case PrinterConnectionState::kError:
      return "error";
  }

  return "unknown";
}

const char* to_string(PrintLifecycleState state) {
  switch (state) {
    case PrintLifecycleState::kUnknown:
      return "unknown";
    case PrintLifecycleState::kIdle:
      return "idle";
    case PrintLifecycleState::kPreparing:
      return "preparing";
    case PrintLifecycleState::kPrinting:
      return "printing";
    case PrintLifecycleState::kPaused:
      return "paused";
    case PrintLifecycleState::kFinished:
      return "finished";
    case PrintLifecycleState::kError:
      return "error";
  }

  return "unknown";
}

const char* to_string(PrinterModel model) {
  switch (model) {
    case PrinterModel::kA1:
      return "A1";
    case PrinterModel::kA1Mini:
      return "A1MINI";
    case PrinterModel::kP1P:
      return "P1P";
    case PrinterModel::kP1S:
      return "P1S";
    case PrinterModel::kP2S:
      return "P2S";
    case PrinterModel::kH2C:
      return "H2C";
    case PrinterModel::kH2D:
      return "H2D";
    case PrinterModel::kH2DPro:
      return "H2DPRO";
    case PrinterModel::kH2S:
      return "H2S";
    case PrinterModel::kX1:
      return "X1";
    case PrinterModel::kX1C:
      return "X1C";
    case PrinterModel::kX1E:
      return "X1E";
    case PrinterModel::kUnknown:
    default:
      return "UNKNOWN";
  }
}

const char* to_string(FieldSource source) {
  switch (source) {
    case FieldSource::kLocal:
      return "local";
    case FieldSource::kCloud:
      return "cloud";
    case FieldSource::kNone:
    default:
      return "none";
  }
}

bool printer_model_has_jpeg_camera(PrinterModel model) {
  switch (model) {
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kP2S:
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
    default:
      return false;
  }
}

bool printer_model_has_rtsp_camera(PrinterModel model) {
  switch (model) {
    case PrinterModel::kP2S:
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
    default:
      return false;
  }
}

bool printer_model_has_chamber_temperature(PrinterModel model) {
  switch (model) {
    case PrinterModel::kP2S:
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
    default:
      return false;
  }
}

bool printer_model_has_secondary_nozzle_temperature(PrinterModel model) {
  switch (model) {
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
    case PrinterModel::kP2S:
    case PrinterModel::kH2C:
    case PrinterModel::kH2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
    default:
      return false;
  }
}

bool printer_model_has_chamber_light(PrinterModel model) {
  switch (model) {
    case PrinterModel::kP1S:
    case PrinterModel::kP2S:
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    default:
      return false;
  }
}

bool printer_model_has_secondary_chamber_light(PrinterModel model) {
  switch (model) {
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
    case PrinterModel::kP2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
    default:
      return false;
  }
}

bool printer_model_supports_local_status(PrinterModel model) {
  switch (model) {
    case PrinterModel::kP2S:
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
    default:
      return true;
  }
}

bool printer_model_requires_developer_mode_for_local_status(PrinterModel model) {
  switch (model) {
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
    case PrinterModel::kP2S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
    default:
      return false;
  }
}

bool printer_model_prefers_cloud_status(PrinterModel model) {
  switch (model) {
    case PrinterModel::kP2S:
    case PrinterModel::kH2C:
    case PrinterModel::kH2D:
    case PrinterModel::kH2DPro:
    case PrinterModel::kH2S:
      return true;
    case PrinterModel::kUnknown:
    case PrinterModel::kA1:
    case PrinterModel::kA1Mini:
    case PrinterModel::kP1P:
    case PrinterModel::kP1S:
    case PrinterModel::kX1:
    case PrinterModel::kX1C:
    case PrinterModel::kX1E:
    default:
      return false;
  }
}

bool printer_serial_family_has_no_chamber_temperature(const std::string& serial) {
  if (serial.size() < 3U) {
    return false;
  }

  const char first = static_cast<char>(std::toupper(static_cast<unsigned char>(serial[0])));
  const char second = static_cast<char>(std::toupper(static_cast<unsigned char>(serial[1])));
  const char third = static_cast<char>(std::toupper(static_cast<unsigned char>(serial[2])));
  return first == '0' && second == '1' && third == 'P';
}

SourceCapabilities default_local_capabilities_for_model(PrinterModel model) {
  SourceCapabilities capabilities;
  capabilities.status = printer_model_supports_local_status(model);
  capabilities.metrics = capabilities.status;
  capabilities.temperatures = capabilities.status;
  capabilities.hms = capabilities.status;
  capabilities.print_error = capabilities.status;
  capabilities.camera_jpeg_socket = printer_model_has_jpeg_camera(model);
  capabilities.camera_rtsp = printer_model_has_rtsp_camera(model);
  capabilities.developer_mode_required =
      printer_model_requires_developer_mode_for_local_status(model);
  return capabilities;
}

SourceCapabilities default_cloud_capabilities() {
  SourceCapabilities capabilities;
  capabilities.status = true;
  capabilities.metrics = true;
  capabilities.temperatures = true;
  capabilities.preview = true;
  capabilities.hms = true;
  capabilities.print_error = true;
  return capabilities;
}

}  // namespace printsphere
