#include <cassert>
#include <iostream>
#include <string>
#include "printsphere/display_power.hpp"

int main() {
  using namespace printsphere;
  bool paused = false, pause_ok = true, write_ok = true, resume_ok = true;
  std::string calls;
  auto transition = [&](ScreenPowerMode target) {
    calls.clear();
    return transition_screen_power(paused, target,
        [&]() { calls += 'P'; return pause_ok; },
        [&](ScreenPowerMode mode) { calls += mode == ScreenPowerMode::kOff ? '0' : 'B'; return write_ok; },
        [&]() { calls += 'R'; return resume_ok; });
  };
  assert(transition(ScreenPowerMode::kDimmed) && calls == "B" && !paused);
  assert(transition(ScreenPowerMode::kOff) && calls == "P0" && paused);
  assert(transition(ScreenPowerMode::kOff) && calls == "0" && paused);
  assert(transition(ScreenPowerMode::kAwake) && calls == "BR" && !paused);
  pause_ok = false;
  assert(!transition(ScreenPowerMode::kOff) && calls == "P" && !paused);
  pause_ok = true;
  write_ok = false;
  assert(!transition(ScreenPowerMode::kOff) && calls == "P0" && paused);
  assert(!transition(ScreenPowerMode::kAwake) && calls == "B" && paused);
  write_ok = true;
  resume_ok = false;
  assert(!transition(ScreenPowerMode::kAwake) && calls == "BR" && paused);
  resume_ok = true;
  assert(transition(ScreenPowerMode::kAwake) && calls == "BR" && !paused);
  // Stress the safety invariant across alternating auto-dim/off/touch wakes.
  for (int i = 0; i < 1000; ++i) {
    assert(transition(ScreenPowerMode::kDimmed) && calls == "B");
    assert(transition(ScreenPowerMode::kOff) && calls == "P0" && paused);
    assert(transition(ScreenPowerMode::kAwake) && calls == "BR" && !paused);
  }
  std::cout << "Display pause/brightness/wake ordering and failure recovery passed\n";
}
