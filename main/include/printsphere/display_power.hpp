#pragma once

namespace printsphere {
enum class ScreenPowerMode { kAwake, kDimmed, kOff };

// The caller owns the hardware state and commits the new mode only on success.
// Never stop TE before pausing the renderer; never resume before restoring TE.
template <class Pause, class Brightness, class Resume>
bool transition_screen_power(bool& renderer_paused, ScreenPowerMode target,
                             Pause pause, Brightness brightness, Resume resume) {
  if (target == ScreenPowerMode::kOff && !renderer_paused) {
    if (!pause()) return false;
    renderer_paused = true;
  }
  // A failed command may leave hardware state uncertain. Keep the worker
  // paused until a nonzero brightness write has been confirmed on a retry.
  if (!brightness(target)) return false;
  if (target != ScreenPowerMode::kOff && renderer_paused) {
    if (!resume()) return false;
    renderer_paused = false;
  }
  return true;
}
}  // namespace printsphere
