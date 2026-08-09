#include "printsphere/p1s_camera_client.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_tls.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.camera";
constexpr uint16_t kCameraPort = 6000;
constexpr size_t kFrameHeaderBytes = 16;
constexpr size_t kMaxFrameBytes = 256U * 1024U;
constexpr size_t kImagePersistentReserveBytes = 20U * 1024U;
constexpr int64_t kAutoRefreshIntervalUs = 1500000;
constexpr int64_t kTlsReadDeadlineUs = 4000000;
constexpr int64_t kTlsWriteDeadlineUs = 2000000;
constexpr uint16_t kTargetWidth = 400;
constexpr uint16_t kTargetHeight = 224;

bool has_frame_data(const std::shared_ptr<std::vector<uint8_t>>& frame_blob) {
  return frame_blob && !frame_blob->empty();
}

uint32_t little_u32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

void store_le32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFU);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

std::array<uint8_t, 80> make_auth_packet(const PrinterConnection& connection) {
  std::array<uint8_t, 80> packet{};
  store_le32(packet.data(), 0x40U);
  store_le32(packet.data() + 4, 0x3000U);
  store_le32(packet.data() + 8, 0U);
  store_le32(packet.data() + 12, 0U);

  const std::string username = connection.mqtt_username.empty() ? "bblp" : connection.mqtt_username;
  const size_t username_bytes = std::min<size_t>(username.size(), 32U);
  const size_t password_bytes = std::min<size_t>(connection.access_code.size(), 32U);

  std::memcpy(packet.data() + 16, username.data(), username_bytes);
  std::memcpy(packet.data() + 48, connection.access_code.data(), password_bytes);
  return packet;
}

const char* ram_region(const void* ptr) {
  if (ptr == nullptr) {
    return "null";
  }
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  return esp_ptr_external_ram(ptr) ? "psram" : "internal";
#else
  return "internal";
#endif
}

size_t allocated_size(const void* ptr) {
  return ptr == nullptr ? 0U : heap_caps_get_allocated_size(const_cast<void*>(ptr));
}

void log_heap_diag(const char* context) {
  ESP_LOGV(kTag,
           "[RAM] %s: int_free=%u int_largest=%u dma_free=%u dma_largest=%u "
           "psram_free=%u psram_largest=%u",
           context != nullptr ? context : "-",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void log_ptr_diag(const char* context, const void* ptr, size_t bytes) {
  ESP_LOGV(kTag,
           "[RAM] %s: ptr=%p bytes=%u alloc=%u ram=%s",
           context != nullptr ? context : "-", ptr,
           static_cast<unsigned>(bytes),
           static_cast<unsigned>(allocated_size(ptr)),
           ram_region(ptr));
  log_heap_diag(context);
}

void log_blob_diag(const char* context, const std::shared_ptr<std::vector<uint8_t>>& blob) {
  const void* data = blob && !blob->empty() ? blob->data() : nullptr;
  ESP_LOGV(kTag,
           "[RAM] %s: size=%u capacity=%u alloc=%u ram=%s data=%p",
           context != nullptr ? context : "-",
           static_cast<unsigned>(blob ? blob->size() : 0U),
           static_cast<unsigned>(blob ? blob->capacity() : 0U),
           static_cast<unsigned>(allocated_size(data)),
           ram_region(data), data);
  log_heap_diag(context);
}

}  // namespace

void P1sCameraClient::configure(PrinterConnection connection) {
  if (connection.mqtt_username.empty()) {
    connection.mqtt_username = "bblp";
  }
  const bool configured = connection.is_ready();

  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    desired_connection_ = std::move(connection);
  }
  reconfigure_requested_.store(true);

  // Keep the last decoded frame across reconfigure so the UI does not flash
  // empty while the TLS session is rebuilt (common after a print finishes).
  P1sCameraSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.frame_blob = snapshot_.frame_blob;
    snapshot.width = snapshot_.width;
    snapshot.height = snapshot_.height;
  }
  snapshot.configured = configured;
  snapshot.enabled = false;
  snapshot.connected = false;
  snapshot.detail = snapshot.configured ? "Camera snapshot ready" : "Camera not configured";
  set_snapshot(std::move(snapshot));
}

bool P1sCameraClient::is_configured() const {
  return desired_connection().is_ready();
}

PrinterConnection P1sCameraClient::desired_connection() const {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return desired_connection_;
}

esp_err_t P1sCameraClient::start() {
  if (task_handle_ != nullptr) {
    return ESP_OK;
  }

  // Place task stack in PSRAM. By the time start() is called, internal RAM is
  // fragmented (Wi-Fi + Cloud MQTT TLS + 44 KB rotation staging) and a 12 KiB
  // contiguous internal-RAM stack allocation reliably fails.
  const BaseType_t result = xTaskCreateWithCaps(
      &P1sCameraClient::task_entry, "p1s_camera", 12288, this, 4, &task_handle_,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}

P1sCameraSnapshot P1sCameraClient::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void P1sCameraClient::observe_printer_snapshot(const PrinterSnapshot& snapshot) {
  const bool now_live = snapshot.print_active ||
                        snapshot.lifecycle == PrintLifecycleState::kPreparing ||
                        snapshot.lifecycle == PrintLifecycleState::kPrinting ||
                        snapshot.lifecycle == PrintLifecycleState::kPaused;
  bool request_reconnect = false;
  {
    std::lock_guard<std::mutex> lock(observed_mutex_);
    observed_model_ = snapshot.local_model;
    observed_rtsp_url_ = snapshot.camera_rtsp_url;
    observed_signature_required_ = snapshot.local_mqtt_signature_required;
    // A1/P1 JPEG sockets often stall when a job ends; force a fresh TLS session.
    if (observed_print_live_ && !now_live) {
      request_reconnect = true;
    }
    observed_print_live_ = now_live;
  }
  if (request_reconnect) {
    force_reconnect_.store(true);
    refresh_requested_.store(true);
  }
}

PrinterModel P1sCameraClient::observed_model() const {
  std::lock_guard<std::mutex> lock(observed_mutex_);
  return observed_model_;
}

std::string P1sCameraClient::observed_rtsp_url() const {
  std::lock_guard<std::mutex> lock(observed_mutex_);
  return observed_rtsp_url_;
}

bool P1sCameraClient::observed_signature_required() const {
  std::lock_guard<std::mutex> lock(observed_mutex_);
  return observed_signature_required_;
}

void P1sCameraClient::task_entry(void* context) {
  static_cast<P1sCameraClient*>(context)->task_loop();
}

void P1sCameraClient::set_snapshot(P1sCameraSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(snapshot);
}

void P1sCameraClient::set_status_snapshot(bool configured, bool enabled, bool connected,
                                          const char* detail) {
  const std::string next_detail = detail != nullptr ? detail : "";
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.configured == configured && snapshot_.enabled == enabled &&
      snapshot_.connected == connected && snapshot_.detail == next_detail) {
    return;
  }
  snapshot_.configured = configured;
  snapshot_.enabled = enabled;
  snapshot_.connected = connected;
  snapshot_.detail = next_detail;
}

void P1sCameraClient::set_frame_snapshot(bool configured, bool enabled, bool connected,
                                         const char* detail,
                                         std::shared_ptr<std::vector<uint8_t>> frame_blob,
                                         uint16_t width, uint16_t height) {
  const std::string next_detail = detail != nullptr ? detail : "";
  const void* next_blob = frame_blob.get();
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.configured == configured && snapshot_.enabled == enabled &&
      snapshot_.connected == connected && snapshot_.detail == next_detail &&
      snapshot_.frame_blob.get() == next_blob && snapshot_.width == width &&
      snapshot_.height == height) {
    return;
  }
  snapshot_.configured = configured;
  snapshot_.enabled = enabled;
  snapshot_.connected = connected;
  snapshot_.detail = next_detail;
  snapshot_.frame_blob = std::move(frame_blob);
  snapshot_.width = width;
  snapshot_.height = height;
}

bool P1sCameraClient::has_cached_frame() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_frame_data(snapshot_.frame_blob);
}

bool P1sCameraClient::read_exact(esp_tls_t* tls, void* buffer, size_t length) {
  auto* out = static_cast<uint8_t*>(buffer);
  size_t offset = 0;
  const int64_t deadline_us = esp_timer_get_time() + kTlsReadDeadlineUs;
  while (offset < length) {
    if (esp_timer_get_time() >= deadline_us) {
      return false;
    }
    const ssize_t read = esp_tls_conn_read(tls, out + offset, length - offset);
    if (read <= 0) {
      return false;
    }
    offset += static_cast<size_t>(read);
  }
  return true;
}

bool P1sCameraClient::write_all(esp_tls_t* tls, const void* buffer, size_t length) {
  const auto* data = static_cast<const uint8_t*>(buffer);
  size_t offset = 0;
  const int64_t deadline_us = esp_timer_get_time() + kTlsWriteDeadlineUs;
  while (offset < length) {
    if (esp_timer_get_time() >= deadline_us) {
      return false;
    }
    const ssize_t written = esp_tls_conn_write(tls, data + offset, length - offset);
    if (written <= 0) {
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool P1sCameraClient::ensure_connected(const PrinterConnection& connection) {
  if (tls_ != nullptr) {
    return true;
  }

  log_heap_diag("camera before tls init");

  esp_tls_cfg_t tls_cfg = {};
  tls_cfg.timeout_ms = 7000;
  tls_cfg.skip_common_name = true;
  tls_cfg.addr_family = ESP_TLS_AF_INET;
  tls_cfg.tls_version = ESP_TLS_VER_TLS_1_2;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
  tls_cfg.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif

  tls_ = esp_tls_init();
  if (tls_ == nullptr) {
    ESP_LOGW(kTag, "Failed to allocate TLS context");
    return false;
  }

  const int connect_result = esp_tls_conn_new_sync(connection.host.c_str(),
                                                   static_cast<int>(connection.host.size()),
                                                   kCameraPort, &tls_cfg, tls_);
  if (connect_result != 1) {
    ESP_LOGW(kTag, "Camera TLS connect failed");
    disconnect();
    return false;
  }

  const auto auth_packet = make_auth_packet(connection);
  if (!write_all(tls_, auth_packet.data(), auth_packet.size())) {
    ESP_LOGW(kTag, "Camera auth write failed");
    disconnect();
    return false;
  }

  ESP_LOGI(kTag, "Connected to local chamber camera on %s:%u", connection.host.c_str(),
           static_cast<unsigned>(kCameraPort));
  log_heap_diag("camera after tls auth");
  return true;
}

void P1sCameraClient::disconnect() {
  if (tls_ != nullptr) {
    esp_tls_conn_destroy(tls_);
    tls_ = nullptr;
  }
}

bool P1sCameraClient::decode_frame_rgb565(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                                          std::shared_ptr<std::vector<uint8_t>>* out_blob,
                                          uint16_t* out_width, uint16_t* out_height) {
  if (!jpeg_blob || jpeg_blob->empty() || out_blob == nullptr || out_width == nullptr ||
      out_height == nullptr) {
    return false;
  }

  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  config.scale.width = kTargetWidth;
  config.scale.height = kTargetHeight;

  jpeg_dec_handle_t decoder = nullptr;
  jpeg_dec_io_t io = {};
  jpeg_dec_header_info_t header = {};

  if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) {
    ESP_LOGW(kTag, "JPEG decoder open failed");
    return false;
  }

  io.inbuf = const_cast<uint8_t*>(jpeg_blob->data());
  io.inbuf_len = static_cast<int>(jpeg_blob->size());

  bool ok = false;
  void* aligned_out = nullptr;
  do {
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK) {
      ESP_LOGW(kTag, "JPEG header parse failed");
      break;
    }

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(decoder, &out_len) != JPEG_ERR_OK || out_len <= 0) {
      ESP_LOGW(kTag, "JPEG output length query failed");
      break;
    }

    aligned_out = jpeg_calloc_align(static_cast<size_t>(out_len), 16);
    if (aligned_out == nullptr) {
      ESP_LOGW(kTag, "JPEG aligned buffer allocation failed");
      log_heap_diag("camera jpeg aligned allocation failed");
      break;
    }
    log_ptr_diag("camera jpeg aligned decode buffer", aligned_out,
                 static_cast<size_t>(out_len));

    io.outbuf = static_cast<uint8_t*>(aligned_out);
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
      ESP_LOGW(kTag, "JPEG decode failed");
      break;
    }

    auto decoded = std::make_shared<std::vector<uint8_t>>();
    decoded->reserve(std::max(static_cast<size_t>(out_len), kImagePersistentReserveBytes));
    decoded->resize(static_cast<size_t>(out_len));
    std::memcpy(decoded->data(), aligned_out, static_cast<size_t>(out_len));
    log_blob_diag("camera rgb565 decoded buffer", decoded);
    *out_blob = std::move(decoded);
    *out_width = config.scale.width > 0U ? config.scale.width : header.width;
    *out_height = config.scale.height > 0U ? config.scale.height : header.height;
    ok = true;
  } while (false);

  if (aligned_out != nullptr) {
    jpeg_free_align(aligned_out);
  }
  if (decoder != nullptr) {
    jpeg_dec_close(decoder);
  }
  return ok;
}

bool P1sCameraClient::fetch_frame_once(const PrinterConnection& connection) {
  last_fetch_error_.clear();
  if (!connection.is_ready()) {
    last_fetch_error_ = "Camera not configured";
    return false;
  }

  if (!ensure_connected(connection)) {
    last_fetch_error_ = "Camera connect failed";
    return false;
  }

  for (int frame_index = 0; frame_index < 3; ++frame_index) {
    if (frame_index > 0 && !enabled_.load()) {
      ESP_LOGD(kTag, "Camera disabled mid-fetch, aborting");
      last_fetch_error_ = "Camera off";
      return false;
    }
    uint8_t header[kFrameHeaderBytes] = {};
    if (!read_exact(tls_, header, sizeof(header))) {
      ESP_LOGW(kTag, "Camera frame header read failed");
      disconnect();
      last_fetch_error_ = "Camera stream timed out";
      return false;
    }

    const uint32_t reserved0 = little_u32(header + 4);
    const uint32_t frame_marker = little_u32(header + 8);
    const uint32_t reserved1 = little_u32(header + 12);
    if (reserved0 != 0U || reserved1 != 0U || (frame_marker != 0U && frame_marker != 1U)) {
      ESP_LOGW(kTag, "Camera frame header unexpected: size=%u marker=%u r0=%u r1=%u",
               static_cast<unsigned>(little_u32(header)), static_cast<unsigned>(frame_marker),
               static_cast<unsigned>(reserved0), static_cast<unsigned>(reserved1));
      disconnect();
      last_fetch_error_ = "Camera stream invalid";
      return false;
    }

    const uint32_t frame_size = little_u32(header);
    if (frame_size < 4U || frame_size > kMaxFrameBytes) {
      ESP_LOGW(kTag, "Camera frame size invalid: %u", static_cast<unsigned>(frame_size));
      disconnect();
      last_fetch_error_ = "Camera stream invalid";
      return false;
    }

    auto jpeg_frame = std::make_shared<std::vector<uint8_t>>();
    jpeg_frame->reserve(std::max(static_cast<size_t>(frame_size), kImagePersistentReserveBytes));
    jpeg_frame->resize(frame_size);
    if (!read_exact(tls_, jpeg_frame->data(), jpeg_frame->size())) {
      ESP_LOGW(kTag, "Camera frame body read failed");
      disconnect();
      last_fetch_error_ = "Camera stream timed out";
      return false;
    }
    log_blob_diag("camera jpeg frame buffer", jpeg_frame);

    if (!enabled_.load()) {
      ESP_LOGD(kTag, "Camera disabled before decode, dropping received frame");
      last_fetch_error_ = "Camera off";
      return false;
    }

    if ((*jpeg_frame)[0] != 0xFFU || (*jpeg_frame)[1] != 0xD8U ||
        (*jpeg_frame)[jpeg_frame->size() - 2] != 0xFFU ||
        (*jpeg_frame)[jpeg_frame->size() - 1] != 0xD9U) {
      ESP_LOGW(kTag, "Camera frame is not a complete JPEG");
      continue;
    }

    std::shared_ptr<std::vector<uint8_t>> rgb565_frame;
    uint16_t width = 0;
    uint16_t height = 0;
    if (!decode_frame_rgb565(jpeg_frame, &rgb565_frame, &width, &height)) {
      continue;
    }

    if (!enabled_.load()) {
      ESP_LOGD(kTag, "Camera disabled after decode, dropping frame");
      last_fetch_error_ = "Camera off";
      return false;
    }

    set_frame_snapshot(true, enabled_.load(), true, "Camera image updated",
                       std::move(rgb565_frame), width, height);
    ESP_LOGI(kTag, "Camera snapshot decoded: %ux%u RGB565", static_cast<unsigned>(width),
             static_cast<unsigned>(height));
    log_heap_diag("camera after snapshot publish");
    return true;
  }

  ESP_LOGW(kTag, "No valid camera JPEG frame received");
  disconnect();
  last_fetch_error_ = "Camera image failed";
  return false;
}

void P1sCameraClient::task_loop() {
  int64_t last_fetch_us = 0;

  while (true) {
    if (reconfigure_requested_.exchange(false)) {
      disconnect();
      refresh_requested_.store(false);
      idle_notified_ = false;
      was_enabled_ = false;
      consecutive_connect_failures_ = 0;
      last_fetch_us = 0;
    }

    if (force_reconnect_.exchange(false)) {
      disconnect();
      consecutive_connect_failures_ = 0;
      last_fetch_us = 0;
      idle_notified_ = false;
    }

    const PrinterConnection connection = desired_connection();
    if (!connection.is_ready()) {
      idle_notified_ = false;
      set_status_snapshot(false, false, false, "Camera not configured");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (!network_ready_.load()) {
      disconnect();
      idle_notified_ = false;
      was_enabled_ = false;
      set_status_snapshot(true, enabled_.load(), false, "Camera waiting for Wi-Fi");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    const bool enabled_now = enabled_.load();
    if (enabled_now && !was_enabled_) {
      // Rising edge: drop any stale TLS session left over from a print and
      // fetch immediately (do not wait for the auto-refresh interval).
      disconnect();
      consecutive_connect_failures_ = 0;
      last_fetch_us = 0;
      refresh_requested_.store(true);
      idle_notified_ = false;
    }
    was_enabled_ = enabled_now;

    if (!enabled_now) {
      disconnect();
      if (!idle_notified_) {
        set_status_snapshot(true, false, false, has_cached_frame() ? "Tap for new image"
                                                                   : "Camera off");
        idle_notified_ = true;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    idle_notified_ = false;

    const PrinterModel model = observed_model();
    const std::string rtsp_url = observed_rtsp_url();
    const bool signature_required = observed_signature_required();
    const bool rtsp_camera = printer_model_has_rtsp_camera(model) || !rtsp_url.empty();
    const bool jpeg_camera =
        !rtsp_camera && (model == PrinterModel::kUnknown || printer_model_has_jpeg_camera(model));
    if (rtsp_camera) {
      disconnect();

      std::string detail;
      if (signature_required) {
        detail = "Enable Developer Mode for RTSP live view";
      } else if (rtsp_url.empty()) {
        detail = "RTSP live view disabled on printer";
      } else {
        detail = "RTSP live view not supported yet on this model";
      }

      set_frame_snapshot(true, true, false, detail.c_str(), nullptr, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!jpeg_camera) {
      disconnect();
      set_frame_snapshot(true, true, false, "Live camera not available on this model", nullptr, 0,
                         0);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    const int64_t now_us = esp_timer_get_time();
    // last_fetch_us == 0 means "fetch ASAP" (page open / reconnect / print end).
    const bool due =
        last_fetch_us == 0 || (now_us - last_fetch_us) >= kAutoRefreshIntervalUs;
    if (!refresh_requested_.exchange(false) && !due) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    set_status_snapshot(true, true, false,
                        has_cached_frame() ? "Refreshing camera..." : "Loading camera image");

    const bool ok = fetch_frame_once(connection);
    last_fetch_us = esp_timer_get_time();
    if (!ok) {
      if (!enabled_.load()) {
        consecutive_connect_failures_ = 0;
        continue;
      }
      ++consecutive_connect_failures_;
      const uint32_t backoff_ms =
          consecutive_connect_failures_ <= 1 ? 2000U :
          consecutive_connect_failures_ <= 2 ? 4000U :
          consecutive_connect_failures_ <= 4 ? 8000U :
          consecutive_connect_failures_ <= 6 ? 15000U : 30000U;
      const char* detail = last_fetch_error_.empty() ? "Camera image failed"
                                                     : last_fetch_error_.c_str();
      // Keep the last good frame; only update the status line.
      set_status_snapshot(true, enabled_.load(), false, detail);
      disconnect();
      vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    } else {
      consecutive_connect_failures_ = 0;
    }
  }
}

}  // namespace printsphere
