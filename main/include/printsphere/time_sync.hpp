#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace printsphere::time_sync {

// Apply the given IANA zone name (e.g. "Europe/Berlin") to the C runtime by
// translating it to a POSIX TZ string and calling setenv("TZ", ...) + tzset().
// An empty or unknown zone falls back to UTC. Safe to call repeatedly; the
// new TZ takes effect for subsequent localtime_r() calls.
void set_timezone_iana(const std::string& iana_name);

// Returns the IANA name that was last applied via set_timezone_iana(), or an
// empty string when no zone has been configured yet.
std::string current_iana();

// Translate a known IANA zone to its POSIX TZ string. Returns an empty string
// when the zone is not in the curated table.
std::string iana_to_posix(const std::string& iana_name);

// Returns the curated list of supported IANA zone names. The vector is built
// fresh on each call (cheap; ~60 entries) and is intended for populating
// configuration UIs.
std::vector<std::string_view> supported_iana_zones();

// Start SNTP (idempotent). Should be invoked once an IP address has been
// acquired so the system clock can sync against pool.ntp.org.
void start_sntp_if_needed();
bool clock_ready();

}  // namespace printsphere::time_sync
