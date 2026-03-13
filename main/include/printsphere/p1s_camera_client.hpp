#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_jpeg_dec.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/config_store.hpp"

namespace printsphere {

struct P1sCameraSnapshot {
  bool configured = false;
  bool enabled = false;
  bool connected = false;
  std::string detail = "Live camera off";
  std::shared_ptr<std::vector<uint8_t>> frame_blob;
  uint16_t width = 0;
  uint16_t height = 0;
};

class P1sCameraClient {
 public:
  P1sCameraClient() = default;

  void configure(PrinterConnection connection);
  bool is_configured() const;
  void set_network_ready(bool ready) { network_ready_.store(ready); }
  void set_enabled(bool enabled) { enabled_.store(enabled); }
  void request_refresh() { refresh_requested_.store(true); }
  esp_err_t start();
  P1sCameraSnapshot snapshot() const;

 private:
  static void task_entry(void* context);

  void task_loop();
  PrinterConnection desired_connection() const;
  bool ensure_connected(const PrinterConnection& connection);
  void disconnect();
  bool fetch_frame_once(const PrinterConnection& connection);
  void set_snapshot(P1sCameraSnapshot snapshot);
  void set_status_snapshot(bool configured, bool enabled, bool connected, const char* detail);
  void set_frame_snapshot(bool configured, bool enabled, bool connected, const char* detail,
                          std::shared_ptr<std::vector<uint8_t>> frame_blob, uint16_t width,
                          uint16_t height);
  bool has_cached_frame() const;
  static bool read_exact(esp_tls_t* tls, void* buffer, size_t length);
  static bool write_all(esp_tls_t* tls, const void* buffer, size_t length);
  static bool decode_frame_rgb565(const std::shared_ptr<std::vector<uint8_t>>& jpeg_blob,
                                  std::shared_ptr<std::vector<uint8_t>>* out_blob, uint16_t* out_width,
                                  uint16_t* out_height);

  mutable std::mutex mutex_{};
  P1sCameraSnapshot snapshot_{};
  mutable std::mutex config_mutex_{};
  PrinterConnection desired_connection_{};
  TaskHandle_t task_handle_ = nullptr;
  esp_tls_t* tls_ = nullptr;
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> refresh_requested_{false};
  std::atomic<bool> reconfigure_requested_{false};
  bool idle_notified_ = false;
};

}  // namespace printsphere
