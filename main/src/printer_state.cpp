#include "printsphere/printer_state.hpp"

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

}  // namespace printsphere
