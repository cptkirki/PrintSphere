#pragma once

#include <string>

#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/printer_state.hpp"

namespace printsphere {

// Shared stage classification helpers.  Accept any case (lowercased internally).
bool is_download_stage(const std::string& stage, const std::string& status);
bool is_filament_stage(const std::string& stage);
bool is_post_download_handoff_stage(const std::string& stage, const std::string& status);

void resolve_ui_state(PrinterSnapshot& snapshot);

PrinterSnapshot merge_status_sources(const PrinterSnapshot& local_snapshot, bool local_printer_enabled,
                                     const BambuCloudSnapshot& cloud_snapshot,
                                     SourceMode source_mode, uint64_t now_ms,
                                     bool wifi_connected, const std::string& wifi_ip,
                                     bool print_activity_seen_this_session);

}  // namespace printsphere
