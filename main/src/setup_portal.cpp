#include "printsphere/setup_portal.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/ui.hpp"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.portal";
constexpr size_t kMaxRequestBody = 4096;
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
  return snapshot.tfa_required ? "2FA Code" : "Email Code";
}

std::string cloud_verify_placeholder(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "Only needed if Bambu requests a 2FA code"
                               : "Only needed if Bambu requests an email code";
}

std::string cloud_verify_note(const BambuCloudSnapshot& snapshot) {
  return snapshot.tfa_required ? "Bambu is currently waiting for a 2FA code."
                               : "Bambu is currently waiting for an email code. The cloud login completes after that step.";
}

void append_cloud_status_fields(std::string* body, const BambuCloudSnapshot& cloud) {
  if (body == nullptr) {
    return;
  }

  *body += ",\"cloud_connected\":";
  *body += (cloud.connected ? "true" : "false");
  *body += ",\"cloud_verification_required\":";
  *body += (cloud.verification_required ? "true" : "false");
  *body += ",\"cloud_tfa_required\":";
  *body += (cloud.tfa_required ? "true" : "false");
  *body += ",\"cloud_configured\":";
  *body += (cloud.configured ? "true" : "false");
  *body += ",\"cloud_detail\":\"" + json_escape(cloud.detail) + "\"";
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

esp_err_t SetupPortal::start() {
  if (server_ != nullptr) {
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  config.max_uri_handlers = 10;

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

  ESP_LOGI(kTag, "Setup portal started");
  return ESP_OK;
}

esp_err_t SetupPortal::handle_root(httpd_req_t* request) {
  const auto* portal = static_cast<const SetupPortal*>(request->user_ctx);
  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials cloud = portal->config_store_.load_cloud_credentials();
  const StatusSourcePreference state_source = portal->config_store_.load_status_source_preference();
  const PrinterConnection printer = portal->config_store_.load_printer_config();
  const ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const BambuCloudSnapshot cloud_snapshot = portal->cloud_client_.snapshot();
  const PrinterSnapshot local_snapshot = portal->printer_client_.snapshot();
  const bool wifi_connected = portal->wifi_manager_.is_station_connected();
  const bool setup_ap_active = portal->wifi_manager_.is_setup_access_point_active();
  const bool wifi_configured = wifi.is_configured();
  const std::string wifi_ip = portal->wifi_manager_.station_ip();
  const bool local_configured = portal->printer_client_.is_configured();
  const bool show_connection_steps = wifi_connected && !setup_ap_active;

  const std::string wifi_badge_value =
      wifi_connected ? ("Connected - " + wifi_ip) : "Not connected";
  const char* wifi_badge_class = wifi_connected ? "ok" : "warn";

  std::string cloud_badge_value = "Not configured";
  const char* cloud_badge_class = "idle";
  if (cloud_snapshot.connected) {
    cloud_badge_value = "Connected";
    cloud_badge_class = "ok";
  } else if (cloud_snapshot.verification_required) {
    cloud_badge_value = "Code required";
    cloud_badge_class = "warn";
  } else if (cloud.is_configured()) {
    cloud_badge_value = "Configured";
    cloud_badge_class = "info";
  }

  const std::string source_badge_value =
      state_source == StatusSourcePreference::kCloud ? "Cloud primary" : "Local primary";
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
  const std::string initial_status_line =
      show_connection_steps ? "Step 2: Connect Bambu Cloud" : "Step 1: Save Wi-Fi";
  const std::string initial_status_detail =
      show_connection_steps
          ? ("ESP on home network: " + wifi_ip)
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

  std::string html;
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
          ".section{padding:22px;display:grid;gap:16px;} .footer-card{padding:18px;display:grid;gap:8px;}"
          ".stack{display:grid;gap:18px;}";
  html += ".hero-top{display:grid;gap:8px;} .eyebrow{font-size:12px;letter-spacing:.14em;text-transform:uppercase;"
          "color:#8fb2d3;} .title{font-size:34px;line-height:1.05;} .subtitle{max-width:720px;color:var(--muted);"
          "line-height:1.55;} .section-head{display:grid;gap:6px;} .section-head p,.hint,.micro{color:var(--muted);"
          "line-height:1.5;} .micro{font-size:13px;} .hint-box{padding:14px 16px;border-radius:18px;background:#0e1620;"
          "border:1px solid #2b3e54;color:var(--muted);} .hint-box strong{color:var(--text);}";
  html += ".badge-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;}"
          ".badge{padding:14px 16px;border-radius:18px;border:1px solid var(--line);background:#0f1721;display:grid;gap:6px;}"
          ".badge-label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#91a6bc;}"
          ".badge-value{font-size:15px;font-weight:700;line-height:1.35;color:var(--text);}"
          ".badge.ok{border-color:#22614c;background:#10231d;} .badge.warn{border-color:#7d6222;background:#241c0e;}"
          ".badge.info{border-color:#31557d;background:#111b29;} .badge.idle{border-color:#334456;background:#121a23;}";
  html += ".grid-2{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;}"
          ".grid-3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;}"
          ".field{display:grid;gap:8px;} .actions{display:flex;flex-wrap:wrap;gap:12px;align-items:center;}"
          ".actions .micro{min-width:220px;} .color-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;}"
          ".color-grid input{min-height:54px;padding:6px 10px;background:#0c131b;}";
  html += ".status-line{font-size:16px;font-weight:700;} .status-detail{color:var(--muted);}"
          ".hidden{display:none !important;}";
  html += "@media(max-width:840px){.badge-grid,.color-grid{grid-template-columns:repeat(2,minmax(0,1fr));}}";
  html += "@media(max-width:640px){body{padding:16px;} .title{font-size:28px;} .badge-grid,.grid-2,.grid-3,.color-grid"
          "{grid-template-columns:1fr;} .hero,.section,.footer-card{padding:18px;}}";
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
  html += "<div class=\"hero-top\"><div class=\"eyebrow\">PrintSphere</div>";
  html += "<h1 class=\"title\">Web Config</h1>";
  html += "<p class=\"subtitle\">";
  html += show_connection_steps
              ? "The ESP is on your home network. You can now finish the full setup with cloud, local printer access and UI tuning."
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
              ? "Wi-Fi is up. You can connect Bambu Cloud and the local MQTT path now without rebooting."
              : "Only the Wi-Fi step is available while the ESP is running its setup access point.";
  html += "</div>";
  html += "</section>";

  html += "<form id=\"config-form\" class=\"stack\">";

  html += "<section class=\"section\">";
  html += "<div class=\"section-head\"><h2>Step 1 - Wi-Fi</h2><p>This network is used for cloud access, local printer access and the setup portal after first boot.</p></div>";
  html += "<div class=\"grid-2\">";
  html += "<div class=\"field\"><label for=\"wifi_ssid\">Wi-Fi SSID</label><input id=\"wifi_ssid\" value=\"";
  html += json_escape(wifi.ssid);
  html += "\" autocomplete=\"off\"></div>";
  html += "<div class=\"field\"><label for=\"wifi_password\">Wi-Fi Password</label><input id=\"wifi_password\" type=\"password\" value=\"";
  html += json_escape(wifi.password);
  html += "\" autocomplete=\"off\"></div>";
  html += "</div>";
  html += "<p class=\"micro\">Current network status: <span id=\"wifi-detail\">";
  html += json_escape(wifi_badge_value);
  html += "</span></p>";
  html += "</section>";

  if (show_connection_steps) {
    html += "<section class=\"section\">";
    html += "<div class=\"section-head\"><h2>Step 2 - Bambu Cloud</h2><p>Primary source for cover image, project metadata and cloud lifecycle. "
            "Use Connect to start the login immediately. If Bambu asks for an email code or 2FA code, you can complete that step here.</p></div>";
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"cloud_email\">Bambu Email</label><input id=\"cloud_email\" value=\"";
    html += json_escape(cloud.email);
    html += "\" autocomplete=\"username\"></div>";
    html += "<div class=\"field\"><label for=\"cloud_password\">Bambu Password</label><input id=\"cloud_password\" type=\"password\" value=\"";
    html += json_escape(cloud.password);
    html += "\" autocomplete=\"current-password\"></div>";
    html += "</div>";
    html += "<div class=\"field\"><label for=\"state_source\">Status Source</label><select id=\"state_source\">";
    html += "<option value=\"cloud\"";
    if (state_source == StatusSourcePreference::kCloud) {
      html += " selected";
    }
    html += ">Cloud primary, local MQTT fallback</option>";
    html += "<option value=\"local\"";
    if (state_source == StatusSourcePreference::kLocal) {
      html += " selected";
    }
    html += ">Local MQTT primary, cloud fallback</option></select></div>";
    html += "<div class=\"hint-box\"><strong>Cloud Status:</strong> <span id=\"cloud-detail\">";
    html += json_escape(cloud_snapshot.detail);
    html += "</span></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"cloud-connect-button\">Connect Cloud</button>";
    html += "<div class=\"micro\">This saves the cloud credentials immediately and starts the login without rebooting.</div></div>";
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
    html += "</section>";
  }

  if (show_connection_steps) {
    html += "<section class=\"section\">";
    html += "<div class=\"section-head\"><h2>Step 3 - Local Printer Path</h2><p>The local MQTT path provides live status, layers, temperatures and also powers camera snapshots on page 3.</p></div>";
    html += "<div class=\"grid-2\">";
    html += "<div class=\"field\"><label for=\"printer_host\">Printer IP or Hostname</label><input id=\"printer_host\" value=\"";
    html += json_escape(printer.host);
    html += "\" autocomplete=\"off\"></div>";
    html += "<div class=\"field\"><label for=\"printer_serial\">Printer Serial Number</label><input id=\"printer_serial\" value=\"";
    html += json_escape(printer.serial);
    html += "\" autocomplete=\"off\"></div>";
    html += "</div>";
    html += "<div class=\"field\"><label for=\"printer_access_code\">Access Code</label><input id=\"printer_access_code\" value=\"";
    html += json_escape(printer.access_code);
    html += "\" autocomplete=\"off\"></div>";
    html += "<div class=\"hint-box\"><strong>Local Status:</strong> <span id=\"local-detail\">";
    html += json_escape(local_snapshot.detail);
    html += "</span></div>";
    html += "<div class=\"actions\"><button type=\"button\" class=\"secondary\" id=\"local-connect-button\">Connect Local</button>";
    html += "<div class=\"micro\">This saves the local printer credentials immediately and reconnects MQTT and camera without rebooting.</div></div>";
    html += "<p class=\"micro\">The local path always remains available as a fallback and is still useful when the cloud is lagging behind.</p>";
    html += "</section>";
  }

  if (show_connection_steps) {
    html += "<section class=\"section\">";
    html += "<div class=\"section-head\"><h2>Arc Colors</h2><p>These groups control ring colors, pulsing states and status colors in the UI. "
            "The values map directly to your native PrintSphere interface.</p></div>";
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
    html += "</section>";
  }

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
  html += "const cloudConnectButton=document.getElementById('cloud-connect-button');";
  html += "const localConnectButton=document.getElementById('local-connect-button');";
  html += "const verifyButton=document.getElementById('verify-button');";
  html += "const arcInputIds=['arc_printing','arc_done','arc_error','arc_idle','arc_preheat','arc_clean','arc_level','arc_cool','arc_idle_active','arc_filament','arc_setup','arc_offline','arc_unknown'];";
  html += "let statusLockUntil=0;";
  html += "let arcPreviewTimer=null;";
  html += "const savedConfig={cloud_email:\"";
  html += json_escape(cloud.email);
  html += "\",cloud_password:\"";
  html += json_escape(cloud.password);
  html += "\",state_source:\"";
  html += to_string(state_source);
  html += "\",printer_host:\"";
  html += json_escape(printer.host);
  html += "\",printer_serial:\"";
  html += json_escape(printer.serial);
  html += "\",printer_access_code:\"";
  html += json_escape(printer.access_code);
  html += "\"};";
  html += "function setStatus(line,detail,lockMs){statusEl.textContent=line||'';statusDetailEl.textContent=detail||'';"
          "statusLockUntil=lockMs?Date.now()+lockMs:0;}";
  html += "function valueOf(id){const el=document.getElementById(id);return el?el.value:'';}";
  html += "function trimmedValue(id){return valueOf(id).trim();}";
  html += "function setBadge(id,label,value,stateClass){const badge=document.getElementById(id);if(!badge)return;"
          "badge.className='badge '+stateClass;badge.innerHTML='<span class=\"badge-label\">'+label+'</span><span class=\"badge-value\">'+value+'</span>';}";
  html += "function updateCloudVerification(body){const note=document.getElementById('cloud-verify-note');"
          "const label=document.getElementById('cloud-verification-label');"
          "const input=document.getElementById('cloud_verification_code');"
          "if(!note||!label||!input)return;"
          "const tfa=!!body.cloud_tfa_required;"
          "const required=!!body.cloud_verification_required;"
          "label.textContent=tfa?'2FA Code':'Email Code';"
          "input.placeholder=tfa?'Only needed if Bambu requests a 2FA code':'Only needed if Bambu requests an email code';"
          "note.textContent=tfa?'Bambu is currently waiting for a 2FA code.':'Bambu is currently waiting for an email code. The cloud login completes after that step.';"
          "note.classList.toggle('hidden',!required);}";
  html += "async function updateHealth(){try{const response=await fetch('/api/health',{cache:'no-store'});"
          "if(!response.ok)return;const body=await response.json();"
          "const wifiValue=body.wifi_connected?('Connected - '+(body.wifi_ip||'')):'Not connected';"
          "setBadge('wifi-badge','Wi-Fi',wifiValue,body.wifi_connected?'ok':'warn');"
          "document.getElementById('wifi-detail').textContent=wifiValue;"
          "let cloudValue='Not configured';let cloudState='idle';"
          "if(body.cloud_connected){cloudValue='Connected';cloudState='ok';}"
          "else if(body.cloud_verification_required){cloudValue='Code required';cloudState='warn';}"
          "else if(body.cloud_configured||(trimmedValue('cloud_email')&&valueOf('cloud_password'))){cloudValue='Configured';cloudState='info';}"
          "setBadge('cloud-badge','Cloud',cloudValue,cloudState);"
          "const cloudDetail=document.getElementById('cloud-detail');if(cloudDetail){cloudDetail.textContent=body.cloud_detail||'No cloud response yet';}"
          "let localValue='Not configured';let localState='idle';"
          "if(body.local_connected){localValue='Connected';localState='ok';}"
          "else if(body.local_error){localValue='Error';localState='warn';}"
          "else if(body.local_configured){localValue='Configured';localState='info';}"
          "setBadge('local-badge','Local Path',localValue,localState);"
          "const localDetail=document.getElementById('local-detail');if(localDetail){localDetail.textContent=body.local_detail||'No local response yet';}"
          "updateCloudVerification(body);"
          "if(Date.now()>statusLockUntil){"
          "if(body.wifi_connected){setStatus(trimmedValue('cloud_email')||body.cloud_connected||body.cloud_verification_required?'Step 2: Connect Bambu Cloud':'Setup ready',body.cloud_detail||('ESP on home network: '+(body.wifi_ip||'')),0);}"
          "else{setStatus('Step 1: Save Wi-Fi','Save Wi-Fi and restart to continue provisioning.',0);}}}"
          "catch(error){if(Date.now()>statusLockUntil){setStatus('Portal reachable','Live status could not be refreshed right now.',0);}}}";
  html += "function buildArcPayload(){const payload={};arcInputIds.forEach((id)=>{const input=document.getElementById(id);if(input){payload[id]=input.value;}});return payload;}";
  html += "function buildPayload(){return Object.assign({wifi_ssid:trimmedValue('wifi_ssid'),"
          "wifi_password:valueOf('wifi_password'),"
          "cloud_email:(document.getElementById('cloud_email')?trimmedValue('cloud_email'):savedConfig.cloud_email),"
          "cloud_password:(document.getElementById('cloud_password')?valueOf('cloud_password'):savedConfig.cloud_password),"
          "state_source:(document.getElementById('state_source')?valueOf('state_source'):savedConfig.state_source)||'cloud',"
          "printer_host:(document.getElementById('printer_host')?trimmedValue('printer_host'):savedConfig.printer_host),"
          "printer_serial:(document.getElementById('printer_serial')?trimmedValue('printer_serial'):savedConfig.printer_serial),"
          "printer_access_code:(document.getElementById('printer_access_code')?trimmedValue('printer_access_code'):savedConfig.printer_access_code)},buildArcPayload());}";
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
  html += "if(cloudConnectButton){cloudConnectButton.addEventListener('click',async()=>{const cloud_email=trimmedValue('cloud_email');"
          "const cloud_password=valueOf('cloud_password');"
          "const state_source=document.getElementById('state_source')?valueOf('state_source'):savedConfig.state_source;"
          "if(!cloud_email||!cloud_password){setStatus('Cloud credentials missing','Enter both Bambu email and password first.',5000);return;}"
          "cloudConnectButton.disabled=true;setStatus('Connecting cloud...','Saving credentials and starting the login now.',8000);"
          "try{const response=await fetch('/api/cloud/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cloud_email,cloud_password,state_source})});"
          "const body=await response.json().catch(()=>({}));updateCloudVerification(body);"
          "if(response.ok){if(body.cloud_connected){setStatus('Cloud connected',body.detail||'Connected to Bambu Cloud.',7000);}"
          "else if(body.cloud_verification_required){setStatus(body.cloud_tfa_required?'2FA required':'Email code required',body.detail||'Enter the requested code to finish the login.',10000);}"
          "else{setStatus('Cloud login started',body.detail||'Waiting for cloud response...',7000);}}"
          "else{setStatus(body.error||'Cloud connect failed',body.detail||'Please review the credentials and try again.',8000);}}"
          "catch(error){setStatus('Cloud request failed','The request to the ESP did not complete successfully.',7000);}finally{cloudConnectButton.disabled=false;updateHealth();}});}";
  html += "if(localConnectButton){localConnectButton.addEventListener('click',async()=>{const printer_host=trimmedValue('printer_host');"
          "const printer_serial=trimmedValue('printer_serial');"
          "const printer_access_code=trimmedValue('printer_access_code');"
          "const state_source=document.getElementById('state_source')?valueOf('state_source'):savedConfig.state_source;"
          "if(!printer_host||!printer_serial||!printer_access_code){setStatus('Local credentials missing','Enter printer host, serial and access code first.',5000);return;}"
          "localConnectButton.disabled=true;setStatus('Connecting local path...','Saving printer credentials and reconnecting MQTT now.',8000);"
          "try{const response=await fetch('/api/local/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({printer_host,printer_serial,printer_access_code,state_source})});"
          "const body=await response.json().catch(()=>({}));"
          "if(response.ok){if(body.local_connected){setStatus('Local path connected',body.detail||'Connected to local Bambu MQTT.',7000);}"
          "else{setStatus('Local path started',body.detail||'Waiting for local printer response...',7000);}}"
          "else{setStatus(body.error||'Local connect failed',body.detail||'Please review the local printer fields and try again.',8000);}}"
          "catch(error){setStatus('Local request failed','The request to the ESP did not complete successfully.',7000);}finally{localConnectButton.disabled=false;updateHealth();}});}";
  html += "if(verifyButton){verifyButton.addEventListener('click',async()=>{const code=trimmedValue('cloud_verification_code');"
          "if(!code){setStatus('Cloud code missing','Please enter the requested code first.',5000);return;}"
          "verifyButton.disabled=true;setStatus('Connecting cloud...','Completing the cloud login now.',8000);"
          "try{const response=await fetch('/api/cloud/verify',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code})});"
          "const body=await response.json().catch(()=>({}));updateCloudVerification(body);"
          "if(response.ok){setStatus('Cloud code accepted',body.detail||'The cloud should connect within a few seconds.',7000);"
          "const codeInput=document.getElementById('cloud_verification_code');if(codeInput){codeInput.value='';}}"
          "else{setStatus(body.error||'Cloud code was rejected',body.detail||'Please check the code and try again.',7000);}}"
          "catch(error){setStatus('Cloud request failed','The request to the ESP did not complete successfully.',7000);}finally{verifyButton.disabled=false;updateHealth();}});}";
  html += "arcInputIds.forEach((id)=>{const input=document.getElementById(id);if(!input)return;"
          "input.addEventListener('input',queueArcPreview);input.addEventListener('change',commitArcColors);});";
  html += "updateHealth();setInterval(updateHealth,4000);";
  html += "</script>";
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
  const auto* portal = static_cast<const SetupPortal*>(request->user_ctx);

  std::string body = "{";
  body += "\"status\":\"ok\",";
  body += "\"portal\":\"setup\",";
  body += "\"wifi_connected\":";
  body += (portal->wifi_manager_.is_station_connected() ? "true" : "false");
  body += ",";
  body += "\"wifi_ip\":\"" + json_escape(portal->wifi_manager_.station_ip()) + "\"";
  const BambuCloudSnapshot cloud = portal->cloud_client_.snapshot();
  const PrinterSnapshot local = portal->printer_client_.snapshot();
  append_cloud_status_fields(&body, cloud);
  append_local_status_fields(&body, local, portal->printer_client_.is_configured());
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_get(httpd_req_t* request) {
  const auto* portal = static_cast<const SetupPortal*>(request->user_ctx);
  const WifiCredentials wifi = portal->config_store_.load_wifi_credentials();
  const BambuCloudCredentials cloud = portal->config_store_.load_cloud_credentials();
  const StatusSourcePreference state_source = portal->config_store_.load_status_source_preference();
  const PrinterConnection printer = portal->config_store_.load_printer_config();
  const ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();

  std::string body = "{";
  body += "\"wifi_ssid\":\"" + json_escape(wifi.ssid) + "\",";
  body += "\"cloud_email\":\"" + json_escape(cloud.email) + "\",";
  body += "\"printer_host\":\"" + json_escape(printer.host) + "\",";
  body += "\"printer_serial\":\"" + json_escape(printer.serial) + "\",";
  body += "\"state_source\":\"";
  body += to_string(state_source);
  body += "\",";
  body += "\"wifi_connected\":";
  body += (portal->wifi_manager_.is_station_connected() ? "true" : "false");
  body += ",";
  body += "\"wifi_ip\":\"" + json_escape(portal->wifi_manager_.station_ip()) + "\"";
  const BambuCloudSnapshot cloud_snapshot = portal->cloud_client_.snapshot();
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
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

esp_err_t SetupPortal::handle_config_post(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const WifiCredentials wifi = {
      .ssid = trim_copy(read_string_field(root, "wifi_ssid")),
      .password = read_string_field(root, "wifi_password"),
  };

  const BambuCloudCredentials cloud = {
      .email = trim_copy(read_string_field(root, "cloud_email")),
      .password = read_string_field(root, "cloud_password"),
  };
  const StatusSourcePreference state_source =
      parse_status_source_preference(trim_copy(read_string_field(root, "state_source")));

  const PrinterConnection printer = {
      .host = trim_copy(read_string_field(root, "printer_host")),
      .serial = trim_copy(read_string_field(root, "printer_serial")),
      .access_code = trim_copy(read_string_field(root, "printer_access_code")),
  };

  ArcColorScheme arc_colors = portal->config_store_.load_arc_color_scheme();
  const bool colors_valid = parse_arc_colors_from_json(root, &arc_colors);

  cJSON_Delete(root);

  if (!colors_valid) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid arc color value");
  }

  const bool local_any =
      !printer.host.empty() || !printer.serial.empty() || !printer.access_code.empty();
  const bool cloud_any = !cloud.email.empty() || !cloud.password.empty();
  const bool local_ready = printer.is_ready();
  const bool cloud_ready = cloud.is_configured();

  if (!local_ready && local_any) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "local printer fields incomplete");
  }
  if (!cloud_ready && cloud_any) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "cloud account fields incomplete");
  }
  if (!wifi.is_configured() && !local_ready && !cloud_ready) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "configure Wi-Fi, cloud or local printer access");
  }
  if (local_ready && !is_valid_printer_host(printer.host)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "printer host must be full IPv4 or hostname");
  }

  ESP_LOGI(kTag,
           "Saving config: wifi_ssid=%s cloud_email_len=%u state_source=%s local_host=%s serial_len=%u access_len=%u",
           wifi.ssid.c_str(), static_cast<unsigned int>(cloud.email.size()),
           to_string(state_source), printer.host.c_str(),
           static_cast<unsigned int>(printer.serial.size()),
           static_cast<unsigned int>(printer.access_code.size()));

  ESP_RETURN_ON_ERROR(portal->config_store_.save_wifi_credentials(wifi), kTag, "save wifi failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_cloud_credentials(cloud), kTag,
                      "save cloud failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_status_source_preference(state_source), kTag,
                      "save state source failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_printer_config(printer), kTag, "save printer failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_arc_color_scheme(arc_colors), kTag,
                      "save arc colors failed");

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

esp_err_t SetupPortal::handle_cloud_connect(httpd_req_t* request) {
  auto* portal = static_cast<SetupPortal*>(request->user_ctx);
  if (portal == nullptr) {
    return ESP_FAIL;
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const BambuCloudCredentials cloud = {
      .email = trim_copy(read_string_field(root, "cloud_email")),
      .password = read_string_field(root, "cloud_password"),
  };
  const StatusSourcePreference state_source =
      parse_status_source_preference(trim_copy(read_string_field(root, "state_source")));
  cJSON_Delete(root);

  if (!cloud.is_configured()) {
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
  ESP_RETURN_ON_ERROR(portal->config_store_.save_status_source_preference(state_source), kTag,
                      "save state source failed");
  portal->cloud_client_.request_reload_from_store();

  const BambuCloudSnapshot before = portal->cloud_client_.snapshot();
  BambuCloudSnapshot current = before;
  for (int attempt = 0; attempt < 40; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = portal->cloud_client_.snapshot();
    if (current.connected || current.verification_required || current.detail != before.detail ||
        current.configured != before.configured) {
      break;
    }
  }

  std::string body = "{\"status\":\"";
  body += current.connected ? "connected"
                            : (current.verification_required ? "verification_required" : "queued");
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

  portal->cloud_client_.submit_verification_code(code);
  BambuCloudSnapshot snapshot = portal->cloud_client_.snapshot();
  const std::string previous_detail = snapshot.detail;
  const bool previous_verification_required = snapshot.verification_required;
  for (int attempt = 0; attempt < 40; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    snapshot = portal->cloud_client_.snapshot();
    if (snapshot.connected || snapshot.detail != previous_detail ||
        snapshot.verification_required != previous_verification_required) {
      break;
    }
  }

  std::string body = "{\"status\":\"accepted\",\"detail\":\"";
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

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const PrinterConnection printer = {
      .host = trim_copy(read_string_field(root, "printer_host")),
      .serial = trim_copy(read_string_field(root, "printer_serial")),
      .access_code = trim_copy(read_string_field(root, "printer_access_code")),
  };
  const StatusSourcePreference state_source =
      parse_status_source_preference(trim_copy(read_string_field(root, "state_source")));
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

  ESP_RETURN_ON_ERROR(portal->config_store_.save_printer_config(printer), kTag, "save printer failed");
  ESP_RETURN_ON_ERROR(portal->config_store_.save_status_source_preference(state_source), kTag,
                      "save state source failed");

  const PrinterSnapshot before = portal->printer_client_.snapshot();
  portal->printer_client_.configure(printer);
  portal->camera_client_.configure(printer);

  PrinterSnapshot current = before;
  for (int attempt = 0; attempt < 40; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = portal->printer_client_.snapshot();
    if (current.connection == PrinterConnectionState::kOnline ||
        current.connection == PrinterConnectionState::kError ||
        current.detail != before.detail || current.stage != before.stage) {
      break;
    }
  }

  std::string body = "{\"status\":\"";
  body += current.connection == PrinterConnectionState::kOnline ? "connected" : "queued";
  body += "\",\"detail\":\"";
  body += json_escape(current.detail);
  body += "\"";
  append_local_status_fields(&body, current, true);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

void SetupPortal::reboot_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
}

}  // namespace printsphere
