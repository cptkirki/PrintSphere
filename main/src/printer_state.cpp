#include "printsphere/printer_state.hpp"

#include <cctype>
#include <cstdio>
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

const char* to_string(PrintCommand command) {
  switch (command) {
    case PrintCommand::kNone:
      return "none";
    case PrintCommand::kPause:
      return "pause";
    case PrintCommand::kResume:
      return "resume";
    case PrintCommand::kStop:
      return "stop";
  }
  return "none";
}

const char* mqtt_command_token(PrintCommand command) {
  switch (command) {
    case PrintCommand::kPause:
      return "pause";
    case PrintCommand::kResume:
      return "resume";
    case PrintCommand::kStop:
      return "stop";
    case PrintCommand::kNone:
    default:
      return "";
  }
}

std::string build_ledctrl_payload(const char* node, bool on) {
  if (node == nullptr || *node == '\0') {
    return {};
  }

  char payload[256];
  const int written = std::snprintf(
      payload, sizeof(payload),
      "{\"system\":{\"sequence_id\":\"0\",\"command\":\"ledctrl\","
      "\"led_node\":\"%s\",\"led_mode\":\"%s\",\"led_on_time\":500,"
      "\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0}}",
      node, on ? "on" : "off");
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    return {};
  }
  return payload;
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
    case PrinterModel::kX2D:
      return "X2D";
    case PrinterModel::kA2L:
      return "A2L";
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

// -----------------------------------------------------------------------------
// Per-model capability table.
//
// Adding a new printer model is a two-step change:
//   1. Add an entry to PrinterModel in printer_state.hpp.
//   2. Add a row here, in the same order as the enum.
// kModelCapabilities is indexed by static_cast<size_t>(PrinterModel) — the
// row order MUST match the enum declaration. caps_for() guards against
// out-of-range values by falling back to the kUnknown row.
//
// All flags are stored once, so a new capability bit needs to be set
// exactly once per model — not in nine parallel switch statements.
// -----------------------------------------------------------------------------

namespace {

struct ModelCapabilityFlags {
  bool jpeg_camera;
  bool rtsp_camera;
  bool chamber_temperature;
  bool secondary_nozzle_temperature;
  bool chamber_light;
  bool secondary_chamber_light;
  bool supports_local_status;
  bool requires_developer_mode_for_local_status;
  bool prefers_cloud_status;
};

// Indexed by static_cast<size_t>(PrinterModel). Order MUST match the enum
// PrinterModel in printer_state.hpp.
constexpr ModelCapabilityFlags kModelCapabilities[] = {
    // jpeg   rtsp   chmbT  sNzlT  chmbL  sChmbL local  devLoc cloud
    // A1/A1 Mini/P1P/A2L still use MQTT node "chamber_light" for the toolhead LED.
    /* kUnknown */ {false, false, false, false, false, false, true,  false, false},
    /* kA1      */ {true,  false, false, false, true,  false, true,  false, false},
    /* kA1Mini  */ {true,  false, false, false, true,  false, true,  false, false},
    /* kP1P     */ {true,  false, false, false, true,  false, true,  false, false},
    /* kP1S     */ {true,  false, false, false, true,  false, true,  false, false},
    /* kP2S     */ {false, true,  true,  false, true,  false, true,  false, true },
    /* kH2C     */ {false, true,  true,  false, true,  true,  true,  true,  true },
    /* kH2D     */ {false, true,  true,  true,  true,  true,  true,  true,  true },
    /* kH2DPro  */ {false, true,  true,  true,  true,  true,  true,  true,  true },
    /* kH2S     */ {false, true,  true,  false, true,  true,  true,  true,  true },
    /* kX1      */ {false, true,  true,  false, true,  false, true,  false, true },
    /* kX1C     */ {false, true,  true,  false, true,  false, true,  false, true },
    /* kX1E     */ {false, true,  true,  false, true,  false, true,  false, true },
    /* kX2D     */ {false, true,  true,  true,  true,  true,  true,  true,  true },
    /* kA2L     */ {true,  false, false, false, true,  false, true,  false, false},
};

constexpr size_t kModelCapabilityCount =
    sizeof(kModelCapabilities) / sizeof(kModelCapabilities[0]);

// Compile-time guard: the table must have one row per enum value.
// Update this constant if the PrinterModel enum gains/loses members.
static_assert(kModelCapabilityCount == 15,
              "kModelCapabilities row count must match PrinterModel enum size");

constexpr const ModelCapabilityFlags& caps_for(PrinterModel model) {
  const auto idx = static_cast<size_t>(model);
  return idx < kModelCapabilityCount ? kModelCapabilities[idx]
                                     : kModelCapabilities[0];  // kUnknown row
}

}  // namespace

bool printer_model_has_jpeg_camera(PrinterModel model) {
  return caps_for(model).jpeg_camera;
}

bool printer_model_has_rtsp_camera(PrinterModel model) {
  return caps_for(model).rtsp_camera;
}

bool printer_model_has_chamber_temperature(PrinterModel model) {
  return caps_for(model).chamber_temperature;
}

bool printer_model_has_secondary_nozzle_temperature(PrinterModel model) {
  return caps_for(model).secondary_nozzle_temperature;
}

bool printer_model_has_chamber_light(PrinterModel model) {
  return caps_for(model).chamber_light;
}

bool printer_model_has_secondary_chamber_light(PrinterModel model) {
  return caps_for(model).secondary_chamber_light;
}

bool printer_model_supports_local_status(PrinterModel model) {
  return caps_for(model).supports_local_status;
}

bool printer_model_requires_developer_mode_for_local_status(PrinterModel model) {
  return caps_for(model).requires_developer_mode_for_local_status;
}

bool printer_model_prefers_cloud_status(PrinterModel model) {
  return caps_for(model).prefers_cloud_status;
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
