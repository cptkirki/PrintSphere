#include "printsphere/debug_log_buffer.hpp"

#ifdef PRINTSPHERE_DEBUG_BUILD

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace printsphere {

namespace {

constexpr size_t kBufSize = 48 * 1024;  // 48 KB ring buffer

char* g_buf = nullptr;
size_t g_write_total = 0;  // monotonically increasing byte counter
std::mutex g_mutex;
vprintf_like_t g_orig_vprintf = nullptr;

int log_hook(const char* fmt, va_list args) {
  // Copy va_list so we can pass the original to the UART vprintf unchanged.
  va_list args_uart;
  va_copy(args_uart, args);

  // IMPORTANT: use a static buffer instead of a stack-allocated one.
  // The hook is called from arbitrary FreeRTOS tasks (including sys_evt which
  // has only ~2304 bytes of stack).  A 512-byte local array would overflow
  // small-stack system tasks.  The static buffer is in BSS (not on the stack)
  // and is safe to use because we hold g_mutex before writing to it.
  if (g_buf != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static char line[512];
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    const size_t len = (n > 0) ? std::min(static_cast<size_t>(n), sizeof(line) - 1) : 0;
    for (size_t i = 0; i < len; ++i) {
      g_buf[g_write_total % kBufSize] = line[i];
      ++g_write_total;
    }
  }

  // Keep original UART output alive.
  int result = 0;
  if (g_orig_vprintf != nullptr) {
    result = g_orig_vprintf(fmt, args_uart);
  }
  va_end(args_uart);
  return result;
}

}  // namespace

void debug_log_init() {
  // Prefer PSRAM to avoid eating internal RAM.
  g_buf = static_cast<char*>(
      heap_caps_malloc(kBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (g_buf == nullptr) {
    g_buf = static_cast<char*>(malloc(kBufSize));
  }
  g_orig_vprintf = esp_log_set_vprintf(log_hook);
}

std::string debug_log_fetch(size_t from_offset, size_t* out_end_offset) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const size_t end = g_write_total;
  if (out_end_offset != nullptr) {
    *out_end_offset = end;
  }
  if (from_offset >= end || g_buf == nullptr) {
    return {};
  }
  // The ring buffer holds at most kBufSize bytes.  Clamp the start offset so
  // we never return data that has already been overwritten.
  const size_t valid_start = (end > kBufSize) ? (end - kBufSize) : 0;
  const size_t actual_start = std::max(from_offset, valid_start);
  const size_t byte_count = end - actual_start;

  std::string result;
  result.resize(byte_count);
  for (size_t i = 0; i < byte_count; ++i) {
    result[i] = g_buf[(actual_start + i) % kBufSize];
  }
  return result;
}

size_t debug_log_end_offset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_write_total;
}

}  // namespace printsphere

#endif  // PRINTSPHERE_DEBUG_BUILD
