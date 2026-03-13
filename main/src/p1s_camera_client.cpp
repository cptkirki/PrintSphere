#include "printsphere/p1s_camera_client.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.camera";
constexpr uint16_t kCameraPort = 6000;
constexpr size_t kFrameHeaderBytes = 16;
constexpr size_t kMaxFrameBytes = 256U * 1024U;
constexpr int64_t kAutoRefreshIntervalUs = 2000000;
constexpr uint16_t kTargetWidth = 400;
constexpr uint16_t kTargetHeight = 224;

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

}  // namespace

void P1sCameraClient::configure(PrinterConnection connection) {
  if (connection.mqtt_username.empty()) {
    connection.mqtt_username = "bblp";
  }

  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    desired_connection_ = std::move(connection);
  }
  reconfigure_requested_.store(true);

  P1sCameraSnapshot snapshot;
  snapshot.configured = desired_connection().is_ready();
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

  const BaseType_t result =
      xTaskCreate(&P1sCameraClient::task_entry, "p1s_camera", 12288, this, 4, &task_handle_);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}

P1sCameraSnapshot P1sCameraClient::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void P1sCameraClient::task_entry(void* context) {
  static_cast<P1sCameraClient*>(context)->task_loop();
}

void P1sCameraClient::set_snapshot(P1sCameraSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(snapshot);
}

bool P1sCameraClient::read_exact(esp_tls_t* tls, void* buffer, size_t length) {
  auto* out = static_cast<uint8_t*>(buffer);
  size_t offset = 0;
  while (offset < length) {
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
  while (offset < length) {
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
      break;
    }

    io.outbuf = static_cast<uint8_t*>(aligned_out);
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
      ESP_LOGW(kTag, "JPEG decode failed");
      break;
    }

    auto decoded = std::make_shared<std::vector<uint8_t>>();
    decoded->resize(static_cast<size_t>(out_len));
    std::memcpy(decoded->data(), aligned_out, static_cast<size_t>(out_len));
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
  if (!connection.is_ready()) {
    return false;
  }

  if (!ensure_connected(connection)) {
    return false;
  }

  for (int frame_index = 0; frame_index < 3; ++frame_index) {
    uint8_t header[kFrameHeaderBytes] = {};
    if (!read_exact(tls_, header, sizeof(header))) {
      ESP_LOGW(kTag, "Camera frame header read failed");
      disconnect();
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
      return false;
    }

    const uint32_t frame_size = little_u32(header);
    if (frame_size < 4U || frame_size > kMaxFrameBytes) {
      ESP_LOGW(kTag, "Camera frame size invalid: %u", static_cast<unsigned>(frame_size));
      disconnect();
      return false;
    }

    auto jpeg_frame = std::make_shared<std::vector<uint8_t>>();
    jpeg_frame->resize(frame_size);
    if (!read_exact(tls_, jpeg_frame->data(), jpeg_frame->size())) {
      ESP_LOGW(kTag, "Camera frame body read failed");
      disconnect();
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

    P1sCameraSnapshot next = snapshot();
    next.configured = true;
    next.enabled = enabled_.load();
    next.connected = true;
    next.detail = "Camera image updated";
    next.frame_blob = std::move(rgb565_frame);
    next.width = width;
    next.height = height;
    set_snapshot(std::move(next));
    ESP_LOGI(kTag, "Camera snapshot decoded: %ux%u RGB565", static_cast<unsigned>(width),
             static_cast<unsigned>(height));
    return true;
  }

  ESP_LOGW(kTag, "No valid camera JPEG frame received");
  return false;
}

void P1sCameraClient::task_loop() {
  int64_t last_fetch_us = 0;

  while (true) {
    if (reconfigure_requested_.exchange(false)) {
      disconnect();
      refresh_requested_.store(false);
      last_fetch_us = 0;
    }

    const PrinterConnection connection = desired_connection();
    if (!connection.is_ready()) {
      P1sCameraSnapshot current = snapshot();
      current.configured = false;
      current.enabled = false;
      current.connected = false;
      current.detail = "Camera not configured";
      set_snapshot(std::move(current));
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (!network_ready_.load()) {
      disconnect();
      P1sCameraSnapshot current = snapshot();
      current.configured = true;
      current.enabled = enabled_.load();
      current.connected = false;
      current.detail = "Camera waiting for Wi-Fi";
      set_snapshot(std::move(current));
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!enabled_.load()) {
      disconnect();
      P1sCameraSnapshot current = snapshot();
      current.configured = true;
      current.enabled = false;
      current.connected = false;
      current.detail = current.frame_blob && !current.frame_blob->empty() ? "Tap for new image"
                                                                          : "Camera off";
      set_snapshot(std::move(current));
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    const int64_t now_us = esp_timer_get_time();
    const bool due = last_fetch_us != 0 && (now_us - last_fetch_us) >= kAutoRefreshIntervalUs;
    if (!refresh_requested_.exchange(false) && !due) {
      P1sCameraSnapshot idle = snapshot();
      idle.configured = true;
      idle.enabled = true;
      idle.connected = tls_ != nullptr;
      idle.detail = "Tap for new image";
      set_snapshot(std::move(idle));
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    P1sCameraSnapshot fetching = snapshot();
    fetching.configured = true;
    fetching.enabled = true;
    fetching.connected = false;
    fetching.detail = "Loading camera image";
    set_snapshot(std::move(fetching));

    const bool ok = fetch_frame_once(connection);
    last_fetch_us = esp_timer_get_time();
    if (!ok) {
      P1sCameraSnapshot retry = snapshot();
      retry.configured = true;
      retry.enabled = enabled_.load();
      retry.connected = false;
      retry.detail = "Camera image failed";
      set_snapshot(std::move(retry));
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
  }
}

}  // namespace printsphere
