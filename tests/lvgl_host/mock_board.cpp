#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "printsphere/board_config.hpp"
std::atomic<uint64_t> host_ticks{0};
std::atomic<unsigned> host_png_decodes{0}, host_png_nonowner_decodes{0};
std::thread::id host_ui_owner;
std::recursive_mutex host_lvgl_mutex;
std::vector<uint16_t> framebuffer;
lv_display_t* bsp_display_start_with_config(bsp_display_cfg_t*) {
  auto* display = lv_display_create(printsphere::board::kDisplayWidth, printsphere::board::kDisplayHeight);
  framebuffer.resize(printsphere::board::kDisplayWidth * printsphere::board::kDisplayHeight);
  lv_display_set_buffers(display, framebuffer.data(), nullptr, framebuffer.size()*2, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
  return display;
}
int bsp_display_lock(uint32_t) { host_lvgl_mutex.lock(); return ESP_OK; }
void bsp_display_unlock() { host_lvgl_mutex.unlock(); }
int bsp_display_brightness_set(int) { return ESP_OK; }
