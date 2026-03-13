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
  TickType_t stop_banner_until_tick_ = 0;
  StatusSourcePreference status_source_preference_ = StatusSourcePreference::kCloud;
};

}  // namespace printsphere
