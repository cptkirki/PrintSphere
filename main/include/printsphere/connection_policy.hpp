#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace printsphere::policy {

enum class HttpFailure { kNone, kTransport, kUnauthorized, kForbidden, kWaf, kCsrf,
                         kRateLimited, kServer, kRejected, kInvalidResponse };

inline const char* failure_name(HttpFailure failure) {
  switch (failure) {
    case HttpFailure::kNone: return "none";
    case HttpFailure::kTransport: return "transport";
    case HttpFailure::kUnauthorized: return "unauthorized";
    case HttpFailure::kForbidden: return "forbidden";
    case HttpFailure::kWaf: return "cloudflare";
    case HttpFailure::kCsrf: return "csrf";
    case HttpFailure::kRateLimited: return "rate_limited";
    case HttpFailure::kServer: return "server";
    case HttpFailure::kRejected: return "rejected";
    case HttpFailure::kInvalidResponse: return "invalid_response";
  }
  return "unknown";
}

inline HttpFailure classify_http(int status, std::string_view body) {
  if (status == 0) return HttpFailure::kTransport;
  if (status == 429) return HttpFailure::kRateLimited;
  if (status == 403 && (body.find("missing_cookie") != body.npos ||
                       body.find("CSRF") != body.npos || body.find("csrf") != body.npos))
    return HttpFailure::kCsrf;
  if (status == 403 && (body.find("cloudflare") != body.npos ||
                       body.find("Cloudflare") != body.npos || body.find("<!DOCTYPE html") != body.npos))
    return HttpFailure::kWaf;
  if (status == 401) return HttpFailure::kUnauthorized;
  if (status == 403) return HttpFailure::kForbidden;
  if (status >= 500) return HttpFailure::kServer;
  if (status >= 400 || status < 200 || status >= 300) return HttpFailure::kRejected;
  return HttpFailure::kNone;
}

inline uint32_t retry_after_seconds(const std::string& value, int64_t now_utc) {
  char* end = nullptr;
  const unsigned long long numeric = std::strtoull(value.c_str(), &end, 10);
  if (end != value.c_str() && *end == 0 && value.front() != '-')
    return static_cast<uint32_t>(std::min<unsigned long long>(numeric, UINT32_MAX));
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  char month_name[4] = {};
  if (std::sscanf(value.c_str(), "%*3s, %d %3s %d %d:%d:%d GMT",
                  &day, month_name, &year, &hour, &minute, &second) != 6) return 300;
  const std::string_view months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const auto position = months.find(month_name);
  if (position == months.npos || position % 3 || year < 1970 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 300;
  const unsigned month = static_cast<unsigned>(position / 3 + 1);
  year -= month <= 2;
  const int era = year / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t epoch = (static_cast<int64_t>(era) * 146097 + doe - 719468) * 86400 +
                         hour * 3600 + minute * 60 + second;
  return static_cast<uint32_t>(std::clamp<int64_t>(epoch - now_utc, 0, UINT32_MAX));
}

// Only fields explicitly documented as fractions may be scaled.
inline float progress_percent(std::string_view key, float value) {
  if (!std::isfinite(value)) return 0.0f;
  if (key == "progress_fraction") value *= 100.0f;
  return std::clamp(value, 0.0f, 100.0f);
}

inline bool same_printer(std::string_view selected, std::string_view source) {
  return !selected.empty() && selected == source;
}

inline bool firmware_compatible(std::string_view current, std::string_view candidate,
                                uint32_t current_secure, uint32_t candidate_secure) {
  return !current.empty() && current == candidate && candidate_secure >= current_secure;
}

inline bool portal_requires_lock(bool enabled, bool provisioned, uint64_t grace_deadline,
                                 uint64_t now_ms) {
  return enabled && provisioned && (grace_deadline == 0 || now_ms >= grace_deadline);
}

// Four regular AMS units plus four single-slot HT units, bounded independently
// from the protocol's sparse IDs. UI indices are never protocol IDs.
constexpr int kAmsUnits = 8;
inline int ams_index(int id) {
  return id >= 0 && id < 4 ? id : (id >= 128 && id < 132 ? id - 128 + 4 : -1);
}
inline int ams_protocol_id(int index) { return index < 4 ? index : index - 4 + 128; }
inline int tray_id(int ams_id, int slot) {
  return ams_id >= 128 ? ams_id : ams_id * 4 + slot;
}

// An early task notification must never permit an early network retry.
class RetryBudget {
 public:
  bool due(int64_t now_us) const { return now_us >= not_before_us_; }
  void defer(int64_t now_us, uint32_t seconds) {
    not_before_us_ = now_us + static_cast<int64_t>(seconds) * 1000000;
  }
  void reset() { not_before_us_ = 0; }
 private:
  int64_t not_before_us_ = 0;
};

}  // namespace printsphere::policy
