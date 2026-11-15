#pragma once
#include <atomic>
#include <cstdint>
extern std::atomic<uint64_t> host_ticks;
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(host_ticks.load() * 1000); }
