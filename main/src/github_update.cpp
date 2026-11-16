#include "printsphere/github_update.hpp"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef PRINTSPHERE_RELEASE_VERSION
#define PRINTSPHERE_RELEASE_VERSION "dev"
#endif

namespace printsphere::github {
namespace {

constexpr char kTag[] = "printsphere.ghota";
constexpr size_t kMaxBodyBytes = 32 * 1024;
constexpr int kHttpTimeoutMs = 20000;
constexpr uint32_t kTaskStack = 12288;
constexpr char kGithubOwner[] = "cptkirki";
constexpr char kGithubRepo[] = "PrintSphere";

#if defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)
constexpr char kBoardToken[] = "2.8c";
#elif defined(PRINTSPHERE_HW_VARIANT_KNOMI_V2)
constexpr char kBoardToken[] = "knomi_v2";
#else
constexpr char kBoardToken[] = "";
#endif

std::string json_escape(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (const char ch : input) {
    switch (ch) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output.push_back(ch); break;
    }
  }
  return output;
}

esp_err_t collect_body(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == nullptr) return ESP_OK;
  auto* body = static_cast<std::string*>(event->user_data);
  if (event->data_len <= 0) return ESP_OK;
  if (body->size() + static_cast<size_t>(event->data_len) > kMaxBodyBytes) {
    return ESP_FAIL;
  }
  body->append(static_cast<const char*>(event->data), static_cast<size_t>(event->data_len));
  return ESP_OK;
}

bool fetch_latest_release(std::string* body, std::string* error) {
  std::string url = "https://api.github.com/repos/";
  url += kGithubOwner;
  url += "/";
  url += kGithubRepo;
  url += "/releases/latest";

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = kHttpTimeoutMs;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = &collect_body;
  config.user_data = body;
  config.disable_auto_redirect = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    *error = "http-begin";
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "PrintSphere/" PRINTSPHERE_RELEASE_VERSION);
  esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");

  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK) {
    *error = "http-connect";
    ESP_LOGW(kTag, "GitHub check failed: %s", esp_err_to_name(err));
    return false;
  }
  if (status != 200) {
    *error = "http-" + std::to_string(status);
    ESP_LOGW(kTag, "GitHub check HTTP %d", status);
    return false;
  }
  return true;
}

UpdateSnapshot parse_release(const std::string& body) {
  UpdateSnapshot info;
  info.current = PRINTSPHERE_RELEASE_VERSION;
  cJSON* root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    info.error = "bad-json";
    return info;
  }
  const cJSON* tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
  if (!cJSON_IsString(tag) || tag->valuestring == nullptr || tag->valuestring[0] == '\0') {
    cJSON_Delete(root);
    info.error = "no-release";
    return info;
  }
  info.latest = tag->valuestring;

  const SemVer current = parse_semver(info.current);
  const SemVer latest = parse_semver(info.latest);
  if (!current.valid || !latest.valid) {
    cJSON_Delete(root);
    info.error = "bad-version";
    return info;
  }
  info.available = compare_semver(latest, current) > 0;

  const cJSON* assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
  if (cJSON_IsArray(assets)) {
    const cJSON* asset = nullptr;
    cJSON_ArrayForEach(asset, assets) {
      const cJSON* name = cJSON_GetObjectItemCaseSensitive(asset, "name");
      const cJSON* url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
      if (!cJSON_IsString(name) || name->valuestring == nullptr) continue;
      if (!cJSON_IsString(url) || url->valuestring == nullptr || url->valuestring[0] == '\0') continue;
      if (!ota_asset_matches(name->valuestring, kBoardToken)) continue;
      info.url = url->valuestring;
      break;
    }
  }
  cJSON_Delete(root);
  if (info.url.empty()) {
    info.error = "no-asset";
    info.available = false;
    return info;
  }
  info.ok = true;
  return info;
}

}  // namespace

std::string UpdateSnapshot::to_json() const {
  std::string body = "{\"checked\":";
  body += checked ? "true" : "false";
  body += ",\"checking\":";
  body += checking ? "true" : "false";
  body += ",\"available\":";
  body += available ? "true" : "false";
  body += ",\"ok\":";
  body += ok ? "true" : "false";
  body += ",\"current\":\"";
  body += json_escape(current);
  body += "\",\"latest\":\"";
  body += json_escape(latest);
  body += "\",\"url\":\"";
  body += json_escape(url);
  body += "\",\"error\":\"";
  body += json_escape(error);
  body += "\"}";
  return body;
}

Checker& Checker::instance() {
  static Checker checker;
  return checker;
}

void Checker::kick(bool force) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.checking) return;
    if (snapshot_.checked && !force) return;
    snapshot_.checking = true;
    snapshot_.checked = false;
    snapshot_.error.clear();
    if (snapshot_.current.empty()) snapshot_.current = PRINTSPHERE_RELEASE_VERSION;
  }
  if (xTaskCreate(&Checker::task, "gh_update", kTaskStack, this, 3, nullptr) != pdPASS) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.checking = false;
    snapshot_.checked = true;
    snapshot_.error = "task-create";
    ESP_LOGW(kTag, "GitHub update check task create failed");
  }
}

UpdateSnapshot Checker::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void Checker::task(void* arg) {
  static_cast<Checker*>(arg)->run_check();
  vTaskDelete(nullptr);
}

void Checker::run_check() {
  UpdateSnapshot result;
  result.current = PRINTSPHERE_RELEASE_VERSION;
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::string body;
    std::string error;
    if (fetch_latest_release(&body, &error)) {
      result = parse_release(body);
      if (result.ok || result.error == "no-asset" || result.error == "no-release" ||
          result.error == "bad-version") {
        break;
      }
    } else {
      result.error = error;
    }
    ESP_LOGW(kTag, "GitHub check retry %d (%s)", attempt + 1, result.error.c_str());
    vTaskDelay(pdMS_TO_TICKS(800 * (attempt + 1)));
  }
  result.checked = true;
  result.checking = false;
  ESP_LOGI(kTag, "GitHub check current=%s latest=%s available=%d err=%s",
           result.current.c_str(), result.latest.c_str(), result.available ? 1 : 0,
           result.error.c_str());
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(result);
}

}  // namespace printsphere::github
