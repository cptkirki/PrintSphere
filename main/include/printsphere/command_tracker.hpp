#pragma once
#include <cstdint>
#include <mutex>
#include <string>

namespace printsphere {
// MQTT publication is not a printer acknowledgement. A command gets one
// sequence ID and remains pending until its own result arrives or times out.
class CommandTracker {
 public:
  struct Snapshot { bool busy = false; std::string detail; };
  bool queue(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_) return false;
    busy_ = true; sequence_ = 0; started_ms_ = now_ms;
    detail_ = "Command queued"; return true;
  }
  void sent(uint32_t sequence, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    sequence_ = sequence; started_ms_ = now_ms; busy_ = true;
    detail_ = "Waiting for printer acknowledgement";
  }
  void result(uint32_t sequence, bool accepted) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!busy_ || !sequence_ || sequence != sequence_) return;
    busy_ = false;
    detail_ = accepted ? "Printer accepted command" : "Printer rejected command";
  }
  void failed() {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false; detail_ = "Command could not be delivered";
  }
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false; sequence_ = 0; detail_.clear();
  }
  Snapshot snapshot(uint64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ && now_ms - started_ms_ >= 15000) {
      busy_ = false; detail_ = "No printer acknowledgement; check printer";
    }
    // Terminal results remain visible briefly, without replacing print state.
    return {busy_, started_ms_ && now_ms - started_ms_ < 30000 ? detail_ : ""};
  }
 private:
  mutable std::mutex mutex_;
  mutable bool busy_ = false;
  uint32_t sequence_ = 0;
  uint64_t started_ms_ = 0;
  mutable std::string detail_;
};
}  // namespace printsphere
