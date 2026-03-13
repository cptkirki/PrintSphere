#pragma once

#include <string>

#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/printer_state.hpp"

namespace printsphere {

void resolve_ui_state(PrinterSnapshot& snapshot);

PrinterSnapshot merge_status_sources(const PrinterSnapshot& local_snapshot, bool local_printer_enabled,
                                     const BambuCloudSnapshot& cloud_snapshot,
                                     StatusSourcePreference status_source_preference,
                                     bool wifi_connected, const std::string& wifi_ip,
                                     bool print_activity_seen_this_session);

}  // namespace printsphere
