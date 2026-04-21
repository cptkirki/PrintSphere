#include "printsphere/setup_portal.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/bambu_status.hpp"
#include "printsphere/debug_log_buffer.hpp"
#include "printsphere/ui.hpp"

namespace printsphere {

namespace {

#ifndef PRINTSPHERE_RELEASE_VERSION
#define PRINTSPHERE_RELEASE_VERSION "dev"
#endif

constexpr char kTag[] = "printsphere.portal";
constexpr size_t kMaxRequestBody = 4096;
constexpr char kPortalSessionCookieName[] = "printsphere_portal_session";
constexpr uint64_t kPortalPinLifetimeMs = 2ULL * 60ULL * 1000ULL;
constexpr uint64_t kPortalSessionLifetimeMs = 10ULL * 60ULL * 1000ULL;
constexpr uint64_t kPortalSessionExtendMs = 5ULL * 60ULL * 1000ULL;
constexpr uint64_t kPortalProvisioningGraceMs = 5ULL * 60ULL * 1000ULL;
constexpr char kPortalReleaseVersion[] = PRINTSPHERE_RELEASE_VERSION;
constexpr char kFaviconSvg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
    "<rect width=\"64\" height=\"64\" rx=\"16\" fill=\"#121a23\"/>"
    "<circle cx=\"32\" cy=\"32\" r=\"18\" fill=\"none\" stroke=\"#f0a64b\" stroke-width=\"8\"/>"
    "</svg>";

std::string json_escape(const std::string& input) {
  std::string output;
  output.reserve(input.size());

  for (const char ch : input) {
    switch (ch) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }

  return output;
}

std::string read_string_field(const cJSON* object, const char* key) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return {};
  }
  return item->valuestring;
}

bool read_bool_field(const cJSON* object, const char* key, bool fallback) {
  if (object == nullptr || key == nullptr || key[0] == '\0') {
    return fallback;
  }

  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsBool(item)) {
    return cJSON_IsTrue(item);
  }
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return fallback;
  }

  std::string value = item->valuestring;
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  value = value.substr(start, end - start);
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  if (value == "1" || value == "true" || value == "on" || value == "enabled") {
    return true;
  }
  if (value == "0" || value == "false" || value == "off" || value == "disabled") {
    return false;
  }
  return fallback;
}

std::string trim_copy(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
    ++start;
  }

  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }

  return input.substr(start, end - start);
}

WifiCredentials merge_wifi_credentials(WifiCredentials submitted,
                                       const WifiCredentials& stored) {
  if (submitted.password.empty() && !submitted.ssid.empty() && submitted.ssid == stored.ssid) {
    submitted.password = stored.password;
  }
  return submitted;
}

BambuCloudCredentials merge_cloud_credentials(BambuCloudCredentials submitted,
                                              const BambuCloudCredentials& stored) {
  if (submitted.password.empty() && !submitted.email.empty() && submitted.email == stored.email) {
    submitted.password = stored.password;
  }
  return submitted;
}

PrinterConnection merge_printer_connection(PrinterConnection submitted,
                                           const PrinterConnection& stored) {
  if (submitted.access_code.empty() && !submitted.serial.empty() &&
      submitted.serial == stored.serial) {
    submitted.access_code = stored.access_code;
  }
  return submitted;
}

bool can_reuse_cloud_session(const BambuCloudCredentials& submitted,
                             const BambuCloudCredentials& stored,
                             const std::string& access_token) {
  return !access_token.empty() && !submitted.email.empty() && submitted.email == stored.email &&
         submitted.region == stored.region;
}

std::string color_to_html_hex(uint32_t color) {
  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "#%06X", static_cast<unsigned int>(color & 0xFFFFFFU));
  return buffer;
}

bool parse_html_color(const std::string& input, uint32_t* color) {
  if (color == nullptr) {
    return false;
  }

  std::string normalized = trim_copy(input);
  if (!normalized.empty() && normalized.front() == '#') {
    normalized.erase(normalized.begin());
  }
  if (normalized.size() != 6U) {
    return false;
  }

  for (const char ch : normalized) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }

  *color = static_cast<uint32_t>(std::strtoul(normalized.c_str(), nullptr, 16));
  return true;
}

bool is_valid_ipv4(const std::string& host) {
  int dots = 0;
  int octet_value = 0;
  int octet_digits = 0;

  for (size_t i = 0; i < host.size(); ++i) {
    const char ch = host[i];
    if (ch == '.') {
      if (octet_digits == 0 || octet_value > 255) {
        return false;
      }
      ++dots;
      octet_value = 0;
      octet_digits = 0;
      continue;
    }

    if (ch < '0' || ch > '9') {
      return false;
    }

    octet_value = (octet_value * 10) + (ch - '0');
    ++octet_digits;
    if (octet_digits > 3) {
      return false;
    }
  }

  return dots == 3 && octet_digits > 0 && octet_value <= 255;
}

bool is_valid_hostname(const std::string& host) {
  if (host.empty() || host.size() > 253) {
    return false;
  }

  bool saw_alpha = false;
  size_t label_len = 0;
  char prev = '\0';
  for (const char ch : host) {
    const bool is_alpha = std::isalpha(static_cast<unsigned char>(ch)) != 0;
    const bool is_digit = std::isdigit(static_cast<unsigned char>(ch)) != 0;
    if (is_alpha) {
      saw_alpha = true;
    }

    if (is_alpha || is_digit || ch == '-') {
      ++label_len;
      if (label_len > 63) {
        return false;
      }
    } else if (ch == '.') {
      if (label_len == 0 || prev == '-') {
        return false;
      }
      label_len = 0;
    } else {
      return false;
    }

    prev = ch;
  }

  return label_len > 0 && prev != '-' && saw_alpha;
}

bool is_valid_printer_host(const std::string& host) {
  return is_valid_ipv4(host) || is_valid_hostname(host);
}

uint64_t now_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

uint32_t remaining_seconds(uint64_t expiry_ms, uint64_t current_ms) {
  if (expiry_ms <= current_ms) {
    return 0;
  }
  return static_cast<uint32_t>((expiry_ms - current_ms + 999ULL) / 1000ULL);
}

std::string duration_text(uint32_t total_seconds) {
  const uint32_t minutes = total_seconds / 60U;
  const uint32_t seconds = total_seconds % 60U;
  char buffer[32] = {};
  if (minutes > 0U) {
    std::snprintf(buffer, sizeof(buffer), "%um %us", static_cast<unsigned int>(minutes),
                  static_cast<unsigned int>(seconds));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%us", static_cast<unsigned int>(seconds));
  }
  return buffer;
}

std::string cookie_value(const std::string& header, const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return {};
  }

  const std::string needle = std::string(key) + "=";
  size_t start = 0;
  while (start < header.size()) {
    size_t end = header.find(';', start);
    if (end == std::string::npos) {
      end = header.size();
    }

    std::string part = trim_copy(header.substr(start, end - start));
    if (part.rfind(needle, 0) == 0) {
      return part.substr(needle.size());
    }

    start = end + 1U;
  }
  return {};
}

std::string request_cookie(httpd_req_t* request, const char* key) {
  if (request == nullptr) {
    return {};
  }

  const size_t header_len = httpd_req_get_hdr_value_len(request, "Cookie");
  if (header_len == 0U) {
    return {};
  }

  std::vector<char> buffer(header_len + 1U, '\0');
  if (httpd_req_get_hdr_value_str(request, "Cookie", buffer.data(), buffer.size()) != ESP_OK) {
    return {};
  }

  return cookie_value(buffer.data(), key);
}

std::string session_cookie_header(const std::string& token, uint32_t max_age_s) {
  return std::string(kPortalSessionCookieName) + "=" + token + "; Max-Age=" +
         std::to_string(max_age_s) + "; HttpOnly; SameSite=Strict; Path=/";
}

void append_portal_access_fields(std::string* body, const PortalAccessSnapshot& access) {
  if (body == nullptr) {
    return;
  }

  *body += ",\"portal_locked\":";
  *body += access.request_authorized ? "false" : "true";
  *body += ",\"portal_lock_enabled\":";
  *body += access.lock_enabled ? "true" : "false";
  *body += ",\"portal_session_active\":";
  *body += access.session_active ? "true" : "false";
  *body += ",\"portal_pin_active\":";
  *body += access.pin_active ? "true" : "false";
  *body += ",\"portal_pin_remaining_s\":";
  *body += std::to_string(access.pin_remaining_s);
  *body += ",\"portal_session_remaining_s\":";
  *body += std::to_string(access.session_remaining_s);
  *body += ",\"portal_detail\":\"" + json_escape(access.detail) + "\"";
  *body += ",\"portal_open_remaining_s\":";
  if (access.grace_remaining_s > 0) {
    *body += std::to_string(access.grace_remaining_s);
  } else if (access.session_active && access.session_remaining_s > 0) {
    *body += std::to_string(access.session_remaining_s);
  } else {
    *body += "0";
  }
}

void send_json(httpd_req_t* request, const std::string& body) {
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t receive_json_body(httpd_req_t* request, cJSON** root_out) {
  if (root_out == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  *root_out = nullptr;
  if (request->content_len <= 0 || request->content_len > static_cast<int>(kMaxRequestBody)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid body length");
  }

  std::unique_ptr<char[]> body(new char[request->content_len + 1]);
  int received = 0;
  while (received < request->content_len) {
    const int ret = httpd_req_recv(request, body.get() + received, request->content_len - received);
    if (ret <= 0) {
      return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "body read failed");
    }
    received += ret;
  }
  body[request->content_len] = '\0';

  cJSON* root = cJSON_Parse(body.get());
  if (root == nullptr) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid json");
  }

  *root_out = root;
  return ESP_OK;
}

bool parse_arc_colors_from_json(const cJSON* root, ArcColorScheme* colors) {
  if (root == nullptr || colors == nullptr) {
    return false;
  }

  const auto parse_arc_field = [&](const char* key, uint32_t* value) -> bool {
    const std::string raw = trim_copy(read_string_field(root, key));
    if (raw.empty()) {
      return true;
    }
    return parse_html_color(raw, value);
  };

  return parse_arc_field("arc_printing", &colors->printing) &&
         parse_arc_field("arc_done", &colors->done) &&
         parse_arc_field("arc_error", &colors->error) &&
         parse_arc_field("arc_idle", &colors->idle) &&
         parse_arc_field("arc_preheat", &colors->preheat) &&
         parse_arc_field("arc_clean", &colors->clean) &&
         parse_arc_field("arc_level", &colors->level) &&
         parse_arc_field("arc_cool", &colors->cool) &&
         parse_arc_field("arc_idle_active", &colors->idle_active) &&
         parse_arc_field("arc_filament", &colors->filament) &&
         parse_arc_field("arc_setup", &colors->setup) &&
         parse_arc_field("arc_offline", &colors->offline) &&
         parse_arc_field("arc_unknown", &colors->unknown);
}

std::string cloud_verify_label(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "2FA Code" : "Verification Code";
}

std::string cloud_verify_placeholder(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "Only needed if Bambu requests a 2FA code"
                               : "Only needed if Bambu requests a verification code";
}

std::string cloud_verify_note(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "Bambu is currently waiting for a 2FA code."
                               : "Bambu is currently waiting for a verification code. The cloud login completes after that step.";
}

SourceMode parse_source_mode_field(const cJSON* root) {
  if (root == nullptr) {
    return SourceMode::kHybrid;
  }

  std::string value = trim_copy(read_string_field(root, "source_mode"));
  if (value.empty()) {
    value = trim_copy(read_string_field(root, "state_source"));
  }
  return parse_source_mode(value);
}

DisplayRotation parse_display_rotation_field(const cJSON* root) {
  if (root == nullptr) {
    return DisplayRotation::k0;
  }

  const std::string value = trim_copy(read_string_field(root, "display_rotation"));
  return parse_display_rotation(value);
}

std::string source_mode_badge_value(SourceMode mode) {
  switch (mode) {
    case SourceMode::kCloudOnly:
      return "Cloud only";
    case SourceMode::kLocalOnly:
      return "Local only";
    case SourceMode::kHybrid:
    default:
      return "Hybrid (recommended)";
  }
}

std::string display_rotation_badge_value(DisplayRotation rotation) {
  return std::string(to_string(rotation)) + " deg";
}

bool cloud_detail_is_transitional(const std::string& detail) {
  const std::string normalized = normalize_bambu_status_token(detail);
  return normalized == "LOGGING IN TO BAMBU CLOUD" ||
         normalized == "WAITING FOR WI-FI FOR BAMBU CLOUD" ||
         normalized == "RESTORED BAMBU CLOUD SESSION";
}

bool cloud_stage_is_user_visible_progress(CloudSetupStage stage) {
  switch (stage) {
    case CloudSetupStage::kEmailCodeRequired:
    case CloudSetupStage::kTfaRequired:
    case CloudSetupStage::kBindingPrinter:
    case CloudSetupStage::kConnectingMqtt:
    case CloudSetupStage::kConnected:
    case CloudSetupStage::kFailed:
      return true;
    case CloudSetupStage::kIdle:
    case CloudSetupStage::kLoggingIn:
    case CloudSetupStage::kCodeSubmitted:
    default:
      return false;
  }
}

bool cloud_portal_ready(const BambuCloudSnapshot& snapshot);

struct CloudPortalPresentation {
  BambuCloudSnapshot snapshot{};
  bool ready = false;
  std::string badge_value = "Not configured";
  const char* badge_class = "idle";
  std::string status_line = "Connect Bambu Cloud";
  std::string status_detail = "No cloud response yet";
};

bool cloud_connect_result_ready(const BambuCloudSnapshot& before,
                                const BambuCloudSnapshot& current) {
  if (!cloud_portal_ready(before) && cloud_portal_ready(current)) {
    return true;
  }
  if ((!before.verification_required && current.verification_required) ||
      (!before.tfa_required && current.tfa_required)) {
    return true;
  }
  if (cloud_stage_is_user_visible_progress(current.setup_stage) &&
      current.setup_stage != before.setup_stage) {
    return true;
  }

  if (current.configured != before.configured ||
      current.resolved_serial != before.resolved_serial) {
    return true;
  }

  if (current.detail == before.detail) {
    return false;
  }

  return !cloud_detail_is_transitional(current.detail);
}

bool cloud_verify_result_ready(const BambuCloudSnapshot& before,
                               const BambuCloudSnapshot& current) {
  if (!cloud_portal_ready(before) && cloud_portal_ready(current)) {
    return true;
  }
  if ((!before.verification_required && current.verification_required) ||
      (!before.tfa_required && current.tfa_required)) {
    return true;
  }
  if (cloud_stage_is_user_visible_progress(current.setup_stage) &&
      current.setup_stage != before.setup_stage) {
    return true;
  }
  if (current.resolved_serial != before.resolved_serial) {
    return true;
  }
  if (current.detail == before.detail) {
    return false;
  }
  if (current.setup_stage == CloudSetupStage::kFailed) {
    return true;
  }
  return !cloud_detail_is_transitional(current.detail) &&
         current.setup_stage != CloudSetupStage::kCodeSubmitted &&
         current.setup_stage != CloudSetupStage::kLoggingIn;
}

bool cloud_portal_ready(const BambuCloudSnapshot& snapshot) {
  if (!snapshot.configured || snapshot.verification_required || snapshot.tfa_required) {
    return false;
  }
  if (snapshot.session_connected || snapshot.connected) {
    return true;
  }
  switch (snapshot.setup_stage) {
    case CloudSetupStage::kBindingPrinter:
    case CloudSetupStage::kConnectingMqtt:
    case CloudSetupStage::kConnected:
      return true;
    case CloudSetupStage::kIdle:
    case CloudSetupStage::kLoggingIn:
    case CloudSetupStage::kEmailCodeRequired:
    case CloudSetupStage::kTfaRequired:
    case CloudSetupStage::kCodeSubmitted:
    case CloudSetupStage::kFailed:
    default:
      return false;
  }
}

BambuCloudSnapshot portal_cloud_view(BambuCloudSnapshot snapshot) {
  if (!cloud_portal_ready(snapshot)) {
    return snapshot;
  }

  snapshot.connected = true;
  snapshot.setup_stage = CloudSetupStage::kConnected;
  if (cloud_detail_is_transitional(snapshot.detail) || snapshot.detail.empty()) {
    snapshot.detail = "Bambu Cloud session is ready.";
  }
  return snapshot;
}

CloudPortalPresentation cloud_portal_presentation(const BambuCloudSnapshot& cloud) {
  CloudPortalPresentation presentation;
  presentation.ready = cloud_portal_ready(cloud);
  presentation.snapshot = portal_cloud_view(cloud);
  presentation.status_detail =
      presentation.snapshot.detail.empty() ? "No cloud response yet" : presentation.snapshot.detail;

  if (presentation.ready) {
    presentation.badge_value = "Connected";
    presentation.badge_class = "ok";
    presentation.status_line = "Cloud connected";
    return presentation;
  }

  if (presentation.snapshot.verification_required) {
    presentation.badge_value = "Code required";
    presentation.badge_class = "warn";
    presentation.status_line =
        presentation.snapshot.tfa_required ? "2FA required" : "Verification code required";
    return presentation;
  }

  switch (presentation.snapshot.setup_stage) {
    case CloudSetupStage::kLoggingIn:
      presentation.badge_value = "Logging in";
      presentation.badge_class = "info";
      presentation.status_line = "Logging in";
      break;
    case CloudSetupStage::kCodeSubmitted:
      presentation.badge_value = "Verifying code";
      presentation.badge_class = "info";
      presentation.status_line = "Verifying code";
      break;
    case CloudSetupStage::kBindingPrinter:
      presentation.badge_value = "Binding printer";
      presentation.badge_class = "info";
      presentation.status_line = "Binding printer";
      break;
    case CloudSetupStage::kConnectingMqtt:
      presentation.badge_value = "Connecting MQTT";
      presentation.badge_class = "info";
      presentation.status_line = "Connecting Cloud MQTT";
      break;
    case CloudSetupStage::kFailed:
      presentation.badge_value = "Login failed";
      presentation.badge_class = "warn";
      presentation.status_line = "Cloud login failed";
      break;
    case CloudSetupStage::kConnected:
      presentation.badge_value = "Connected";
      presentation.badge_class = "ok";
      presentation.status_line = "Cloud connected";
      break;
    case CloudSetupStage::kIdle:
    case CloudSetupStage::kEmailCodeRequired:
    case CloudSetupStage::kTfaRequired:
    default:
      break;
  }

  if (presentation.snapshot.configured && presentation.badge_value == "Not configured") {
    presentation.badge_value = "Configured";
    presentation.badge_class = "info";
  }

  return presentation;
}

bool cloud_login_still_pending(const BambuCloudSnapshot& snapshot) {
  if (!snapshot.configured || cloud_portal_ready(snapshot) || snapshot.verification_required ||
      snapshot.tfa_required) {
    return false;
  }

  return snapshot.setup_stage != CloudSetupStage::kFailed;
}

void append_cloud_status_fields(std::string* body, const BambuCloudSnapshot& cloud) {
  if (body == nullptr) {
    return;
  }

  const CloudPortalPresentation portal = cloud_portal_presentation(cloud);
  *body += ",\"cloud_connected\":";
  *body += (portal.ready ? "true" : "false");
  *body += ",\"cloud_live_connected\":";
  *body += (cloud.connected ? "true" : "false");
  *body += ",\"cloud_session_connected\":";
  *body += (cloud.session_connected ? "true" : "false");
  *body += ",\"cloud_verification_required\":";
  *body += (portal.snapshot.verification_required ? "true" : "false");
  *body += ",\"cloud_tfa_required\":";
  *body += (portal.snapshot.tfa_required ? "true" : "false");
  *body += ",\"cloud_configured\":";
  *body += (portal.snapshot.configured ? "true" : "false");
  *body += ",\"cloud_portal_ready\":";
  *body += (portal.ready ? "true" : "false");
  *body += ",\"cloud_setup_stage\":\"";
  *body += json_escape(to_string(portal.snapshot.setup_stage));
  *body += "\"";
  *body += ",\"cloud_detail\":\"" + json_escape(portal.snapshot.detail) + "\"";
  *body += ",\"cloud_resolved_serial\":\"" + json_escape(portal.snapshot.resolved_serial) + "\"";
  *body += ",\"cloud_badge_value\":\"" + json_escape(portal.badge_value) + "\"";
  *body += ",\"cloud_badge_state\":\"" + json_escape(portal.badge_class) + "\"";
  *body += ",\"cloud_status_line\":\"" + json_escape(portal.status_line) + "\"";
  *body += ",\"cloud_status_detail\":\"" + json_escape(portal.status_detail) + "\"";
}

void append_local_status_fields(std::string* body, const PrinterSnapshot& local, bool local_configured) {
  if (body == nullptr) {
    return;
  }

  *body += ",\"local_error\":";
  *body += (local.connection == PrinterConnectionState::kError ? "true" : "false");
  *body += ",\"local_connected\":";
  *body += (local.connection == PrinterConnectionState::kOnline ? "true" : "false");
  *body += ",\"local_configured\":";
  *body += (local_configured ? "true" : "false");
  *body += ",\"local_detail\":\"" + json_escape(local.detail) + "\"";
}

}  // namespace

void SetupPortal::request_unlock_pin() {
  const uint64_t current_ms = now_ms();
  std::lock_guard<std::mutex> lock(access_mutex_);
  prune_access_state_locked(current_ms);
  if (!is_lock_required()) {
    unlock_pin_.clear();
    unlock_pin_expiry_ms_ = 0;
    session_token_.clear();
    session_expiry_ms_ = 0;
    return;
  }
  unlock_pin_ = generate_unlock_pin();
  unlock_pin_expiry_ms_ = current_ms + kPortalPinLifetimeMs;
}

PortalAccessSnapshot SetupPortal::access_snapshot(bool request_authorized) {
  const bool lock_enabled = is_lock_enabled();
  const bool lock_required = is_lock_required();
  const uint64_t current_ms = now_ms();
  std::lock_guard<std::mutex> lock(access_mutex_);
  prune_access_state_locked(current_ms);

  PortalAccessSnapshot snapshot;
  snapshot.lock_enabled = lock_enabled;
  snapshot.request_authorized = lock_required ? request_authorized : true;
  snapshot.session_active = !session_token_.empty();
  snapshot.pin_active = !unlock_pin_.empty();
  snapshot.session_remaining_s = remaining_seconds(session_expiry_ms_, current_ms);
  snapshot.pin_remaining_s = remaining_seconds(unlock_pin_expiry_ms_, current_ms);
  snapshot.pin_code = unlock_pin_;

  if (!lock_required) {
    unlock_pin_.clear();
    unlock_pin_expiry_ms_ = 0;
    session_token_.clear();
    session_expiry_ms_ = 0;
    snapshot.session_active = false;
    snapshot.pin_active = false;
    snapshot.session_remaining_s = 0;
    snapshot.pin_remaining_s = 0;
    snapshot.pin_code.clear();

    if (!lock_enabled) {
      snapshot.detail =
          "Portal PIN lock is disabled. Web Config stays open on your home network.";
    } else if (wifi_manager_.is_setup_access_point_active()) {
      snapshot.detail = "Web Config is open while the setup access point is active.";
    } else if (!wifi_manager_.is_station_connected()) {
      snapshot.detail = "Web Config is open while PrintSphere is waiting for Wi-Fi credentials.";
    } else if (provisioning_grace_expiry_ms_ != 0 && current_ms < provisioning_grace_expiry_ms_) {
      const uint32_t grace_remaining_s =
          static_cast<uint32_t>((provisioning_grace_expiry_ms_ - current_ms) / 1000ULL);
      snapshot.grace_remaining_s = grace_remaining_s;
      snapshot.detail = "Web Config is open for " +
                        duration_text(std::max<uint32_t>(grace_remaining_s, 1U)) +
                        " after initial setup. Finish your configuration now.";
    } else {
      switch (config_store_.load_source_mode()) {
        case SourceMode::kCloudOnly:
          snapshot.detail = "Web Config is open until Bambu Cloud is connected.";
          break;
        case SourceMode::kLocalOnly:
          snapshot.detail = "Web Config is open until the local printer path is connected.";
          break;
        case SourceMode::kHybrid:
        default:
          snapshot.detail =
              "Web Config is open until either Bambu Cloud or the local printer path is connected.";
          break;
      }
    }
  } else if (snapshot.pin_active) {
    snapshot.detail =
        "Unlock PIN is visible on the device display for " +
        duration_text(std::max<uint32_t>(snapshot.pin_remaining_s, 1U)) + ".";
  } else if (snapshot.session_active && request_authorized) {
    snapshot.detail =
        "Web Config unlocked for " +
        duration_text(std::max<uint32_t>(snapshot.session_remaining_s, 1U)) + ".";
  } else if (snapshot.session_active) {
    snapshot.detail = "A browser session is active. Hold the display to show a new unlock PIN.";
  } else {
    snapshot.detail =
        "Hold anywhere on the device display for one second to show an unlock PIN.";
  }

  return snapshot;
}

bool SetupPortal::is_provisioning_complete() const {
  if (wifi_manager_.is_setup_access_point_active() || !wifi_manager_.is_station_connected()) {
    return false;
  }

  const SourceMode source_mode = config_store_.load_source_mode();
  const PrinterSnapshot local = printer_client_.snapshot();
  const BambuCloudSnapshot cloud = cloud_client_.snapshot();
  const bool local_connected = local.connection == PrinterConnectionState::kOnline;
  const bool cloud_connected = cloud_portal_ready(cloud);

  switch (source_mode) {
    case SourceMode::kCloudOnly:
      return cloud_connected;
    case SourceMode::kLocalOnly:
      return local_connected;
    case SourceMode::kHybrid:
    default:
      return cloud_connected || local_connected;
  }
}

void SetupPortal::prune_access_state_locked(uint64_t current_ms) {
  if (unlock_pin_expiry_ms_ != 0 && current_ms >= unlock_pin_expiry_ms_) {
    unlock_pin_.clear();
    unlock_pin_expiry_ms_ = 0;
  }
  if (session_expiry_ms_ != 0 && current_ms >= session_expiry_ms_) {
    session_token_.clear();
    session_expiry_ms_ = 0;
  }
}

std::string SetupPortal::generate_unlock_pin() {
  char buffer[7] = {};
  std::snprintf(buffer, sizeof(buffer), "%06u",
                static_cast<unsigned int>(esp_random() % 1000000U));
  return buffer;
}

std::string SetupPortal::generate_session_token() {
  char buffer[33] = {};
  std::snprintf(buffer, sizeof(buffer), "%08x%08x%08x%08x",
                static_cast<unsigned int>(esp_random()),
                static_cast<unsigned int>(esp_random()),
                static_cast<unsigned int>(esp_random()),
                static_cast<unsigned int>(esp_random()));
  return buffer;
}

bool SetupPortal::is_lock_required() const {
  if (wifi_manager_.is_setup_access_point_active()) {
    return false;
  }

  if (!is_lock_enabled()) {
    return false;
  }

  if (!is_provisioning_complete()) {
    return false;
  }

  // Grace period: keep portal open for a few minutes after first provisioning
  const uint64_t current_ms = now_ms();
  if (provisioning_grace_expiry_ms_ == 0) {
    provisioning_grace_expiry_ms_ = current_ms + kPortalProvisioningGraceMs;
    ESP_LOGI(kTag, "Provisioning complete — portal grace period active for %u s",
             static_cast<unsigned>(kPortalProvisioningGraceMs / 1000ULL));
  }
  if (current_ms < provisioning_grace_expiry_ms_) {
    return false;
  }

  return true;
}

bool SetupPortal::is_lock_enabled() const { return config_store_.load_portal_lock_enabled(); }

bool SetupPortal::is_request_authorized(httpd_req_t* request) {
  if (!is_lock_required()) {
    return true;
  }

  const std::string presented = request_cookie(request, kPortalSessionCookieName);
  if (presented.empty()) {
    return false;
  }

  const uint64_t current_ms = now_ms();
  std::lock_guard<std::mutex> lock(access_mutex_);
  prune_access_state_locked(current_ms);
  return !session_token_.empty() && presented == session_token_;
}

esp_err_t SetupPortal::send_locked_response(httpd_req_t* request) {
  PortalAccessSnapshot access = access_snapshot(false);
  const std::string clear_cookie = session_cookie_header("", 0);
  httpd_resp_set_status(request, "423 Locked");
  httpd_resp_set_hdr(request, "Set-Cookie", clear_cookie.c_str());

  std::string body = "{\"error\":\"Portal locked\"";
  append_portal_access_fields(&body, access);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::send_unlock_page(httpd_req_t* request) {
  const PortalAccessSnapshot access = access_snapshot(false);
  const std::string clear_cookie = session_cookie_header("", 0);

  std::string html;
  html.reserve(5000);
  html += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>PrintSphere Unlock</title><style>";
  html += "body{margin:0;font-family:'Segoe UI',sans-serif;background:radial-gradient(circle at top,#172633,#071018 62%);color:#f8fafc;min-height:100vh;display:grid;place-items:center;padding:20px;}";
  html += ".card{width:min(420px,100%);background:rgba(6,18,26,.92);border:1px solid rgba(240,166,75,.28);border-radius:26px;padding:28px;box-shadow:0 22px 80px rgba(0,0,0,.45);}";
  html += "h1{margin:0 0 10px;font-size:32px;}p{margin:0 0 14px;line-height:1.45;color:#cbd5e1;}label{display:block;margin:18px 0 8px;color:#f8fafc;font-weight:600;}";
  html += "input{width:100%;box-sizing:border-box;border-radius:16px;border:1px solid #365064;background:#0f1e29;color:#f8fafc;padding:16px 18px;font-size:26px;letter-spacing:.22em;text-align:center;}";
  html += "button{width:100%;margin-top:16px;border:0;border-radius:16px;background:#f0a64b;color:#071018;padding:14px 18px;font-size:18px;font-weight:700;cursor:pointer;}";
  html += ".status{margin:10px 0 0;color:#f0a64b;font-weight:700;}.micro{margin-top:18px;font-size:14px;color:#94a3b8;}</style></head><body><main class=\"card\">";
  html += "<h1>Portal Locked</h1>";
  html += "<p id=\"detail\">";
  html += json_escape(access.detail);
  html += "</p>";
  html += "<form id=\"unlock-form\"><label for=\"pin\">Unlock PIN</label><input id=\"pin\" inputmode=\"numeric\" autocomplete=\"one-time-code\" maxlength=\"6\" placeholder=\"000000\">";
  html += "<button type=\"submit\" id=\"unlock-button\">Unlock Web Config</button></form>";
  html += "<div class=\"status\" id=\"status\">Waiting for PIN</div>";
  html += "<p class=\"micro\">Long-press anywhere on the PrintSphere display for one second. The 6-digit PIN appears on the device, not in the browser.</p>";
  html += "</main><script>";
  html += "const form=document.getElementById('unlock-form');const pin=document.getElementById('pin');const statusEl=document.getElementById('status');const detailEl=document.getElementById('detail');const button=document.getElementById('unlock-button');";
  html += "async function refresh(){try{const response=await fetch('/api/health',{cache:'no-store'});const body=await response.json().catch(()=>({}));if(body.portal_locked===false){window.location.reload();return;}detailEl.textContent=body.portal_detail||'Hold anywhere on the device display for one second to show an unlock PIN.';statusEl.textContent=body.portal_pin_active?'PIN visible on the display':'Portal locked';}catch(error){statusEl.textContent='Portal unreachable';}}";
  html += "form.addEventListener('submit',async(event)=>{event.preventDefault();const code=pin.value.trim();if(!code){statusEl.textContent='Enter the PIN from the display';return;}button.disabled=true;statusEl.textContent='Unlocking...';try{const response=await fetch('/api/unlock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({pin:code})});const body=await response.json().catch(()=>({}));if(response.ok){window.location.reload();return;}statusEl.textContent=body.error||'Unlock failed';detailEl.textContent=body.detail||'The PIN was rejected.';pin.select();}catch(error){statusEl.textContent='Unlock request failed';}finally{button.disabled=false;}});";
  html += "refresh();setInterval(refresh,2000);</script></body></html>";

  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Set-Cookie", clear_cookie.c_str());
  return httpd_resp_send(request, html.c_str(), html.size());
}

esp_err_t SetupPortal::start() {
  if (server_ != nullptr) {
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 12288;  // 8192 was too small — handle_root builds ~40KB HTML on the stack
  // Leave some headroom for portal endpoints so feature additions do not silently exhaust slots.
  config.max_uri_handlers = 28;
  config.recv_wait_timeout = 30;

  ESP_RETURN_ON_ERROR(httpd_start(&server_, &config), kTag, "httpd_start failed");

  httpd_uri_t root_uri = {};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = &SetupPortal::handle_root;
  root_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &root_uri), kTag, "root handler failed");

  httpd_uri_t favicon_uri = {};
  favicon_uri.uri = "/favicon.ico";
  favicon_uri.method = HTTP_GET;
  favicon_uri.handler = &SetupPortal::handle_favicon;
  favicon_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &favicon_uri), kTag,
                      "favicon handler failed");

  httpd_uri_t health_uri = {};
  health_uri.uri = "/api/health";
  health_uri.method = HTTP_GET;
  health_uri.handler = &SetupPortal::handle_health;
  health_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &health_uri), kTag,
                      "health handler failed");

  httpd_uri_t unlock_uri = {};
  unlock_uri.uri = "/api/unlock";
  unlock_uri.method = HTTP_POST;
  unlock_uri.handler = &SetupPortal::handle_unlock;
  unlock_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &unlock_uri), kTag,
                      "unlock handler failed");

  httpd_uri_t wifi_scan_uri = {};
  wifi_scan_uri.uri = "/api/wifi/scan";
  wifi_scan_uri.method = HTTP_GET;
  wifi_scan_uri.handler = &SetupPortal::handle_wifi_scan;
  wifi_scan_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &wifi_scan_uri), kTag,
                      "wifi scan handler failed");

  httpd_uri_t config_get_uri = {};
  config_get_uri.uri = "/api/config";
  config_get_uri.method = HTTP_GET;
  config_get_uri.handler = &SetupPortal::handle_config_get;
  config_get_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &config_get_uri), kTag,
                      "config get handler failed");

  httpd_uri_t config_post_uri = {};
  config_post_uri.uri = "/api/config";
  config_post_uri.method = HTTP_POST;
  config_post_uri.handler = &SetupPortal::handle_config_post;
  config_post_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &config_post_uri), kTag,
                      "config post handler failed");

  httpd_uri_t arc_preview_uri = {};
  arc_preview_uri.uri = "/api/arc/preview";
  arc_preview_uri.method = HTTP_POST;
  arc_preview_uri.handler = &SetupPortal::handle_arc_preview;
  arc_preview_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &arc_preview_uri), kTag,
                      "arc preview handler failed");

  httpd_uri_t arc_commit_uri = {};
  arc_commit_uri.uri = "/api/arc/commit";
  arc_commit_uri.method = HTTP_POST;
  arc_commit_uri.handler = &SetupPortal::handle_arc_commit;
  arc_commit_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &arc_commit_uri), kTag,
                      "arc commit handler failed");

  httpd_uri_t source_mode_uri = {};
  source_mode_uri.uri = "/api/source-mode";
  source_mode_uri.method = HTTP_POST;
  source_mode_uri.handler = &SetupPortal::handle_source_mode_post;
  source_mode_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &source_mode_uri), kTag,
                      "source mode handler failed");

  httpd_uri_t display_rotation_uri = {};
  display_rotation_uri.uri = "/api/display-rotation";
  display_rotation_uri.method = HTTP_POST;
  display_rotation_uri.handler = &SetupPortal::handle_display_rotation_post;
  display_rotation_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &display_rotation_uri), kTag,
                      "display rotation handler failed");

  httpd_uri_t battery_display_uri = {};
  battery_display_uri.uri = "/api/battery-display";
  battery_display_uri.method = HTTP_POST;
  battery_display_uri.handler = &SetupPortal::handle_battery_display_post;
  battery_display_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &battery_display_uri), kTag,
                      "battery display handler failed");

  httpd_uri_t portal_access_uri = {};
  portal_access_uri.uri = "/api/portal-access";
  portal_access_uri.method = HTTP_POST;
  portal_access_uri.handler = &SetupPortal::handle_portal_access_post;
  portal_access_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &portal_access_uri), kTag,
                      "portal access handler failed");

  httpd_uri_t ams_display_uri = {};
  ams_display_uri.uri = "/api/ams-display";
  ams_display_uri.method = HTTP_POST;
  ams_display_uri.handler = &SetupPortal::handle_ams_display_post;
  ams_display_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ams_display_uri), kTag,
                      "ams display handler failed");

  httpd_uri_t cloud_connect_uri = {};
  cloud_connect_uri.uri = "/api/cloud/connect";
  cloud_connect_uri.method = HTTP_POST;
  cloud_connect_uri.handler = &SetupPortal::handle_cloud_connect;
  cloud_connect_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &cloud_connect_uri), kTag,
                      "cloud connect handler failed");

  httpd_uri_t cloud_verify_uri = {};
  cloud_verify_uri.uri = "/api/cloud/verify";
  cloud_verify_uri.method = HTTP_POST;
  cloud_verify_uri.handler = &SetupPortal::handle_cloud_verify;
  cloud_verify_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &cloud_verify_uri), kTag,
                      "cloud verify handler failed");

  httpd_uri_t local_connect_uri = {};
  local_connect_uri.uri = "/api/local/connect";
  local_connect_uri.method = HTTP_POST;
  local_connect_uri.handler = &SetupPortal::handle_local_connect;
  local_connect_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &local_connect_uri), kTag,
                      "local connect handler failed");

  httpd_uri_t printers_get_uri = {};
  printers_get_uri.uri = "/api/printers";
  printers_get_uri.method = HTTP_GET;
  printers_get_uri.handler = &SetupPortal::handle_printers_get;
  printers_get_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &printers_get_uri), kTag,
                      "printers get handler failed");

  httpd_uri_t printers_select_uri = {};
  printers_select_uri.uri = "/api/printers/select";
  printers_select_uri.method = HTTP_POST;
  printers_select_uri.handler = &SetupPortal::handle_printers_select;
  printers_select_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &printers_select_uri), kTag,
                      "printers select handler failed");

  httpd_uri_t printers_save_uri = {};
  printers_save_uri.uri = "/api/printers/save";
  printers_save_uri.method = HTTP_POST;
  printers_save_uri.handler = &SetupPortal::handle_printers_save;
  printers_save_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &printers_save_uri), kTag,
                      "printers save handler failed");

  httpd_uri_t printers_delete_uri = {};
  printers_delete_uri.uri = "/api/printers/delete";
  printers_delete_uri.method = HTTP_POST;
  printers_delete_uri.handler = &SetupPortal::handle_printers_delete;
  printers_delete_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &printers_delete_uri), kTag,
                      "printers delete handler failed");

  httpd_uri_t printers_clear_local_uri = {};
  printers_clear_local_uri.uri = "/api/printers/clear-local";
  printers_clear_local_uri.method = HTTP_POST;
  printers_clear_local_uri.handler = &SetupPortal::handle_printers_clear_local;
  printers_clear_local_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &printers_clear_local_uri), kTag,
                      "printers clear-local handler failed");

  httpd_uri_t session_extend_uri = {};
  session_extend_uri.uri = "/api/session/extend";
  session_extend_uri.method = HTTP_POST;
  session_extend_uri.handler = &SetupPortal::handle_session_extend;
  session_extend_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &session_extend_uri), kTag,
                      "session extend handler failed");

  httpd_uri_t ota_upload_uri = {};
  ota_upload_uri.uri = "/api/ota/upload";
  ota_upload_uri.method = HTTP_POST;
  ota_upload_uri.handler = &SetupPortal::handle_ota_upload;
  ota_upload_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ota_upload_uri), kTag,
                      "ota upload handler failed");

  httpd_uri_t ota_url_uri = {};
  ota_url_uri.uri = "/api/ota/url";
  ota_url_uri.method = HTTP_POST;
  ota_url_uri.handler = &SetupPortal::handle_ota_url;
  ota_url_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ota_url_uri), kTag,
                      "ota url handler failed");

  httpd_uri_t ota_status_uri = {};
  ota_status_uri.uri = "/api/ota/status";
  ota_status_uri.method = HTTP_GET;
  ota_status_uri.handler = &SetupPortal::handle_ota_status;
  ota_status_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ota_status_uri), kTag,
                      "ota status handler failed");

#ifdef PRINTSPHERE_DEBUG_BUILD
  httpd_uri_t debug_log_uri = {};
  debug_log_uri.uri = "/api/debug/log";
  debug_log_uri.method = HTTP_GET;
  debug_log_uri.handler = &SetupPortal::handle_debug_log;
  debug_log_uri.user_ctx = this;
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &debug_log_uri), kTag,
                      "debug log handler failed");
#endif

  ESP_LOGI(kTag, "Setup portal started");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_root(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_unlock_page(request);
  }

  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials cloud = portal->config_store_.load_cloud_credentials();
  const SourceMode source_mode = portal->config_store_.load_source_mode();
  const DisplayRotation display_rotation = portal->config_store_.load_display_rotation();
  const bool portal_lock_enabled = portal->config_store_.load_portal_lock_enabled();
  const bool filament_wake = portal->config_store_.load_filament_wake_enabled();
  const bool filament_anim = portal->config_store_.load_filament_anim_enabled();
  const PrinterProfile active_profile = portal->config_store_.load_active_printer_profile();
  const PrinterConnection printer = active_profile.to_connection();
  const ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const CloudPortalPresentation cloud_portal =
      cloud_portal_presentation(portal->cloud_client_.refreshed_snapshot());
  const BambuCloudSnapshot& cloud_snapshot = cloud_portal.snapshot;
  const PrinterSnapshot local_snapshot = portal->printer_client_.snapshot();
  const auto all_cloud_devices = portal->cloud_client_.get_cloud_devices();
  const auto all_profiles = portal->config_store_.load_printer_profiles();
  const std::string effective_printer_serial = [&]() -> std::string {
    if (!printer.serial.empty()) return printer.serial;
    for (const auto& cd : all_cloud_devices) {
      bool has_local = false;
      for (const auto& p : all_profiles) {
        if (p.serial == cd.serial && p.has_local_config()) { has_local = true; break; }
      }
      if (!has_local) return cd.serial;
    }
    return cloud_snapshot.resolved_serial;
  }();
  const bool wifi_connected = portal->wifi_manager_.is_station_connected();
  const bool setup_ap_active = portal->wifi_manager_.is_setup_access_point_active();
  const bool wifi_configured = wifi.is_configured();
  const bool wifi_password_saved = !wifi.password.empty();
  const bool cloud_password_saved = !cloud.password.empty();
  const bool printer_access_code_saved = !printer.access_code.empty();
  const std::string wifi_ip = portal->wifi_manager_.station_ip();
  const bool local_configured = portal->printer_client_.is_configured();
  const bool show_connection_steps = wifi_connected && !setup_ap_active;
  const std::string wifi_password_placeholder =
      wifi_password_saved ? "Leave empty to keep saved Wi-Fi password" : "Enter Wi-Fi password";
  const std::string cloud_password_placeholder =
      cloud_password_saved ? "Leave empty to keep saved Bambu password" : "Enter Bambu password";
  const std::string printer_access_code_placeholder =
      printer_access_code_saved ? "Leave empty to keep saved access code" : "Enter printer access code";

  const std::string wifi_badge_value =
      wifi_connected ? ("Connected - " + wifi_ip) : "Not connected";
  const char* wifi_badge_class = wifi_connected ? "ok" : "warn";

  const std::string cloud_badge_value = cloud_portal.badge_value;
  const char* cloud_badge_class = cloud_portal.badge_class;

  const std::string source_badge_value = source_mode_badge_value(source_mode);
  std::string local_badge_value = "Not configured";
  const char* local_badge_class = "idle";
  if (local_snapshot.connection == PrinterConnectionState::kOnline) {
    local_badge_value = "Connected";
    local_badge_class = "ok";
  } else if (local_snapshot.connection == PrinterConnectionState::kError) {
    local_badge_value = "Error";
    local_badge_class = "warn";
  } else if (local_configured) {
    local_badge_value = "Configured";
    local_badge_class = "info";
  }
  const bool setup_fully_complete = show_connection_steps &&
      (cloud_snapshot.configured || local_configured) &&
      (!cloud_snapshot.configured || cloud_portal.ready ||
       (!cloud_snapshot.verification_required && !cloud_snapshot.tfa_required &&
        cloud_snapshot.setup_stage != CloudSetupStage::kFailed)) &&
      (!local_configured || local_snapshot.connection != PrinterConnectionState::kError);
  const std::string initial_status_line =
      setup_fully_complete ? std::string{} :
      show_connection_steps
          ? (cloud_portal.ready || cloud_snapshot.verification_required ||
                     cloud_snapshot.setup_stage == CloudSetupStage::kLoggingIn ||
                     cloud_snapshot.setup_stage == CloudSetupStage::kCodeSubmitted
                 ? cloud_portal.status_line
                 : "Connect Bambu Cloud")
          : "Save Wi-Fi";
  const std::string initial_status_detail =
      setup_fully_complete ? std::string{} :
      show_connection_steps
          ? (cloud_portal.ready || cloud_snapshot.verification_required ||
                     cloud_snapshot.setup_stage == CloudSetupStage::kLoggingIn ||
                     cloud_snapshot.setup_stage == CloudSetupStage::kCodeSubmitted
                 ? cloud_portal.status_detail
                 : ("ESP on home network: " + wifi_ip))
          : (!wifi_configured
                 ? "Enter your home Wi-Fi, save it and let the ESP reboot."
                 : "Update Wi-Fi if needed, save, then reopen the portal on the ESP home-network IP.");
  const std::string cloud_code_label = cloud_verify_label(cloud_snapshot);
  const std::string cloud_code_placeholder = cloud_verify_placeholder(cloud_snapshot);
  const std::string cloud_code_note = cloud_verify_note(cloud_snapshot);
  const std::string save_button_label =
      show_connection_steps ? "Save and Restart" : "Save Wi-Fi and Restart";
  const std::string save_button_hint =
      show_connection_steps
          ? "Use this mainly when Wi-Fi changes. Cloud and local connect buttons apply those paths live without a reboot."
          : "This first save writes Wi-Fi to NVS and restarts the ESP.";
  const ArcColorScheme default_arc_colors = {};
  const bool arc_colors_custom =
      arc_colors.printing != default_arc_colors.printing ||
      arc_colors.done != default_arc_colors.done || arc_colors.error != default_arc_colors.error ||
      arc_colors.idle != default_arc_colors.idle ||
      arc_colors.preheat != default_arc_colors.preheat ||
      arc_colors.clean != default_arc_colors.clean || arc_colors.level != default_arc_colors.level ||
      arc_colors.cool != default_arc_colors.cool ||
      arc_colors.idle_active != default_arc_colors.idle_active ||
      arc_colors.filament != default_arc_colors.filament ||
      arc_colors.setup != default_arc_colors.setup ||
      arc_colors.offline != default_arc_colors.offline ||
      arc_colors.unknown != default_arc_colors.unknown;
  const std::string wifi_section_badge_value =
      wifi_connected ? "Connected" : (wifi_configured ? "Saved" : "Needs setup");
  const char* wifi_section_badge_class =
      wifi_connected ? "ok" : (wifi_configured ? "info" : "warn");
  const std::string rotation_section_badge_value = display_rotation_badge_value(display_rotation);
  const std::string portal_lock_badge_value =
      setup_ap_active ? "Open in setup AP"
                      : (portal_lock_enabled ? "PIN lock on" : "Open on LAN");
  const char* portal_lock_badge_class =
      setup_ap_active ? "info" : (portal_lock_enabled ? "info" : "warn");
  const std::string ams_display_badge_value = filament_wake || !filament_anim ? "On" : "Off";
  const char* ams_display_badge_class = filament_wake || !filament_anim ? "info" : "idle";
  const std::string arc_badge_value = arc_colors_custom ? "Custom" : "Default";
  const char* arc_badge_class = arc_colors_custom ? "info" : "idle";
  const int device_settings_count = show_connection_steps ? 5 : 4;
  const std::string device_settings_badge_value =
      std::to_string(device_settings_count) + (device_settings_count == 1 ? " Panel" : " Panels");
  const bool device_settings_section_open = true;
  const bool wifi_section_open = !wifi_configured || !wifi_connected || setup_ap_active;
  const bool rotation_section_open = false;
  const bool energy_section_open = false;
  const bool ams_display_section_open = false;
  const bool connection_mode_section_open = false;
  const bool portal_access_section_open = !portal_lock_enabled && !setup_ap_active;
  const bool cloud_section_configured =
      cloud_snapshot.configured || cloud_portal.ready || cloud_snapshot.verification_required ||
      cloud_snapshot.tfa_required || cloud.has_identity();
  const bool cloud_section_open =
      !cloud_section_configured || cloud_snapshot.verification_required ||
      cloud_snapshot.tfa_required || cloud_snapshot.setup_stage == CloudSetupStage::kFailed;
  const bool local_section_open =
      !local_configured || local_snapshot.connection == PrinterConnectionState::kError;
  const bool has_cloud_without_local = [&]() {
    for (const auto& cd : all_cloud_devices) {
      bool found_local = false;
      for (const auto& p : all_profiles) {
        if (p.serial == cd.serial && p.has_local_config()) { found_local = true; break; }
      }
      if (!found_local) return true;
    }
    return false;
  }();
  const bool local_needs_setup = !local_configured || has_cloud_without_local;
  const bool arc_section_open = false;
  const BatteryDisplayPolicy bat_policy = portal->config_store_.load_battery_display_policy();
  const std::string bat_badge_value =
      bat_policy.dim_enabled || bat_policy.screen_off_enabled ? "Active" : "Disabled";
  const char* bat_badge_class =
      bat_policy.dim_enabled || bat_policy.screen_off_enabled ? "info" : "idle";
  const PowerSnapshot power = portal->pmu_manager_.sample();
  std::string html;

  const auto add_badge = [](std::string* html, const char* id, const std::string& label,
                            const std::string& value, const char* state_class) {
    *html += "<div class=\"badge ";
    *html += state_class;
    *html += "\"";
    if (id != nullptr && id[0] != '\0') {
      *html += " id=\"";
      *html += id;
      *html += "\"";
    }
    *html += "><span class=\"badge-label\">";
    *html += label;
    *html += "</span><span class=\"badge-value\">";
    *html += json_escape(value);
    *html += "</span></div>";
  };
  const auto add_summary_pill = [](std::string* html, const std::string& value,
                                   const char* state_class, const char* id = nullptr) {
    *html += "<span class=\"summary-pill ";
    *html += state_class;
    *html += "\"";
    if (id != nullptr && id[0] != '\0') {
      *html += " id=\"";
      *html += id;
      *html += "\"";
    }
    *html += ">";
    *html += json_escape(value);
    *html += "</span>";
  };
  const auto begin_collapsible_section =
      [&html, &add_summary_pill](const char* title, const char* detail,
                                 const std::string& badge_value, const char* badge_class,
                                 bool open, const char* pill_id = nullptr) {
        html += "<details class=\"section\"";
        if (open) {
          html += " open";
        }
        html += "><summary class=\"section-summary\"><div class=\"section-summary-main section-head\"><h2>";
        html += title;
        html += "</h2><p>";
        html += detail;
        html += "</p></div><div class=\"section-summary-side\">";
        add_summary_pill(&html, badge_value, badge_class, pill_id);
        html += "<span class=\"section-toggle-icon\" aria-hidden=\"true\"></span></div></summary><div class=\"section-body\">";
      };
  const auto end_collapsible_section = [&html]() { html += "</div></details>"; };
  const auto begin_settings_panel =
      [&html, &add_summary_pill](const char* title, const char* detail,
                                 const std::string& badge_value, const char* badge_class,
                                 bool open) {
        html += "<details class=\"settings-panel\"";
        if (open) {
          html += " open";
        }
        html += "><summary class=\"settings-panel-summary\"><div class=\"settings-panel-copy\"><h3>";
        html += title;
        html += "</h3><p>";
        html += detail;
        html += "</p></div><div class=\"settings-panel-side\">";
        add_summary_pill(&html, badge_value, badge_class);
        html += "<span class=\"settings-panel-icon\" aria-hidden=\"true\"></span></div></summary><div class=\"settings-panel-body\">";
      };
  const auto end_settings_panel = [&html]() { html += "</div></details>"; };
  html += "<!doctype html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>PrintSphere Web Config</title>";
  html += "<style>";
  html += ":root{--bg:#0b1015;--panel:#121a23;--panel-2:#172230;--line:#263548;--text:#eef4fb;"
          "--muted:#99a9bb;--accent:#f0a64b;--accent-2:#42c291;--warn:#f5c24f;--danger:#ff6b6b;}";
  html += "*{box-sizing:border-box;}html,body{margin:0;}body{font-family:'Segoe UI',system-ui,"
          "sans-serif;background:radial-gradient(circle at top,#182433 0,#0b1015 55%);color:var(--text);"
          "padding:24px;}main{max-width:1040px;margin:0 auto;display:grid;gap:18px;}";
  html += "h1,h2,h3,p{margin:0;}a{color:inherit;}label{display:block;margin:0 0 8px;font-size:14px;"
          "font-weight:600;color:#d5e1ed;}input,select{width:100%;padding:13px 14px;border-radius:14px;"
          "border:1px solid var(--line);background:#0f1721;color:var(--text);outline:none;}"
          "input:focus,select:focus{border-color:#4d86c7;box-shadow:0 0 0 3px rgba(77,134,199,0.18);}";
  html += "button{border:none;border-radius:999px;padding:13px 18px;font-weight:700;cursor:pointer;}"
          "button:disabled{opacity:.65;cursor:wait;}button.primary{background:linear-gradient(135deg,"
          "#f0a64b,#ffd07a);color:#1b1203;}button.secondary{background:#203044;color:var(--text);"
          "border:1px solid #34506e;}";
  html += ".hero,.section,.footer-card{background:rgba(18,26,35,0.94);border:1px solid var(--line);"
          "border-radius:24px;box-shadow:0 14px 32px rgba(0,0,0,.2);} .hero{padding:24px;display:grid;gap:18px;}"
          ".section{padding:0;overflow:hidden;} .section-body{padding:0 22px 22px;display:grid;gap:16px;}"
          ".footer-card{padding:18px;display:grid;gap:8px;} .stack{display:grid;gap:18px;}";
  html += ".hero-top{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap;} .hero-brand{display:flex;align-items:center;gap:12px;flex-wrap:wrap;}"
          " .eyebrow{font-size:18px;font-weight:800;letter-spacing:.12em;text-transform:uppercase;"
          "color:#b9d4ef;} .hero-version{display:inline-flex;align-items:center;justify-content:center;padding:6px 12px;"
          "border-radius:999px;border:1px solid #31557d;background:#111b29;color:#dceafa;font-size:12px;font-weight:700;"
          "letter-spacing:.08em;text-transform:uppercase;text-decoration:none;transition:border-color .18s ease,background .18s ease,color .18s ease;}"
          " .hero-version:hover{border-color:#4d86c7;background:#142235;color:#f8fbff;} .title{font-size:34px;line-height:1.05;} .subtitle{max-width:720px;color:var(--muted);"
          "line-height:1.55;} .section-head p,.hint,.micro{color:var(--muted);"
          "line-height:1.5;} .micro{font-size:13px;} .section-summary{display:grid;grid-template-columns:1fr auto;"
          "gap:6px 14px;align-items:center;padding:22px;cursor:pointer;list-style:none;} .section-summary::-webkit-details-marker{display:none;}"
          ".section-summary-main{display:contents;}"
          " .section-summary h2{grid-column:1;grid-row:1;margin:0;}"
          " .section-summary p{grid-column:1/-1;grid-row:2;margin:0;}"
          " .section-summary-side{grid-column:2;grid-row:1;display:flex;align-items:center;gap:10px;}"
          ".summary-pill{display:inline-flex;align-items:center;justify-content:center;padding:8px 12px;border-radius:999px;"
          "border:1px solid var(--line);font-size:12px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;"
          "background:#0f1721;color:var(--text);white-space:nowrap;} .summary-pill.ok{border-color:#22614c;background:#10231d;}"
          ".summary-pill.warn{border-color:#7d6222;background:#241c0e;} .summary-pill.info{border-color:#31557d;background:#111b29;}"
          ".summary-pill.idle{border-color:#334456;background:#121a23;} .section-toggle-icon{width:32px;height:32px;border-radius:999px;"
          "border:1px solid #365064;background:#0f1721;display:grid;place-items:center;flex:0 0 auto;} .section-toggle-icon::before{content:'+';"
          "font-size:20px;line-height:1;color:#cfe0f1;} details[open]>.section-summary .section-toggle-icon::before{content:'-';}"
          ".hint-box{padding:14px 16px;border-radius:18px;background:#0e1620;"
          "border:1px solid #2b3e54;color:var(--muted);} .hint-box strong{color:var(--text);}";
  html += ".badge-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;}"
          ".badge{padding:14px 16px;border-radius:18px;border:1px solid var(--line);background:#0f1721;display:grid;gap:6px;}"
          ".badge-label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#91a6bc;}"
          ".badge-value{font-size:15px;font-weight:700;line-height:1.35;color:var(--text);}"
          ".badge.ok{border-color:#22614c;background:#10231d;} .badge.warn{border-color:#7d6222;background:#241c0e;}"
          ".badge.info{border-color:#31557d;background:#111b29;} .badge.idle{border-color:#334456;background:#121a23;}";
  html += ".settings-accordion{display:grid;gap:12px;} .settings-panel{border:1px solid var(--line);border-radius:20px;"
          "background:#0f1721;overflow:hidden;} .settings-panel[open]{border-color:#365064;background:#111b29;}"
          ".settings-panel-summary{display:grid;grid-template-columns:1fr auto;gap:10px 14px;align-items:start;"
          "padding:16px 18px;cursor:pointer;list-style:none;} .settings-panel-summary::-webkit-details-marker{display:none;}"
          ".settings-panel-copy{display:grid;gap:4px;} .settings-panel-copy h3{font-size:17px;line-height:1.2;}"
          ".settings-panel-copy p{font-size:13px;line-height:1.45;color:var(--muted);} .settings-panel-side{display:flex;"
          "align-items:center;gap:10px;padding-top:2px;} .settings-panel-icon{width:28px;height:28px;border-radius:999px;border:1px solid #365064;"
          "background:#0c131b;display:grid;place-items:center;flex:0 0 auto;} .settings-panel-icon::before{content:'+';"
          "font-size:18px;line-height:1;color:#cfe0f1;} details[open]>.settings-panel-summary .settings-panel-icon::before{content:'-';}"
          ".settings-panel-body{padding:0 18px 18px;display:grid;gap:14px;}";
  html += ".grid-2{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,460px),1fr));gap:14px;}"
          ".grid-3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;}"
          ".field{display:grid;gap:8px;} .actions{display:flex;flex-wrap:wrap;gap:12px;align-items:center;}"
          ".actions .micro{min-width:220px;} .color-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;}"
          ".color-grid input{min-height:54px;padding:6px 10px;background:#0c131b;}";
  html += ".status-line{font-size:16px;font-weight:700;} .status-detail{color:var(--muted);}"
          ".hidden{display:none !important;}";
  html += ".printer-card{padding:16px 18px;border-radius:18px;border:1px solid var(--line);background:#0f1721;"
          "display:grid;gap:10px;margin-bottom:12px;transition:border-color .2s,box-shadow .2s;}"
          ".printer-card.active{border-color:#22614c;box-shadow:0 0 12px rgba(66,194,145,.15);}"
          ".printer-card.cloud-only{border-style:dashed;}"
          ".printer-name-input{background:transparent;border:1px solid var(--line);border-radius:8px;"
          "color:#fff;font-weight:700;font-size:1em;padding:4px 8px;flex:1;min-width:0;}"
          ".printer-name-input:focus{outline:none;border-color:#42c291;}"
          "#portal-timer{display:inline-flex;align-items:center;justify-content:center;padding:6px 12px;"
          "border-radius:999px;border:1px solid #7d6222;background:#241c0e;color:#f0a64b;font-size:12px;font-weight:700;"
          "letter-spacing:.08em;white-space:nowrap;cursor:pointer;transition:border-color .18s ease,background .18s ease;}"
          "#portal-timer:hover{border-color:#f0a64b;background:#352a12;}"
          ".printer-card-header{display:flex;align-items:center;justify-content:space-between;gap:10px;}"
          ".printer-card-body{display:flex;flex-wrap:wrap;gap:8px;}"
          ".printer-tag{display:inline-flex;padding:4px 10px;border-radius:999px;border:1px solid var(--line);"
          "font-size:12px;font-weight:600;color:var(--muted);background:#121a23;}"
          ".printer-tag.ok{border-color:#22614c;color:#42c291;}"
          ".printer-card-actions{display:flex;gap:8px;flex-wrap:wrap;}";
  html += "@media(max-width:840px){.badge-grid,.color-grid{grid-template-columns:repeat(2,minmax(0,1fr));}}";
  html += "@media(max-width:640px){body{padding:16px;} .title{font-size:28px;} .badge-grid,.grid-2,.grid-3,.color-grid"
          "{grid-template-columns:1fr;} .hero,.footer-card{padding:18px;} .hero-brand{gap:10px;} .eyebrow{font-size:16px;}"
          ".section-summary{padding:18px;flex-direction:column;}"
          ".section-summary-side,.settings-panel-side{width:100%;justify-content:space-between;} .section-body{padding:0 18px 18px;}"
          ".settings-panel-summary{padding:16px;} .settings-panel-body{padding:0 16px 16px;}}";
  html += "</style></head><body><main>";

  const auto add_color_field = [&html](const char* id, const char* label, uint32_t color) {
    html += "<div class=\"field\"><label for=\"";
    html += id;
    html += "\">";
    html += label;
    html += "</label><input type=\"color\" id=\"";
    html += id;
    html += "\" value=\"";
    html += color_to_html_hex(color);
    html += "\"></div>";
  };

  html += "<section class=\"hero\">";
  html += "<div class=\"hero-top\"><div class=\"hero-brand\"><div class=\"eyebrow\">PrintSphere</div><a class=\"hero-version\" href=\"https://github.com/cptkirki/PrintSphere\" target=\"_blank\" rel=\"noopener noreferrer\">";
  html += json_escape(kPortalReleaseVersion);
  html += "</a></div><div id=\"portal-timer\" style=\"display:none\" title=\"Click to extend session by 5 minutes\"></div></div>";
  html += "<h1 class=\"title\">Web Config</h1>";
  html += "<p class=\"subtitle\">";
  html += show_connection_steps
              ? "The ESP is on your home network. You can manage cloud access, local printer access and UI tuning below."
              : "In setup AP mode this page only asks for your home Wi-Fi. Save it, let the ESP reboot, then reopen the portal on the home-network IP.";
  html += "</p></div>";
  html += "<div class=\"badge-grid\">";
  add_badge(&html, "wifi-badge", "Wi-Fi", wifi_badge_value, wifi_badge_class);
  if (show_connection_steps) {
    add_badge(&html, "cloud-badge", "Cloud", cloud_badge_value, cloud_badge_class);
    add_badge(&html, "source-badge", "Source", source_badge_value, "info");
    add_badge(&html, "local-badge", "Local Path", local_badge_value, local_badge_class);
  }
  html += "</div>";
  html += "<div class=\"hint-box\"><strong>Note:</strong> ";
  html += show_connection_steps
              ? "Wi-Fi is up. Review or update cloud, local printer access and UI settings below."
              : "Only the Wi-Fi step is available while the ESP is running its setup access point.";
  html += "</div>";
  html += "</section>";

  // --- Reorderable provisioning section renderers ---
  const auto render_wifi_section = [&]() {
    begin_collapsible_section(
        "Wi-Fi",
        "This network is used for cloud access, local printer access and the setup portal after first boot.",
        wifi_section_badge_value, wifi_section_badge_class, wifi_section_open);
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"wifi-scan-button\">Scan Networks</button>";
    html += "<div class=\"micro\" id=\"wifi-scan-detail\">Scan for visible networks or type an SSID manually.</div></div>";
    html += "<div class=\"field\" id=\"wifi-ssid-manual-field\"><label for=\"wifi_ssid\">Wi-Fi SSID</label><input id=\"wifi_ssid\" value=\"";
    html += json_escape(wifi.ssid);
    html += "\" autocomplete=\"off\"></div>";
    html += "<div class=\"field\" id=\"wifi-ssid-scan-field\" style=\"display:none\"><label for=\"wifi_ssid_select\">Detected Networks</label><select id=\"wifi_ssid_select\"><option value=\"\">Select detected Wi-Fi...</option></select></div>";
    html += "<div class=\"field\"><label for=\"wifi_password\">Wi-Fi Password</label><input id=\"wifi_password\" type=\"password\" value=\"\" placeholder=\"";
    html += json_escape(wifi_password_placeholder);
    html += "\" autocomplete=\"off\"></div>";
    html += "<p class=\"micro\">Current network status: <span id=\"wifi-detail\">";
    html += json_escape(wifi_badge_value);
    html += "</span></p>";
    end_collapsible_section();
  };
  const auto render_cloud_section = [&]() {
    begin_collapsible_section(
        "Bambu Cloud",
        "Primary source for cloud monitoring, cover image, project metadata and cloud lifecycle. Use Connect to start the login immediately. If Bambu asks for a verification code or 2FA code, you can complete that step here.",
        cloud_badge_value, cloud_badge_class, cloud_section_open, "cloud-section-pill");
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"cloud_email\">Bambu Email / Phone</label><input id=\"cloud_email\" value=\"";
    html += json_escape(cloud.email);
    html += "\" autocomplete=\"username\"></div>";
    html += "<div class=\"field\"><label for=\"cloud_password\">Bambu Password</label><input id=\"cloud_password\" type=\"password\" value=\"\" placeholder=\"";
    html += json_escape(cloud_password_placeholder);
    html += "\" autocomplete=\"current-password\"></div>";
    html += "</div>";
    html += "<div class=\"field\"><label for=\"cloud_region\">Cloud Region</label><select id=\"cloud_region\">";
    html += "<option value=\"us\"";
    if (cloud.region == CloudRegion::kUS) {
      html += " selected";
    }
    html += ">US</option>";
    html += "<option value=\"eu\"";
    if (cloud.region == CloudRegion::kEU) {
      html += " selected";
    }
    html += ">EU</option>";
    html += "<option value=\"cn\"";
    if (cloud.region == CloudRegion::kCN) {
      html += " selected";
    }
    html += ">CN</option></select></div>";
    html += "<div class=\"hint-box\"><strong>Printer Status:</strong> <span id=\"cloud-detail\">";
    html += json_escape(cloud_snapshot.detail);
    html += "</span></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"cloud-connect-button\">Connect Cloud</button>";
    html += "<div class=\"micro\">This saves the cloud credentials immediately. In Hybrid and Cloud only it also starts the login without rebooting.</div></div>";
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"cloud_verification_code\" id=\"cloud-verification-label\">";
    html += json_escape(cloud_code_label);
    html += "</label><input id=\"cloud_verification_code\" value=\"\" autocomplete=\"one-time-code\" placeholder=\"";
    html += json_escape(cloud_code_placeholder);
    html += "\"></div>";
    html += "<div class=\"field\"><label>&nbsp;</label><button type=\"button\" class=\"secondary\" id=\"verify-button\">Submit Code</button></div>";
    html += "</div>";
    html += "<p class=\"micro";
    if (!cloud_snapshot.verification_required) {
      html += " hidden";
    }
    html += "\" id=\"cloud-verify-note\">";
    html += json_escape(cloud_code_note);
    html += "</p>";
    end_collapsible_section();
  };
  const auto render_local_section = [&]() {
    begin_collapsible_section(
        "Local Printer Path",
        "The local MQTT path provides live status, layers, temperatures and also powers camera snapshots on page 3.",
        local_badge_value, local_badge_class, local_section_open, "local-section-pill");
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"printer_host\">Printer IP or Hostname</label><input id=\"printer_host\" value=\"";
    html += json_escape(printer.host);
    html += "\" autocomplete=\"off\"></div>";
    html += "<div class=\"field\"><label for=\"printer_serial\">Printer Serial Number</label><input id=\"printer_serial\" value=\"";
    html += json_escape(effective_printer_serial);
    html += "\" autocomplete=\"off\"></div>";
    html += "</div>";
    html += "<div class=\"field\"><label for=\"printer_access_code\">Access Code</label><input id=\"printer_access_code\" type=\"password\" value=\"\" placeholder=\"";
    html += json_escape(printer_access_code_placeholder);
    html += "\" autocomplete=\"off\"></div>";
    html += "<div class=\"hint-box\"><strong>Local Status:</strong> <span id=\"local-detail\">";
    html += json_escape(local_snapshot.detail);
    html += "</span></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"local-connect-button\">Connect Local</button>";
    html += "<div class=\"micro\">This saves the local printer credentials immediately. In Hybrid and Local only it also reconnects MQTT and camera without rebooting.</div></div>";
    html += "<p class=\"micro\">In Hybrid mode PrintSphere auto-picks the better status path for your printer and still uses the local camera when available.</p>";
    end_collapsible_section();
  };

  html += "<form id=\"config-form\" class=\"stack\">";

  // Unconfigured provisioning sections at top
  if (!wifi_configured) {
    render_wifi_section();
  }
  if (show_connection_steps && !cloud_section_configured) {
    render_cloud_section();
  }
  if (show_connection_steps && local_needs_setup) {
    render_local_section();
  }

  if (wifi_configured) {
    begin_collapsible_section(
        "Device Settings",
        "Grouped into a vertical accordion so the restart-based device options stay compact on phones and easy to scan on desktop.",
        device_settings_badge_value, "info", device_settings_section_open);
    html += "<div class=\"settings-accordion\">";
    begin_settings_panel(
        "Screen Rotation",
        "Hardware panel orientation and touch alignment. This change applies after restart.",
        rotation_section_badge_value, "info", rotation_section_open);
    html += "<div class=\"field\"><label for=\"display_rotation\">Screen Rotation</label><select id=\"display_rotation\">";
    html += "<option value=\"0\"";
    if (display_rotation == DisplayRotation::k0) {
      html += " selected";
    }
    html += ">0 deg (default)</option>";
    html += "<option value=\"90\"";
    if (display_rotation == DisplayRotation::k90) {
      html += " selected";
    }
    html += ">90 deg</option>";
    html += "<option value=\"180\"";
    if (display_rotation == DisplayRotation::k180) {
      html += " selected";
    }
    html += ">180 deg</option>";
    html += "<option value=\"270\"";
    if (display_rotation == DisplayRotation::k270) {
      html += " selected";
    }
    html += ">270 deg</option></select></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"primary hidden\" id=\"display-rotation-apply-button\">Apply + Restart</button>";
    html += "<div class=\"micro hidden\" id=\"display-rotation-apply-hint\">The new panel orientation is applied on the next boot so display and touch stay in sync.</div></div>";
    html += "<p class=\"micro\">Current orientation: ";
    html += json_escape(display_rotation_badge_value(display_rotation));
    html += "</p>";
    end_settings_panel();

    begin_settings_panel(
        "Energy",
        "Display dim/off behaviour for battery and optionally USB power. Touch wake stays active.",
        bat_badge_value, bat_badge_class, energy_section_open);

  // Dim on/off
  html += "<div class=\"field\"><label for=\"bat_dim_enabled\">Dim Display on Battery</label><select id=\"bat_dim_enabled\">";
  html += "<option value=\"true\"";
  if (bat_policy.dim_enabled) html += " selected";
  html += ">Yes: reduce brightness after idle timeout</option>";
  html += "<option value=\"false\"";
  if (!bat_policy.dim_enabled) html += " selected";
  html += ">No: keep display at full brightness</option></select></div>";

  // Dim options group (brightness + timeouts), hidden when dim disabled
  html += "<div id=\"bat-dim-options-group\"";
  if (!bat_policy.dim_enabled) html += " style=\"display:none\"";
  html += ">";
  html += "<div class=\"field\"><label for=\"bat_dim_brightness\">Dim Brightness (%)</label><select id=\"bat_dim_brightness\">";
  html += "<option value=\"0\"";
  if (bat_policy.dim_brightness_percent == 0) html += " selected";
  html += ">Auto (~33% of current brightness)</option>";
  {
    const int steps[] = {5, 10, 15, 20, 25, 30, 40, 50};
    for (int step : steps) {
      html += "<option value=\"";
      html += std::to_string(step);
      html += "\"";
      if (bat_policy.dim_brightness_percent == step) html += " selected";
      html += ">";
      html += std::to_string(step);
      html += "%</option>";
    }
  }
  html += "</select></div>";
  html += "<div class=\"field\"><label for=\"bat_dim_timeout_idle\">Dim after (idle)</label><select id=\"bat_dim_timeout_idle\">";
  {
    const uint32_t vals[] = {5, 10, 15, 20, 30, 45, 60};
    for (uint32_t v : vals) {
      html += "<option value=\"";
      html += std::to_string(v);
      html += "\"";
      if (bat_policy.dim_timeout_idle_s == v) html += " selected";
      html += ">";
      html += (v < 60 ? std::to_string(v) + " s" : std::to_string(v / 60) + " min");
      html += "</option>";
    }
  }
  html += "</select></div>";
  html += "<div class=\"field\"><label for=\"bat_dim_timeout_active\">Dim after (during print)</label><select id=\"bat_dim_timeout_active\">";
  {
    const uint32_t vals[] = {15, 20, 30, 45, 60, 90};
    for (uint32_t v : vals) {
      html += "<option value=\"";
      html += std::to_string(v);
      html += "\"";
      if (bat_policy.dim_timeout_active_s == v) html += " selected";
      html += ">";
      const uint32_t mins = v / 60, secs = v % 60;
      if (v < 60) html += std::to_string(v) + " s";
      else if (secs) html += std::to_string(mins) + " min " + std::to_string(secs) + " s";
      else html += std::to_string(mins) + " min";
      html += "</option>";
    }
  }
  html += "</select></div>";
  html += "</div>"; // bat-dim-options-group

  // Screen off on/off
  html += "<div class=\"field\"><label for=\"bat_off_enabled\">Turn Off Display on Battery</label><select id=\"bat_off_enabled\">";
  html += "<option value=\"true\"";
  if (bat_policy.screen_off_enabled) html += " selected";
  html += ">Yes: turn off display after extended idle time</option>";
  html += "<option value=\"false\"";
  if (!bat_policy.screen_off_enabled) html += " selected";
  html += ">No: keep display on (may dim if enabled above)</option></select></div>";

  // Off options group (timeouts), hidden when off disabled
  html += "<div id=\"bat-off-options-group\"";
  if (!bat_policy.screen_off_enabled) html += " style=\"display:none\"";
  html += ">";
  html += "<div class=\"field\"><label for=\"bat_off_timeout_idle\">Screen-off after (idle)</label><select id=\"bat_off_timeout_idle\">";
  {
    const uint32_t vals[] = {30, 45, 60, 90, 120, 180, 300};
    for (uint32_t v : vals) {
      html += "<option value=\"";
      html += std::to_string(v);
      html += "\"";
      if (bat_policy.off_timeout_idle_s == v) html += " selected";
      html += ">";
      const uint32_t mins = v / 60, secs = v % 60;
      if (v < 60) html += std::to_string(v) + " s";
      else if (secs) html += std::to_string(mins) + " min " + std::to_string(secs) + " s";
      else html += std::to_string(mins) + " min";
      html += "</option>";
    }
  }
  html += "</select></div>";
  html += "<div class=\"field\"><label for=\"bat_off_timeout_active\">Screen-off after (during print)</label><select id=\"bat_off_timeout_active\">";
  {
    const uint32_t vals[] = {60, 90, 120, 180, 300, 600};
    for (uint32_t v : vals) {
      html += "<option value=\"";
      html += std::to_string(v);
      html += "\"";
      if (bat_policy.off_timeout_active_s == v) html += " selected";
      html += ">";
      const uint32_t mins = v / 60;
      const uint32_t secs = v % 60;
      if (secs) {
        html += std::to_string(mins) + " min " + std::to_string(secs) + " s";
      } else {
        html += std::to_string(mins) + " min";
      }
      html += "</option>";
    }
  }
  html += "</select></div>";
  html += "</div>"; // bat-off-options-group

  // USB power save
  html += "<div class=\"field\"><label for=\"bat_usb_ps\">Also apply when USB-powered</label>"
          "<select id=\"bat_usb_ps\">";
  html += "<option value=\"true\"";
  if (bat_policy.usb_power_save_enabled) html += " selected";
  html += ">Yes: dim and turn off display on USB too</option>";
  html += "<option value=\"false\"";
  if (!bat_policy.usb_power_save_enabled) html += " selected";
  html += ">No: keep display always on when USB-powered</option></select></div>";

  // Live battery info (if PMU available)
  if (power.available && power.battery_present) {
    char bat_info[80] = {};
    std::snprintf(bat_info, sizeof(bat_info), "Battery: %u%% \xC2\xB7 %s",
                  static_cast<unsigned>(power.battery_percent),
                  power.charging ? "Charging" : (power.usb_present ? "USB (not charging)" : "On battery"));
    html += "<p class=\"micro\">";
    html += bat_info;
    if (power.temperature_c > 0.0f) {
      char tmp[24] = {};
      std::snprintf(tmp, sizeof(tmp), " \xC2\xB7 %.1f\xC2\xB0" "C", power.temperature_c);
      html += tmp;
    }
    html += "</p>";
  } else if (power.available && !power.battery_present && power.usb_present) {
    html += "<p class=\"micro\">USB powered \xC2\xB7 No battery detected</p>";
  }

  html += "<div class=\"actions\"><button type=\"button\" class=\"primary hidden\" id=\"bat-display-apply-button\">Apply + Restart</button>";
  html += "<div class=\"micro hidden\" id=\"bat-display-apply-hint\">Energy settings apply after the ESP restarts.</div></div>";
    end_settings_panel();

    begin_settings_panel(
        "AMS Display",
        "Wake behaviour and animation policy for automatic filament changes.",
        ams_display_badge_value, ams_display_badge_class, ams_display_section_open);
    html += "<div class=\"field\"><label for=\"filament_wake\">Filament Change Wake</label><select id=\"filament_wake\">";
    html += "<option value=\"true\"";
    if (filament_wake) {
      html += " selected";
    }
    html += ">Enabled: allow screen to sleep during AMS changes, wake for external spool</option>";
    html += "<option value=\"false\"";
    if (!filament_wake) {
      html += " selected";
    }
    html += ">Disabled: screen stays on during all filament changes while printing</option></select></div>";
    html += "<div class=\"field\"><label for=\"filament_anim\">Filament Change Animation</label><select id=\"filament_anim\">";
    html += "<option value=\"true\"";
    if (filament_anim) {
      html += " selected";
    }
    html += ">Enabled: show loading / unloading arc animation</option>";
    html += "<option value=\"false\"";
    if (!filament_anim) {
      html += " selected";
    }
    html += ">Disabled: show 'printing' during automatic AMS filament changes</option></select></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"primary hidden\" id=\"ams-display-apply-button\">Apply + Restart</button>";
    html += "<div class=\"micro hidden\" id=\"ams-display-apply-hint\">AMS display settings apply after the ESP restarts.</div></div>";
    end_settings_panel();

    if (show_connection_steps) {
      begin_settings_panel(
          "Connection Mode",
          "Choose which runtime path drives the UI. This rewires active clients after restart.",
          source_badge_value, "info", connection_mode_section_open);
      html += "<div class=\"field\"><label for=\"source_mode\">Connection Mode</label><select id=\"source_mode\">";
      html += "<option value=\"hybrid\"";
      if (source_mode == SourceMode::kHybrid) {
        html += " selected";
      }
      html += ">Hybrid (recommended): auto-pick the best status path for your printer, keep local camera when available</option>";
      html += "<option value=\"cloud_only\"";
      if (source_mode == SourceMode::kCloudOnly) {
        html += " selected";
      }
      html += ">Cloud only: cloud monitoring and preview, local MQTT/camera disabled</option>";
      html += "<option value=\"local_only\"";
      if (source_mode == SourceMode::kLocalOnly) {
        html += " selected";
      }
      html += ">Local only</option></select></div>";
      html += "<div class=\"actions\"><button type=\"button\" class=\"primary hidden\" id=\"source-mode-apply-button\">Apply + Restart</button>";
      html += "<div class=\"micro hidden\" id=\"source-mode-apply-hint\">A mode change rewires the active clients, so the ESP restarts right away after saving it.</div></div>";
      end_settings_panel();
    }

    begin_settings_panel(
        "Portal Access",
        "PIN protection for the web portal on your home network after provisioning.",
        portal_lock_badge_value, portal_lock_badge_class, portal_access_section_open);
    html += "<div class=\"field\"><label for=\"portal_lock_enabled\">Portal Lock</label><select id=\"portal_lock_enabled\">";
    html += "<option value=\"true\"";
    if (portal_lock_enabled) {
      html += " selected";
    }
    html += ">Enabled: require PIN unlock on the home network</option>";
    html += "<option value=\"false\"";
    if (!portal_lock_enabled) {
      html += " selected";
    }
    html += ">Disabled: keep the portal open on the home network</option></select></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"primary hidden\" id=\"portal-access-apply-button\">Apply + Restart</button>";
    html += "<div class=\"micro hidden\" id=\"portal-access-apply-hint\">Portal access changes apply after the ESP restarts so the new lock mode is active immediately.</div></div>";
    html += "<div class=\"hint-box\"><strong>Security:</strong> ";
    html += setup_ap_active
                ? "The portal always stays open while the setup access point is active."
                : (portal_lock_enabled
                       ? "Long-press on the device display to show a temporary 6-digit unlock PIN whenever you need browser access."
                       : "With the PIN lock disabled, anyone on the same home network can open Web Config without the device-generated PIN.");
    html += "</div>";
    html += "<p class=\"micro\">The lock never applies while PrintSphere is still in setup AP mode.</p>";
    end_settings_panel();
    html += "</div>";
    end_collapsible_section();
  } // wifi_configured (Device Settings)

  // --- Printer Selection section ---
  if (show_connection_steps) {
    const auto profiles = portal->config_store_.load_printer_profiles();
    const uint8_t active_idx = portal->config_store_.load_active_printer_index();
    const auto cloud_devices = portal->cloud_client_.get_cloud_devices();
    const std::string printer_count_str = std::to_string(profiles.size());
    const char* printer_badge_class = profiles.empty() ? "idle" : "ok";
    begin_collapsible_section(
        "Printer Selection",
        "Switch between configured printers without rebooting. Cloud printers are automatically matched with local profiles by serial number.",
        printer_count_str + (profiles.size() == 1 ? " Printer" : " Printers"),
        printer_badge_class, false);
    html += "<div id=\"printer-list\" data-initial-count=\"";
    html += std::to_string(profiles.size() + cloud_devices.size());
    html += "\">";
    // Render saved profiles
    for (const auto& p : profiles) {
      const bool is_active = (p.index == active_idx);
      const bool has_cloud = p.cloud_bound;
      html += "<div class=\"printer-card";
      if (is_active) html += " active";
      html += "\" data-index=\"";
      html += std::to_string(p.index);
      html += "\" data-serial=\"";
      html += json_escape(p.serial);
      html += "\">";
      html += "<div class=\"printer-card-header\">";
      html += "<input type=\"text\" class=\"printer-name-input\" data-index=\"";
      html += std::to_string(p.index);
      html += "\" data-serial=\"";
      html += json_escape(p.serial);
      html += "\" value=\"";
      const std::string card_name = p.display_name.empty()
          ? (!p.model.empty() ? p.model : ("Printer " + std::to_string(p.index + 1)))
          : p.display_name;
      html += json_escape(card_name);
      html += "\" placeholder=\"Profile name\">";
      html += "<span class=\"summary-pill ";
      html += is_active ? "ok" : "idle";
      html += "\">";
      html += is_active ? "Active" : "Idle";
      html += "</span></div>";
      html += "<div class=\"printer-card-body\">";
      if (!p.model.empty()) {
        html += "<span class=\"printer-tag\">";
        html += json_escape(p.model);
        html += "</span>";
      }
      if (p.has_local_config()) {
        html += "<span class=\"printer-tag\">Local: ";
        html += json_escape(p.host);
        html += "</span>";
      }
      if (has_cloud) {
        html += "<span class=\"printer-tag ok\">Cloud</span>";
      }
      if (!p.serial.empty()) {
        html += "<span class=\"printer-tag\">";
        html += json_escape(p.serial.substr(0, 6) + "...");
        html += "</span>";
      }
      html += "</div>";
      html += "<div class=\"printer-card-actions\">";
      if (!is_active) {
        html += "<button type=\"button\" class=\"secondary printer-select-btn\" data-index=\"";
        html += std::to_string(p.index);
        html += "\">Select</button>";
      }
      if (p.has_local_config()) {
        html += "<button type=\"button\" class=\"secondary printer-clear-local-btn\" data-index=\"";
        html += std::to_string(p.index);
        html += "\">Clear Local</button>";
      }
      html += "<button type=\"button\" class=\"secondary printer-delete-btn\" data-index=\"";
      html += std::to_string(p.index);
      html += "\">Delete</button></div></div>";
    }
    // Show cloud-only devices that don't have a local profile yet
    for (const auto& cd : cloud_devices) {
      bool already_saved = false;
      for (const auto& p : profiles) {
        if (p.serial == cd.serial) { already_saved = true; break; }
      }
      if (already_saved) continue;
      html += "<div class=\"printer-card cloud-only\" data-serial=\"";
      html += json_escape(cd.serial);
      html += "\">";
      html += "<div class=\"printer-card-header\"><strong>";
      html += json_escape(cd.display_name.empty() ? to_string(cd.model) : cd.display_name);
      html += "</strong><span class=\"summary-pill info\">Cloud only</span></div>";
      html += "<div class=\"printer-card-body\">";
      html += "<span class=\"printer-tag\">";
      html += to_string(cd.model);
      html += "</span>";
      html += "<span class=\"printer-tag\">";
      html += cd.online ? "Online" : "Offline";
      html += "</span>";
      html += "<span class=\"printer-tag\">";
      html += json_escape(cd.serial.substr(0, 6) + "...");
      html += "</span></div>";
      html += "<div class=\"printer-card-actions\">";
      html += "<button type=\"button\" class=\"secondary printer-add-cloud-btn\" data-serial=\"";
      html += json_escape(cd.serial);
      html += "\" data-name=\"";
      html += json_escape(cd.display_name);
      html += "\" data-model=\"";
      html += to_string(cd.model);
      html += "\">Add to Profiles</button></div></div>";
    }
    html += "</div>";
    if (profiles.empty() && cloud_devices.empty()) {
      html += "<div class=\"hint-box\" id=\"printer-empty-hint\"><strong>No printers configured yet.</strong> ";
      if (cloud.is_configured()) {
        html += "Waiting for Bambu Cloud binding list to finish loading — this page refreshes automatically once your printers appear.";
      } else {
        html += "Use Cloud Login below or enter a local printer (host, serial, access code) to add your first printer.";
      }
      html += "</div>";
    }
    if (cloud.is_configured()) {
      html += "<div style=\"margin-top:12px\"><button type=\"button\" class=\"secondary\" id=\"load-cloud-printers-btn\">Load Cloud Printers</button>"
              "<span id=\"cloud-printers-status\" style=\"margin-left:10px;font-size:0.85em;color:#888\"></span></div>";
    }
    html += "<div class=\"hint-box\"><strong>Tip:</strong> Selecting a printer reconnects all paths (cloud, local MQTT, camera) live — no reboot needed.</div>";
    end_collapsible_section();
  }

  // Configured provisioning sections (sunk to bottom)
  if (show_connection_steps && cloud_section_configured) {
    render_cloud_section();
  }
  if (show_connection_steps && !local_needs_setup) {
    render_local_section();
  }
  if (wifi_configured) {
    render_wifi_section();
  }

  if (show_connection_steps) {
    begin_collapsible_section(
        "Arc Colors",
        "These groups control ring colors, pulsing states and status colors in the UI. The values map directly to your native PrintSphere interface.",
        arc_badge_value, arc_badge_class, arc_section_open);
    html += "<p class=\"micro\">Color changes preview live immediately and are saved automatically. No restart is needed for color tuning.</p>";
    html += "<div class=\"color-grid\">";
    add_color_field("arc_printing", "Printing", arc_colors.printing);
    add_color_field("arc_done", "Done", arc_colors.done);
    add_color_field("arc_error", "Error", arc_colors.error);
    add_color_field("arc_idle", "Idle", arc_colors.idle);
    add_color_field("arc_preheat", "Preheat", arc_colors.preheat);
    add_color_field("arc_clean", "Clean", arc_colors.clean);
    add_color_field("arc_level", "Level", arc_colors.level);
    add_color_field("arc_cool", "Cool", arc_colors.cool);
    add_color_field("arc_idle_active", "Idle Active", arc_colors.idle_active);
    add_color_field("arc_filament", "Filament", arc_colors.filament);
    add_color_field("arc_setup", "Setup", arc_colors.setup);
    add_color_field("arc_offline", "Offline", arc_colors.offline);
    add_color_field("arc_unknown", "Fallback", arc_colors.unknown);
    html += "</div>";
    end_collapsible_section();
  }

  // --- Firmware Update (OTA) section ---
  if (show_connection_steps) {
    begin_collapsible_section(
        "Firmware Update",
        "Upload a compiled PrintSphere .bin firmware image to update the device over-the-air without a USB connection.",
        "OTA", "idle", false);
    html += "<div class=\"hint-box\"><strong>Warning:</strong> The device restarts immediately after a successful flash. Only upload firmware built for PrintSphere (ESP32-S3).</div>";
    html += "<div class=\"field\"><label for=\"ota_file\">Firmware .bin file</label>";
    html += "<input type=\"file\" id=\"ota_file\" accept=\".bin\"></div>";
    html += "<div id=\"ota-progress-wrap\" style=\"display:none;margin:4px 0;height:8px;border-radius:6px;background:#0e1620;overflow:hidden\">";
    html += "<div id=\"ota-progress-bar\" style=\"height:100%;width:0%;background:var(--accent);transition:width .25s;\"></div></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"ota-upload-button\">Upload &amp; Flash</button>";
    html += "<div class=\"micro\" id=\"ota-status\">Select a .bin file built for PrintSphere (ESP32-S3).</div></div>";
    html += "<hr style=\"border:none;border-top:1px solid var(--line);margin:16px 0 4px\">";
    html += "<div class=\"field\"><label for=\"ota_url\">Or flash from URL</label>";
    html += "<input type=\"url\" id=\"ota_url\" placeholder=\"https://github.com/cptkirki/PrintSphere/blob/main/release/ota/printsphere_ota.bin\" autocomplete=\"off\" spellcheck=\"false\"></div>";
    html += "<p class=\"micro\">GitHub blob links (github.com/&hellip;/blob/&hellip;) are converted to raw download URLs automatically.</p>";
    html += "<div id=\"ota-url-progress-wrap\" style=\"display:none;margin:4px 0;height:8px;border-radius:6px;background:#0e1620;overflow:hidden\">";
    html += "<div id=\"ota-url-progress-bar\" style=\"height:100%;width:0%;background:var(--accent);transition:width .3s;\"></div></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"ota-url-button\">Flash from URL</button>";
    html += "<div class=\"micro\" id=\"ota-url-status\">Enter a direct .bin URL or a GitHub blob link above.</div></div>";
    end_collapsible_section();
  }

#ifdef PRINTSPHERE_DEBUG_BUILD
  if (show_connection_steps) {
    begin_collapsible_section(
        "Debug Console",
        "Live ESP-IDF log stream captured on-device. Only available in debug builds.",
        "DBG", "idle", false);
    html += "<div style=\"background:#161b22;border:1px solid #30363d;border-radius:8px;"
            "padding:10px 14px;margin-bottom:12px;font-size:12.5px;line-height:1.6;"
            "color:#8b949e;\">"
            "&#x1F512;&nbsp;<strong style=\"color:#c9d1d9;\">Anonymised log</strong> &mdash; "
            "IPs, MACs, Wi-Fi credentials, cloud tokens and pre-signed URLs are automatically "
            "replaced before this log is written. The full log remains available on the serial "
            "(UART) console for local debugging."
            "</div>";
    html += "<div id=\"dbg-terminal\""
            " style=\"background:#0d1117;border:1px solid #21262d;border-radius:10px;"
            "height:320px;overflow-y:auto;padding:10px 12px;font-family:monospace;"
            "font-size:11.5px;line-height:1.55;color:#c9d1d9;\">"
            "<pre id=\"dbg-log\" style=\"margin:0;white-space:pre-wrap;word-break:break-all;\"></pre>"
            "</div>";
    html += "<div class=\"actions\" style=\"flex-wrap:wrap;gap:8px;\">";
    html += "<button type=\"button\" class=\"secondary\" id=\"dbg-download-btn\">Download Log</button>";
    html += "<button type=\"button\" class=\"secondary\" id=\"dbg-clear-btn\">Clear</button>";
    html += "<label style=\"display:flex;align-items:center;gap:6px;font-size:13px;font-weight:400;"
            "color:var(--muted);cursor:pointer;margin:0;\">";
    html += "<input type=\"checkbox\" id=\"dbg-autoscroll\" checked style=\"width:auto;\"> Auto-scroll</label>";
    html += "<span id=\"dbg-status\" class=\"micro\" style=\"margin-left:auto;\">Connecting&hellip;</span>";
    html += "</div>";
    end_collapsible_section();
  }
#endif

  html += "<section class=\"footer-card\">";
  html += "<div class=\"actions\">";
  html += "<button type=\"submit\" class=\"primary\" id=\"save-button\">";
  html += json_escape(save_button_label);
  html += "</button>";
  html += "<div class=\"micro\">";
  html += json_escape(save_button_hint);
  html += "</div>";
  html += "</div>";
  html += "<div class=\"status-line\" id=\"status\">";
  html += json_escape(initial_status_line);
  html += "</div><div class=\"status-detail\" id=\"status-detail\">";
  html += json_escape(initial_status_detail);
  html += "</div>";
  html += "</section>";
  html += "</form>";

  html += "<script>";
  html += "const statusEl=document.getElementById('status');";
  html += "const statusDetailEl=document.getElementById('status-detail');";
  html += "const saveButton=document.getElementById('save-button');";
  html += "const wifiScanButton=document.getElementById('wifi-scan-button');";
  html += "const wifiSsidSelect=document.getElementById('wifi_ssid_select');";
  html += "const wifiScanDetail=document.getElementById('wifi-scan-detail');";
  html += "const wifiSsidManualField=document.getElementById('wifi-ssid-manual-field');";
  html += "const wifiSsidScanField=document.getElementById('wifi-ssid-scan-field');";
  html += "const displayRotationSelect=document.getElementById('display_rotation');";
  html += "const displayRotationApplyButton=document.getElementById('display-rotation-apply-button');";
  html += "const displayRotationApplyHint=document.getElementById('display-rotation-apply-hint');";
  html += "const portalLockSelect=document.getElementById('portal_lock_enabled');";
  html += "const portalAccessApplyButton=document.getElementById('portal-access-apply-button');";
  html += "const portalAccessApplyHint=document.getElementById('portal-access-apply-hint');";
  html += "const sourceModeSelect=document.getElementById('source_mode');";
  html += "const sourceModeApplyButton=document.getElementById('source-mode-apply-button');";
  html += "const sourceModeApplyHint=document.getElementById('source-mode-apply-hint');";
  html += "const batDimSelect=document.getElementById('bat_dim_enabled');";
  html += "const batDimOptionsGroup=document.getElementById('bat-dim-options-group');";
  html += "const batOffOptionsGroup=document.getElementById('bat-off-options-group');";
  html += "const batDimBrightnessSelect=document.getElementById('bat_dim_brightness');";
  html += "const batDimTimeoutIdleSelect=document.getElementById('bat_dim_timeout_idle');";
  html += "const batDimTimeoutActiveSelect=document.getElementById('bat_dim_timeout_active');";
  html += "const batOffSelect=document.getElementById('bat_off_enabled');";
  html += "const batOffTimeoutIdleSelect=document.getElementById('bat_off_timeout_idle');";
  html += "const batOffTimeoutActiveSelect=document.getElementById('bat_off_timeout_active');";
  html += "const batUsbPsSelect=document.getElementById('bat_usb_ps');";
  html += "const batDisplayApplyButton=document.getElementById('bat-display-apply-button');";
  html += "const batDisplayApplyHint=document.getElementById('bat-display-apply-hint');";
  html += "const amsDisplayApplyButton=document.getElementById('ams-display-apply-button');";
  html += "const amsDisplayApplyHint=document.getElementById('ams-display-apply-hint');";
  html += "const cloudConnectButton=document.getElementById('cloud-connect-button');";
  html += "const localConnectButton=document.getElementById('local-connect-button');";
  html += "const verifyButton=document.getElementById('verify-button');";
  html += "const arcInputIds=['arc_printing','arc_done','arc_error','arc_idle','arc_preheat','arc_clean','arc_level','arc_cool','arc_idle_active','arc_filament','arc_setup','arc_offline','arc_unknown'];";
  html += "let statusLockUntil=0;";
  html += "let arcPreviewTimer=null;";
  html += "let healthTimer=null;";
  html += "let portalTimerSeconds=0;let portalTimerInterval=null;";
  html += "function updatePortalTimer(){"
          "const el=document.getElementById('portal-timer');"
          "if(!el)return;"
          "if(portalTimerSeconds<=0){el.style.display='none';return;}"
          "const m=Math.floor(portalTimerSeconds/60);const s=portalTimerSeconds%60;"
          "el.textContent=(m>0?(m+'m '+s+'s'):(s+'s'));"
          "el.style.display='';portalTimerSeconds--;}";
  html += "function extendSession(){"
          "fetch('/api/session/extend',{method:'POST',credentials:'same-origin'})"
          ".then(r=>r.json()).then(d=>{if(d.remaining_s)syncPortalTimer(d.remaining_s);})"
          ".catch(()=>{});}";
  html += "function syncPortalTimer(secs){"
          "portalTimerSeconds=secs||0;"
          "if(portalTimerSeconds>0&&!portalTimerInterval){"
          "portalTimerInterval=setInterval(updatePortalTimer,1000);updatePortalTimer();}"
          "else if(portalTimerSeconds<=0&&portalTimerInterval){"
          "clearInterval(portalTimerInterval);portalTimerInterval=null;updatePortalTimer();}}";
  html += "let cloudFollowupTimer=null;";
  html += "let cloudFollowupUntil=0;";
  html += "let cloudFollowupDetail='';";
  html += "let localFollowupTimer=null;";
  html += "let localFollowupUntil=0;";
  html += "let localFollowupDetail='';";
  html += "let cloudSuccessReloadScheduled=false;";
  html += "let healthInFlight=false;";
  html += "let wifiScanInFlight=false;";
  html += "document.querySelectorAll('.settings-accordion').forEach(group=>{group.querySelectorAll('.settings-panel').forEach(panel=>{panel.addEventListener('toggle',()=>{if(!panel.open)return;group.querySelectorAll('.settings-panel').forEach(other=>{if(other!==panel)other.open=false;});});});});";
  html += "const savedConfig={cloud_email:\"";
  html += json_escape(cloud.email);
  html += "\",cloud_region:\"";
  html += json_escape(to_string(cloud.region));
  html += "\",source_mode:\"";
  html += to_string(source_mode);
  html += "\",display_rotation:\"";
  html += to_string(display_rotation);
  html += "\",portal_lock_enabled:";
  html += portal_lock_enabled ? "true" : "false";
  html += ",printer_host:\"";
  html += json_escape(printer.host);
  html += "\",printer_serial:\"";
  html += json_escape(effective_printer_serial);
  html += "\",wifi_password_saved:";
  html += wifi_password_saved ? "true" : "false";
  html += ",cloud_password_saved:";
  html += cloud_password_saved ? "true" : "false";
  html += ",printer_access_code_saved:";
  html += printer_access_code_saved ? "true" : "false";
  html += ",bat_dim_enabled:";
  html += bat_policy.dim_enabled ? "true" : "false";
  html += ",bat_dim_brightness:\"";
  html += std::to_string(bat_policy.dim_brightness_percent);
  html += "\",bat_off_enabled:";
  html += bat_policy.screen_off_enabled ? "true" : "false";
  html += ",bat_dim_timeout_idle:";
  html += std::to_string(bat_policy.dim_timeout_idle_s);
  html += ",bat_dim_timeout_active:";
  html += std::to_string(bat_policy.dim_timeout_active_s);
  html += ",bat_off_timeout_idle:";
  html += std::to_string(bat_policy.off_timeout_idle_s);
  html += ",bat_off_timeout_active:";
  html += std::to_string(bat_policy.off_timeout_active_s);
  html += ",bat_usb_ps:";
  html += bat_policy.usb_power_save_enabled ? "true" : "false";
  html += ",filament_wake:";
  html += filament_wake ? "true" : "false";
  html += ",filament_anim:";
  html += filament_anim ? "true" : "false";
  html += "};";
  html += "function setStatus(line,detail,lockMs){statusEl.textContent=line||'';statusDetailEl.textContent=detail||'';"
          "statusLockUntil=lockMs?Date.now()+lockMs:0;}";
  html += "function valueOf(id){const el=document.getElementById(id);return el?el.value:'';}";
  html += "function trimmedValue(id){return valueOf(id).trim();}";
  html += "function stopCloudFollowup(){if(cloudFollowupTimer){clearTimeout(cloudFollowupTimer);cloudFollowupTimer=null;}cloudFollowupUntil=0;cloudFollowupDetail='';}";
  html += "function stopLocalFollowup(){if(localFollowupTimer){clearTimeout(localFollowupTimer);localFollowupTimer=null;}localFollowupUntil=0;localFollowupDetail='';}";
  html += "function schedulePortalReload(delayMs){setTimeout(()=>window.location.reload(),delayMs||250);}";
  html += "function cloudResolvedSerial(body){return ((body&&body.cloud_resolved_serial)||'').trim();}";
  html += "function cloudSetupStage(body){return ((body&&body.cloud_setup_stage)||'idle');}";
  html += "function cloudPortalReady(body){return !!(body&&body.cloud_portal_ready);}";
  html += "function cloudStageIsCodeRequired(stage){return stage==='email_code_required'||stage==='tfa_required';}";
  html += "function cloudStageIsBusy(stage){return stage==='logging_in'||stage==='code_submitted'||stage==='binding_printer'||stage==='connecting_mqtt';}";
  html += "function cloudSessionLooksReady(body){const stage=cloudSetupStage(body);return !!(body&&body.cloud_connected)||stage==='connected';}";
  html += "function cloudProvisioningReady(body){const stage=cloudSetupStage(body);return cloudPortalReady(body)||stage==='binding_printer'||stage==='connecting_mqtt'||stage==='connected';}";
  html += "function cloudStageStatusLabel(stage){if(stage==='binding_printer')return 'Binding printer';if(stage==='connecting_mqtt')return 'Connecting Cloud MQTT';if(stage==='code_submitted')return 'Verifying code';if(stage==='logging_in')return 'Logging in';return 'Finishing login';}";
  html += "function cloudDetailLooksTransitional(detail){const text=(detail||'').trim();return text==='Logging in to Bambu Cloud'||text==='Waiting for Wi-Fi for Bambu Cloud'||text==='Restored Bambu Cloud session';}";
  html += "function localSerialInputReady(){const input=document.getElementById('printer_serial');return !!(input&&input.value.trim());}";
  html += "function applyResolvedSerial(body){const serial=((body&&body.cloud_resolved_serial)||'').trim();"
          "const input=document.getElementById('printer_serial');if(!serial||!input)return;"
          "const current=input.value.trim();const saved=(savedConfig.printer_serial||'').trim();"
          "if(!current||current===saved||current===serial){input.value=serial;savedConfig.printer_serial=serial;}}";
  html += "function populateWifiNetworks(body){if(!wifiSsidSelect)return;"
          "const previousValue=wifiSsidSelect.value;const currentSsid=trimmedValue('wifi_ssid');"
          "const networks=Array.isArray(body&&body.networks)?body.networks:[];"
          "wifiSsidSelect.innerHTML='<option value=\"\">Select detected Wi-Fi...</option>';"
          "networks.forEach((ssid)=>{const option=document.createElement('option');option.value=ssid;option.textContent=ssid;wifiSsidSelect.appendChild(option);});"
          "var manualOpt=document.createElement('option');manualOpt.value='__manual__';manualOpt.textContent='\u270e Type SSID manually\u2026';wifiSsidSelect.appendChild(manualOpt);"
          "if(currentSsid&&networks.includes(currentSsid)){wifiSsidSelect.value=currentSsid;}"
          "else if(previousValue&&networks.includes(previousValue)){wifiSsidSelect.value=previousValue;}"
          "if(networks.length&&wifiSsidScanField&&wifiSsidManualField){wifiSsidScanField.style.display='';wifiSsidManualField.style.display='none';}}";
  html += "async function refreshWifiScan(){if(!wifiSsidSelect||wifiScanInFlight)return;"
          "wifiScanInFlight=true;"
          "if(wifiScanButton){wifiScanButton.disabled=true;}if(wifiScanDetail){wifiScanDetail.textContent='Scanning nearby Wi-Fi networks...';}"
          "try{const response=await fetch('/api/wifi/scan',{cache:'no-store'});const body=await response.json().catch(()=>({}));"
          "if(response.ok){populateWifiNetworks(body);if(wifiScanDetail){const count=Array.isArray(body.networks)?body.networks.length:0;"
          "wifiScanDetail.textContent=count?('Found '+count+' visible network'+(count===1?'':'s')+'. Hidden SSIDs can still be typed manually.'):'No visible networks found right now. Hidden SSIDs can still be typed manually.';}}"
          "else if(wifiScanDetail){wifiScanDetail.textContent=body.error||'Wi-Fi scan failed right now.';}}"
          "catch(error){if(wifiScanDetail){wifiScanDetail.textContent='Wi-Fi scan request failed.';}}"
          "finally{wifiScanInFlight=false;if(wifiScanButton){wifiScanButton.disabled=false;}}}";
  html += "function setBadge(id,label,value,stateClass){const badge=document.getElementById(id);if(!badge)return;"
          "badge.className='badge '+stateClass;badge.innerHTML='<span class=\"badge-label\">'+label+'</span><span class=\"badge-value\">'+value+'</span>';}"
          "function setPill(id,value,stateClass){const p=document.getElementById(id);if(!p)return;"
          "p.className='summary-pill '+stateClass;p.textContent=value;}"; 
  html += "function renderCloudStatus(body){"
          "var cv='Not configured',cs='idle';"
          "if(body&&body.cloud_badge_value){cv=body.cloud_badge_value;cs=body.cloud_badge_state||'ok';}"
          "else{var st=cloudSetupStage(body);"
          "if(body&&(body.cloud_connected||body.cloud_portal_ready||st==='connected')){cv='Connected';cs='ok';}"
          "else if(cloudStageIsCodeRequired(st)){cv='Code required';cs='warn';}"
          "else if(st==='logging_in'){cv='Logging in';cs='info';}"
          "else if(st==='code_submitted'){cv='Verifying code';cs='info';}"
          "else if(st==='binding_printer'){cv='Binding printer';cs='info';}"
          "else if(st==='connecting_mqtt'){cv='Connecting MQTT';cs='info';}"
          "else if(st==='failed'){cv='Login failed';cs='warn';}"
          "else if(Date.now()<cloudFollowupUntil){cv='Finishing login';cs='info';}"
          "else if((body&&body.cloud_configured)||(!!trimmedValue('cloud_email')&&(!!savedConfig.cloud_password_saved||!!valueOf('cloud_password')))){cv='Configured';cs='info';}}"
          "setBadge('cloud-badge','Cloud',cv,cs);"
          "setPill('cloud-section-pill',cv,cs);"
          "var cd=document.getElementById('cloud-detail');"
          "if(cd){"
          "var dt=body&&(body.cloud_status_detail||body.cloud_detail||body.detail);"
          "if(dt){cd.textContent=dt;}"
          "else if(Date.now()<cloudFollowupUntil&&cloudFollowupDetail){cd.textContent=cloudFollowupDetail;}"
          "else if(cv==='Configured'){cd.textContent='Cloud credentials are saved on the ESP.';}"
          "else{cd.textContent='No cloud response yet';}}"
          "if(body){updateCloudVerification(body);applyResolvedSerial(body);}"
          "var sr=cloudSessionLooksReady(body),xr=!!cloudResolvedSerial(body)||localSerialInputReady(),st2=cloudSetupStage(body);"
          "if(body&&(cloudStageIsCodeRequired(st2)||st2==='failed'||(sr&&xr))){stopCloudFollowup();}}";
  html += "function applyHealthBody(body){if(!body)return null;"
          "if(body.cloud_badge_value){setBadge('cloud-badge','Cloud',body.cloud_badge_value,body.cloud_badge_state||'ok');}"
          "const wifiValue=body.wifi_connected?('Connected - '+(body.wifi_ip||'')):'Not connected';"
          "setBadge('wifi-badge','Wi-Fi',wifiValue,body.wifi_connected?'ok':'warn');"
          "var wd=document.getElementById('wifi-detail');if(wd){wd.textContent=wifiValue;}"
          "renderCloudStatus(body);"
          "const sourceMode=body.source_mode||savedConfig.source_mode||'hybrid';"
          "const sourceValue=sourceMode==='local_only'?'Local only':(sourceMode==='cloud_only'?'Cloud only':'Hybrid (recommended)');"
          "setBadge('source-badge','Source',sourceValue,'info');"
          "renderLocalStatus(body);"
          "if(Date.now()>statusLockUntil){"
          "if(body.wifi_connected){const setupDone=(body.cloud_configured||body.local_configured)&&(!body.cloud_configured||body.cloud_portal_ready||(!body.cloud_verification_required&&!body.cloud_tfa_required&&cloudSetupStage(body)!=='failed'))&&(!body.local_configured||!body.local_error);if(setupDone){setStatus('','',0);}else if(body.cloud_status_line){setStatus(body.cloud_status_line,body.cloud_status_detail||body.cloud_detail||body.detail||'',0);}"
          "else{const stage=cloudSetupStage(body);setStatus(trimmedValue('cloud_email')||body.cloud_connected||cloudStageIsCodeRequired(stage)||cloudStageIsBusy(stage)?'Connect Bambu Cloud':'Setup ready',body.cloud_detail||body.detail||('ESP on home network: '+(body.wifi_ip||'')),0);}}"
          "else{setStatus('Save Wi-Fi','Save Wi-Fi and restart to continue provisioning.',0);}}"
          "syncPortalTimer(body.portal_open_remaining_s||0);"
          "return body;}";
  html += "async function forceHealthRefresh(){try{const response=await fetch('/api/health',{cache:'no-store'});"
          "if(!response.ok)return null;const body=await response.json();if(body.portal_locked){window.location.reload();return null;}"
          "return applyHealthBody(body);}catch(error){return null;}}";
  html += "function startCloudFollowup(detail,timeoutMs){"
          "cloudFollowupDetail=detail||'Completing the cloud login and device binding now.';"
          "cloudFollowupUntil=Date.now()+(timeoutMs||12000);"
          "renderCloudStatus({cloud_connected:false,cloud_verification_required:false,cloud_configured:true,cloud_setup_stage:'logging_in',cloud_detail:cloudFollowupDetail});"
          "if(cloudFollowupTimer){clearTimeout(cloudFollowupTimer);cloudFollowupTimer=null;}"
          "const poll=async()=>{"
          "const body=await forceHealthRefresh();"
          "const stage=cloudSetupStage(body);"
          "const serialReady=!!cloudResolvedSerial(body)||localSerialInputReady();"
          "if(body&&cloudProvisioningReady(body)){const detail=(cloudDetailLooksTransitional(body.cloud_detail||body.detail)?'Bambu Cloud session is ready.':(body.cloud_detail||body.detail||'Bambu Cloud session is ready.'));setStatus('Cloud connected',detail,5000);stopCloudFollowup();if(!cloudSuccessReloadScheduled){cloudSuccessReloadScheduled=true;schedulePortalReload(serialReady?350:600);}return;}"
          "if(body&&stage==='binding_printer'){setStatus('Cloud connected',body.cloud_detail||body.detail||'Resolving your printer from the Bambu Cloud account now.',4000);}"
          "if(body&&stage==='connecting_mqtt'){setStatus('Cloud connected',body.cloud_detail||body.detail||'Connecting the live cloud monitor now.',4000);}"
          "if(body&&cloudStageIsCodeRequired(stage)){setStatus(body.cloud_tfa_required?'2FA required':'Verification code required',body.cloud_detail||'Enter the requested code to finish the login.',10000);stopCloudFollowup();return;}"
          "if(body&&stage==='failed'){setStatus('Cloud login failed',body.cloud_detail||body.detail||'The Bambu Cloud login did not complete.',8000);stopCloudFollowup();return;}"
          "if(Date.now()>=cloudFollowupUntil){if(body&&cloudSessionLooksReady(body)&&!serialReady){cloudFollowupUntil=Date.now()+5000;cloudFollowupTimer=setTimeout(poll,350);return;}if(body&&cloudStageIsBusy(stage)){setStatus('Cloud login still running',body.cloud_detail||body.detail||'The cloud login is still progressing in the background.',6000);}stopCloudFollowup();return;}"
          "cloudFollowupTimer=setTimeout(poll,350);};"
          "cloudFollowupTimer=setTimeout(poll,350);}";
  html += "function renderLocalStatus(body){"
          "let localValue='Not configured';let localState='idle';"
          "if(body&&body.local_connected){localValue='Connected';localState='ok';}"
          "else if(body&&body.local_error){localValue='Error';localState='warn';}"
          "else if(Date.now()<localFollowupUntil){localValue='Connecting';localState='info';}"
          "else if(body&&body.local_configured){localValue='Configured';localState='info';}"
          "setBadge('local-badge','Local Path',localValue,localState);"
          "setPill('local-section-pill',localValue,localState);"
          "const localDetail=document.getElementById('local-detail');"
          "if(localDetail){"
          "if(body&&body.local_detail){localDetail.textContent=body.local_detail;}"
          "else if(Date.now()<localFollowupUntil&&localFollowupDetail){localDetail.textContent=localFollowupDetail;}"
          "else{localDetail.textContent='No local response yet';}}"
          "if(body&&(body.local_connected||body.local_error)){stopLocalFollowup();}}";
  html += "function startLocalFollowup(detail,timeoutMs){"
          "localFollowupDetail=detail||'Saving printer credentials and connecting to local MQTT now.';"
          "localFollowupUntil=Date.now()+(timeoutMs||12000);"
          "renderLocalStatus({local_connected:false,local_error:false,local_configured:true,local_detail:localFollowupDetail});"
          "if(localFollowupTimer){clearTimeout(localFollowupTimer);localFollowupTimer=null;}"
          "const poll=async()=>{"
          "let body=await updateHealth();"
          "if(!body){body=await forceHealthRefresh();}"
          "if(body&&body.local_connected){setStatus('Local path connected',body.local_detail||'Connected to local Bambu MQTT.',5000);stopLocalFollowup();return;}"
          "if(body&&body.local_error){setStatus('Local path error',body.local_detail||'The local MQTT path returned an error.',8000);stopLocalFollowup();return;}"
          "if(Date.now()>=localFollowupUntil){await forceHealthRefresh();stopLocalFollowup();return;}"
          "localFollowupTimer=setTimeout(poll,600);};"
          "localFollowupTimer=setTimeout(poll,600);}";
  html += "function updateSourceModeControls(){"
          "if(!sourceModeSelect||!sourceModeApplyButton||!sourceModeApplyHint)return;"
          "const selected=valueOf('source_mode')||'hybrid';"
          "const changed=selected!==(savedConfig.source_mode||'hybrid');"
          "sourceModeApplyButton.classList.toggle('hidden',!changed);"
          "sourceModeApplyHint.classList.toggle('hidden',!changed);"
          "if(!changed){sourceModeApplyButton.disabled=false;}}";
  html += "function updateDisplayRotationControls(){"
          "if(!displayRotationSelect||!displayRotationApplyButton||!displayRotationApplyHint)return;"
          "const selected=valueOf('display_rotation')||'0';"
          "const changed=selected!==(savedConfig.display_rotation||'0');"
          "displayRotationApplyButton.classList.toggle('hidden',!changed);"
          "displayRotationApplyHint.classList.toggle('hidden',!changed);"
          "if(!changed){displayRotationApplyButton.disabled=false;}}";
  html += "function updatePortalAccessControls(){"
          "if(!portalLockSelect||!portalAccessApplyButton||!portalAccessApplyHint)return;"
          "const selected=valueOf('portal_lock_enabled')==='true';"
          "const changed=selected!==(savedConfig.portal_lock_enabled!==false);"
          "portalAccessApplyButton.classList.toggle('hidden',!changed);"
          "portalAccessApplyHint.classList.toggle('hidden',!changed);"
          "if(!changed){portalAccessApplyButton.disabled=false;}}";
  html += "function updateAmsDisplayControls(){"
          "if(!amsDisplayApplyButton||!amsDisplayApplyHint)return;"
          "const wakeNow=document.getElementById('filament_wake')?valueOf('filament_wake')==='true':savedConfig.filament_wake===true;"
          "const animNow=document.getElementById('filament_anim')?valueOf('filament_anim')==='true':savedConfig.filament_anim!==false;"
          "const changed=wakeNow!==(savedConfig.filament_wake===true)"
              "||animNow!==(savedConfig.filament_anim!==false);"
          "amsDisplayApplyButton.classList.toggle('hidden',!changed);"
          "amsDisplayApplyHint.classList.toggle('hidden',!changed);"
          "if(!changed){amsDisplayApplyButton.disabled=false;}}";
  html += "function updateBatDisplayControls(){"
          "if(batDimSelect&&batDimOptionsGroup){batDimOptionsGroup.style.display=batDimSelect.value==='true'?'':'none';}"
          "if(batOffSelect&&batOffOptionsGroup){batOffOptionsGroup.style.display=batOffSelect.value==='true'?'':'none';}"
          "if(!batDisplayApplyButton||!batDisplayApplyHint)return;"
          "const dimNow=batDimSelect?batDimSelect.value==='true':savedConfig.bat_dim_enabled!==false;"
          "const pctNow=batDimBrightnessSelect?batDimBrightnessSelect.value:String(savedConfig.bat_dim_brightness||'0');"
          "const offNow=batOffSelect?batOffSelect.value==='true':savedConfig.bat_off_enabled!==false;"
          "const diIdleNow=batDimTimeoutIdleSelect?batDimTimeoutIdleSelect.value:String(savedConfig.bat_dim_timeout_idle||20);"
          "const diActNow=batDimTimeoutActiveSelect?batDimTimeoutActiveSelect.value:String(savedConfig.bat_dim_timeout_active||30);"
          "const ofIdleNow=batOffTimeoutIdleSelect?batOffTimeoutIdleSelect.value:String(savedConfig.bat_off_timeout_idle||60);"
          "const ofActNow=batOffTimeoutActiveSelect?batOffTimeoutActiveSelect.value:String(savedConfig.bat_off_timeout_active||120);"
          "const usbPsNow=batUsbPsSelect?batUsbPsSelect.value==='true':savedConfig.bat_usb_ps===true;"
          "const changed=dimNow!==(savedConfig.bat_dim_enabled!==false)"
              "||pctNow!==String(savedConfig.bat_dim_brightness||'0')"
              "||offNow!==(savedConfig.bat_off_enabled!==false)"
              "||diIdleNow!==String(savedConfig.bat_dim_timeout_idle||20)"
              "||diActNow!==String(savedConfig.bat_dim_timeout_active||30)"
              "||ofIdleNow!==String(savedConfig.bat_off_timeout_idle||60)"
              "||ofActNow!==String(savedConfig.bat_off_timeout_active||120)"
              "||usbPsNow!==(savedConfig.bat_usb_ps===true);"
          "batDisplayApplyButton.classList.toggle('hidden',!changed);"
          "batDisplayApplyHint.classList.toggle('hidden',!changed);"
          "if(!changed){batDisplayApplyButton.disabled=false;}}";
  html += "function updateCloudVerification(body){const note=document.getElementById('cloud-verify-note');"
          "const label=document.getElementById('cloud-verification-label');"
          "const input=document.getElementById('cloud_verification_code');"
          "if(!note||!label||!input)return;"
          "const stage=cloudSetupStage(body);"
          "const tfa=stage==='tfa_required'||!!body.cloud_tfa_required;"
          "const required=cloudStageIsCodeRequired(stage);"
          "label.textContent=tfa?'2FA Code':'Verification Code';"
          "input.placeholder=tfa?'Only needed if Bambu requests a 2FA code':'Only needed if Bambu requests a verification code';"
          "note.textContent=tfa?'Bambu is currently waiting for a 2FA code.':'Bambu is currently waiting for a verification code. The cloud login completes after that step.';"
          "note.classList.toggle('hidden',!required);}";
  html += "async function updateHealth(){if(healthInFlight)return null;healthInFlight=true;try{const response=await fetch('/api/health',{cache:'no-store'});"
          "if(!response.ok)return null;const body=await response.json();"
          "if(body&&body.cloud_badge_value){setBadge('cloud-badge','Cloud',body.cloud_badge_value,body.cloud_badge_state||'ok');}"
          "if(body.portal_locked){window.location.reload();return null;}"
          "return applyHealthBody(body);}"
          "catch(error){if(Date.now()>statusLockUntil){setStatus('Portal reachable','Live status could not be refreshed right now.',0);}}"
          "finally{healthInFlight=false;}return null;}";
  html += "function buildArcPayload(){const payload={};arcInputIds.forEach((id)=>{const input=document.getElementById(id);if(input){payload[id]=input.value;}});return payload;}";
  html += "function buildPayload(){return Object.assign({wifi_ssid:trimmedValue('wifi_ssid'),"
          "wifi_password:valueOf('wifi_password'),"
          "cloud_email:(document.getElementById('cloud_email')?trimmedValue('cloud_email'):savedConfig.cloud_email),"
          "cloud_region:(document.getElementById('cloud_region')?valueOf('cloud_region'):savedConfig.cloud_region||'eu'),"
          "cloud_password:(document.getElementById('cloud_password')?valueOf('cloud_password'):''),"
          "display_rotation:(document.getElementById('display_rotation')?valueOf('display_rotation'):savedConfig.display_rotation)||'0',"
          "portal_lock_enabled:(document.getElementById('portal_lock_enabled')?valueOf('portal_lock_enabled')==='true':savedConfig.portal_lock_enabled!==false),"
          "filament_wake:(document.getElementById('filament_wake')?valueOf('filament_wake')==='true':savedConfig.filament_wake===true),"
          "filament_anim:(document.getElementById('filament_anim')?valueOf('filament_anim')==='true':savedConfig.filament_anim!==false),"
          "source_mode:(document.getElementById('source_mode')?valueOf('source_mode'):savedConfig.source_mode)||'hybrid',"
          "printer_host:(document.getElementById('printer_host')?trimmedValue('printer_host'):savedConfig.printer_host),"
          "printer_serial:(document.getElementById('printer_serial')?trimmedValue('printer_serial'):savedConfig.printer_serial),"
          "printer_access_code:(document.getElementById('printer_access_code')?trimmedValue('printer_access_code'):'' ),"
          "bat_dim_enabled:(batDimSelect?batDimSelect.value==='true':savedConfig.bat_dim_enabled!==false),"
          "bat_dim_brightness:(batDimBrightnessSelect?batDimBrightnessSelect.value:String(savedConfig.bat_dim_brightness||'0')),"
          "bat_off_enabled:(batOffSelect?batOffSelect.value==='true':savedConfig.bat_off_enabled!==false),"
          "bat_dim_timeout_idle:Number(batDimTimeoutIdleSelect?batDimTimeoutIdleSelect.value:savedConfig.bat_dim_timeout_idle||20),"
          "bat_dim_timeout_active:Number(batDimTimeoutActiveSelect?batDimTimeoutActiveSelect.value:savedConfig.bat_dim_timeout_active||30),"
          "bat_off_timeout_idle:Number(batOffTimeoutIdleSelect?batOffTimeoutIdleSelect.value:savedConfig.bat_off_timeout_idle||60),"
          "bat_off_timeout_active:Number(batOffTimeoutActiveSelect?batOffTimeoutActiveSelect.value:savedConfig.bat_off_timeout_active||120)"
          "},buildArcPayload());}";
  html += "async function postArcColors(url){const response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(buildArcPayload())});"
          "const body=await response.json().catch(()=>({}));if(!response.ok){throw new Error(body.error||'Arc color request failed');}return body;}";
  html += "function queueArcPreview(){clearTimeout(arcPreviewTimer);arcPreviewTimer=setTimeout(async()=>{try{await postArcColors('/api/arc/preview');}"
          "catch(error){setStatus('Arc preview failed',error.message||'Live color preview could not be applied.',4000);}},140);}";
  html += "async function commitArcColors(){clearTimeout(arcPreviewTimer);try{await postArcColors('/api/arc/commit');"
          "setStatus('Arc colors saved live','The current color palette is active immediately and stored without a reboot.',4000);}"
          "catch(error){setStatus('Arc color save failed',error.message||'Live color save could not be completed.',5000);}}";
  html += "document.getElementById('config-form').addEventListener('submit',async(event)=>{event.preventDefault();"
          "saveButton.disabled=true;setStatus('Saving configuration...','The ESP restarts automatically after a successful save.',15000);"
          "try{const response=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(buildPayload())});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'Saving failed','Please review the fields and try again.',8000);saveButton.disabled=false;}}"
          "catch(error){setStatus('Saving failed','The request to the ESP could not be completed.',8000);saveButton.disabled=false;}});";
  html += "document.addEventListener('keydown',function(e){if(e.key==='Enter'&&e.target.tagName==='INPUT'&&e.target.type!=='submit'){e.preventDefault();e.target.blur();}});";
  html += "{const pt=document.getElementById('portal-timer');if(pt){pt.addEventListener('click',extendSession);}}";
  html += "if(displayRotationSelect){displayRotationSelect.addEventListener('change',updateDisplayRotationControls);}";
  html += "if(displayRotationApplyButton){displayRotationApplyButton.addEventListener('click',async()=>{const display_rotation=valueOf('display_rotation')||'0';"
          "if(display_rotation===(savedConfig.display_rotation||'0')){updateDisplayRotationControls();return;}"
          "displayRotationApplyButton.disabled=true;setStatus('Applying screen rotation...','Saving the new panel orientation and restarting the ESP now.',15000);"
          "try{const response=await fetch('/api/display-rotation',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({display_rotation})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){savedConfig.display_rotation=display_rotation;updateDisplayRotationControls();setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'Rotation change failed',body.detail||'The new display rotation could not be saved.',8000);displayRotationApplyButton.disabled=false;updateDisplayRotationControls();}}"
          "catch(error){setStatus('Rotation change failed','The request to the ESP could not be completed.',8000);displayRotationApplyButton.disabled=false;updateDisplayRotationControls();}});}";
  html += "if(portalLockSelect){portalLockSelect.addEventListener('change',updatePortalAccessControls);}";
  html += "if(portalAccessApplyButton){portalAccessApplyButton.addEventListener('click',async()=>{const portal_lock_enabled=valueOf('portal_lock_enabled')==='true';"
          "if(portal_lock_enabled===(savedConfig.portal_lock_enabled!==false)){updatePortalAccessControls();return;}"
          "portalAccessApplyButton.disabled=true;setStatus('Applying portal access...','Saving the new portal lock mode and restarting the ESP now.',15000);"
          "try{const response=await fetch('/api/portal-access',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({portal_lock_enabled})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){savedConfig.portal_lock_enabled=portal_lock_enabled;updatePortalAccessControls();setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'Portal access change failed',body.detail||'The new portal access mode could not be saved.',8000);portalAccessApplyButton.disabled=false;updatePortalAccessControls();}}"
          "catch(error){setStatus('Portal access change failed','The request to the ESP could not be completed.',8000);portalAccessApplyButton.disabled=false;updatePortalAccessControls();}});}";
  html += "{const amsIds=['filament_wake','filament_anim'];"
          "amsIds.forEach(id=>{const el=document.getElementById(id);if(el)el.addEventListener('change',updateAmsDisplayControls);});}";  
  html += "if(amsDisplayApplyButton){amsDisplayApplyButton.addEventListener('click',async()=>{"
          "const filament_wake=document.getElementById('filament_wake')?valueOf('filament_wake')==='true':savedConfig.filament_wake===true;"
          "const filament_anim=document.getElementById('filament_anim')?valueOf('filament_anim')==='true':savedConfig.filament_anim!==false;"
          "amsDisplayApplyButton.disabled=true;setStatus('Applying AMS display settings...','Saving settings and restarting the ESP now.',15000);"
          "try{const response=await fetch('/api/ams-display',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({filament_wake,filament_anim})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){savedConfig.filament_wake=filament_wake;savedConfig.filament_anim=filament_anim;"
              "updateAmsDisplayControls();setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'AMS display change failed',body.detail||'The AMS display settings could not be saved.',8000);amsDisplayApplyButton.disabled=false;updateAmsDisplayControls();}}"
          "catch(error){setStatus('AMS display change failed','The request to the ESP could not be completed.',8000);amsDisplayApplyButton.disabled=false;updateAmsDisplayControls();}});}";
  html += "if(batDimSelect){batDimSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batDimBrightnessSelect){batDimBrightnessSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batOffSelect){batOffSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batDimTimeoutIdleSelect){batDimTimeoutIdleSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batDimTimeoutActiveSelect){batDimTimeoutActiveSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batOffTimeoutIdleSelect){batOffTimeoutIdleSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batOffTimeoutActiveSelect){batOffTimeoutActiveSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batUsbPsSelect){batUsbPsSelect.addEventListener('change',updateBatDisplayControls);}";
  html += "if(batDisplayApplyButton){batDisplayApplyButton.addEventListener('click',async()=>{"
          "const bat_dim_enabled=batDimSelect?batDimSelect.value==='true':savedConfig.bat_dim_enabled!==false;"
          "const bat_dim_brightness=batDimBrightnessSelect?batDimBrightnessSelect.value:String(savedConfig.bat_dim_brightness||'0');"
          "const bat_off_enabled=batOffSelect?batOffSelect.value==='true':savedConfig.bat_off_enabled!==false;"
          "const bat_dim_timeout_idle=Number(batDimTimeoutIdleSelect?batDimTimeoutIdleSelect.value:savedConfig.bat_dim_timeout_idle||20);"
          "const bat_dim_timeout_active=Number(batDimTimeoutActiveSelect?batDimTimeoutActiveSelect.value:savedConfig.bat_dim_timeout_active||30);"
          "const bat_off_timeout_idle=Number(batOffTimeoutIdleSelect?batOffTimeoutIdleSelect.value:savedConfig.bat_off_timeout_idle||60);"
          "const bat_off_timeout_active=Number(batOffTimeoutActiveSelect?batOffTimeoutActiveSelect.value:savedConfig.bat_off_timeout_active||120);"
          "const bat_usb_ps=batUsbPsSelect?batUsbPsSelect.value==='true':savedConfig.bat_usb_ps===true;"
          "batDisplayApplyButton.disabled=true;setStatus('Applying energy settings...','Saving settings and restarting the ESP now.',15000);"
          "try{const response=await fetch('/api/battery-display',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({bat_dim_enabled,bat_dim_brightness,bat_off_enabled,bat_dim_timeout_idle,bat_dim_timeout_active,bat_off_timeout_idle,bat_off_timeout_active,bat_usb_ps})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){savedConfig.bat_dim_enabled=bat_dim_enabled;savedConfig.bat_dim_brightness=bat_dim_brightness;savedConfig.bat_off_enabled=bat_off_enabled;"
              "savedConfig.bat_dim_timeout_idle=bat_dim_timeout_idle;savedConfig.bat_dim_timeout_active=bat_dim_timeout_active;"
              "savedConfig.bat_off_timeout_idle=bat_off_timeout_idle;savedConfig.bat_off_timeout_active=bat_off_timeout_active;"
              "savedConfig.bat_usb_ps=bat_usb_ps;"
              "updateBatDisplayControls();setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'Energy settings change failed',body.detail||'The energy settings could not be saved.',8000);batDisplayApplyButton.disabled=false;updateBatDisplayControls();}}"
          "catch(error){setStatus('Energy settings change failed','The request to the ESP could not be completed.',8000);batDisplayApplyButton.disabled=false;updateBatDisplayControls();}})}";  html += "if(sourceModeSelect){sourceModeSelect.addEventListener('change',updateSourceModeControls);}";
  html += "if(sourceModeApplyButton){sourceModeApplyButton.addEventListener('click',async()=>{const source_mode=valueOf('source_mode')||'hybrid';"
          "if(source_mode===(savedConfig.source_mode||'hybrid')){updateSourceModeControls();return;}"
          "sourceModeApplyButton.disabled=true;setStatus('Applying connection mode...','Saving the new mode and restarting the ESP now.',15000);"
          "try{const response=await fetch('/api/source-mode',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({source_mode})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){savedConfig.source_mode=source_mode;updateSourceModeControls();setStatus('Saved. Restarting ESP...','The connection will drop briefly during reboot.',30000);}"
          "else{setStatus(body.error||'Mode change failed',body.detail||'The new connection mode could not be saved.',8000);sourceModeApplyButton.disabled=false;updateSourceModeControls();}}"
          "catch(error){setStatus('Mode change failed','The request to the ESP could not be completed.',8000);sourceModeApplyButton.disabled=false;updateSourceModeControls();}});}";
  html += "if(cloudConnectButton){cloudConnectButton.addEventListener('click',async()=>{const cloud_email=trimmedValue('cloud_email');"
          "const cloud_region=(document.getElementById('cloud_region')?valueOf('cloud_region'):'eu')||'eu';"
          "const cloud_password=valueOf('cloud_password');"
          "const source_mode=savedConfig.source_mode||'hybrid';"
          "if(!cloud_email){setStatus('Cloud credentials missing','Enter the Bambu email first.',5000);return;}"
          "stopCloudFollowup();cloudConnectButton.disabled=true;setStatus('Connecting cloud...','Saving credentials and starting the login now.',8000);"
          "try{const response=await fetch('/api/cloud/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cloud_email,cloud_password,cloud_region,source_mode})});"
          "const body=await response.json().catch(()=>({}));renderCloudStatus(body);"
          "if(body.cloud_badge_value){setBadge('cloud-badge','Cloud',body.cloud_badge_value,body.cloud_badge_state||'ok');}"
          "if(response.ok){savedConfig.cloud_email=cloud_email;savedConfig.cloud_region=cloud_region;"
          "savedConfig.cloud_password_saved=!!cloud_password||savedConfig.cloud_password_saved;"
          "const stage=cloudSetupStage(body);"
          "if(body.cloud_connected||cloudSessionLooksReady(body)){setStatus('Cloud connected',body.detail||body.cloud_detail||'Connected to Bambu Cloud.',7000);if(!cloudSuccessReloadScheduled){cloudSuccessReloadScheduled=true;schedulePortalReload(350);}}"
          "else if(cloudStageIsCodeRequired(stage)){setStatus(stage==='tfa_required'?'2FA required':'Verification code required',body.detail||body.cloud_detail||'Enter the requested code to finish the login.',10000);}"
          "else if(stage==='failed'){setStatus('Cloud connect failed',body.detail||body.cloud_detail||'The cloud login did not complete.',8000);}"
          "else{const detail=body.detail||body.cloud_detail||'Waiting for cloud response...';setStatus(cloudStageStatusLabel(stage),detail,4000);startCloudFollowup(detail,15000);}}"
          "else{setStatus(body.error||'Cloud connect failed',body.detail||'Please review the credentials and try again.',8000);}}"
          "catch(error){setStatus('Cloud request failed','The request to the ESP did not complete successfully.',7000);}finally{cloudConnectButton.disabled=false;if(!cloudSuccessReloadScheduled){await forceHealthRefresh();}}});}"; 
  html += "if(localConnectButton){localConnectButton.addEventListener('click',async()=>{const printer_host=trimmedValue('printer_host');"
          "const printer_serial=trimmedValue('printer_serial');"
          "const printer_access_code=trimmedValue('printer_access_code');"
          "const source_mode=savedConfig.source_mode||'hybrid';"
          "if(!printer_host||!printer_serial){setStatus('Local credentials missing','Enter printer host and serial first.',5000);return;}"
          "stopLocalFollowup();localConnectButton.disabled=true;setStatus('Connecting local path...','Saving printer credentials and reconnecting MQTT now.',8000);"
          "try{const response=await fetch('/api/local/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({printer_host,printer_serial,printer_access_code,source_mode})});"
          "const body=await response.json().catch(()=>({}));renderLocalStatus(body);"
          "if(response.ok){if(body.local_connected){setStatus('Local path connected',body.detail||'Connected to local Bambu MQTT.',7000);schedulePortalReload(800);}"
          "else if(body.local_error){setStatus('Local path error',body.detail||'The local MQTT path returned an error.',8000);}"
          "else{const detail=body.detail||'Waiting for local printer response...';setStatus('Local path started',detail,4000);startLocalFollowup(detail,12000);schedulePortalReload(2000);}}"
          "else{setStatus(body.error||'Local connect failed',body.detail||'Please review the local printer fields and try again.',8000);}}"
          "catch(error){setStatus('Local request failed','The request to the ESP did not complete successfully.',7000);}finally{localConnectButton.disabled=false;updateHealth();}});}";
  html += "if(verifyButton){verifyButton.addEventListener('click',async()=>{const code=trimmedValue('cloud_verification_code');"
          "if(!code){setStatus('Cloud code missing','Please enter the requested code first.',5000);return;}"
          "stopCloudFollowup();verifyButton.disabled=true;setStatus('Connecting cloud...','Completing the cloud login now.',8000);"
          "try{const response=await fetch('/api/cloud/verify',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code})});"
          "const body=await response.json().catch(()=>({}));renderCloudStatus(body);"
          "if(body.cloud_badge_value){setBadge('cloud-badge','Cloud',body.cloud_badge_value,body.cloud_badge_state||'ok');}"
          "if(response.ok){"
          "const stage=cloudSetupStage(body);"
          "if(body.cloud_connected||cloudSessionLooksReady(body)){setStatus('Cloud connected',body.detail||body.cloud_detail||'Connected to Bambu Cloud.',7000);if(!cloudSuccessReloadScheduled){cloudSuccessReloadScheduled=true;schedulePortalReload(350);}}"
          "else if(cloudStageIsCodeRequired(stage)){setStatus(stage==='tfa_required'?'2FA required':'Verification code required',body.detail||body.cloud_detail||'Please check the code and try again.',8000);}"
          "else if(stage==='failed'){setStatus('Cloud code was rejected',body.detail||body.cloud_detail||'Please check the code and try again.',7000);}"
          "else{const detail=body.detail||body.cloud_detail||'The cloud accepted the code and is finishing login now.';setStatus(cloudStageStatusLabel(stage),detail,4000);startCloudFollowup(detail,15000);}"
          "const codeInput=document.getElementById('cloud_verification_code');if(codeInput){codeInput.value='';}}"
          "else{setStatus(body.error||'Cloud code was rejected',body.detail||'Please check the code and try again.',7000);}}"
          "catch(error){setStatus('Cloud request failed','The request to the ESP did not complete successfully.',7000);}finally{verifyButton.disabled=false;if(!cloudSuccessReloadScheduled){await forceHealthRefresh();}}});}"; 
  html += "if(wifiSsidSelect){wifiSsidSelect.addEventListener('change',()=>{if(wifiSsidSelect.value==='__manual__'){if(wifiSsidScanField){wifiSsidScanField.style.display='none';}if(wifiSsidManualField){wifiSsidManualField.style.display='';}var inp=document.getElementById('wifi_ssid');if(inp){inp.focus();}return;}if(wifiSsidSelect.value){const wifiInput=document.getElementById('wifi_ssid');if(wifiInput){wifiInput.value=wifiSsidSelect.value;}}});}";  
  html += "if(wifiScanButton){wifiScanButton.addEventListener('click',refreshWifiScan);}";
  html += "arcInputIds.forEach((id)=>{const input=document.getElementById(id);if(!input)return;"
          "input.addEventListener('input',queueArcPreview);input.addEventListener('change',commitArcColors);});";
  html += "updateDisplayRotationControls();updatePortalAccessControls();updateSourceModeControls();updateHealth();healthTimer=setInterval(updateHealth,4000);window.addEventListener('beforeunload',()=>{if(healthTimer){clearInterval(healthTimer);healthTimer=null;}stopCloudFollowup();stopLocalFollowup();});";

  // --- Printer selection JS ---
  // Auto-refresh guard: if the page was rendered while the Cloud binding call
  // hadn't returned yet (or with a stale legacy NVS before migration), the
  // server-rendered count is 0.  Poll /api/printers every 4 s and reload the
  // page when the backend reports more profiles/cloud devices than we were
  // rendered with.  This specifically addresses the "Printers page empty"
  // report for users with ≥1 cloud-bound printer — their devices appear after
  // the Cloud login/bindings fetch completes (T ≈ 5-45 s after boot).
  html += "(function(){const list=document.getElementById('printer-list');"
          "if(!list)return;"
          "const initial=parseInt(list.dataset.initialCount||'0',10);"
          "let pollTimer=null;let polls=0;"
          "async function poll(){polls++;"
          "try{const r=await fetch('/api/printers',{cache:'no-store'});"
          "if(!r.ok)return;const d=await r.json();"
          "const total=((d.profiles||[]).length)+((d.cloud_devices||[]).length);"
          "if(total>initial){clearInterval(pollTimer);pollTimer=null;location.reload();}"
          "}catch(e){}"
          // Stop polling after ~3 minutes to avoid indefinite network chatter.
          "if(polls>=45){clearInterval(pollTimer);pollTimer=null;}}"
          "pollTimer=setInterval(poll,4000);"
          "window.addEventListener('beforeunload',()=>{if(pollTimer){clearInterval(pollTimer);pollTimer=null;}});"
          "})();";

  html += "(function(){const btn=document.getElementById('load-cloud-printers-btn');"
          "if(!btn)return;"
          "const st=document.getElementById('cloud-printers-status');"
          "const list=document.getElementById('printer-list');"
          "btn.addEventListener('click',async()=>{"
          "btn.disabled=true;if(st)st.textContent='Loading...';"
          "try{const r=await fetch('/api/printers');const d=await r.json();"
          "const savedSerials=new Set((d.profiles||[]).map(p=>p.serial));"
          "const cloud=(d.cloud_devices||[]).filter(c=>!savedSerials.has(c.serial));"
          "if(cloud.length===0){if(st)st.textContent='No additional cloud printers found.';btn.disabled=false;return;}"
          "list.querySelectorAll('.printer-card.cloud-only').forEach(c=>c.remove());"
          "cloud.forEach(cd=>{"
          "const card=document.createElement('div');card.className='printer-card cloud-only';card.dataset.serial=cd.serial;"
          "const name=cd.display_name||cd.model||cd.serial;"
          "card.innerHTML=\'<div class=\"printer-card-header\"><strong>\'+name+\'</strong><span class=\"summary-pill info\">Cloud only</span></div>\'"
          "+\'<div class=\"printer-card-body\"><span class=\"printer-tag\">\'+(cd.model||\'\')+\'</span>\'"
          "+\'<span class=\"printer-tag\">\'+( cd.online?\"Online\":\"Offline\")+\'</span>\'"
          "+\'<span class=\"printer-tag\">\'+(cd.serial||\'\').substring(0,6)+\'...</span></div>\'"
          "+\'<div class=\"printer-card-actions\"><button type=\"button\" class=\"secondary printer-add-cloud-btn\" data-serial=\"\'+cd.serial+\'\" data-name=\"\'+( cd.display_name||\'\')+\'\" data-model=\"\'+( cd.model||\'\')+\'\">Add to Profiles</button></div>\';"
          "list.appendChild(card);});"
          "list.querySelectorAll('.printer-add-cloud-btn').forEach(b=>{"
          "b.addEventListener('click',async()=>{"
          "b.disabled=true;"
          "try{const r2=await fetch('/api/printers/save',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({serial:b.dataset.serial,display_name:b.dataset.name,model:b.dataset.model,cloud_bound:true})});"
          "if(r2.ok){location.reload();}else{b.disabled=false;}}catch(e){b.disabled=false;}});});"
          "if(st)st.textContent=cloud.length+' cloud printer'+(cloud.length>1?'s':'')+' found.';"
          "btn.disabled=false;"
          "}catch(e){if(st)st.textContent='Failed to load cloud printers.';btn.disabled=false;}});})();";
  html += "document.querySelectorAll('.printer-name-input').forEach(inp=>{"
          "let timer=null;"
          "inp.addEventListener('keydown',(e)=>{if(e.key==='Enter'){e.preventDefault();inp.blur();}});"
          "inp.addEventListener('input',()=>{"
          "clearTimeout(timer);"
          "timer=setTimeout(async()=>{"
          "const name=inp.value.trim();"
          "if(!name)return;"
          "try{await fetch('/api/printers/save',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({serial:inp.dataset.serial,display_name:name})});"
          "}catch(e){}},800);});});";
  html += "document.querySelectorAll('.printer-select-btn').forEach(btn=>{"
          "btn.addEventListener('click',async()=>{"
          "btn.disabled=true;setStatus('Switching printer...','Reconnecting all paths to the selected printer.',10000);"
          "try{const r=await fetch('/api/printers/select',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({index:parseInt(btn.dataset.index)})});"
          "const body=await r.json();"
          "if(r.ok){setStatus('Printer switched','Now monitoring '+( body.printer||'selected printer')+'. The page will reload shortly.',5000);"
          "setTimeout(()=>location.reload(),2000);}else{setStatus(body.error||'Switch failed',body.detail||'',6000);btn.disabled=false;}}"
          "catch(e){setStatus('Switch failed','Request could not be completed.',6000);btn.disabled=false;}});});";
  html += "document.querySelectorAll('.printer-clear-local-btn').forEach(btn=>{"
          "btn.addEventListener('click',async()=>{"
          "if(!confirm('Clear local connection (host & access code) for this printer?'))return;"
          "btn.disabled=true;"
          "try{const r=await fetch('/api/printers/clear-local',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({index:parseInt(btn.dataset.index)})});"
          "if(r.ok){location.reload();}else{const body=await r.json();setStatus(body.error||'Clear failed',body.detail||'',6000);btn.disabled=false;}}"
          "catch(e){setStatus('Clear failed','',6000);btn.disabled=false;}});});";
  html += "document.querySelectorAll('.printer-delete-btn').forEach(btn=>{"
          "btn.addEventListener('click',async()=>{"
          "if(!confirm('Delete this printer profile?'))return;"
          "btn.disabled=true;"
          "try{const r=await fetch('/api/printers/delete',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({index:parseInt(btn.dataset.index)})});"
          "if(r.ok){location.reload();}else{const body=await r.json();setStatus(body.error||'Delete failed',body.detail||'',6000);btn.disabled=false;}}"
          "catch(e){setStatus('Delete failed','',6000);btn.disabled=false;}});});";
  html += "document.querySelectorAll('.printer-add-cloud-btn').forEach(btn=>{"
          "btn.addEventListener('click',async()=>{"
          "btn.disabled=true;"
          "try{const r=await fetch('/api/printers/save',{method:'POST',headers:{'Content-Type':'application/json'},"
          "body:JSON.stringify({serial:btn.dataset.serial,display_name:btn.dataset.name,model:btn.dataset.model})});"
          "if(r.ok){location.reload();}else{const body=await r.json();setStatus(body.error||'Save failed',body.detail||'',6000);btn.disabled=false;}}"
          "catch(e){setStatus('Save failed','',6000);btn.disabled=false;}});});";

  html += "(function(){";
  html += "var otaFile=document.getElementById('ota_file');";
  html += "var otaBtn=document.getElementById('ota-upload-button');";
  html += "var otaStatus=document.getElementById('ota-status');";
  html += "var otaWrap=document.getElementById('ota-progress-wrap');";
  html += "var otaBar=document.getElementById('ota-progress-bar');";
  html += "if(!otaBtn||!otaFile)return;";
  html += "otaBtn.addEventListener('click',function(){";
  html += "var file=otaFile.files&&otaFile.files[0];";
  html += "if(!file){otaStatus.textContent='Select a .bin file first.';return;}";
  html += "if(!confirm('This will flash new firmware and restart PrintSphere. Continue?'))return;";
  html += "otaBtn.disabled=true;otaWrap.style.display='';otaBar.style.width='0%';";
  html += "otaStatus.textContent='Uploading...';";
  html += "var xhr=new XMLHttpRequest();";
  html += "xhr.upload.onprogress=function(e){if(e.lengthComputable){";
  html += "var pct=Math.round(e.loaded/e.total*100);";
  html += "otaBar.style.width=pct+'%';";
  html += "otaStatus.textContent='Uploading '+pct+'% ('+Math.round(e.loaded/1024)+' KB)';}};";
  html += "xhr.onload=function(){";
  html += "var body={};try{body=JSON.parse(xhr.responseText||'{}');}catch(e){}";
  html += "if(xhr.status===200){otaBar.style.width='100%';";
  html += "otaStatus.textContent='Flash successful - device is restarting...';";
  html += "setStatus('Firmware flashed','PrintSphere is rebooting to the new firmware now.',60000);}";
  html += "else{otaStatus.textContent='Upload failed: '+(body.error||xhr.statusText||'unknown error');otaBtn.disabled=false;}};";
  html += "xhr.onerror=function(){otaStatus.textContent='Upload failed - network error.';otaBtn.disabled=false;};";
  html += "xhr.open('POST','/api/ota/upload');";
  html += "xhr.setRequestHeader('Content-Type','application/octet-stream');";
  html += "xhr.send(file);});";
  html += "var otaUrlInput=document.getElementById('ota_url');";
  html += "var otaUrlBtn=document.getElementById('ota-url-button');";
  html += "var otaUrlStatus=document.getElementById('ota-url-status');";
  html += "var otaUrlWrap=document.getElementById('ota-url-progress-wrap');";
  html += "var otaUrlBar=document.getElementById('ota-url-progress-bar');";
  html += "var otaUrlPoll=null;";
  html += "function githubToRaw(u){"
          "if(u.indexOf('github.com/')===-1)return u;"
          "var p=u.replace('https://github.com/','');"
          "var parts=p.split('/');"
          "if(parts.length<4||parts[2]!=='blob')return u;"
          "return 'https://raw.githubusercontent.com/'+parts[0]+'/'+parts[1]+'/'+parts.slice(3).join('/');}";
  html += "function startOtaUrlPoll(){"
          "var poll=function(){"
          "fetch('/api/ota/status',{cache:'no-store'})"
          ".then(function(r){return r.json();})"
          ".then(function(b){"
          "if(b.state==='downloading'){"
          "if(otaUrlWrap)otaUrlWrap.style.display='';"
          "if(otaUrlBar)otaUrlBar.style.width=(b.progress||0)+'%';"
          "if(otaUrlStatus)otaUrlStatus.textContent='Downloading '+(b.progress||0)+'%...';"
          "otaUrlPoll=setTimeout(poll,600);"
          "}else if(b.state==='done'){"
          "if(otaUrlBar)otaUrlBar.style.width='100%';"
          "if(otaUrlStatus)otaUrlStatus.textContent='Flash successful \u2014 device is restarting...';"
          "setStatus('Firmware flashed','PrintSphere is rebooting to the new firmware now.',60000);"
          "}else if(b.state==='failed'){"
          "if(otaUrlStatus)otaUrlStatus.textContent='Flash failed: '+(b.error||'unknown error');"
          "if(otaUrlBtn)otaUrlBtn.disabled=false;"
          "}})"
          ".catch(function(){otaUrlPoll=setTimeout(poll,1500);});"
          "};otaUrlPoll=setTimeout(poll,600);}";
  html += "if(otaUrlBtn){"
          "otaUrlBtn.addEventListener('click',function(){"
          "if(otaUrlPoll){clearTimeout(otaUrlPoll);otaUrlPoll=null;}"
          "var raw=otaUrlInput?githubToRaw(otaUrlInput.value.trim()):'';"
          "if(!raw){if(otaUrlStatus)otaUrlStatus.textContent='Enter a URL first.';return;}"
          "if(raw.indexOf('https://')!==0){if(otaUrlStatus)otaUrlStatus.textContent='Only HTTPS URLs are supported.';return;}"
          "if(!confirm('Flash firmware from:\\n'+raw+'\\n\\nThe device restarts immediately. Continue?'))return;"
          "otaUrlBtn.disabled=true;"
          "if(otaUrlWrap)otaUrlWrap.style.display='';"
          "if(otaUrlBar)otaUrlBar.style.width='0%';"
          "if(otaUrlStatus)otaUrlStatus.textContent='Starting download...';"
          "fetch('/api/ota/url',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({url:raw})})"
          ".then(function(r){return r.json().then(function(b){return{ok:r.ok,body:b};});})"
          ".then(function(res){"
          "if(res.ok){startOtaUrlPoll();}"
          "else{if(otaUrlStatus)otaUrlStatus.textContent='Error: '+(res.body&&res.body.error?res.body.error:'request failed');otaUrlBtn.disabled=false;}"
          "}).catch(function(){if(otaUrlStatus)otaUrlStatus.textContent='Request failed.';otaUrlBtn.disabled=false;});"
          "});}";
  html += "})();";
  html += "</script>";

#ifdef PRINTSPHERE_DEBUG_BUILD
  // Debug console JS — isolated IIFE, runs only in debug builds.
  html += "<script>";
  html += "(function(){";
  html += "var dbgLog=document.getElementById('dbg-log');";
  html += "var dbgTerminal=document.getElementById('dbg-terminal');";
  html += "var dbgStatus=document.getElementById('dbg-status');";
  html += "var dbgOffset=0;";
  html += "var dbgFull='';";
  html += "if(!dbgLog)return;";  // section absent (e.g. not logged in)
  html += "function dbgPoll(){"
          "fetch('/api/debug/log?offset='+dbgOffset,{credentials:'same-origin',cache:'no-store'})"
          ".then(function(r){"
          "var end=parseInt(r.headers.get('X-Log-End-Offset')||String(dbgOffset));"
          "return r.text().then(function(t){return{t:t,end:end};});"
          "}).then(function(res){"
          "if(res.t&&res.t.length>0){"
          "dbgFull+=res.t;dbgOffset=res.end;"
          "dbgLog.textContent=dbgFull;"
          "var as=document.getElementById('dbg-autoscroll');"
          "if(as&&as.checked&&dbgTerminal){dbgTerminal.scrollTop=dbgTerminal.scrollHeight;}"
          "}"
          "if(dbgStatus)dbgStatus.textContent='Last update: '+new Date().toLocaleTimeString();"
          "}).catch(function(){if(dbgStatus)dbgStatus.textContent='Polling error';});"
          "}";
  html += "setInterval(dbgPoll,1500);dbgPoll();";
  html += "var dlBtn=document.getElementById('dbg-download-btn');";
  html += "if(dlBtn){dlBtn.addEventListener('click',function(){"
          "var blob=new Blob([dbgFull],{type:'text/plain'});"
          "var url=URL.createObjectURL(blob);"
          "var a=document.createElement('a');a.href=url;"
          "a.download='printsphere_debug_log.txt';"
          "document.body.appendChild(a);a.click();"
          "document.body.removeChild(a);URL.revokeObjectURL(url);"
          "});}";
  html += "var clrBtn=document.getElementById('dbg-clear-btn');";
  html += "if(clrBtn){clrBtn.addEventListener('click',function(){"
          "dbgFull='';if(dbgLog)dbgLog.textContent='';"
          "if(dbgStatus)dbgStatus.textContent='Cleared';"
          "});}";
  html += "})();";
  html += "</script>";
#endif
  html += "</main></body></html>";

  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, html.c_str(), html.size());
}

esp_err_t SetupPortal::handle_favicon(httpd_req_t* request) {
  httpd_resp_set_type(request, "image/svg+xml");
  httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
  return httpd_resp_send(request, kFaviconSvg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SetupPortal::handle_health(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  const bool request_authorized = portal->is_request_authorized(request);
  const PortalAccessSnapshot access = portal->access_snapshot(request_authorized);

  std::string body = "{";
  body += "\"status\":\"ok\",";
  body += "\"portal\":\"setup\"";
  append_portal_access_fields(&body, access);
  if (request_authorized) {
    body += ",\"source_mode\":\"";
    body += to_string(portal->config_store_.load_source_mode());
    body += "\",";
    body += "\"wifi_connected\":";
    body += (portal->wifi_manager_.is_station_connected() ? "true" : "false");
    body += ",";
    body += "\"wifi_ip\":\"" + json_escape(portal->wifi_manager_.station_ip()) + "\"";
    const BambuCloudSnapshot cloud = portal->cloud_client_.refreshed_snapshot();
    const PrinterSnapshot local = portal->printer_client_.snapshot();
    append_cloud_status_fields(&body, cloud);
    append_local_status_fields(&body, local, portal->printer_client_.is_configured());
  }
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_unlock(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  if (!portal->is_lock_required()) {
    const PortalAccessSnapshot access = portal->access_snapshot(true);
    std::string body = "{\"status\":\"open\",\"detail\":\"";
    body += json_escape(access.detail);
    body += "\"";
    append_portal_access_fields(&body, access);
    body += "}";
    send_json(request, body);
    return ESP_OK;
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const std::string pin = trim_copy(read_string_field(root, "pin"));
  cJSON_Delete(root);
  if (pin.empty()) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "unlock pin missing");
  }

  const uint64_t current_ms = now_ms();
  bool unlocked = false;
  {
    std::lock_guard<std::mutex> lock(portal->access_mutex_);
    portal->prune_access_state_locked(current_ms);
    if (!portal->unlock_pin_.empty() && pin == portal->unlock_pin_) {
      portal->session_token_ = generate_session_token();
      portal->session_expiry_ms_ = current_ms + kPortalSessionLifetimeMs;
      portal->unlock_pin_.clear();
      portal->unlock_pin_expiry_ms_ = 0;
      unlocked = true;
    }
  }

  if (!unlocked) {
    return portal->send_locked_response(request);
  }

  const PortalAccessSnapshot access = portal->access_snapshot(true);
  const std::string session_cookie =
      session_cookie_header(portal->session_token_,
                            static_cast<uint32_t>(kPortalSessionLifetimeMs / 1000ULL));
  httpd_resp_set_hdr(request, "Set-Cookie", session_cookie.c_str());

  std::string body = "{\"status\":\"unlocked\",\"detail\":\"";
  body += json_escape(access.detail);
  body += "\"";
  append_portal_access_fields(&body, access);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_session_extend(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  uint32_t remaining_s = 0;
  {
    const uint64_t current_ms = now_ms();
    std::lock_guard<std::mutex> lock(portal->access_mutex_);
    portal->prune_access_state_locked(current_ms);
    if (!portal->session_token_.empty()) {
      // Locked session: extend the session token expiry
      portal->session_expiry_ms_ += kPortalSessionExtendMs;
      remaining_s = remaining_seconds(portal->session_expiry_ms_, now_ms());
    } else if (portal->provisioning_grace_expiry_ms_ > current_ms) {
      // Grace period (no lock active): extend the grace window
      portal->provisioning_grace_expiry_ms_ += kPortalSessionExtendMs;
      remaining_s = static_cast<uint32_t>(
          (portal->provisioning_grace_expiry_ms_ - current_ms) / 1000ULL);
    }
  }

  if (!portal->session_token_.empty()) {
    const std::string cookie = session_cookie_header(
        portal->session_token_,
        remaining_s > 0 ? remaining_s : 0);
    httpd_resp_set_hdr(request, "Set-Cookie", cookie.c_str());
  }

  std::string body = "{\"status\":\"ok\",\"remaining_s\":";
  body += std::to_string(remaining_s);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_wifi_scan(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  const std::vector<std::string> networks = portal->wifi_manager_.scan_visible_networks();
  std::string body = "{\"status\":\"ok\",\"networks\":[";
  for (size_t i = 0; i < networks.size(); ++i) {
    if (i > 0U) {
      body += ",";
    }
    body += "\"";
    body += json_escape(networks[i]);
    body += "\"";
  }
  body += "]}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_get(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials cloud = portal->config_store_.load_cloud_credentials();
  const SourceMode source_mode = portal->config_store_.load_source_mode();
  const DisplayRotation display_rotation = portal->config_store_.load_display_rotation();
  const bool portal_lock_enabled = portal->config_store_.load_portal_lock_enabled();
  const PrinterConnection printer = portal->config_store_.load_active_printer_profile().to_connection();
  const ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const BatteryDisplayPolicy bat_policy_get = portal->config_store_.load_battery_display_policy();
  const BambuCloudSnapshot cloud_snapshot = portal->cloud_client_.snapshot();
  const std::string effective_printer_serial = [&]() -> std::string {
    if (!printer.serial.empty()) return printer.serial;
    const auto cloud_devs = portal->cloud_client_.get_cloud_devices();
    const auto profiles = portal->config_store_.load_printer_profiles();
    for (const auto& cd : cloud_devs) {
      bool has_local = false;
      for (const auto& p : profiles) {
        if (p.serial == cd.serial && p.has_local_config()) { has_local = true; break; }
      }
      if (!has_local) return cd.serial;
    }
    return cloud_snapshot.resolved_serial;
  }();

  std::string body = "{";
  body += "\"wifi_ssid\":\"" + json_escape(wifi.ssid) + "\",";
  body += "\"cloud_email\":\"" + json_escape(cloud.email) + "\",";
  body += "\"cloud_region\":\"" + json_escape(to_string(cloud.region)) + "\",";
  body += "\"printer_host\":\"" + json_escape(printer.host) + "\",";
  body += "\"printer_serial\":\"" + json_escape(effective_printer_serial) + "\",";
  body += "\"source_mode\":\"";
  body += to_string(source_mode);
  body += "\",";
  body += "\"display_rotation\":\"";
  body += to_string(display_rotation);
  body += "\",";
  body += "\"portal_lock_enabled\":";
  body += portal_lock_enabled ? "true" : "false";
  body += ",";
  body += "\"state_source\":\"";
  body += to_string(source_mode);
  body += "\",";
  body += "\"wifi_connected\":";
  body += (portal->wifi_manager_.is_station_connected() ? "true" : "false");
  body += ",";
  body += "\"wifi_ip\":\"" + json_escape(portal->wifi_manager_.station_ip()) + "\"";
  const PrinterSnapshot local_snapshot = portal->printer_client_.snapshot();
  append_cloud_status_fields(&body, cloud_snapshot);
  append_local_status_fields(&body, local_snapshot, portal->printer_client_.is_configured());
  body += ",\"arc_printing\":\"" + color_to_html_hex(arc_colors.printing) + "\"";
  body += ",\"arc_done\":\"" + color_to_html_hex(arc_colors.done) + "\"";
  body += ",\"arc_error\":\"" + color_to_html_hex(arc_colors.error) + "\"";
  body += ",\"arc_idle\":\"" + color_to_html_hex(arc_colors.idle) + "\"";
  body += ",\"arc_preheat\":\"" + color_to_html_hex(arc_colors.preheat) + "\"";
  body += ",\"arc_clean\":\"" + color_to_html_hex(arc_colors.clean) + "\"";
  body += ",\"arc_level\":\"" + color_to_html_hex(arc_colors.level) + "\"";
  body += ",\"arc_cool\":\"" + color_to_html_hex(arc_colors.cool) + "\"";
  body += ",\"arc_idle_active\":\"" + color_to_html_hex(arc_colors.idle_active) + "\"";
  body += ",\"arc_filament\":\"" + color_to_html_hex(arc_colors.filament) + "\"";
  body += ",\"arc_setup\":\"" + color_to_html_hex(arc_colors.setup) + "\"";
  body += ",\"arc_offline\":\"" + color_to_html_hex(arc_colors.offline) + "\"";
  body += ",\"arc_unknown\":\"" + color_to_html_hex(arc_colors.unknown) + "\"";
  body += ",\"bat_dim_enabled\":";
  body += bat_policy_get.dim_enabled ? "true" : "false";
  body += ",\"bat_dim_brightness\":";
  body += std::to_string(bat_policy_get.dim_brightness_percent);
  body += ",\"bat_off_enabled\":";
  body += bat_policy_get.screen_off_enabled ? "true" : "false";
  body += ",\"filament_wake\":";
  body += portal->config_store_.load_filament_wake_enabled() ? "true" : "false";
  body += ",\"filament_anim\":";
  body += portal->config_store_.load_filament_anim_enabled() ? "true" : "false";
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const WifiCredentials stored_wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials stored_cloud = portal->config_store_.load_cloud_credentials();
  const PrinterConnection stored_printer = portal->config_store_.load_active_printer_profile().to_connection();
  const std::string stored_cloud_access_token = portal->config_store_.load_cloud_access_token();
  const bool stored_portal_lock_enabled = portal->config_store_.load_portal_lock_enabled();

  const WifiCredentials wifi = merge_wifi_credentials({
      .ssid = trim_copy(read_string_field(root, "wifi_ssid")),
      .password = read_string_field(root, "wifi_password"),
  }, stored_wifi);

  const BambuCloudCredentials cloud = merge_cloud_credentials({
      .email = trim_copy(read_string_field(root, "cloud_email")),
      .password = read_string_field(root, "cloud_password"),
      .region = parse_cloud_region(trim_copy(read_string_field(root, "cloud_region"))),
  }, stored_cloud);
  const SourceMode source_mode = parse_source_mode_field(root);
  const DisplayRotation display_rotation = parse_display_rotation_field(root);
  const bool portal_lock_enabled =
      read_bool_field(root, "portal_lock_enabled", stored_portal_lock_enabled);
  const bool filament_wake =
      read_bool_field(root, "filament_wake", portal->config_store_.load_filament_wake_enabled());
  const bool filament_anim =
      read_bool_field(root, "filament_anim", portal->config_store_.load_filament_anim_enabled());

  const PrinterConnection printer = merge_printer_connection({
      .host = trim_copy(read_string_field(root, "printer_host")),
      .serial = trim_copy(read_string_field(root, "printer_serial")),
      .access_code = trim_copy(read_string_field(root, "printer_access_code")),
  }, stored_printer);

  ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const bool colors_valid = parse_arc_colors_from_json(root, &arc_colors);

  const BatteryDisplayPolicy stored_bat_policy = portal->config_store_.load_battery_display_policy();
  BatteryDisplayPolicy bat_policy_post;
  bat_policy_post.dim_enabled = read_bool_field(root, "bat_dim_enabled", stored_bat_policy.dim_enabled);
  bat_policy_post.screen_off_enabled = read_bool_field(root, "bat_off_enabled", stored_bat_policy.screen_off_enabled);
  {
    const std::string pct_str = trim_copy(read_string_field(root, "bat_dim_brightness"));
    if (!pct_str.empty()) {
      const long parsed = std::strtol(pct_str.c_str(), nullptr, 10);
      bat_policy_post.dim_brightness_percent = (parsed >= 0 && parsed <= 100) ? static_cast<int>(parsed) : stored_bat_policy.dim_brightness_percent;
    } else {
      bat_policy_post.dim_brightness_percent = stored_bat_policy.dim_brightness_percent;
    }
  }
  {
    const auto read_bu = [&](const char* key, uint32_t fallback) -> uint32_t {
      const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
      if (cJSON_IsNumber(item)) {
        const long v = static_cast<long>(item->valuedouble);
        if (v >= 1 && v <= 3600) return static_cast<uint32_t>(v);
      }
      return fallback;
    };
    bat_policy_post.dim_timeout_idle_s   = read_bu("bat_dim_timeout_idle",   stored_bat_policy.dim_timeout_idle_s);
    bat_policy_post.dim_timeout_active_s = read_bu("bat_dim_timeout_active", stored_bat_policy.dim_timeout_active_s);
    bat_policy_post.off_timeout_idle_s   = read_bu("bat_off_timeout_idle",   stored_bat_policy.off_timeout_idle_s);
    bat_policy_post.off_timeout_active_s = read_bu("bat_off_timeout_active", stored_bat_policy.off_timeout_active_s);
    bat_policy_post.usb_power_save_enabled = read_bool_field(root, "bat_usb_ps", stored_bat_policy.usb_power_save_enabled);
  }

  cJSON_Delete(root);

  if (!colors_valid) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid arc color value");
  }

  const bool local_requires_complete_fields =
      !printer.host.empty() || !printer.access_code.empty();
  const bool cloud_any = !cloud.email.empty() || !cloud.password.empty();
  const bool local_ready = printer.is_ready();
  const bool cloud_ready =
      cloud.is_configured() || can_reuse_cloud_session(cloud, stored_cloud, stored_cloud_access_token);

  if (!local_ready && local_requires_complete_fields) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "local printer fields incomplete");
  }
  if (!cloud_ready && cloud_any) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "cloud account fields incomplete");
  }
  if (!wifi.is_configured() && !local_ready && !cloud_ready) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "configure Wi-Fi, cloud or local printer access");
  }
  if (!printer.host.empty() && !is_valid_printer_host(printer.host)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "printer host must be full IPv4 or hostname");
  }

  ESP_LOGI(kTag,
           "Saving config: wifi_ssid=%s cloud_email_len=%u source_mode=%s portal_lock=%s local_host=%s serial_len=%u access_len=%u",
           wifi.ssid.c_str(), static_cast<unsigned int>(cloud.email.size()),
           to_string(source_mode), portal_lock_enabled ? "enabled" : "disabled",
           printer.host.c_str(),
           static_cast<unsigned int>(printer.serial.size()),
           static_cast<unsigned int>(printer.access_code.size()));

  ESP_RETURN_ON_ERROR(portal->config_store_.save_wifi_credentials(wifi), kTag, "save wifi failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_cloud_credentials(cloud), kTag,
                      "save cloud failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_display_rotation(display_rotation), kTag,
                      "save display rotation failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_portal_lock_enabled(portal_lock_enabled), kTag,
                      "save portal lock failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_filament_wake_enabled(filament_wake), kTag,
                      "save filament wake failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_filament_anim_enabled(filament_anim), kTag,
                      "save filament anim failed");
  // Update active printer profile with form values
  {
    PrinterProfile profile = portal->config_store_.load_active_printer_profile();
    if (!printer.serial.empty()) {
      profile.serial = printer.serial;
      profile.host = printer.host;
      profile.access_code = printer.access_code;
      portal->config_store_.save_printer_profile(profile);
    }
  }
  ESP_RETURN_ON_ERROR(portal->config_store_.save_arc_color_scheme(arc_colors), kTag,
                      "save arc colors failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_battery_display_policy(bat_policy_post), kTag,
                      "save battery display policy failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_arc_preview(httpd_req_t* request) {
  return handle_arc_update(request, false);
}

esp_err_t SetupPortal::handle_arc_commit(httpd_req_t* request) {
  return handle_arc_update(request, true);
}

esp_err_t SetupPortal::handle_arc_update(httpd_req_t* request, bool persist) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const bool colors_valid = parse_arc_colors_from_json(root, &arc_colors);
  cJSON_Delete(root);

  if (!colors_valid) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid arc color value");
  }

  portal->ui_.set_arc_color_scheme(arc_colors);
  if (persist) {
    const esp_err_t save_err = portal->config_store_.save_arc_color_scheme(arc_colors);
    if (save_err != ESP_OK) {
      ESP_LOGE(kTag, "save live arc colors failed: %s", esp_err_to_name(save_err));
      return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "save live arc colors failed");
    }
  }

  std::string body = "{\"status\":\"";
  body += persist ? "saved" : "previewed";
  body += "\",\"persisted\":";
  body += persist ? "true" : "false";
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_source_mode_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving source mode only: %s", to_string(source_mode));
  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_display_rotation_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const DisplayRotation rotation = parse_display_rotation_field(root);
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving display rotation only: %s", to_string(rotation));
  ESP_RETURN_ON_ERROR(portal->config_store_.save_display_rotation(rotation), kTag,
                      "save display rotation failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_battery_display_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const BatteryDisplayPolicy stored = portal->config_store_.load_battery_display_policy();
  BatteryDisplayPolicy policy;
  policy.dim_enabled = read_bool_field(root, "bat_dim_enabled", stored.dim_enabled);
  policy.screen_off_enabled = read_bool_field(root, "bat_off_enabled", stored.screen_off_enabled);
  {
    const std::string pct_str = trim_copy(read_string_field(root, "bat_dim_brightness"));
    if (!pct_str.empty()) {
      const long parsed = std::strtol(pct_str.c_str(), nullptr, 10);
      policy.dim_brightness_percent = (parsed >= 0 && parsed <= 100) ? static_cast<int>(parsed) : stored.dim_brightness_percent;
    } else {
      policy.dim_brightness_percent = stored.dim_brightness_percent;
    }
  }
  const auto read_bat_uint = [&](const char* key, uint32_t fallback) -> uint32_t {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
      const long v = static_cast<long>(item->valuedouble);
      if (v >= 1 && v <= 3600) return static_cast<uint32_t>(v);
    }
    return fallback;
  };
  policy.dim_timeout_idle_s   = read_bat_uint("bat_dim_timeout_idle",   stored.dim_timeout_idle_s);
  policy.dim_timeout_active_s = read_bat_uint("bat_dim_timeout_active", stored.dim_timeout_active_s);
  policy.off_timeout_idle_s   = read_bat_uint("bat_off_timeout_idle",   stored.off_timeout_idle_s);
  policy.off_timeout_active_s = read_bat_uint("bat_off_timeout_active", stored.off_timeout_active_s);
  policy.usb_power_save_enabled = read_bool_field(root, "bat_usb_ps", stored.usb_power_save_enabled);
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving energy policy: dim=%s pct=%d dim_idle=%us dim_act=%us off=%s off_idle=%us off_act=%us usb_ps=%s",
           policy.dim_enabled ? "yes" : "no", policy.dim_brightness_percent,
           policy.dim_timeout_idle_s, policy.dim_timeout_active_s,
           policy.screen_off_enabled ? "yes" : "no",
           policy.off_timeout_idle_s, policy.off_timeout_active_s,
           policy.usb_power_save_enabled ? "yes" : "no");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_battery_display_policy(policy), kTag,
                      "save battery display policy failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_portal_access_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const bool current_enabled = portal->config_store_.load_portal_lock_enabled();
  const bool portal_lock_enabled = read_bool_field(root, "portal_lock_enabled", current_enabled);
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving portal access only: %s",
           portal_lock_enabled ? "lock enabled" : "lock disabled");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_portal_lock_enabled(portal_lock_enabled), kTag,
                      "save portal lock failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_ams_display_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const bool filament_wake = read_bool_field(root, "filament_wake",
      portal->config_store_.load_filament_wake_enabled());
  const bool filament_anim = read_bool_field(root, "filament_anim",
      portal->config_store_.load_filament_anim_enabled());
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving AMS display: wake=%d anim=%d",
           filament_wake, filament_anim);
  ESP_RETURN_ON_ERROR(portal->config_store_.save_filament_wake_enabled(filament_wake), kTag,
                      "save filament wake failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_filament_anim_enabled(filament_anim), kTag,
                      "save filament anim failed");

  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_cloud_connect(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const BambuCloudCredentials stored_cloud = portal->config_store_.load_cloud_credentials();
  const std::string stored_cloud_access_token = portal->config_store_.load_cloud_access_token();
  const BambuCloudCredentials cloud = merge_cloud_credentials({
      .email = trim_copy(read_string_field(root, "cloud_email")),
      .password = read_string_field(root, "cloud_password"),
      .region = parse_cloud_region(trim_copy(read_string_field(root, "cloud_region"))),
  }, stored_cloud);
  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  if (!cloud.is_configured() &&
      !can_reuse_cloud_session(cloud, stored_cloud, stored_cloud_access_token)) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Cloud credentials incomplete\",\"detail\":\"Enter both Bambu email and password first.\"}");
    return ESP_OK;
  }

  if (!portal->wifi_manager_.is_station_connected()) {
    httpd_resp_set_status(request, "409 Conflict");
    send_json(request,
              "{\"error\":\"Wi-Fi not ready\",\"detail\":\"Save Wi-Fi and reboot first. Cloud login starts after the ESP is on your home network.\"}");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(portal->config_store_.save_cloud_credentials(cloud), kTag, "save cloud failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");
  if (source_mode == SourceMode::kLocalOnly) {
    send_json(
        request,
        "{\"status\":\"saved\",\"detail\":\"Cloud credentials saved. Switch source mode to Hybrid or Cloud only to connect.\",\"cloud_connected\":false,\"cloud_verification_required\":false,\"cloud_tfa_required\":false,\"cloud_configured\":true,\"cloud_detail\":\"Cloud credentials saved. Switch source mode to Hybrid or Cloud only to connect.\",\"cloud_resolved_serial\":\"\"}");
    return ESP_OK;
  }
  portal->cloud_client_.request_reload_from_store();

  const BambuCloudSnapshot before = portal->cloud_client_.snapshot();
  BambuCloudSnapshot current = before;
  for (int attempt = 0; attempt < 60; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = portal->cloud_client_.refreshed_snapshot();
    if (cloud_connect_result_ready(before, current)) {
      break;
    }
  }

  const bool login_still_pending = cloud_login_still_pending(current);
  if (current.setup_stage == CloudSetupStage::kFailed) {
    httpd_resp_set_status(request, "502 Bad Gateway");
  }

  std::string body = "{\"status\":\"";
  if (cloud_portal_ready(current)) {
    body += "connected";
  } else if (current.verification_required) {
    body += "verification_required";
  } else if (current.setup_stage == CloudSetupStage::kFailed) {
    body += "failed";
  } else if (login_still_pending) {
    body += "queued";
  } else {
    body += "saved";
  }
  body += "\",\"detail\":\"";
  body += json_escape(current.detail);
  body += "\"";
  append_cloud_status_fields(&body, current);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_cloud_verify(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const std::string code = trim_copy(read_string_field(root, "code"));
  cJSON_Delete(root);
  if (code.empty()) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "verification code missing");
  }

  BambuCloudSnapshot snapshot = portal->cloud_client_.snapshot();
  const std::string previous_detail = snapshot.detail;
  const std::string previous_resolved_serial = snapshot.resolved_serial;
  const bool previous_connected = snapshot.connected;
  const bool previous_session_connected = snapshot.session_connected;
  const bool previous_verification_required = snapshot.verification_required;
  const bool previous_configured = snapshot.configured;
  const CloudSetupStage previous_setup_stage = snapshot.setup_stage;
  BambuCloudSnapshot previous_snapshot;
  previous_snapshot.configured = previous_configured;
  previous_snapshot.connected = previous_connected;
  previous_snapshot.session_connected = previous_session_connected;
  previous_snapshot.setup_stage = previous_setup_stage;
  previous_snapshot.detail = previous_detail;
  previous_snapshot.resolved_serial = previous_resolved_serial;
  previous_snapshot.verification_required = previous_verification_required;

  portal->cloud_client_.submit_verification_code(code);
  for (int attempt = 0; attempt < 80; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    snapshot = portal->cloud_client_.refreshed_snapshot();
    if (cloud_verify_result_ready(previous_snapshot, snapshot)) {
      break;
    }
  }

  const bool login_still_pending = cloud_login_still_pending(snapshot);
  if (snapshot.verification_required) {
    httpd_resp_set_status(request, "409 Conflict");
  } else if (snapshot.setup_stage == CloudSetupStage::kFailed ||
             (!cloud_portal_ready(snapshot) && !login_still_pending)) {
    httpd_resp_set_status(request, "502 Bad Gateway");
  }

  std::string body = "{\"status\":\"";
  if (cloud_portal_ready(snapshot)) {
    body += "connected";
  } else if (snapshot.verification_required) {
    body += "verification_required";
  } else if (login_still_pending) {
    body += "queued";
  } else {
    body += "failed";
  }
  body += "\",\"detail\":\"";
  body += json_escape(snapshot.detail);
  body += "\"";
  append_cloud_status_fields(&body, snapshot);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_local_connect(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const PrinterConnection stored_printer = portal->config_store_.load_active_printer_profile().to_connection();
  const PrinterConnection printer = merge_printer_connection({
      .host = trim_copy(read_string_field(root, "printer_host")),
      .serial = trim_copy(read_string_field(root, "printer_serial")),
      .access_code = trim_copy(read_string_field(root, "printer_access_code")),
  }, stored_printer);
  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  if (!printer.is_ready()) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Local printer fields incomplete\",\"detail\":\"Enter printer host, serial and access code first.\"}");
    return ESP_OK;
  }
  if (!is_valid_printer_host(printer.host)) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Invalid printer host\",\"detail\":\"Printer host must be a full IPv4 address or hostname.\"}");
    return ESP_OK;
  }
  if (!portal->wifi_manager_.is_station_connected()) {
    httpd_resp_set_status(request, "409 Conflict");
    send_json(request,
              "{\"error\":\"Wi-Fi not ready\",\"detail\":\"Save Wi-Fi and reboot first. The local path starts after the ESP is on your home network.\"}");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(portal->config_store_.save_source_mode(source_mode), kTag,
                      "save source mode failed");

  // Upsert into multi-profile system
  {
    auto profiles = portal->config_store_.load_printer_profiles();
    PrinterProfile profile;
    bool found = false;
    for (const auto& p : profiles) {
      if (p.serial == printer.serial) { profile = p; found = true; break; }
    }
    if (!found) {
      profile.index = static_cast<uint8_t>(profiles.size());
    }
    profile.serial = printer.serial;
    profile.host = printer.host;
    profile.access_code = printer.access_code;
    if (profile.display_name.empty() || profile.model.empty() || !profile.cloud_bound) {
      // Try to resolve model name from cloud device list
      const auto cloud_devs = portal->cloud_client_.get_cloud_devices();
      for (const auto& cd : cloud_devs) {
        if (cd.serial == printer.serial) {
          if (profile.display_name.empty()) {
            profile.display_name = !cd.display_name.empty() ? cd.display_name : to_string(cd.model);
          }
          if (profile.model.empty()) {
            profile.model = to_string(cd.model);
          }
          profile.cloud_bound = true;
          break;
        }
      }
    }
    if (profile.index < kMaxPrinterProfiles) {
      portal->config_store_.save_printer_profile(profile);
      portal->config_store_.save_active_printer_index(profile.index);
    }
  }
  if (source_mode == SourceMode::kCloudOnly) {
    send_json(
        request,
        "{\"status\":\"saved\",\"detail\":\"Local printer credentials saved. Switch source mode to Hybrid or Local only to connect.\",\"local_error\":false,\"local_connected\":false,\"local_configured\":true,\"local_detail\":\"Local printer credentials saved. Switch source mode to Hybrid or Local only to connect.\"}");
    return ESP_OK;
  }

  const PrinterSnapshot before = portal->printer_client_.snapshot();
  portal->printer_client_.configure(printer);
  portal->camera_client_.configure(printer);

  PrinterSnapshot current = before;
  for (int attempt = 0; attempt < 80; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = portal->printer_client_.snapshot();
    if (current.connection == PrinterConnectionState::kOnline ||
        current.connection == PrinterConnectionState::kError ||
        current.local_configured != before.local_configured ||
        current.detail != before.detail || current.stage != before.stage ||
        current.resolved_serial != before.resolved_serial) {
      break;
    }
  }

  const bool local_pending = current.local_configured &&
                             current.connection != PrinterConnectionState::kOnline &&
                             current.connection != PrinterConnectionState::kError;
  if (current.connection == PrinterConnectionState::kError) {
    httpd_resp_set_status(request, "502 Bad Gateway");
  }

  std::string body = "{\"status\":\"";
  if (current.connection == PrinterConnectionState::kOnline) {
    body += "connected";
  } else if (current.connection == PrinterConnectionState::kError) {
    body += "error";
  } else if (local_pending) {
    body += "queued";
  } else {
    body += "saved";
  }
  body += "\",\"detail\":\"";
  body += json_escape(current.detail);
  body += "\"";
  append_local_status_fields(&body, current, true);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Printer profile REST endpoints
// ---------------------------------------------------------------------------

esp_err_t SetupPortal::handle_printers_get(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) return ESP_FAIL;
  if (!portal->is_request_authorized(request)) return portal->send_locked_response(request);

  const auto profiles = portal->config_store_.load_printer_profiles();
  const uint8_t active_idx = portal->config_store_.load_active_printer_index();
  const auto cloud_devices = portal->cloud_client_.get_cloud_devices();

  std::string body = "{\"active\":";
  body += std::to_string(active_idx);
  body += ",\"profiles\":[";
  for (size_t i = 0; i < profiles.size(); ++i) {
    if (i > 0) body += ",";
    const auto& p = profiles[i];
    body += "{\"index\":";
    body += std::to_string(p.index);
    body += ",\"serial\":\"";
    body += json_escape(p.serial);
    body += "\",\"host\":\"";
    body += json_escape(p.host);
    body += "\",\"display_name\":\"";
    body += json_escape(p.display_name);
    body += "\",\"model\":\"";
    body += json_escape(p.model);
    body += "\",\"has_local\":";
    body += p.has_local_config() ? "true" : "false";
    body += ",\"cloud_bound\":";
    body += p.cloud_bound ? "true" : "false";
    body += "}";
  }
  body += "],\"cloud_devices\":[";
  for (size_t i = 0; i < cloud_devices.size(); ++i) {
    if (i > 0) body += ",";
    const auto& cd = cloud_devices[i];
    body += "{\"serial\":\"";
    body += json_escape(cd.serial);
    body += "\",\"display_name\":\"";
    body += json_escape(cd.display_name);
    body += "\",\"model\":\"";
    body += to_string(cd.model);
    body += "\",\"online\":";
    body += cd.online ? "true" : "false";
    body += "}";
  }
  body += "]}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_printers_select(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) return ESP_FAIL;
  if (!portal->is_request_authorized(request)) return portal->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const cJSON* index_item = cJSON_GetObjectItem(root, "index");
  if (!cJSON_IsNumber(index_item)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "index field required");
  }
  const uint8_t new_index = static_cast<uint8_t>(cJSON_GetNumberValue(index_item));
  cJSON_Delete(root);

  const auto profiles = portal->config_store_.load_printer_profiles();
  const PrinterProfile* selected = nullptr;
  for (const auto& p : profiles) {
    if (p.index == new_index) { selected = &p; break; }
  }
  if (selected == nullptr) {
    httpd_resp_set_status(request, "404 Not Found");
    send_json(request, "{\"error\":\"Profile not found\"}");
    return ESP_OK;
  }

  portal->config_store_.save_active_printer_index(new_index);

  // Live-reconnect all clients
  const PrinterConnection conn = selected->to_connection();
  if (conn.is_ready()) {
    portal->printer_client_.configure(conn);
    portal->camera_client_.configure(conn);
  }
  const BambuCloudCredentials cloud_creds = portal->config_store_.load_cloud_credentials();
  portal->cloud_client_.configure(cloud_creds, selected->serial);

  std::string body = "{\"status\":\"ok\",\"printer\":\"";
  body += json_escape(selected->display_name.empty() ? selected->serial : selected->display_name);
  body += "\"}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_printers_save(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) return ESP_FAIL;
  if (!portal->is_request_authorized(request)) return portal->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const std::string serial = trim_copy(read_string_field(root, "serial"));
  const std::string host = trim_copy(read_string_field(root, "host"));
  const std::string access_code = trim_copy(read_string_field(root, "access_code"));
  const std::string display_name = trim_copy(read_string_field(root, "display_name"));
  const std::string model = trim_copy(read_string_field(root, "model"));
  const cJSON* cloud_bound_item = cJSON_GetObjectItem(root, "cloud_bound");
  const bool cloud_bound_explicit = cJSON_IsBool(cloud_bound_item) && cJSON_IsTrue(cloud_bound_item);
  cJSON_Delete(root);

  if (serial.empty()) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request, "{\"error\":\"Serial number is required\"}");
    return ESP_OK;
  }

  // Check if a profile with this serial already exists — update it
  auto profiles = portal->config_store_.load_printer_profiles();
  PrinterProfile profile;
  bool found = false;
  for (const auto& p : profiles) {
    if (p.serial == serial) {
      profile = p;
      found = true;
      break;
    }
  }
  if (!found) {
    profile.index = static_cast<uint8_t>(profiles.size());
    if (profile.index >= kMaxPrinterProfiles) {
      httpd_resp_set_status(request, "507 Insufficient Storage");
      send_json(request, "{\"error\":\"Maximum number of printer profiles reached\"}");
      return ESP_OK;
    }
  }

  profile.serial = serial;
  if (!host.empty()) profile.host = host;
  if (!access_code.empty()) profile.access_code = access_code;
  if (!display_name.empty()) profile.display_name = display_name;
  if (!model.empty()) profile.model = model;
  if (cloud_bound_explicit) profile.cloud_bound = true;
  // Update cloud_bound from live cloud device list
  if (!profile.cloud_bound) {
    const auto cloud_devs = portal->cloud_client_.get_cloud_devices();
    for (const auto& cd : cloud_devs) {
      if (cd.serial == serial) { profile.cloud_bound = true; break; }
    }
  }
  portal->config_store_.save_printer_profile(profile);

  std::string body = "{\"status\":\"saved\",\"index\":";
  body += std::to_string(profile.index);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_printers_delete(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) return ESP_FAIL;
  if (!portal->is_request_authorized(request)) return portal->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const cJSON* index_item = cJSON_GetObjectItem(root, "index");
  if (!cJSON_IsNumber(index_item)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "index field required");
  }
  const uint8_t del_index = static_cast<uint8_t>(cJSON_GetNumberValue(index_item));
  cJSON_Delete(root);

  const uint8_t active_idx = portal->config_store_.load_active_printer_index();
  const esp_err_t err = portal->config_store_.delete_printer_profile(del_index);
  if (err != ESP_OK) {
    httpd_resp_set_status(request, "404 Not Found");
    send_json(request, "{\"error\":\"Profile not found\"}");
    return ESP_OK;
  }

  // If the deleted profile was the active one, clear legacy config and disconnect clients
  if (del_index == active_idx) {
    const PrinterConnection empty_conn;
    portal->printer_client_.configure(empty_conn);
    portal->camera_client_.configure(empty_conn);
    // Reconfigure cloud to drop the serial binding
    const BambuCloudCredentials cloud_creds = portal->config_store_.load_cloud_credentials();
    portal->cloud_client_.configure(cloud_creds, "");
    // Switch active to first remaining profile if any
    const auto remaining = portal->config_store_.load_printer_profiles();
    if (!remaining.empty()) {
      portal->config_store_.save_active_printer_index(remaining.front().index);
      const PrinterConnection new_conn = remaining.front().to_connection();
      if (new_conn.is_ready()) {
        portal->printer_client_.configure(new_conn);
        portal->camera_client_.configure(new_conn);
      }
      portal->cloud_client_.configure(cloud_creds, remaining.front().serial);
    }
  }

  send_json(request, "{\"status\":\"deleted\"}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_printers_clear_local(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) return ESP_FAIL;
  if (!portal->is_request_authorized(request)) return portal->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const cJSON* index_item = cJSON_GetObjectItem(root, "index");
  if (!cJSON_IsNumber(index_item)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "index field required");
  }
  const uint8_t idx = static_cast<uint8_t>(cJSON_GetNumberValue(index_item));
  cJSON_Delete(root);

  auto profiles = portal->config_store_.load_printer_profiles();
  PrinterProfile* target = nullptr;
  for (auto& p : profiles) {
    if (p.index == idx) { target = &p; break; }
  }
  if (target == nullptr) {
    httpd_resp_set_status(request, "404 Not Found");
    send_json(request, "{\"error\":\"Profile not found\"}");
    return ESP_OK;
  }

  target->host.clear();
  target->access_code.clear();
  portal->config_store_.save_printer_profile(*target);

  // If this is the active profile, disconnect local clients
  const uint8_t active_idx = portal->config_store_.load_active_printer_index();
  if (idx == active_idx) {
    const PrinterConnection empty_conn;
    portal->printer_client_.configure(empty_conn);
    portal->camera_client_.configure(empty_conn);
  }

  send_json(request, "{\"status\":\"local_cleared\"}");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_ota_upload(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  const int total = request->content_len;
  if (total <= 0 || total > 8 * 1024 * 1024) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request, "{\"error\":\"Invalid firmware size\",\"detail\":\"Expected a .bin file up to 8 MB.\"}");
    return ESP_OK;
  }

  const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
  if (update_partition == nullptr) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    send_json(request,
              "{\"error\":\"No OTA partition\",\"detail\":\"This build was flashed without an OTA partition layout.\"}");
    return ESP_OK;
  }

  ESP_LOGI(kTag, "OTA upload: %d bytes -> partition '%s'", total, update_partition->label);

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(update_partition, static_cast<size_t>(total), &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(err));
    httpd_resp_set_status(request, "500 Internal Server Error");
    send_json(request, "{\"error\":\"OTA begin failed\",\"detail\":\"Could not start the OTA write session.\"}");
    return ESP_OK;
  }

  constexpr size_t kOtaBufSize = 4096;
  std::unique_ptr<char[]> buf(new (std::nothrow) char[kOtaBufSize]);
  if (!buf) {
    esp_ota_abort(ota_handle);
    httpd_resp_set_status(request, "500 Internal Server Error");
    send_json(request, "{\"error\":\"Out of memory\",\"detail\":\"Could not allocate OTA receive buffer.\"}");
    return ESP_OK;
  }

  int received = 0;
  while (received < total) {
    const int to_recv = std::min(static_cast<int>(kOtaBufSize), total - received);
    const int ret = httpd_req_recv(request, buf.get(), to_recv);
    if (ret <= 0) {
      esp_ota_abort(ota_handle);
      ESP_LOGE(kTag, "OTA receive error at %d/%d bytes (ret=%d)", received, total, ret);
      return ret == 0 ? ESP_OK : ESP_FAIL;
    }
    err = esp_ota_write(ota_handle, buf.get(), static_cast<size_t>(ret));
    if (err != ESP_OK) {
      esp_ota_abort(ota_handle);
      ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(err));
      httpd_resp_set_status(request, "500 Internal Server Error");
      send_json(request, "{\"error\":\"OTA write failed\",\"detail\":\"Flash write error.\"}");
      return ESP_OK;
    }
    received += ret;
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(err));
    httpd_resp_set_status(request, "422 Unprocessable Entity");
    send_json(
        request,
        "{\"error\":\"Firmware validation failed\",\"detail\":\"The uploaded file does not appear to be a valid PrintSphere firmware image.\"}");
    return ESP_OK;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    httpd_resp_set_status(request, "500 Internal Server Error");
    send_json(
        request,
        "{\"error\":\"Boot partition switch failed\",\"detail\":\"The firmware was written but the boot slot could not be updated.\"}");
    return ESP_OK;
  }

  ESP_LOGI(kTag, "OTA upload complete: %d bytes written, scheduling reboot", received);
  send_json(request, "{\"status\":\"success\",\"rebooting\":true}");
  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }
  return ESP_OK;
}

void SetupPortal::reboot_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
}

esp_err_t SetupPortal::handle_ota_url(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  std::string url = trim_copy(read_string_field(root, "url"));
  cJSON_Delete(root);

  if (url.empty()) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "url field missing");
  }

  // Convert GitHub blob URL to raw download URL
  const std::string gh_prefix = "https://github.com/";
  const std::string blob_marker = "/blob/";
  if (url.rfind(gh_prefix, 0) == 0) {
    const size_t blob_pos = url.find(blob_marker, gh_prefix.size());
    if (blob_pos != std::string::npos) {
      url = "https://raw.githubusercontent.com/" +
            url.substr(gh_prefix.size(), blob_pos - gh_prefix.size()) + "/" +
            url.substr(blob_pos + blob_marker.size());
      ESP_LOGI(kTag, "OTA URL converted to raw: %s", url.c_str());
    }
  }

  if (url.rfind("https://", 0) != 0) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request, "{\"error\":\"Only HTTPS URLs are supported\"}");
    return ESP_OK;
  }

  if (!portal->wifi_manager_.is_station_connected()) {
    httpd_resp_set_status(request, "409 Conflict");
    send_json(request, "{\"error\":\"Wi-Fi not connected\",\"detail\":\"Connect to Wi-Fi first.\"}");
    return ESP_OK;
  }

  {
    std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
    if (portal->ota_url_status_.state == OtaUrlState::kDownloading) {
      httpd_resp_set_status(request, "409 Conflict");
      send_json(request, "{\"error\":\"OTA download already in progress\"}");
      return ESP_OK;
    }
    portal->ota_url_pending_ = url;
    portal->ota_url_status_.state = OtaUrlState::kDownloading;
    portal->ota_url_status_.progress_percent = 0;
    portal->ota_url_status_.error.clear();
  }

  ESP_LOGI(kTag, "Starting OTA URL task: %s", url.c_str());
  xTaskCreate(&SetupPortal::ota_url_task, "ota_url", 8192, portal, 5, nullptr);

  std::string body = "{\"status\":\"started\",\"url\":\"";
  body += json_escape(url);
  body += "\"}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_ota_status(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  OtaUrlState state;
  int progress;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
    state = portal->ota_url_status_.state;
    progress = portal->ota_url_status_.progress_percent;
    error = portal->ota_url_status_.error;
  }

  const char* state_str = "idle";
  switch (state) {
    case OtaUrlState::kDownloading: state_str = "downloading"; break;
    case OtaUrlState::kDone:        state_str = "done";        break;
    case OtaUrlState::kFailed:      state_str = "failed";      break;
    default: break;
  }

  std::string body = "{\"state\":\"";
  body += state_str;
  body += "\",\"progress\":";
  body += std::to_string(progress);
  body += ",\"error\":\"";
  body += json_escape(error);
  body += "\"}";
  send_json(request, body);
  return ESP_OK;
}

void SetupPortal::ota_url_task(void* context) {
  auto* portal = static_cast<SetupPortal*>(context);

  std::string url;
  {
    std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
    url = portal->ota_url_pending_;
  }

  ESP_LOGI(kTag, "OTA URL task: %s", url.c_str());

  esp_http_client_config_t http_cfg = {};
  http_cfg.url = url.c_str();
  http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  http_cfg.timeout_ms = 30000;
  http_cfg.keep_alive_enable = true;

  esp_https_ota_config_t ota_cfg = {};
  ota_cfg.http_config = &http_cfg;

  esp_https_ota_handle_t ota_handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "OTA URL begin failed: %s", esp_err_to_name(err));
    {
      std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
      portal->ota_url_status_.state = OtaUrlState::kFailed;
      portal->ota_url_status_.error = esp_err_to_name(err);
    }
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    err = esp_https_ota_perform(ota_handle);
    if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      break;
    }
    const int len_read = esp_https_ota_get_image_len_read(ota_handle);
    const int total = esp_https_ota_get_image_size(ota_handle);
    if (total > 0) {
      std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
      portal->ota_url_status_.progress_percent = (len_read * 100) / total;
    }
  }

  if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota_handle)) {
    ESP_LOGE(kTag, "OTA URL download error: %s", esp_err_to_name(err));
    esp_https_ota_abort(ota_handle);
    {
      std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
      portal->ota_url_status_.state = OtaUrlState::kFailed;
      portal->ota_url_status_.error = (err != ESP_OK)
          ? std::string(esp_err_to_name(err)) : std::string("Incomplete download");
    }
    vTaskDelete(nullptr);
    return;
  }

  err = esp_https_ota_finish(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "OTA URL finish failed: %s", esp_err_to_name(err));
    {
      std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
      portal->ota_url_status_.state = OtaUrlState::kFailed;
      portal->ota_url_status_.error =
          std::string("Firmware validation failed: ") + esp_err_to_name(err);
    }
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(kTag, "OTA URL complete, scheduling reboot");
  {
    std::lock_guard<std::mutex> lock(portal->ota_url_mutex_);
    portal->ota_url_status_.state = OtaUrlState::kDone;
    portal->ota_url_status_.progress_percent = 100;
  }
  if (!portal->reboot_requested_) {
    portal->reboot_requested_ = true;
    xTaskCreate(&SetupPortal::reboot_task, "portal_reboot", 2048, portal, 4, nullptr);
  }
  vTaskDelete(nullptr);
}

#ifdef PRINTSPHERE_DEBUG_BUILD
esp_err_t SetupPortal::handle_debug_log(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }
  if (!portal->is_request_authorized(request)) {
    return portal->send_locked_response(request);
  }

  // Parse optional ?offset=N query parameter.
  size_t from_offset = 0;
  char query_buf[64] = {};
  if (httpd_req_get_url_query_str(request, query_buf, sizeof(query_buf)) == ESP_OK) {
    char val[24] = {};
    if (httpd_query_key_value(query_buf, "offset", val, sizeof(val)) == ESP_OK) {
      from_offset = static_cast<size_t>(strtoul(val, nullptr, 10));
    }
  }

  size_t end_offset = 0;
  std::string log_text = debug_log_fetch(from_offset, &end_offset);

  char end_hdr[24];
  snprintf(end_hdr, sizeof(end_hdr), "%zu", end_offset);
  httpd_resp_set_hdr(request, "X-Log-End-Offset", end_hdr);
  httpd_resp_set_hdr(request, "Access-Control-Expose-Headers", "X-Log-End-Offset");
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");

  if (log_text.empty()) {
    return httpd_resp_sendstr(request, "");
  }
  return httpd_resp_send(request, log_text.data(), static_cast<ssize_t>(log_text.size()));
}
#endif  // PRINTSPHERE_DEBUG_BUILD

}  // namespace printsphere
