#pragma once
#include <cstring>
#include "esp_app_desc.h"
#include "printsphere/connection_policy.hpp"

namespace printsphere {
inline bool firmware_matches(const esp_app_desc_t& current, const esp_app_desc_t& candidate) {
  return candidate.magic_word == ESP_APP_DESC_MAGIC_WORD &&
      policy::firmware_compatible(
          std::string_view(current.project_name, strnlen(current.project_name, sizeof(current.project_name))),
          std::string_view(candidate.project_name, strnlen(candidate.project_name, sizeof(candidate.project_name))),
          current.secure_version, candidate.secure_version);
}
}  // namespace printsphere
