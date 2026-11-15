#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace printsphere {

// Byte length of the UTF-8 character that starts at `lead`. Invalid bytes are
// treated as a single-byte character so truncation still makes forward progress.
inline size_t utf8_codepoint_size(unsigned char lead) {
  if ((lead & 0x80U) == 0U) {
    return 1U;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    return 2U;
  }
  if ((lead & 0xF0U) == 0xE0U) {
    return 3U;
  }
  if ((lead & 0xF8U) == 0xF0U) {
    return 4U;
  }
  return 1U;
}

// Largest prefix of `value` that fits in `max_bytes` without splitting a
// UTF-8 codepoint. Used for C-string buffers and UI ellipsis.
inline size_t utf8_prefix_bytes(std::string_view value, size_t max_bytes) {
  if (value.size() <= max_bytes) {
    return value.size();
  }

  size_t len = max_bytes;
  while (len > 0U && (static_cast<unsigned char>(value[len]) & 0xC0U) == 0x80U) {
    --len;
  }
  return len;
}

inline std::string utf8_truncate_bytes(std::string_view value, size_t max_bytes) {
  return std::string(value.substr(0, utf8_prefix_bytes(value, max_bytes)));
}

inline std::string utf8_truncate_chars(std::string_view value, size_t max_chars) {
  size_t offset = 0U;
  size_t chars = 0U;
  while (offset < value.size() && chars < max_chars) {
    const size_t width = utf8_codepoint_size(static_cast<unsigned char>(value[offset]));
    if (offset + width > value.size()) {
      break;
    }
    offset += width;
    ++chars;
  }
  return std::string(value.substr(0, offset));
}

inline void utf8_copy_c_str(char* dest, size_t dest_size, std::string_view value) {
  if (dest == nullptr || dest_size == 0U) {
    return;
  }
  const size_t n = utf8_prefix_bytes(value, dest_size - 1U);
  if (n > 0U) {
    std::memcpy(dest, value.data(), n);
  }
  dest[n] = '\0';
}

}  // namespace printsphere
