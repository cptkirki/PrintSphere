#include "printsphere/bambustat_compact_status.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace printsphere {

namespace {

// Lowercase a copy of `key` and return true if it matches `target`
// exactly. Case-insensitive, dash-insensitive comparison keeps us
// aligned with the redaction-policy regex catalog (e.g. "Access-Code"
// must match "access_code").
bool ci_key_equals(const char* key, const char* target) {
  if (key == nullptr || target == nullptr) {
    return false;
  }
  size_t i = 0;
  for (; target[i] != '\0'; ++i) {
    const char ch = key[i];
    if (ch == '\0') {
      return false;
    }
    char expected = static_cast<char>(std::tolower(static_cast<unsigned char>(target[i])));
    char actual = ch;
    if (expected == '-') {
      expected = '_';
    }
    if (actual == '-') {
      actual = '_';
    }
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(actual))) != expected) {
      return false;
    }
  }
  // key must also be exhausted (no trailing chars).
  return key[i] == '\0';
}

// Pull a string and copy it; returns true when the key was present and
// parsed successfully (including the explicit-null JSON case).
bool cjson_assign_string(const cJSON* node, std::string* out) {
  if (out == nullptr) {
    return false;
  }
  if (node == nullptr || cJSON_IsNull(node)) {
    out->clear();
    return false;
  }
  if (!cJSON_IsString(node)) {
    return false;
  }
  *out = node->valuestring != nullptr ? node->valuestring : "";
  return true;
}

// Pull a float from a number node or a numeric string. Sets *out to
// the value, returns true on success.
bool cjson_assign_number_f(const cJSON* node, float* out) {
  if (node == nullptr || out == nullptr || cJSON_IsNull(node)) {
    return false;
  }
  if (cJSON_IsNumber(node)) {
    *out = static_cast<float>(node->valuedouble);
    return true;
  }
  if (cJSON_IsString(node) && node->valuestring != nullptr) {
    char* end = nullptr;
    const float parsed = std::strtof(node->valuestring, &end);
    if (end != node->valuestring && parsed >= 0.0f) {
      *out = parsed;
      return true;
    }
  }
  return false;
}

// Pull an integer from a number node or a numeric string. Integer
// fields are layer counts which are always non-negative.
bool cjson_assign_number_int(const cJSON* node, int* out) {
  if (node == nullptr || out == nullptr || cJSON_IsNull(node)) {
    return false;
  }
  if (cJSON_IsNumber(node)) {
    const double value = node->valuedouble;
    if (value < 0.0) {
      return false;
    }
    *out = static_cast<int>(value);
    return true;
  }
  if (cJSON_IsString(node) && node->valuestring != nullptr) {
    char* end = nullptr;
    const long parsed = std::strtol(node->valuestring, &end, 10);
    if (end != node->valuestring && parsed >= 0) {
      *out = static_cast<int>(parsed);
      return true;
    }
  }
  return false;
}

// Count forbidden-key occurrences inside the entire subtree rooted at
// `node`. A forbidden key on an object increments by one regardless
// of value type or nesting level. Safe-field keys do not increment.
uint32_t count_forbidden_in_subtree(const cJSON* node) {
  if (node == nullptr) {
    return 0;
  }
  uint32_t counted = 0;
  if (cJSON_IsObject(node)) {
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
      const char* child_key = child->string != nullptr ? child->string : "";
      if (bambustat_is_forbidden_payload_key(child_key)) {
        counted += 1;
      }
      counted += count_forbidden_in_subtree(child);
    }
  } else if (cJSON_IsArray(node)) {
    const int array_size = cJSON_GetArraySize(node);
    for (int i = 0; i < array_size; ++i) {
      const cJSON* element = cJSON_GetArrayItem(node, i);
      counted += count_forbidden_in_subtree(element);
    }
  }
  return counted;
}

// Pull a top-level string field by case-sensitive key. Returns true
// only when the key was present and the value is a non-null string.
bool read_top_string(const cJSON* root, const char* key, std::string* out) {
  if (root == nullptr || key == nullptr) {
    return false;
  }
  const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, key);
  return cjson_assign_string(node, out);
}

}  // namespace

bool bambustat_is_forbidden_payload_key(const char* key) {
  if (key == nullptr) {
    return false;
  }
  for (size_t i = 0; i < kBambustatForbiddenPayloadKeyCount; ++i) {
    if (ci_key_equals(key, kBambustatForbiddenPayloadKeys[i])) {
      return true;
    }
  }
  return false;
}

bool bambustat_is_hardware_safe_field(const char* key) {
  if (key == nullptr) {
    return false;
  }
  for (size_t i = 0; i < kBambustatHardwareSafeFieldCount; ++i) {
    if (ci_key_equals(key, kBambustatHardwareSafeFieldNames[i])) {
      return true;
    }
  }
  return false;
}

uint32_t bambustat_compact_status_suppressed_count(
    const BambustatCompactStatus& status) {
  return status.suppressed_forbidden_keys;
}

void bambustat_reset_compact_status(BambustatCompactStatus* status) {
  if (status == nullptr) {
    return;
  }
  *status = BambustatCompactStatus{};
}

bool bambustat_parse_compact_status_cjson(const cJSON* root,
                                          BambustatCompactStatus* out) {
  if (out == nullptr) {
    return false;
  }
  bambustat_reset_compact_status(out);

  if (root == nullptr || cJSON_IsNull(root)) {
    out->parse_ok = true;
    out->parse_error = "empty_payload";
    return true;
  }
  if (!cJSON_IsObject(root)) {
    out->parse_error = "expected_object_root";
    return false;
  }

  // Count forbidden keys up-front across the whole subtree. This
  // intentionally walks the entire payload even when a forbidden key
  // is buried inside `ams_slot_summary` or other nested objects.
  out->suppressed_forbidden_keys = count_forbidden_in_subtree(root);

  uint32_t known_hits = 0;

  if (read_top_string(root, "material", &out->material)) {
    ++known_hits;
  }
  if (read_top_string(root, "color", &out->color)) {
    ++known_hits;
  }
  if (read_top_string(root, "color_hex", &out->color_hex)) {
    ++known_hits;
  }
  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "remaining_grams");
    if (cjson_assign_number_f(node, &out->remaining_grams)) {
      out->remaining_grams_known = true;
      ++known_hits;
    }
  }

  if (read_top_string(root, "storage_label", &out->storage_label)) {
    ++known_hits;
  }
  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "low_stock_warning");
    if (cJSON_IsBool(node)) {
      out->low_stock_warning = cJSON_IsTrue(node);
      out->low_stock_warning_present = true;
      ++known_hits;
    }
  }
  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "unbound_spool_hint");
    if (cJSON_IsBool(node)) {
      out->unbound_spool_hint = cJSON_IsTrue(node);
      out->unbound_spool_hint_present = true;
      ++known_hits;
    }
  }

  if (read_top_string(root, "printer_state", &out->printer_state)) {
    ++known_hits;
  }
  if (read_top_string(root, "task_title", &out->task_title)) {
    ++known_hits;
  }
  if (read_top_string(root, "task_short_title", &out->task_short_title)) {
    ++known_hits;
  }
  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "progress_percent");
    if (cjson_assign_number_f(node, &out->progress_percent)) {
      out->progress_percent_known = true;
      ++known_hits;
    }
  }

  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "bed_temp");
    if (cjson_assign_number_f(node, &out->bed_temp_c)) {
      out->bed_temp_known = true;
      ++known_hits;
    }
  }
  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "chamber_temp");
    if (cjson_assign_number_f(node, &out->chamber_temp_c)) {
      out->chamber_temp_known = true;
      ++known_hits;
    }
  }
  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "layer_current");
    if (cjson_assign_number_int(node, &out->layer_current)) {
      out->layer_current_known = true;
      ++known_hits;
    }
  }
  {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "layer_total");
    if (cjson_assign_number_int(node, &out->layer_total)) {
      out->layer_total_known = true;
      ++known_hits;
    }
  }

  if (read_top_string(root, "eta_text", &out->eta_text)) {
    ++known_hits;
  }
  if (read_top_string(root, "ams_slot_summary", &out->ams_slot_summary)) {
    ++known_hits;
  }
  if (read_top_string(root, "compact_printer_health_summary",
                      &out->compact_printer_health_summary)) {
    ++known_hits;
  }

  out->known_field_hits = known_hits;

  // Count top-level keys (after forbidden-key suppression) for the
  // visible audit. The values themselves are never surfaced — only
  // the presence of hardware-safe fields is.
  for (const cJSON* child = root->child; child != nullptr; child = child->next) {
    if (child->string == nullptr) {
      continue;
    }
    if (!bambustat_is_forbidden_payload_key(child->string)) {
      out->parsed_top_level_keys += 1;
    }
  }

  out->parse_ok = true;
  return true;
}

bool bambustat_parse_compact_status_json(const char* json_body, size_t len,
                                         BambustatCompactStatus* out) {
  if (out == nullptr) {
    return false;
  }
  bambustat_reset_compact_status(out);

  if (json_body == nullptr || len == 0) {
    out->parse_ok = true;
    out->parse_error = "empty_payload";
    return true;
  }

  cJSON* root = cJSON_ParseWithLength(json_body, len);
  if (root == nullptr) {
    out->parse_error = "cjson_parse_failed";
    return false;
  }

  const bool ok = bambustat_parse_compact_status_cjson(root, out);
  cJSON_Delete(root);
  return ok;
}

}  // namespace printsphere
