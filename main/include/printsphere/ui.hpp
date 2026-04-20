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
  void set_display_rotation(DisplayRotation rotation);
  esp_err_t initialize();
  void set_arc_color_scheme(const ArcColorScheme& colors);
  void apply_snapshot(const PrinterSnapshot& snapshot);
  void update_power_save(bool on_battery, bool print_active);
  void set_battery_display_policy(const BatteryDisplayPolicy& policy);
  bool is_low_power_mode_active() const;
  ScreenPowerMode screen_power_mode() const { return screen_power_mode_; }
  bool is_config_page_active() const { return !scrolling_ && active_page_ == 0; }
  bool is_page2_active() const { return !scrolling_ && active_page_ == 3; }
  bool is_camera_page_active() const { return !scrolling_ && active_page_ == 4; }
  bool is_camera_page_visible() const { return active_page_ == 4; }
  bool is_page_transition_active() const { return scrolling_; }
  void set_portal_access_state(bool lock_enabled, bool request_authorized, bool session_active,
                               bool pin_active, const std::string& pin_code,
                               uint32_t pin_remaining_s, uint32_t session_remaining_s);
  bool consume_camera_refresh_request();
  bool consume_chamber_light_toggle_request();
  bool consume_portal_unlock_request();

  struct PrinterCardInfo {
    uint8_t index = 0;
    std::string name;
    std::string model;
    std::string host;
    bool active = false;
    bool connected = false;
  };
  void update_printer_cards(const std::vector<PrinterCardInfo>& cards);
  int consume_printer_switch_request();
  void request_wake_display();

 private:
  esp_err_t build_dashboard();
  void apply_ring_visual_locked(const PrinterSnapshot& snapshot);
  void apply_snapshot_locked(const PrinterSnapshot& snapshot, bool force_ring_refresh,
                             std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw = nullptr,
                             const lv_image_dsc_t* pre_decoded_dsc = nullptr);
  bool ensure_preview_image_loaded_locked(
      bool force_reload,
      std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw = nullptr,
      const lv_image_dsc_t* pre_decoded_dsc = nullptr);
  void release_preview_image_locked();
  void apply_page_visibility();
  void apply_logo_visibility();
  void update_page_availability_locked(const PrinterSnapshot& snapshot);
  void note_activity(bool wake_display);
  void wake_display();
  void apply_brightness_policy();
  void set_pager_scroll_locked(bool locked);
  void set_active_page(int page);
  int clamp_enabled_page(int page) const;
  int next_enabled_page(int page, int direction) const;
  int nearest_enabled_page_for_scroll() const;
  bool page_enabled(int page) const;
  lv_obj_t* page_object(int page) const;
  void handle_pager_event(lv_event_t* event);
  void handle_screen_event(lv_event_t* event);
  void handle_logo_event(lv_event_t* event);
  void update_portal_access_visuals_locked();
  void compute_portal_texts_locked();
  void set_brightness_percent(int brightness_percent);
  void stop_ring_animations_locked();
  static void pulse_anim_exec_cb(void* var, int32_t scale);
  static void pager_event_cb(lv_event_t* event);
  static void screen_event_cb(lv_event_t* event);
  static void logo_event_cb(lv_event_t* event);

  bool initialized_ = false;
  lv_display_t* display_ = nullptr;
  lv_obj_t* screen_ = nullptr;
  lv_obj_t* pager_ = nullptr;
  lv_obj_t* fixed_overlay_ = nullptr;
  lv_obj_t* page0_ = nullptr;
  lv_obj_t* page0_title_ = nullptr;
  lv_obj_t* page0_card_list_ = nullptr;
  lv_obj_t* page0_empty_note_ = nullptr;

  struct PrinterCardWidgets {
    lv_obj_t* card = nullptr;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* model_label = nullptr;
    lv_obj_t* host_label = nullptr;
    lv_obj_t* status_dot = nullptr;
    uint8_t profile_index = 0;
  };
  std::vector<PrinterCardWidgets> page0_cards_;
  std::vector<PrinterCardInfo>    last_printer_cards_;  // change-detection cache
  int pending_printer_switch_ = -1;

  void rebuild_printer_cards_locked(const std::vector<PrinterCardInfo>& cards);
  void replay_card_animations_locked();
  void apply_page0_parallax();
  static void printer_card_click_cb(lv_event_t* event);

  // --- AMS page (page index 1) ---
  lv_obj_t* ams_page_ = nullptr;
  lv_obj_t* ams_tray_row_ = nullptr;
  lv_obj_t* ams_tray_col_[kMaxAmsTrays] = {};
  lv_obj_t* ams_tray_rect_[kMaxAmsTrays] = {};
  lv_obj_t* ams_tray_fill_[kMaxAmsTrays] = {};   // dark overlay for empty portion
  lv_obj_t* ams_tray_pct_[kMaxAmsTrays] = {};    // percentage label inside rect
  lv_obj_t* ams_tray_slot_label_[kMaxAmsTrays] = {};
  lv_obj_t* ams_tray_type_[kMaxAmsTrays] = {};
  lv_obj_t* ams_tray_arrow_[kMaxAmsTrays] = {};  // green triangle indicator below active pill
  lv_obj_t* ams_ext_col_ = nullptr;           // external spool column
  lv_obj_t* ams_ext_rect_ = nullptr;          // external spool pill rect
  lv_obj_t* ams_ext_type_ = nullptr;          // "EXT" label at top of ext pill
  lv_obj_t* ams_ext_mat_ = nullptr;           // material type label (e.g. PLA) below EXT
  lv_obj_t* ams_ext_arrow_ = nullptr;         // green triangle indicator below ext pill
  lv_obj_t* ams_shelf_ = nullptr;             // gray shelf behind upper pills
  lv_obj_t* ams_base_ = nullptr;              // dark base behind lower pills
  bool ams_ext_spool_shown_ = false;          // tracks current ext spool layout state
  lv_obj_t* ams_humidity_drop_ = nullptr;
  lv_obj_t* ams_humidity_label_ = nullptr;
  lv_obj_t* ams_temp_label_ = nullptr;
  lv_obj_t* ams_note_ = nullptr;
  bool ams_page_available_ = false;

  lv_obj_t* page1_ = nullptr;
  lv_obj_t* page2_ = nullptr;
  lv_obj_t* page3_ = nullptr;
  lv_obj_t* status_arc_ = nullptr;
  lv_obj_t* progress_label_ = nullptr;
  lv_obj_t* battery_icon_label_ = nullptr;
  lv_obj_t* battery_pct_label_ = nullptr;
  lv_obj_t* badge_slot_ = nullptr;
  lv_obj_t* logo_badge_ = nullptr;
  lv_obj_t* logo_image_ = nullptr;
  lv_obj_t* status_label_ = nullptr;
  lv_obj_t* detail_label_ = nullptr;
  lv_obj_t* layer_label_ = nullptr;
  lv_obj_t* nozzle_prefix_label_ = nullptr;
  lv_obj_t* nozzle_value_label_ = nullptr;
  lv_obj_t* nozzle_aux_label_ = nullptr;
  lv_obj_t* bed_prefix_label_ = nullptr;
  lv_obj_t* bed_value_label_ = nullptr;
  lv_obj_t* bed_aux_label_ = nullptr;
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
  lv_obj_t* portal_hint_label_ = nullptr;
  lv_obj_t* portal_overlay_card_ = nullptr;
  lv_obj_t* portal_overlay_title_ = nullptr;
  lv_obj_t* portal_overlay_value_ = nullptr;
  lv_obj_t* portal_overlay_detail_ = nullptr;
  lv_timer_t* ring_anim_timer_ = nullptr;  // unused, ambient sweep timer removed
  int user_brightness_percent_ = 80;
  int applied_brightness_percent_ = -1;
  bool gesture_active_ = false;
  bool overlay_visible_ = false;
  bool scrolling_ = false;
  bool deferred_snapshot_pending_ = false;
  bool detail_visible_ = true;
  bool show_logo_ = false;
  bool accent_initialized_ = false;
  bool preview_page_available_ = true;
  bool preview_image_visible_ = false;
  bool preview_text_image_mode_ = false;
  bool camera_page_available_ = true;
  bool camera_image_visible_ = false;
  bool camera_text_image_mode_ = false;
  bool nozzle_aux_visible_ = false;
  bool bed_aux_visible_ = false;
  bool ring_animation_active_ = false;
  bool swipe_switched_ = false;
  bool pager_scroll_locked_ = false;
  uint8_t active_ring_anim_kind_ = 0;
  uint32_t pulse_base_hex_ = 0;
  bool pulse_both_parts_ = false;
  lv_coord_t gesture_start_x_ = 0;
  lv_coord_t gesture_start_y_ = 0;
  int gesture_start_brightness_ = 80;
  int active_page_ = 0;
  ArcColorScheme arc_colors_{};
  uint32_t last_accent_hex_ = 0;
  uint32_t last_ring_main_hex_ = UINT32_MAX;
  uint32_t last_ring_indicator_hex_ = UINT32_MAX;
  uint32_t last_ring_text_hex_ = UINT32_MAX;
  std::atomic<uint32_t> last_activity_tick_ms_{0};
  ScreenPowerMode screen_power_mode_ = ScreenPowerMode::kAwake;
  std::string last_ui_status_;
  bool last_print_active_ = false;
  std::string last_diag_status_;
  std::string last_diag_detail_;
  std::string last_diag_stage_;
  lv_image_dsc_t preview_image_dsc_{};
  std::shared_ptr<std::vector<uint8_t>> last_preview_blob_{};
  std::shared_ptr<std::vector<uint8_t>> last_preview_raw_{};
  lv_image_dsc_t camera_image_dscs_[2]{};
  std::shared_ptr<std::vector<uint8_t>> camera_blobs_[2]{};
  uint8_t active_camera_slot_ = 0;
  bool camera_slot_initialized_ = false;
  uint16_t last_camera_width_ = 0;
  uint16_t last_camera_height_ = 0;
  bool logo_clickable_ = false;
  bool logo_recolor_enabled_ = false;
  uint32_t logo_recolor_hex_ = 0;
  bool portal_lock_enabled_ = true;
  bool portal_request_authorized_ = false;
  bool portal_session_active_ = false;
  bool portal_pin_active_ = false;
  uint64_t portal_hint_boot_ms_ = 0;
  uint32_t portal_pin_remaining_s_ = 0;
  uint32_t portal_session_remaining_s_ = 0;
  std::string portal_pin_code_;
  std::string portal_hint_text_;
  std::string portal_overlay_title_text_;
  std::string portal_overlay_value_text_;
  std::string portal_overlay_detail_text_;
  mutable std::mutex camera_refresh_mutex_{};
  bool camera_refresh_requested_ = false;
  std::atomic<bool> chamber_light_toggle_requested_{false};
  std::atomic<bool> portal_unlock_requested_{false};
  PrinterSnapshot deferred_snapshot_{};
  PrinterSnapshot last_snapshot_{};
  DisplayRotation display_rotation_ = DisplayRotation::k0;
  BatteryDisplayPolicy battery_display_policy_{};
};

}  // namespace printsphere
