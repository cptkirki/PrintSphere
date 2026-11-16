#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace printsphere {
struct UiTimingSummary {
  uint32_t samples = 0;
  uint32_t mean_us = 0;
  uint32_t p95_upper_us = 0;
  uint32_t max_us = 0;
};
class UiTimingAccumulator {
 public:
  void add(uint64_t duration_us) {
    const auto value = static_cast<uint32_t>(std::min<uint64_t>(duration_us, UINT32_MAX));
    ++count_; sum_ += value; max_ = std::max(max_, value);
    for (std::size_t i = 0; i < limits_.size(); ++i) {
      if (value <= limits_[i]) { ++bins_[i]; break; }
    }
  }
  UiTimingSummary summary() const {
    if (!count_) return {};
    uint32_t cumulative = 0, p95 = max_;
    const uint64_t threshold = (static_cast<uint64_t>(count_) * 95 + 99) / 100;
    for (std::size_t i = 0; i < limits_.size(); ++i) {
      cumulative += bins_[i];
      if (cumulative >= threshold) { p95 = std::min(limits_[i], max_); break; }
    }
    return {count_, static_cast<uint32_t>(sum_ / count_), p95, max_};
  }
 private:
  inline static constexpr std::array<uint32_t, 16> limits_ = {
      500, 1000, 2000, 4000, 8000, 12000, 16000, 20000,
      25000, 33000, 50000, 66000, 100000, 200000, 500000, UINT32_MAX};
  std::array<uint32_t, 16> bins_{};
  uint32_t count_ = 0, max_ = 0;
  uint64_t sum_ = 0;
};
struct UiPerformanceSnapshot {
  uint32_t window_ms = 0;
  UiTimingSummary render_cycle; // includes flush callbacks, not CPU-only time
  UiTimingSummary flush_callback;
  UiTimingSummary flush_wait;
  UiTimingSummary widget_update;
  UiTimingSummary scroll_render_cycle;
};
enum class UiWorkPhase : uint8_t { kIdle, kWidgets, kRender, kFlush, kFlushWait, kPower };
inline const char* ui_work_phase_name(UiWorkPhase phase) {
  switch (phase) {
    case UiWorkPhase::kWidgets: return "widgets";
    case UiWorkPhase::kRender: return "render";
    case UiWorkPhase::kFlush: return "flush_callback";
    case UiWorkPhase::kFlushWait: return "flush_wait";
    case UiWorkPhase::kPower: return "power_command";
    default: return "idle";
  }
}
}  // namespace printsphere
