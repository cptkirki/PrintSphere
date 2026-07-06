#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "cJSON.h"

namespace printsphere {

// HARDWARE-003: PMD-007 compact-status consumer for the round AMOLED 1.75"
// screen. This header is the build-only, read-only parser/mapper surface.
// It must NOT fetch, decode, store, or forward any forbidden payload field.
// See: ../../../3d-filament-inventory/docs/bambustat-compact-status-contract.json
// See: main/notes/redaction-policy.md
constexpr const char* kBambustatCompactStatusContractVersion =
    "2026-06-22-pmd-007";

// Stable hardware-safe field set from PMD-007 (the only keys the
// caller is allowed to render on the round AMOLED). Order in this
// array is the source-of-truth order from the contract.
constexpr const char* kBambustatHardwareSafeFieldNames[] = {
    "material",
    "color",
    "color_hex",
    "remaining_grams",
    "storage_label",
    "low_stock_warning",
    "unbound_spool_hint",
    "printer_state",
    "task_title",
    "task_short_title",
    "progress_percent",
    "bed_temp",
    "chamber_temp",
    "layer_current",
    "layer_total",
    "eta_text",
    "ams_slot_summary",
    "compact_printer_health_summary",
};
constexpr size_t kBambustatHardwareSafeFieldCount =
    sizeof(kBambustatHardwareSafeFieldNames) /
    sizeof(kBambustatHardwareSafeFieldNames[0]);

// Forbidden payload keys (PMD-007 contract deny-list + a small
// forward-compat set from the round-AMOLED redaction policy). Any
// matching JSON key is suppressed and counted. Matching is
// case-insensitive.
constexpr const char* kBambustatForbiddenPayloadKeys[] = {
    // contract.json forbidden_payload_fields
    "order_id",
    "customer_id",
    "quote_id",
    "revenue",
    "profit",
    "payment",
    "address",
    "phone",
    "media_asset_id",
    "recognized_candidate_id",
    "access_code",
    "token",
    "secret",
    "nvs",
    // redaction-policy.md aligned extensions
    "customer",
    "customer_name",
    "customer_email",
    "order_number",
    "order_total",
    "quote_total",
    "material_cost",
    "margin",
    "markup",
    "media_url",
    "thumbnail_url",
    "screenshot_url",
    "image_base64",
    "file_data",
    "raw_media",
    "mqtt_message",
    "mqtt_payload",
    "mqtt_raw",
    "mqtt_pushall",
    "bambu_host",
    "printer_ip",
    "cookie",
    "bambucloud_session",
    "password",
    "client_secret",
    "app_secret",
    "api_secret",
};
constexpr size_t kBambustatForbiddenPayloadKeyCount =
    sizeof(kBambustatForbiddenPayloadKeys) /
    sizeof(kBambustatForbiddenPayloadKeys[0]);

// Hardware-safe, view-shaped model returned to the UI layer. Only
// fields that the contract marks as safe to display are populated.
// Tri-state booleans distinguish "absent" from "false" so the UI
// can suppress the row entirely instead of showing a misleading 0.
struct BambustatCompactStatus {
  std::string material;
  std::string color;
  std::string color_hex;
  bool remaining_grams_known = false;
  float remaining_grams = 0.0f;
  std::string storage_label;
  bool low_stock_warning = false;
  bool low_stock_warning_present = false;
  bool unbound_spool_hint = false;
  bool unbound_spool_hint_present = false;
  std::string printer_state;
  std::string task_title;
  std::string task_short_title;
  bool progress_percent_known = false;
  float progress_percent = 0.0f;
  bool bed_temp_known = false;
  float bed_temp_c = 0.0f;
  bool chamber_temp_known = false;
  float chamber_temp_c = 0.0f;
  bool layer_current_known = false;
  int layer_current = 0;
  bool layer_total_known = false;
  int layer_total = 0;
  std::string eta_text;
  std::string ams_slot_summary;
  std::string compact_printer_health_summary;

  // Redaction / safety audit counters.
  uint32_t parsed_top_level_keys = 0;
  uint32_t suppressed_forbidden_keys = 0;
  uint32_t known_field_hits = 0;
  bool parse_ok = false;
  std::string parse_error;
};

// Forbidden-key classification helpers. Matching is case-insensitive
// and treats '-' and '_' as equivalent. A key that is both a safe
// field and on the deny-list is reported as forbidden; the contract
// stable field set never overlaps with the deny-list, so this branch
// is defensive.
bool bambustat_is_forbidden_payload_key(const char* key);
bool bambustat_is_hardware_safe_field(const char* key);

// Returns the number of forbidden-key occurrences skipped during the
// most recent parse, summed across both top-level and nested objects.
// Forbidden keys contribute their *presence* but never their value.
uint32_t bambustat_compact_status_suppressed_count(
    const BambustatCompactStatus& status);

// Top-level JSON parse entry point. `json_body` does NOT have to be
// null-terminated; `len` bytes are read. Returns true on a successful
// parse even when the payload is empty (PMD-007 permits nulls).
//
// On entry, `out` is reset to a known state. On false return, the
// caller must NOT read suppressed_forbidden_keys from `out`; treat
// the whole struct as invalid.
bool bambustat_parse_compact_status_json(const char* json_body, size_t len,
                                         BambustatCompactStatus* out);

// Variant that takes a pre-parsed cJSON tree (lets callers share a
// parse pass with firmware-internal consumers).
bool bambustat_parse_compact_status_cjson(const cJSON* root,
                                          BambustatCompactStatus* out);

// Forced reset that clears every byte of a model to its default. Lets
// callers reuse a stack-resident model between polls without leak.
void bambustat_reset_compact_status(BambustatCompactStatus* status);

}  // namespace printsphere
