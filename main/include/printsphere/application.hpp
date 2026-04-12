#pragma once

#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/p1s_camera_client.hpp"
#include "printsphere/pmu.hpp"
#include "printsphere/printer_client.hpp"
#include "printsphere/setup_portal.hpp"
#include "printsphere/ui.hpp"
#include "printsphere/wifi_manager.hpp"

namespace printsphere {

class Application {
 public:
  Application();
  void run();

 private:
  ConfigStore config_store_{};
  WifiManager wifi_manager_{};
  BambuCloudClient cloud_client_{};
  PrinterClient printer_client_{};
  P1sCameraClient camera_client_{};
  Ui ui_{};
  SetupPortal setup_portal_;
  PmuManager pmu_manager_{};
  bool local_printer_enabled_ = false;
  bool print_activity_seen_this_session_ = false;
  bool last_local_print_live_ = false;
  bool last_cloud_print_live_ = false;
  TickType_t stop_banner_until_tick_ = 0;
  SourceMode source_mode_ = SourceMode::kHybrid;
  SourceMode last_source_mode_ = SourceMode::kHybrid;
  bool last_wifi_connected_ = false;
  bool last_camera_page_active_ = false;
  bool hybrid_cloud_gate_open_ = false;
  TickType_t hybrid_cloud_gate_deadline_ = 0;
  TickType_t hybrid_camera_cooldown_deadline_ = 0;
  bool filament_wake_enabled_ = false;
  bool filament_anim_enabled_ = true;
  bool chamber_light_override_active_ = false;
  bool chamber_light_override_on_ = false;
  uint64_t chamber_light_override_until_ms_ = 0;
};

}  // namespace printsphere
