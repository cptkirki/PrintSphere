#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include "nvs.h"
#include "printsphere/config_store.hpp"

namespace {
std::map<std::string, std::string> committed, pending;
bool fail_commit = false;
unsigned commit_count = 0, string_reads = 0;
}
int nvs_flash_init() { return 0; }
int nvs_flash_erase() { committed.clear(); return 0; }
int nvs_open(const char*, int, nvs_handle_t* handle) { *handle = 1; return 0; }
void nvs_close(nvs_handle_t) { pending.clear(); }
int nvs_set_blob(nvs_handle_t, const char* key, const void* value, size_t length) {
  pending[key] = std::string(static_cast<const char*>(value), length); return 0;
}
int nvs_get_blob(nvs_handle_t, const char* key, void* value, size_t* length) {
  const auto found = committed.find(key);
  if (found == committed.end()) return ESP_ERR_NOT_FOUND;
  if (value) {
    if (*length < found->second.size()) return ESP_ERR_INVALID_ARG;
    std::memcpy(value, found->second.data(), found->second.size());
  }
  *length = found->second.size(); return 0;
}
int nvs_get_str(nvs_handle_t handle, const char* key, char* value, size_t* length) {
  ++string_reads;
  return nvs_get_blob(handle, key, value, length);
}
int nvs_commit(nvs_handle_t) {
  ++commit_count;
  if (fail_commit) { fail_commit = false; return ESP_FAIL; }
  committed.insert(pending.begin(), pending.end());
  for (const auto& entry : pending) committed[entry.first] = entry.second;
  pending.clear(); return 0;
}

int main() {
  using namespace printsphere;
  ConfigStore config;
  assert(config.initialize() == ESP_OK);
  assert(!config.load_portal_provisioned());
  assert(config.save_wifi_credentials({"old", "old-password"}) == ESP_OK);
  fail_commit = true;
  assert(config.save_wifi_credentials({"new", "new-password"}) != ESP_OK);
  assert(config.load_wifi_credentials().ssid == "old");
  assert(config.load_wifi_credentials().password == "old-password");
  assert(config.save_source_mode(SourceMode::kCloudOnly)==ESP_OK);
  const auto before_reads=string_reads;
  for(int i=0;i<100;++i) assert(config.load_source_mode()==SourceMode::kCloudOnly);
  assert(string_reads==before_reads);
  fail_commit=true;
  assert(config.save_source_mode(SourceMode::kLocalOnly)!=ESP_OK);
  fail_commit=false;
  assert(config.load_source_mode()==SourceMode::kCloudOnly);
  assert(config.save_source_mode(SourceMode::kHybrid)==ESP_OK);
  assert(config.load_source_mode()==SourceMode::kHybrid);
  BambuCloudCredentials a{"a@example.invalid", "secret", CloudRegion::kEU};
  BambuCloudCredentials b{"b@example.invalid", "different", CloudRegion::kCN};
  assert(config.save_cloud_credentials(a) == ESP_OK);
  assert(config.save_cloud_access_token("token-A", 2000000000, &a) == ESP_OK);
  assert(config.load_cloud_token_expiry() == 2000000000);
  assert(config.save_cloud_credentials(b) == ESP_OK);
  assert(config.load_cloud_access_token().empty());
  assert(config.save_cloud_access_token("late-token-A", 2000000000, &a) == ESP_ERR_INVALID_STATE);
  assert(config.save_cloud_access_token("token-B", 2000000000, &b) == ESP_OK);
  assert(config.clear_cloud_access_token(&a) == ESP_ERR_INVALID_STATE);
  assert(config.load_cloud_access_token() == "token-B");
  b.password.clear();
  assert(config.save_cloud_credentials(b) == ESP_OK);
  assert(config.load_cloud_credentials().password.empty());
  assert(config.load_cloud_access_token() == "token-B");

  for (int i = 0; i < 16; ++i) {
    PrinterProfile profile;
    profile.index = i;
    profile.serial = "SERIAL-" + std::to_string(i);
    profile.host = std::string(120, 'h');
    profile.display_name = std::string(100, 'd');
    profile.access_code = "12345678";
    assert(config.save_printer_profile(profile) == ESP_OK);
  }
  assert(committed.at("prn_cfg").size() > 4096); // valid NVS blob beyond string-page limit
  assert(config.save_active_printer_index(15) == ESP_OK);
  fail_commit = true;
  assert(config.delete_printer_profile(0) != ESP_OK);
  assert(config.load_printer_profile_count() == 16);
  assert(config.load_active_printer_profile().serial == "SERIAL-15");
  assert(config.delete_printer_profile(0) == ESP_OK);
  assert(config.load_printer_profile_count() == 15);
  assert(config.load_active_printer_profile().serial == "SERIAL-15");
  assert(config.load_active_printer_index() == 14);
  assert(config.mark_portal_provisioned() == ESP_OK);
  ConfigStore rebooted;
  assert(rebooted.initialize() == ESP_OK);
  assert(rebooted.load_portal_provisioned());
  assert(rebooted.load_active_printer_profile().serial == "SERIAL-15");

  // Cloud discovery must populate the same profiles used by the device UI.
  committed.clear();
  pending.clear();
  assert(config.save_cloud_credentials(a) == ESP_OK);
  PrinterProfile local;
  local.serial = "FIRST";
  local.host = "192.0.2.10";
  local.access_code = "local-code";
  local.display_name = "My custom name";
  assert(config.save_printer_profile(local) == ESP_OK);
  PrinterProfile first, second;
  first.serial = "FIRST";
  first.display_name = "Cloud name";
  first.model = "P1S";
  second.serial = "SECOND";
  second.display_name = "Second printer";
  second.model = "A1";
  const std::vector<PrinterProfile> account_devices{first, second, second, PrinterProfile{}};
  assert(config.sync_cloud_printer_profiles(account_devices, a) == ESP_OK);
  auto discovered = config.load_printer_profiles();
  assert(discovered.size() == 2);
  assert(discovered[0].display_name == "My custom name");
  assert(discovered[0].host == local.host && discovered[0].access_code == local.access_code);
  assert(discovered[0].cloud_bound && discovered[1].cloud_bound);
  assert(discovered[1].serial == "SECOND" && discovered[1].index == 1);
  assert(discovered[1].host.empty() && discovered[1].access_code.empty());
  assert(config.load_active_printer_profile().serial == "FIRST");
  assert(config.save_active_printer_index(1) == ESP_OK);
  const auto before_repeat = commit_count;
  assert(config.sync_cloud_printer_profiles({second, first}, a) == ESP_OK);
  assert(commit_count == before_repeat); // unchanged polls never wear NVS
  assert(config.load_active_printer_profile().serial == "SECOND");
  assert(config.sync_cloud_printer_profiles({}, a) == ESP_OK);
  assert(config.load_printer_profile_count() == 2); // absent/offline never erases saved printers
  PrinterProfile third;
  third.serial = "THIRD";
  fail_commit = true;
  assert(config.sync_cloud_printer_profiles({third}, a) != ESP_OK);
  assert(config.load_printer_profile_count() == 2);
  assert(config.load_active_printer_profile().serial == "SECOND");
  assert(config.save_cloud_credentials(b) == ESP_OK);
  assert(config.sync_cloud_printer_profiles({third}, a) == ESP_ERR_INVALID_STATE);
  assert(config.load_printer_profile_count() == 2);
  std::vector<PrinterProfile> too_many;
  for (int i = 0; i < 16; ++i) {
    PrinterProfile p;
    p.serial = "EXTRA-" + std::to_string(i);
    too_many.push_back(p);
  }
  assert(config.sync_cloud_printer_profiles(too_many, b) == ESP_ERR_NO_MEM);
  assert(config.load_printer_profile_count() == 2); // capacity errors don't partially import
  assert(rebooted.load_active_printer_profile().serial == "SECOND");
  std::cout << "Config persistence/fault/account isolation/profile/reboot regressions passed\n";
}
