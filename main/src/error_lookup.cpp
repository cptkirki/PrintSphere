#include "printsphere/error_lookup.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

#include "esp_log.h"

extern const char error_lookup_tsv_start[] asm("_binary_error_lookup_tsv_start");
extern const char error_lookup_tsv_end[] asm("_binary_error_lookup_tsv_end");

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.lookup";

struct LookupCacheEntry {
  bool valid = false;
  ErrorLookupDomain domain = ErrorLookupDomain::kPrintError;
  uint64_t code = 0;
  PrinterModel model = PrinterModel::kUnknown;
  std::string text;
};

struct ParsedLookupLine {
  bool valid = false;
  char domain = '\0';
  std::string_view code;
  std::string_view models;
  std::string_view message;
};

std::mutex g_lookup_mutex;
LookupCacheEntry g_cache{};

char lookup_domain_marker(ErrorLookupDomain domain) {
  switch (domain) {
    case ErrorLookupDomain::kDeviceHms:
      return 'H';
    case ErrorLookupDomain::kPrintError:
    default:
      return 'E';
  }
}

std::string format_lookup_code(ErrorLookupDomain domain, uint64_t code) {
  char buffer[24] = {};
  switch (domain) {
    case ErrorLookupDomain::kDeviceHms:
      std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(code));
      break;
    case ErrorLookupDomain::kPrintError:
    default:
      std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned int>(code));
      break;
  }
  return buffer;
}

std::string format_generic_print_error_detail(int print_error_code, int hms_count) {
  if (print_error_code == 0 && hms_count == 0) {
    return {};
  }

  if (print_error_code != 0) {
    char error_buffer[32] = {};
    std::snprintf(error_buffer, sizeof(error_buffer), "%08X",
                  static_cast<unsigned int>(print_error_code));
    std::string detail = "Print error ";
    detail += std::string(error_buffer, 4);
    detail += "_";
    detail += std::string(error_buffer + 4, 4);
    if (hms_count > 0) {
      detail += " + HMS ";
      detail += std::to_string(hms_count);
    }
    return detail;
  }

  return "HMS alerts: " + std::to_string(hms_count);
}

bool ensure_storage_ready_locked() {
  return &error_lookup_tsv_end[0] > &error_lookup_tsv_start[0];
}

ParsedLookupLine parse_lookup_line(std::string_view line) {
  ParsedLookupLine parsed{};
  if (line.empty() || line[0] == '#') {
    return parsed;
  }

  const auto tab1 = line.find('\t');
  if (tab1 == std::string_view::npos) return parsed;
  const auto tab2 = line.find('\t', tab1 + 1);
  if (tab2 == std::string_view::npos) return parsed;
  const auto tab3 = line.find('\t', tab2 + 1);
  if (tab3 == std::string_view::npos) return parsed;

  std::string_view domain_field = line.substr(0, tab1);
  if (domain_field.empty()) return parsed;

  parsed.valid = true;
  parsed.domain = domain_field[0];
  parsed.code = line.substr(tab1 + 1, tab2 - tab1 - 1);
  parsed.models = line.substr(tab2 + 1, tab3 - tab2 - 1);
  parsed.message = line.substr(tab3 + 1);
  return parsed;
}

bool model_list_matches(std::string_view models, std::string_view model_token) {
  if (models.empty() || models == "-" || model_token.empty() || model_token == "UNKNOWN") {
    return false;
  }

  size_t cursor = 0;
  while (cursor < models.size()) {
    const size_t comma = models.find(',', cursor);
    const size_t end = (comma == std::string_view::npos) ? models.size() : comma;
    const std::string_view token = models.substr(cursor, end - cursor);
    if (token == model_token) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    cursor = comma + 1;
  }
  return false;
}

std::string lookup_error_text_uncached(ErrorLookupDomain domain, uint64_t code, PrinterModel model) {
  if (code == 0 || !ensure_storage_ready_locked()) {
    return {};
  }

  const char target_domain = lookup_domain_marker(domain);
  const std::string target_code = format_lookup_code(domain, code);
  const std::string_view model_token = to_string(model);

  std::string default_message;
  std::string matched_message;
  std::string fallback_message;

  const char* cursor = error_lookup_tsv_start;
  const char* const end = error_lookup_tsv_end;

  while (cursor < end) {
    const char* line_end = static_cast<const char*>(std::memchr(cursor, '\n', end - cursor));
    if (line_end == nullptr) {
      line_end = end;
    }
    std::string_view line(cursor, line_end - cursor);
    cursor = line_end + 1;

    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.remove_suffix(1);
    }

    ParsedLookupLine entry = parse_lookup_line(line);
    if (!entry.valid) {
      continue;
    }
    if (entry.domain < target_domain) {
      continue;
    }
    if (entry.domain > target_domain) {
      break;
    }

    const int cmp = entry.code.compare(target_code);
    if (cmp < 0) {
      continue;
    }
    if (cmp > 0) {
      break;
    }

    if (fallback_message.empty()) {
      fallback_message.assign(entry.message);
    }
    if (entry.models.empty() || entry.models == "-") {
      if (default_message.empty()) {
        default_message.assign(entry.message);
      }
      continue;
    }
    if (model_list_matches(entry.models, model_token)) {
      matched_message.assign(entry.message);
      break;
    }
  }

  if (!matched_message.empty()) {
    return matched_message;
  }
  if (!default_message.empty()) {
    return default_message;
  }
  return fallback_message;
}

}  // namespace

bool initialize_error_lookup_storage() {
  std::lock_guard<std::mutex> lock(g_lookup_mutex);
  const bool ready = ensure_storage_ready_locked();
  if (ready) {
    ESP_LOGI(kTag, "Embedded error lookup ready (%u bytes)",
             static_cast<unsigned int>(error_lookup_tsv_end - error_lookup_tsv_start));
  }
  return ready;
}

std::string lookup_error_text(ErrorLookupDomain domain, uint64_t code, PrinterModel model) {
  if (code == 0) {
    return {};
  }

  std::lock_guard<std::mutex> lock(g_lookup_mutex);
  if (g_cache.valid && g_cache.domain == domain && g_cache.code == code && g_cache.model == model) {
    return g_cache.text;
  }

  std::string result = lookup_error_text_uncached(domain, code, model);
  g_cache.valid = true;
  g_cache.domain = domain;
  g_cache.code = code;
  g_cache.model = model;
  g_cache.text = result;
  return result;
}

std::string format_resolved_error_detail(int print_error_code, int hms_count, PrinterModel model) {
  if (print_error_code == 0) {
    return hms_count > 0 ? "HMS alerts: " + std::to_string(hms_count) : std::string{};
  }

  std::string detail = lookup_error_text(ErrorLookupDomain::kPrintError,
                                         static_cast<uint32_t>(print_error_code), model);
  if (detail.empty()) {
    return format_generic_print_error_detail(print_error_code, hms_count);
  }
  if (hms_count > 0) {
    detail += " + HMS ";
    detail += std::to_string(hms_count);
  }
  return detail;
}

}  // namespace printsphere
