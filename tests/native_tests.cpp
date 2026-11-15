#include <cassert>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include "printsphere/bambu_protocol.hpp"
#include "printsphere/command_tracker.hpp"

using namespace printsphere;
struct Json {
  cJSON* value;
  explicit Json(const char* text) : value(cJSON_Parse(text)) { assert(value); }
  ~Json() { cJSON_Delete(value); }
};

int main() {
  using policy::HttpFailure;
  assert(policy::classify_http(403, "<html>Cloudflare</html>") == HttpFailure::kWaf);
  assert(policy::classify_http(403, R"({"error":"missing_cookie"})") == HttpFailure::kCsrf);
  assert(policy::classify_http(403, "access denied") == HttpFailure::kForbidden);
  assert(policy::classify_http(401, "unauthorized") == HttpFailure::kUnauthorized);
  assert(policy::classify_http(429, "Cloudflare") == HttpFailure::kRateLimited);
  assert(policy::classify_http(503, "") == HttpFailure::kServer);
  assert(policy::classify_http(0, "") == HttpFailure::kTransport);
  assert(policy::classify_http(204, "") == HttpFailure::kNone);
  // Both delta-seconds and an HTTP-date must preserve the server's retry floor.
  assert(policy::retry_after_seconds("900", 0) == 900);
  assert(policy::retry_after_seconds("Thu, 01 Jan 1970 00:15:00 GMT", 0) == 900);
  assert(policy::retry_after_seconds("Sun, 15 Nov 2026 20:00:00 GMT", 1794772800) == 0);
  assert(policy::retry_after_seconds("invalid", 0) == 300);
  policy::RetryBudget budget;
  budget.defer(1000000, 60);
  for (int64_t wake = 1000000; wake < 61000000; wake += 100000) assert(!budget.due(wake));
  assert(budget.due(61000000));
  for (float value : {0.f, 1.f, 2.f, 99.f, 100.f})
    assert(policy::progress_percent("mc_percent", value) == value);
  assert(policy::progress_percent("progress_fraction", .25f) == 25.f);
  assert(policy::progress_percent("mc_percent", std::numeric_limits<float>::quiet_NaN()) == 0.f);
  assert(!policy::same_printer("A", "B"));
  assert(!policy::same_printer("A", ""));
  assert(policy::same_printer("A", "A"));
  assert(policy::portal_requires_lock(true, true, 0, 1));
  assert(policy::portal_requires_lock(true, true, 5000, 5000));
  assert(!policy::portal_requires_lock(true, true, 5000, 4999));
  assert(policy::firmware_compatible("printsphere_2_8c_p2", "printsphere_2_8c_p2", 1, 1));
  assert(!policy::firmware_compatible("printsphere_2_8c_p2", "printsphere_1_75_p2", 1, 1));
  assert(!policy::firmware_compatible("printsphere_2_8c_p2", "printsphere_idf", 0, 0));
  assert(!policy::firmware_compatible("printsphere_2_8c_p2", "printsphere_2_8c_p2", 2, 1));

  Json delta(R"({"id":0,"state":1})");
  Json empty(R"({"id":0,"tray_type":""})");
  assert(!tray_is_explicitly_empty(delta.value));
  assert(tray_is_explicitly_empty(empty.value));
  Json snow(R"({"device":{"extruder":{"info":[{"id":0,"snow":2},{"id":1,"snow":32768}]}}})");
  assert(protocol_snow_tray(snow.value, 0) == 2);
  assert(protocol_snow_tray(snow.value, 1) == 128);
  assert(policy::ams_index(128) == 4);
  assert(policy::ams_index(131) == 7);
  assert(policy::ams_index(255) == -1);
  Json temperatures(R"([{"id":0,"temp":235},{"id":1,"temp":210}])");
  float active = 200, secondary = 190;
  bool active_present = false, secondary_present = false;
  merge_nozzle_temperature_patch(temperatures.value, 0, &active, &active_present,
                                 &secondary, &secondary_present);
  assert(active == 235 && secondary == 210 && active_present && secondary_present);
  Json temperature_delta(R"([{"id":1,"temp":215}])");
  active_present = secondary_present = false;
  merge_nozzle_temperature_patch(temperature_delta.value, 0, &active, &active_present,
                                 &secondary, &secondary_present);
  assert(active == 235 && secondary == 215 && !active_present && secondary_present);
  AmsSnapshot ams;
  ams.units[0].present = true;
  ams.units[0].trays[0].present = true;
  ams.units[0].trays[0].material_type = "PLA";
  Json patch(R"({"ams":[{"id":0,"tray":[{"id":0,"state":1}]}]})");
  apply_ams_presence(patch.value, ams);
  assert(ams.units[0].trays[0].material_type == "PLA");
  Json presence(R"({"ams":[{"id":"128","tray_exist_bits":"1"},{"id":0,"tray_exist_bits":"0"}]})");
  apply_ams_presence(presence.value, ams);
  assert(ams.units[4].present && ams.units[4].trays[0].present && ams.count == 5);
  assert(!ams.units[0].trays[0].present && ams.units[0].trays[0].material_type.empty());
  auto current = std::make_shared<AmsSnapshot>(ams);
  std::shared_ptr<const AmsSnapshot> published = current;
  auto next = detached_ams(current);
  std::thread writer([&] { next->units[4].trays[0].material_type = "PETG"; });
  writer.join();
  assert(published->units[4].trays[0].material_type.empty());
  assert(next->units[4].trays[0].material_type == "PETG");
  Json core(R"({"mc_percent":1})"), auxiliary(R"({"ams":{}})");
  assert(has_core_status(core.value));
  assert(!has_core_status(auxiliary.value));
  CommandTracker command;
  assert(command.queue(100));
  assert(!command.queue(101));
  command.sent(42, 101);
  command.result(41, true);
  assert(command.snapshot(102).busy);
  command.result(42, false);
  assert(!command.snapshot(103).busy);
  assert(command.snapshot(103).detail == "Printer rejected command");
  assert(command.queue(200));
  assert(!command.snapshot(15200).busy);
  std::cout << "Native auth/retry/identity/protocol/ownership/control regressions passed\n";
}
