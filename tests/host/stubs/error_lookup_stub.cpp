#include "printsphere/error_lookup.hpp"

#include <cstdio>
#include <string>

namespace printsphere {

bool initialize_error_lookup_storage() { return true; }

std::string lookup_error_text(ErrorLookupDomain, uint64_t code, PrinterModel) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "code_%llu", static_cast<unsigned long long>(code));
  return buffer;
}

std::string format_resolved_error_detail(
  int print_error_code,
  const std::vector<uint64_t>& hms_codes, int /*hms_count*/,
  PrinterModel
) {
  if (print_error_code != 0) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "print_err_%d", print_error_code);
    return buffer;
  }

  if (!hms_codes.empty()) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "hms_%llu", static_cast<unsigned long long>(hms_codes.front()));
    return buffer;
  }
  
  return {};
}

}  // namespace printsphere
