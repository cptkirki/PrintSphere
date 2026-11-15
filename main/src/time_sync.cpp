#include "printsphere/time_sync.hpp"

#include <array>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

namespace printsphere::time_sync {

namespace {
constexpr char kTag[] = "printsphere.time";

std::mutex g_timezone_mutex;
bool g_sntp_started = false;
std::string g_current_iana{};

struct TzEntry {
  const char* iana;
  const char* posix;
};

// Curated IANA -> POSIX TZ mapping covering the most common zones the web
// browser is likely to report via Intl.DateTimeFormat().resolvedOptions().
// POSIX strings encode the DST rules statically; they remain valid as long as
// the political DST rules of the zone do not change.
constexpr std::array<TzEntry, 56> kZones = {{
    // Universal
    {"UTC",                "UTC0"},
    {"Etc/UTC",            "UTC0"},
    {"Etc/GMT",            "GMT0"},

    // Europe
    {"Europe/London",      "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin",      "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon",      "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Berlin",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam",   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen",  "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm",   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Budapest",    "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens",      "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki",    "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest",   "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Sofia",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul",    "<+03>-3"},
    {"Europe/Moscow",      "MSK-3"},
    {"Europe/Kyiv",        "EET-2EEST,M3.5.0/3,M10.5.0/4"},

    // Americas
    {"America/New_York",      "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto",       "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago",       "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver",        "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix",       "MST7"},
    {"America/Los_Angeles",   "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage",     "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Honolulu",      "HST10"},
    {"America/Sao_Paulo",     "<-03>3"},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"America/Mexico_City",   "CST6"},
    {"America/Bogota",        "<-05>5"},
    {"America/Lima",          "<-05>5"},
    {"America/Santiago",      "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},

    // Asia / Middle East
    {"Asia/Dubai",         "<+04>-4"},
    {"Asia/Tehran",        "<+0330>-3:30"},
    {"Asia/Kolkata",       "IST-5:30"},
    {"Asia/Calcutta",      "IST-5:30"},
    {"Asia/Bangkok",       "<+07>-7"},
    {"Asia/Jakarta",       "WIB-7"},
    {"Asia/Singapore",     "<+08>-8"},
    {"Asia/Hong_Kong",     "HKT-8"},
    {"Asia/Shanghai",      "CST-8"},
    {"Asia/Taipei",        "CST-8"},
    {"Asia/Seoul",         "KST-9"},
    {"Asia/Tokyo",         "JST-9"},

    // Oceania
    {"Australia/Perth",    "AWST-8"},
    {"Australia/Sydney",   "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland",   "NZST-12NZDT,M9.5.0,M4.1.0/3"},
}};
}  // namespace

std::string iana_to_posix(const std::string& iana_name) {
  if (iana_name.empty()) {
    return {};
  }
  for (const auto& entry : kZones) {
    if (iana_name == entry.iana) {
      return entry.posix;
    }
  }
  return {};
}

std::vector<std::string_view> supported_iana_zones() {
  std::vector<std::string_view> out;
  out.reserve(kZones.size());
  for (const auto& entry : kZones) {
    out.emplace_back(entry.iana);
  }
  return out;
}

void set_timezone_iana(const std::string& iana_name) {
  std::lock_guard<std::mutex> lock(g_timezone_mutex);
  std::string posix = iana_to_posix(iana_name);
  if (posix.empty()) {
    if (!iana_name.empty()) {
      ESP_LOGW(kTag, "Unknown IANA zone '%s' \u2014 falling back to UTC", iana_name.c_str());
    }
    posix = "UTC0";
  }
  setenv("TZ", posix.c_str(), 1);
  tzset();
  g_current_iana = iana_name;
  ESP_LOGI(kTag, "Timezone applied: %s (%s)",
           iana_name.empty() ? "UTC" : iana_name.c_str(), posix.c_str());
}

std::string current_iana() { std::lock_guard<std::mutex> lock(g_timezone_mutex); return g_current_iana; }

bool clock_ready() { return std::time(nullptr) >= 1735689600; }

void start_sntp_if_needed() {
  if (g_sntp_started) {
    return;
  }
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  cfg.start = true;
  cfg.smooth_sync = false;
  const esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(err));
    return;
  }
  g_sntp_started = true;
  ESP_LOGI(kTag, "SNTP started (pool.ntp.org)");
}

}  // namespace printsphere::time_sync
