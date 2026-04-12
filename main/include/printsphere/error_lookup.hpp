#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "printsphere/printer_state.hpp"

namespace printsphere {

enum class ErrorLookupDomain : uint8_t {
  kPrintError,
  kDeviceHms,
};

bool initialize_error_lookup_storage();
std::string lookup_error_text(ErrorLookupDomain domain, uint64_t code, PrinterModel model);
std::string format_resolved_error_detail(int print_error_code,
                                         const std::vector<uint64_t>& hms_codes, int hms_count,
                                         PrinterModel model);

/// Returns true for HMS codes that should be suppressed from the error display.
/// Currently only filters the benign cloud status notification 050002000003000A.
inline bool is_hms_suppressed(uint64_t hms_code) {
  return hms_code == 0x050002000003000AULL;
}

}  // namespace printsphere
