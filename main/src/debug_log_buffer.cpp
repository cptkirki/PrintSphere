#include "printsphere/debug_log_buffer.hpp"

#ifdef PRINTSPHERE_DEBUG_BUILD

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_heap_caps.h"
#include "esp_log.h"

#ifndef PRINTSPHERE_RELEASE_VERSION
#define PRINTSPHERE_RELEASE_VERSION "dev"
#endif

namespace printsphere {

namespace {

constexpr size_t kBufSize = 48 * 1024;  // 48 KB ring buffer

char* g_buf = nullptr;
size_t g_write_total = 0;  // monotonically increasing byte counter
std::mutex g_mutex;
vprintf_like_t g_orig_vprintf = nullptr;

// ─── Sensitive data scrubber ─────────────────────────────────────────────────
// Only the web-accessible ring buffer is scrubbed; UART output (g_orig_vprintf)
// is intentionally left unmodified so developers with physical serial access
// see the full log.

static bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Replace "KEY<sep>VALUE" where VALUE ends at any char in terminators.
static void scrub_kv(std::string& s, const char* key_sep, const char* terminators,
                     const char* replacement) {
  const size_t klen = strlen(key_sep);
  const size_t rlen = strlen(replacement);
  size_t pos = 0;
  while ((pos = s.find(key_sep, pos)) != std::string::npos) {
    const size_t val_start = pos + klen;
    size_t val_end = val_start;
    while (val_end < s.size() && strchr(terminators, s[val_end]) == nullptr) {
      ++val_end;
    }
    if (val_end > val_start) {
      s.replace(val_start, val_end - val_start, replacement);
      pos = val_start + rlen;
    } else {
      pos = val_end;
    }
  }
}

// Replace IPv4 addresses (4 groups of digits separated by exactly 3 dots).
static void scrub_ipv4(std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    if (!isdigit((unsigned char)s[i])) { ++i; continue; }
    // Don't match if preceded by a letter/underscore (e.g. version "v1.3.2").
    if (i > 0 && (isalpha((unsigned char)s[i - 1]) || s[i - 1] == '_')) { ++i; continue; }
    size_t j = i;
    int dots = 0, groups = 0;
    bool valid = true;
    while (j < s.size()) {
      const size_t g_start = j;
      while (j < s.size() && isdigit((unsigned char)s[j])) ++j;
      if (j == g_start || j - g_start > 3) { valid = false; break; }
      ++groups;
      if (j < s.size() && s[j] == '.') { ++dots; ++j; } else break;
    }
    if (valid && dots == 3 && groups == 4) {
      s.replace(i, j - i, "[IP]");
      i += 4;
    } else {
      i = (j > i) ? j : i + 1;
    }
  }
}

// Replace MAC addresses in the form HH:HH:HH:HH:HH:HH.
static void scrub_mac(std::string& s) {
  if (s.size() < 17) return;
  size_t i = 0;
  while (i + 17 <= s.size()) {
    bool ok = true;
    for (int b = 0; b < 6 && ok; ++b) {
      const size_t p = i + static_cast<size_t>(b) * 3;
      ok = is_hex(s[p]) && is_hex(s[p + 1]);
      if (b < 5) ok = ok && (s[p + 2] == ':');
    }
    if (ok) {
      s.replace(i, 17, "[MAC]");
      i += 5;  // len("[MAC]")
    } else {
      ++i;
    }
  }
}

// Replace the string value of a JSON/structured key: "key":"value" or "key": "value".
static void scrub_json_field(std::string& s, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t nlen = needle.size();
  size_t pos = 0;
  while ((pos = s.find(needle, pos)) != std::string::npos) {
    size_t p = pos + nlen;
    while (p < s.size() && (s[p] == ' ' || s[p] == ':')) ++p;
    if (p < s.size() && s[p] == '"') {
      const size_t val_start = p + 1;
      const size_t val_end = s.find('"', val_start);
      if (val_end != std::string::npos) {
        s.replace(val_start, val_end - val_start, "[hidden]");
        pos = val_start + 8;
        continue;
      }
    }
    pos += nlen;
  }
}

// Replace pre-signed cloud URLs (AWS S3 / similar) that contain signing params.
// The entire URL (from https:// to the next whitespace/quote/newline) is replaced.
static void scrub_presigned_urls(std::string& s) {
  static const char kAmz[] = "X-Amz-";
  size_t pos = 0;
  while ((pos = s.find(kAmz, pos)) != std::string::npos) {
    // Walk backwards to the start of the URL.
    size_t url_start = pos;
    while (url_start >= 8 && !(s[url_start] == 'h' &&
           s.compare(url_start, 8, "https://") == 0)) {
      --url_start;
    }
    if (s.compare(url_start, 8, "https://") != 0) { ++pos; continue; }
    // Walk forwards to the end of the URL (whitespace, quote, or end).
    size_t url_end = pos;
    while (url_end < s.size() &&
           s[url_end] != ' ' && s[url_end] != '\t' &&
           s[url_end] != '\n' && s[url_end] != '\r' &&
           s[url_end] != '"' && s[url_end] != '\'') {
      ++url_end;
    }
    s.replace(url_start, url_end - url_start, "[presigned-url-hidden]");
    pos = url_start + 21;  // len("[presigned-url-hidden]")
  }
}

static std::string scrub_for_web(const char* text, size_t len) {
  std::string s(text, len);

  // Pre-signed cloud URLs (contain AWS signing params — reveal credentials
  // and private resource paths).
  scrub_presigned_urls(s);

  // WiFi credentials and AP info.
  scrub_kv(s, "SSID=", " \t\n\r,", "[hidden]");
  scrub_kv(s, "PASS=", " \t\n\r,", "[hidden]");
  // "wifi:connected with <ssid>,"
  scrub_kv(s, "connected with ", ",", "[hidden]");

  // IP addresses in various esp-netif / app log formats.
  scrub_kv(s, "IP=",    " \t\n\r,", "[IP]");
  scrub_kv(s, "ip: ",   ",\t\n\r ", "[IP]");
  scrub_kv(s, "mask: ", ",\t\n\r ", "[IP]");
  scrub_kv(s, "gw: ",   ",\t\n\r ", "[IP]");
  scrub_ipv4(s);  // catch any remaining standalone IPv4 addresses.

  // MAC addresses.
  scrub_mac(s);

  // Sensitive JSON / structured-log fields.
  static const char* const kSensitiveKeys[] = {
      "password", "token", "access_token", "access_code",
      "passcode", "email", "host", nullptr};
  for (const char* const* k = kSensitiveKeys; *k != nullptr; ++k) {
    scrub_json_field(s, *k);
  }

  return s;
}

// Write bytes directly into the ring buffer (called only while g_mutex is held).
static void buf_write(const char* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    g_buf[g_write_total % kBufSize] = data[i];
    ++g_write_total;
  }
}

int log_hook(const char* fmt, va_list args) {
  va_list args_uart;
  va_copy(args_uart, args);

  // IMPORTANT: static buffer — keeps this off the stack so small-stack system
  // tasks (e.g. sys_evt, ~2304 bytes) do not overflow.  Safe because we hold
  // g_mutex before using it.
  if (g_buf != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static char line[512];
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    const size_t len = (n > 0) ? std::min(static_cast<size_t>(n), sizeof(line) - 1) : 0;
    if (len > 0) {
      std::string scrubbed = scrub_for_web(line, len);
      buf_write(scrubbed.data(), scrubbed.size());
    }
  }

  int result = 0;
  if (g_orig_vprintf != nullptr) {
    result = g_orig_vprintf(fmt, args_uart);
  }
  va_end(args_uart);
  return result;
}

}  // namespace

void debug_log_init() {
  g_buf = static_cast<char*>(
      heap_caps_malloc(kBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (g_buf == nullptr) {
    g_buf = static_cast<char*>(malloc(kBufSize));
  }
  g_orig_vprintf = esp_log_set_vprintf(log_hook);

  // Write firmware version banner directly into the ring buffer.
  // This is not written to UART (avoid double-printing at boot).
  if (g_buf != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static const char banner[] =
        "\n"
        "===========================================\n"
        "  bambustat Debug Log\n"
        "  FW : " PRINTSPHERE_RELEASE_VERSION "\n"
        "  Built: " __DATE__ " " __TIME__ "\n"
        "===========================================\n"
        "\n";
    buf_write(banner, sizeof(banner) - 1);
  }
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
