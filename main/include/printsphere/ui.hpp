#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lvgl.h"
#include "esp_err.h"
#include "printsphere/config_store.hpp"
#include "printsphere/printer_state.hpp"

namespace printsphere {

enum class ScreenPowerMode : uint8_t {
  kAwake,
  kDimmed,
  kOff,
};

class Ui {
 public:
  esp_err_t initialize();
  void set_arc_color_scheme(const ArcColorScheme& colors);
  void apply_snapshot(const PrinterSnapshot& snapshot);
  void update_power_save(bool on_battery, bool print_active);
  bool is_low_power_mode_active() const;
  ScreenPowerMode screen_power_mode() const { return screen_power_mode_; }
  bool is_page2_active() const { return active_page_ == 1; }
  bool is_camera_page_active() const { return active_page_ == 2; }
  bool consume_camera_refresh_request();

 private:
  esp_err_t build_dashboard();
  void apply_ring_visual_locked(const PrinterSnapshot& snapshot);
  void apply_snapshot_locked(const PrinterSnapshot& snapshot, bool force_ring_refresh);
  bool ensure_preview_image_loaded_locked(bool force_reload);
  void release_preview_image_locked();
  void apply_page_visibility();
  void apply_logo_visibility();
  void handle_ring_timer();
  void note_activity(bool wake_display);
  void wake_display();
  void apply_brightness_policy();
  void set_active_page(int page);
  void handle_pager_event(lv_event_t* event);
  void handle_screen_event(lv_event_t* event);
  void set_brightness_percent(int brightness_percent);
  static void ring_timer_cb(lv_timer_t* timer);
  static void pager_event_cb(lv_event_t* event);
  static void screen_event_cb(lv_event_t* event);

  bool initialized_ = false;
  lv_display_t* display_ = nullptr;
  lv_obj_t* screen_ = nullptr;
  lv_obj_t* pager_ = nullptr;
  lv_obj_t* fixed_overlay_ = nullptr;
  lv_obj_t* page1_ = nullptr;
  lv_obj_t* page2_ = nullptr;
  lv_obj_t* page3_ = nullptr;
  lv_obj_t* status_arc_ = nullptr;
  lv_obj_t* progress_label_ = nullptr;
  lv_obj_t* battery_icon_label_ = nullptr;
  lv_obj_t* battery_pct_label_ = nullptr;
  lv_obj_t* badge_slot_ = nullptr;
  lv_obj_t* sync_spinner_ = nullptr;
  lv_obj_t* sync_label_ = nullptr;
  lv_obj_t* logo_badge_ = nullptr;
  lv_obj_t* logo_image_ = nullptr;
  lv_obj_t* status_label_ = nullptr;
  lv_obj_t* detail_label_ = nullptr;
  lv_obj_t* layer_label_ = nullptr;
  lv_obj_t* nozzle_prefix_label_ = nullptr;
  lv_obj_t* nozzle_value_label_ = nullptr;
  lv_obj_t* bed_prefix_label_ = nullptr;
  lv_obj_t* bed_value_label_ = nullptr;
  lv_obj_t* remaining_prefix_label_ = nullptr;
  lv_obj_t* remaining_label_ = nullptr;
  lv_obj_t* remaining_row_ = nullptr;
  lv_obj_t* brightness_overlay_ = nullptr;
  lv_obj_t* page2_shell_ = nullptr;
  lv_obj_t* page2_image_ = nullptr;
  lv_obj_t* page2_note_ = nullptr;
  lv_obj_t* page2_subnote_ = nullptr;
  lv_obj_t* page3_image_ = nullptr;
  lv_obj_t* page3_note_ = nullptr;
  lv_obj_t* page3_subnote_ = nullptr;
  lv_timer_t* ring_anim_timer_ = nullptr;
  int user_brightness_percent_ = 80;
  int applied_brightness_percent_ = -1;
  bool gesture_active_ = false;
  bool overlay_visible_ = false;
  bool scrolling_ = false;
  bool deferred_snapshot_pending_ = false;
  bool detail_visible_ = true;
  bool show_logo_ = false;
  bool accent_initialized_ = false;
  bool preview_image_visible_ = false;
  bool preview_text_image_mode_ = false;
  bool camera_image_visible_ = false;
  bool camera_text_image_mode_ = false;
  bool ring_animation_active_ = false;
  bool swipe_switched_ = false;
  lv_coord_t gesture_start_x_ = 0;
  lv_coord_t gesture_start_y_ = 0;
  int gesture_start_brightness_ = 80;
  int active_page_ = 0;
  ArcColorScheme arc_colors_{};
  uint32_t last_accent_hex_ = 0;
  std::atomic<uint32_t> last_activity_tick_ms_{0};
  ScreenPowerMode screen_power_mode_ = ScreenPowerMode::kAwake;
  std::string last_ui_status_;
  bool last_print_active_ = false;
  lv_image_dsc_t preview_image_dsc_{};
  std::shared_ptr<std::vector<uint8_t>> last_preview_blob_{};
  std::shared_ptr<std::vector<uint8_t>> last_preview_raw_{};
  lv_image_dsc_t camera_image_dsc_{};
  std::shared_ptr<std::vector<uint8_t>> last_camera_blob_{};
  uint16_t last_camera_width_ = 0;
  uint16_t last_camera_height_ = 0;
  mutable std::mutex camera_refresh_mutex_{};
  bool camera_refresh_requested_ = false;
  PrinterSnapshot deferred_snapshot_{};
  PrinterSnapshot last_snapshot_{};
};

}  // namespace printsphere
