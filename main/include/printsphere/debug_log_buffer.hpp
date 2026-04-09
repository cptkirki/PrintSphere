#pragma once

#ifdef PRINTSPHERE_DEBUG_BUILD

#include <cstddef>
#include <string>

namespace printsphere {

/// Install the custom vprintf hook that captures all ESP-IDF log output into a
/// ring buffer.  Must be called once, early, before significant log output is
/// generated (ideally at the very start of app_main).
void debug_log_init();

/// Return buffered log text written since byte position \p from_offset.
/// \p out_end_offset receives the new write-head position for use in the next
/// incremental call.  Returns an empty string when no new data is available.
std::string debug_log_fetch(size_t from_offset, size_t* out_end_offset);

/// Total bytes ever written to the ring buffer (monotonically increasing).
size_t debug_log_end_offset();

}  // namespace printsphere

#endif  // PRINTSPHERE_DEBUG_BUILD
