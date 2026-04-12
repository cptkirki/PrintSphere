#include "printsphere/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

#include "misc/cache/instance/lv_image_cache.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "png.h"
#include "printsphere/board_config.hpp"

extern "C" {
extern const lv_image_dsc_t bambuicon_small;
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_32;
extern const lv_font_t dosis_40;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t mdi_30;
extern const lv_font_t mdi_40;
}

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.ui";
constexpr int kDefaultBrightnessPercent = 80;
constexpr int kRingStrokeWidth = 22;
constexpr int kRemainingRowY = 172;
constexpr int kPage2PreviewSize = 320;
constexpr int kPage2PreviewYOffset = -12;
constexpr int kPage2NoteWithImageY = 138;
constexpr int kPage2SubnoteWithImageY = 188;
constexpr int kPage3CameraWidth = 400;
constexpr int kPage3CameraHeight = 224;
constexpr int kPage3CameraYOffset = 20;
constexpr int kPage3NoteWithImageY = 150;
constexpr int kPage3SubnoteWithImageY = 182;
constexpr int kAuxTempRowY = 28;
constexpr int kSwipeThresholdPx = 24;
constexpr int kRotatedVisualOffsetX = 0;
constexpr int kRotatedVisualOffsetY = 0;
constexpr int kManualMinBrightnessPercent = 4;
constexpr uint32_t kRingAnimationTickMs = 33U;
constexpr uint8_t kRingPulseDepthPercent = 55U;
constexpr uint32_t kDotPulseDurationMs = 1500U;
constexpr int32_t kDotPulseOpaMin = 80;
constexpr int32_t kDotPulseOpaMax = 255;
constexpr int32_t kParallaxTitleMaxY = -18;
constexpr int32_t kParallaxCardsMaxY = -8;
constexpr uint32_t kBatteryDimTimeoutIdleMs = 20000U;
constexpr uint32_t kBatteryOffTimeoutIdleMs = 60000U;
constexpr uint32_t kBatteryDimTimeoutActiveMs = 30000U;
constexpr uint32_t kBatteryOffTimeoutActiveMs = 120000U;
constexpr uint64_t kPortalHintIntroMs = 5ULL * 60ULL * 1000ULL;
constexpr uint32_t kCardRevealDurationMs = 300U;
constexpr int32_t kCardRevealYStart = 28;
constexpr uint32_t kCardRevealStaggerMs = 55U;
constexpr uint32_t kPrintRotPeriodMs = 18000U;  // 18s per ambient sweep rotation
constexpr uint32_t kRingBaseDark = 0x101010;
constexpr uint32_t kRingIdleSolid = 0x404040;
constexpr char kDegreeC[] = "\xC2\xB0""C";
constexpr char kMdiClock[] = "\xF3\xB1\x91\x8E";
constexpr char kMdiNozzle[] = "\xF3\xB0\xB9\x9B";
constexpr char kMdiBed[] = "\xF3\xB1\xA1\x9B";
constexpr char kMdiSync[] = "\xF3\xB1\x9B\x87";
constexpr char kMdiBatteryCharging0[] = "\xF3\xB0\xA0\x92";
constexpr char kMdiBatteryCharging10[] = "\xF3\xB0\xA0\x88";
constexpr char kMdiBatteryCharging20[] = "\xF3\xB0\xA0\x89";
constexpr char kMdiBatteryCharging30[] = "\xF3\xB0\xA0\x8A";
constexpr char kMdiBatteryCharging40[] = "\xF3\xB0\xA0\x8B";
constexpr char kMdiBatteryCharging50[] = "\xF3\xB0\xA0\x8C";
constexpr char kMdiBatteryCharging60[] = "\xF3\xB0\xA0\x8D";
constexpr char kMdiBatteryCharging70[] = "\xF3\xB0\xA0\x8E";
constexpr char kMdiBatteryCharging80[] = "\xF3\xB0\xA0\x8F";
constexpr char kMdiBatteryCharging90[] = "\xF3\xB0\xA0\x90";
constexpr char kMdiBatteryCharging100[] = "\xF3\xB0\xA0\x87";
constexpr char kMdiBatteryAlert[] = "\xF3\xB1\x83\x8D";
constexpr char kMdiBattery10[] = "\xF3\xB0\x81\xBA";
constexpr char kMdiBattery20[] = "\xF3\xB0\x81\xBB";
constexpr char kMdiBattery30[] = "\xF3\xB0\x81\xBC";
constexpr char kMdiBattery40[] = "\xF3\xB0\x81\xBD";
constexpr char kMdiBattery50[] = "\xF3\xB0\x81\xBE";
constexpr char kMdiBattery60[] = "\xF3\xB0\x81\xBF";
constexpr char kMdiBattery70[] = "\xF3\xB0\x82\x80";
constexpr char kMdiBattery80[] = "\xF3\xB0\x82\x81";
constexpr char kMdiBattery90[] = "\xF3\xB0\x82\x82";
constexpr char kMdiBattery100[] = "\xF3\xB0\x81\xB9";

class LvglLockGuard {
 public:
  explicit LvglLockGuard(uint32_t timeout_ms, const char* caller = "?")
      : caller_(caller) {
    const uint32_t before = esp_log_timestamp();
    locked_ = bsp_display_lock(timeout_ms) == ESP_OK;
    const uint32_t wait_ms = esp_log_timestamp() - before;
    if (!locked_) {
      ESP_LOGW(kTag, "LVGL lock FAILED after %lums (timeout=%lu, caller=%s)",
               (unsigned long)wait_ms, (unsigned long)timeout_ms, caller_);
    } else if (wait_ms > 150) {
      ESP_LOGW(kTag, "LVGL lock waited %lums (caller=%s)",
               (unsigned long)wait_ms, caller_);
    }
    acquired_ts_ = esp_log_timestamp();
  }

  ~LvglLockGuard() {
    if (locked_) {
      const uint32_t held_ms = esp_log_timestamp() - acquired_ts_;
      bsp_display_unlock();
      if (held_ms > 80) {
        ESP_LOGW(kTag, "LVGL lock held %lums (caller=%s)",
                 (unsigned long)held_ms, caller_);
      }
    }
  }

  bool locked() const { return locked_; }

 private:
  const char* caller_ = "?";
  uint32_t acquired_ts_ = 0;
  bool locked_ = false;
};

bsp_display_rotation_t bsp_rotation_for(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
      return BSP_DISPLAY_ROTATE_90;
    case DisplayRotation::k180:
      return BSP_DISPLAY_ROTATE_180;
    case DisplayRotation::k270:
      return BSP_DISPLAY_ROTATE_270;
    case DisplayRotation::k0:
    default:
      return BSP_DISPLAY_ROTATE_0;
  }
}

void apply_touch_rotation_flags(DisplayRotation rotation, bsp_display_cfg_t* cfg) {
  if (cfg == nullptr) {
    return;
  }

  switch (rotation) {
    case DisplayRotation::k90:
      cfg->touch_flags.swap_xy = 1;
      cfg->touch_flags.mirror_x = 0;
      cfg->touch_flags.mirror_y = 1;
      break;
    case DisplayRotation::k180:
      cfg->touch_flags.swap_xy = 0;
      cfg->touch_flags.mirror_x = 0;
      cfg->touch_flags.mirror_y = 0;
      break;
    case DisplayRotation::k270:
      cfg->touch_flags.swap_xy = 1;
      cfg->touch_flags.mirror_x = 1;
      cfg->touch_flags.mirror_y = 0;
      break;
    case DisplayRotation::k0:
    default:
      cfg->touch_flags.swap_xy = 0;
      cfg->touch_flags.mirror_x = 1;
      cfg->touch_flags.mirror_y = 1;
      break;
  }
}

void make_transparent(lv_obj_t* obj) {
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
}

void set_hidden(lv_obj_t* obj, bool hidden) {
  if (obj == nullptr) {
    return;
  }

  const bool currently_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
  if (currently_hidden == hidden) {
    return;
  }

  if (hidden) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

void set_clickable(lv_obj_t* obj, bool clickable) {
  if (obj == nullptr) {
    return;
  }

  const bool currently_clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  if (currently_clickable == clickable) {
    return;
  }

  if (clickable) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  }
}

void enable_touch_bubble(lv_obj_t* obj) {
  if (obj == nullptr) {
    return;
  }
  lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

int display_rotation_visual_offset_x(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
    case DisplayRotation::k270:
      return kRotatedVisualOffsetX;
    case DisplayRotation::k0:
    case DisplayRotation::k180:
    default:
      return 0;
  }
}

int display_rotation_visual_offset_y(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
    case DisplayRotation::k270:
      return kRotatedVisualOffsetY;
    case DisplayRotation::k0:
    case DisplayRotation::k180:
    default:
      return 0;
  }
}

void apply_display_rotation_visual_offset(lv_obj_t* obj, DisplayRotation rotation) {
  if (obj == nullptr) {
    return;
  }

  lv_obj_set_style_translate_x(obj, display_rotation_visual_offset_x(rotation), 0);
  lv_obj_set_style_translate_y(obj, display_rotation_visual_offset_y(rotation), 0);
}

void set_label_text_if_changed(lv_obj_t* label, const char* text) {
  if (label == nullptr || text == nullptr) {
    return;
  }

  const char* current = lv_label_get_text(label);
  if (current != nullptr && std::strcmp(current, text) == 0) {
    return;
  }

  lv_label_set_text(label, text);
}

void set_label_text_if_changed(lv_obj_t* label, const std::string& text) {
  set_label_text_if_changed(label, text.c_str());
}

std::string optional_temperature_text(const char* label, float temperature_c, bool known = false) {
  if (label == nullptr || (!known && temperature_c <= 0.0f)) {
    return {};
  }

  char buffer[40] = {};
  std::snprintf(buffer, sizeof(buffer), "%s %.0f%s", label, temperature_c, kDegreeC);
  return buffer;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool decode_preview_png(const std::shared_ptr<std::vector<uint8_t>>& encoded_blob,
                        std::shared_ptr<std::vector<uint8_t>>* decoded_blob,
                        lv_image_dsc_t* image_dsc) {
  if (!encoded_blob || encoded_blob->empty() || decoded_blob == nullptr || image_dsc == nullptr) {
    return false;
  }

  png_image image;
  std::memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;

  if (!png_image_begin_read_from_memory(&image, encoded_blob->data(), encoded_blob->size())) {
    ESP_LOGW(kTag, "Preview PNG header decode failed");
    return false;
  }

  image.format = PNG_FORMAT_BGRA;
  const size_t row_stride = static_cast<size_t>(image.width) * 4U;
  const size_t decoded_size = PNG_IMAGE_SIZE(image);
  auto raw = std::make_shared<std::vector<uint8_t>>();
  raw->resize(decoded_size);

  const bool ok = png_image_finish_read(&image, nullptr, raw->data(),
                                        static_cast<png_int_32>(row_stride), nullptr) != 0;
  if (!ok) {
    ESP_LOGW(kTag, "Preview PNG decode failed: %s", image.message);
    png_image_free(&image);
    return false;
  }

  png_image_free(&image);

  std::memset(image_dsc, 0, sizeof(*image_dsc));
  image_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  image_dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
  image_dsc->header.flags = 0;
  image_dsc->header.w = static_cast<uint16_t>(image.width);
  image_dsc->header.h = static_cast<uint16_t>(image.height);
  image_dsc->header.stride = static_cast<uint16_t>(row_stride);
  image_dsc->data_size = static_cast<uint32_t>(raw->size());
  image_dsc->data = raw->data();
  *decoded_blob = std::move(raw);
  return true;
}

bool configure_camera_rgb565(const std::shared_ptr<std::vector<uint8_t>>& decoded_blob,
                             uint16_t width, uint16_t height, lv_image_dsc_t* image_dsc) {
  if (!decoded_blob || decoded_blob->empty() || image_dsc == nullptr || width == 0U || height == 0U) {
    return false;
  }

  std::memset(image_dsc, 0, sizeof(*image_dsc));
  image_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  image_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
  image_dsc->header.flags = 0;
  image_dsc->header.w = width;
  image_dsc->header.h = height;
  image_dsc->header.stride = static_cast<uint16_t>(width * sizeof(uint16_t));
  image_dsc->data_size = static_cast<uint32_t>(decoded_blob->size());
  image_dsc->data = decoded_blob->data();
  return true;
}

bool stage_contains(const std::string& stage, const char* needle) {
  return stage.find(needle) != std::string::npos;
}

bool contains_token(const std::string& value, const char* token) {
  return value.find(token) != std::string::npos;
}

std::string effective_status(const PrinterSnapshot& snapshot) {
  if (!snapshot.raw_status.empty()) {
    return lower_copy(snapshot.raw_status);
  }
  return {};
}

std::string effective_stage(const PrinterSnapshot& snapshot) {
  if (!snapshot.raw_stage.empty()) {
    return lower_copy(snapshot.raw_stage);
  }
  return lower_copy(snapshot.stage);
}

bool is_finished_status(const std::string& status) {
  return status == "finish" || status == "finished" || contains_token(status, "finish") ||
         contains_token(status, "success") || contains_token(status, "complete");
}

uint32_t scale_color(uint32_t color, uint16_t scale_0_to_255) {
  const uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
  const uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFF);
  const uint8_t b = static_cast<uint8_t>(color & 0xFF);
  const uint8_t sr =
      static_cast<uint8_t>((static_cast<uint32_t>(r) * scale_0_to_255 + 127U) / 255U);
  const uint8_t sg =
      static_cast<uint8_t>((static_cast<uint32_t>(g) * scale_0_to_255 + 127U) / 255U);
  const uint8_t sb =
      static_cast<uint8_t>((static_cast<uint32_t>(b) * scale_0_to_255 + 127U) / 255U);
  return (static_cast<uint32_t>(sr) << 16) | (static_cast<uint32_t>(sg) << 8) | sb;
}

// pulse_between() removed — color pulses are now driven by lv_anim_t
// in apply_ring_visual_locked() via pulse_anim_exec_cb.

enum class RingAnimKind : uint8_t {
  kNone,           // Static — no animation
  kFilamentLoad,   // Arc value sweeps 0→100, repeat
  kFilamentUnload, // Arc value sweeps 100→0, repeat
  kPulseBoth,      // Both MAIN & INDICATOR color pulse
  kPulseIndicator, // Only INDICATOR color pulses
  kAmbientSweep,   // Slow rotation (printing state)
};

struct RingVisual {
  uint32_t main_hex = kRingBaseDark;
  uint32_t indicator_hex = 0xFFFFFF;
  int value_override = -1;
  RingAnimKind anim_kind = RingAnimKind::kNone;
  uint32_t pulse_base_hex = 0;    // Base color for pulse animations
  uint32_t pulse_period_ms = 0;   // Period for pulse animations
  bool animated() const { return anim_kind != RingAnimKind::kNone; }
};

RingVisual lifecycle_ring_visual(const PrinterSnapshot& snapshot, const ArcColorScheme& colors) {
  const std::string stage = effective_stage(snapshot);
  const std::string status = effective_status(snapshot);
  const int progress = std::clamp(static_cast<int>(std::lround(snapshot.progress_percent)), 0, 100);
  const bool is_filament = stage_contains(stage, "filament_loading") ||
                           stage_contains(stage, "filament_unloading") ||
                           stage_contains(stage, "changing_filament") ||
                           stage_contains(stage, "loading") || stage_contains(stage, "unloading");
  const bool is_download = stage_contains(stage, "model_download") || stage_contains(stage, "download") ||
                           contains_token(status, "download") || snapshot.ui_status == "downloading";
  const bool done_strict =
      snapshot.ui_status == "done" || snapshot.lifecycle == PrintLifecycleState::kFinished ||
      (is_finished_status(status) && stage_contains(stage, "idle") && progress == 100);

  RingVisual visual = {};

  if (is_filament) {
    const bool is_loading =
        stage_contains(stage, "filament_loading") || stage_contains(stage, "changing_filament");
    visual.main_hex = kRingBaseDark;
    visual.indicator_hex = colors.filament;
    visual.anim_kind = is_loading ? RingAnimKind::kFilamentLoad : RingAnimKind::kFilamentUnload;
    return visual;
  }

  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    visual.main_hex = colors.setup;
    visual.indicator_hex = colors.setup;
    return visual;
  }
  if (snapshot.connection == PrinterConnectionState::kError || snapshot.has_error ||
      snapshot.lifecycle == PrintLifecycleState::kError) {
    visual.main_hex = colors.error;
    visual.indicator_hex = colors.error;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.error;
    visual.pulse_period_ms = 1600U;
    return visual;
  }
  if (!snapshot.wifi_connected) {
    visual.main_hex = colors.offline;
    visual.indicator_hex = colors.offline;
    return visual;
  }

  if (done_strict) {
    visual.main_hex = colors.done;
    visual.indicator_hex = colors.done;
    return visual;
  }

  if (is_download) {
    visual.main_hex = kRingBaseDark;
    visual.indicator_hex = colors.preheat;
    visual.anim_kind = RingAnimKind::kPulseIndicator;
    visual.pulse_base_hex = colors.preheat;
    visual.pulse_period_ms = 1400U;
    return visual;
  }

  if (stage_contains(stage, "heatbed_preheating") || stage_contains(stage, "nozzle_preheating") ||
      stage_contains(stage, "heating_hotend") || stage_contains(stage, "heating_chamber") ||
      stage_contains(stage, "waiting_for_heatbed_temperature") ||
      stage_contains(stage, "thermal_preconditioning") || stage_contains(stage, "preheat") ||
      status == "prepare" || snapshot.ui_status == "preparing" ||
      snapshot.ui_status == "preheating" ||
      snapshot.lifecycle == PrintLifecycleState::kPreparing) {
    visual.main_hex = colors.preheat;
    visual.indicator_hex = colors.preheat;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.preheat;
    visual.pulse_period_ms = 1400U;
    return visual;
  }

  if (stage_contains(stage, "cleaning_nozzle_tip") || stage_contains(stage, "clean") ||
      snapshot.ui_status == "clean nozzle") {
    visual.main_hex = colors.clean;
    visual.indicator_hex = colors.clean;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.clean;
    visual.pulse_period_ms = 1200U;
    return visual;
  }

  if (stage_contains(stage, "auto_bed_leveling") || stage_contains(stage, "bed_level") ||
      stage_contains(stage, "level") || stage_contains(stage, "measuring_surface") ||
      snapshot.ui_status == "bed level") {
    visual.main_hex = colors.level;
    visual.indicator_hex = colors.level;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.level;
    visual.pulse_period_ms = 1400U;
    return visual;
  }

  if (stage_contains(stage, "cool") || stage_contains(stage, "heated_bedcooling")) {
    visual.main_hex = colors.cool;
    visual.indicator_hex = colors.cool;
    return visual;
  }

  const bool idleish = snapshot.lifecycle == PrintLifecycleState::kIdle ||
                       stage_contains(stage, "idle") || stage_contains(stage, "offline");
  if (idleish) {
    if (stage_contains(stage, "offline") || status == "offline" || snapshot.ui_status == "offline") {
      visual.main_hex = colors.offline;
      visual.indicator_hex = colors.offline;
      return visual;
    }
    const bool active_job = snapshot.print_active ||
                            (snapshot.progress_percent > 0.0f && snapshot.progress_percent < 100.0f) ||
                            status == "running" || status == "prepare" ||
                            snapshot.lifecycle == PrintLifecycleState::kPrinting ||
                            snapshot.lifecycle == PrintLifecycleState::kPaused ||
                            snapshot.lifecycle == PrintLifecycleState::kPreparing;
    if (active_job) {
      visual.main_hex = kRingBaseDark;
      visual.indicator_hex = colors.idle_active;
    } else {
      visual.main_hex = colors.idle;
      visual.indicator_hex = colors.idle;
    }
    return visual;
  }

  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPaused:
      visual.main_hex = kRingBaseDark;
      visual.indicator_hex = colors.printing;
      visual.anim_kind = RingAnimKind::kAmbientSweep;
      return visual;
    case PrintLifecycleState::kPreparing:
      visual.main_hex = colors.preheat;
      visual.indicator_hex = colors.preheat;
      return visual;
    case PrintLifecycleState::kFinished:
      visual.main_hex = colors.done;
      visual.indicator_hex = colors.done;
      return visual;
    case PrintLifecycleState::kIdle:
      visual.main_hex = colors.idle;
      visual.indicator_hex = colors.idle;
      return visual;
    case PrintLifecycleState::kUnknown:
    default:
      break;
  }

  if (snapshot.connection == PrinterConnectionState::kBooting ||
      snapshot.connection == PrinterConnectionState::kConnecting ||
      snapshot.connection == PrinterConnectionState::kReadyForLanConnect) {
    visual.main_hex = colors.setup;
    visual.indicator_hex = colors.setup;
    return visual;
  }

  visual.main_hex = colors.unknown;
  visual.indicator_hex = colors.unknown;
  return visual;
}

uint32_t stable_status_text_hex(const PrinterSnapshot& snapshot, const ArcColorScheme& colors) {
  const std::string stage = effective_stage(snapshot);
  const std::string status = effective_status(snapshot);
  const int progress = std::clamp(static_cast<int>(std::lround(snapshot.progress_percent)), 0, 100);
  const bool is_filament = stage_contains(stage, "filament_loading") ||
                           stage_contains(stage, "filament_unloading") ||
                           stage_contains(stage, "changing_filament") ||
                           stage_contains(stage, "loading") || stage_contains(stage, "unloading");
  const bool is_download = stage_contains(stage, "model_download") || stage_contains(stage, "download") ||
                           contains_token(status, "download") || snapshot.ui_status == "downloading";
  const bool done_strict =
      snapshot.ui_status == "done" || snapshot.lifecycle == PrintLifecycleState::kFinished ||
      (is_finished_status(status) && stage_contains(stage, "idle") && progress == 100);

  if (is_filament) {
    return colors.filament;
  }
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return colors.setup;
  }
  if (snapshot.connection == PrinterConnectionState::kError || snapshot.has_error ||
      snapshot.lifecycle == PrintLifecycleState::kError) {
    return colors.error;
  }
  if (!snapshot.wifi_connected) {
    return colors.offline;
  }
  if (done_strict) {
    return colors.done;
  }
  if (is_download) {
    return colors.preheat;
  }
  if (stage_contains(stage, "heatbed_preheating") || stage_contains(stage, "nozzle_preheating") ||
      stage_contains(stage, "heating_hotend") || stage_contains(stage, "heating_chamber") ||
      stage_contains(stage, "waiting_for_heatbed_temperature") ||
      stage_contains(stage, "thermal_preconditioning") || stage_contains(stage, "preheat") ||
      status == "prepare" || snapshot.ui_status == "preparing" ||
      snapshot.ui_status == "preheating" ||
      snapshot.lifecycle == PrintLifecycleState::kPreparing) {
    return colors.preheat;
  }
  if (stage_contains(stage, "cleaning_nozzle_tip") || stage_contains(stage, "clean") ||
      snapshot.ui_status == "clean nozzle") {
    return colors.clean;
  }
  if (stage_contains(stage, "auto_bed_leveling") || stage_contains(stage, "bed_level") ||
      stage_contains(stage, "level") || stage_contains(stage, "measuring_surface") ||
      snapshot.ui_status == "bed level") {
    return colors.level;
  }
  if (stage_contains(stage, "cool") || stage_contains(stage, "heated_bedcooling")) {
    return colors.cool;
  }

  const bool idleish = snapshot.lifecycle == PrintLifecycleState::kIdle ||
                       stage_contains(stage, "idle") || stage_contains(stage, "offline");
  if (idleish) {
    const bool offline =
        stage_contains(stage, "offline") || status == "offline" || snapshot.ui_status == "offline";
    if (offline) {
      return colors.offline;
    }
    const bool active_job = snapshot.print_active ||
                            (snapshot.progress_percent > 0.0f && snapshot.progress_percent < 100.0f) ||
                            status == "running" || status == "prepare" ||
                            snapshot.lifecycle == PrintLifecycleState::kPrinting ||
                            snapshot.lifecycle == PrintLifecycleState::kPaused ||
                            snapshot.lifecycle == PrintLifecycleState::kPreparing;
    return active_job ? colors.idle_active : colors.idle;
  }

  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPaused:
      return colors.printing;
    case PrintLifecycleState::kPreparing:
      return colors.preheat;
    case PrintLifecycleState::kFinished:
      return colors.done;
    case PrintLifecycleState::kIdle:
      return colors.idle;
    case PrintLifecycleState::kUnknown:
    default:
      break;
  }

  if (snapshot.connection == PrinterConnectionState::kBooting ||
      snapshot.connection == PrinterConnectionState::kConnecting ||
      snapshot.connection == PrinterConnectionState::kReadyForLanConnect) {
    return colors.setup;
  }

  return colors.unknown;
}

// ring_visual_is_animated() removed — animation state is now tracked
// via active_ring_anim_kind_ in apply_ring_visual_locked().

std::string lifecycle_label(const PrinterSnapshot& snapshot) {
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return "setup";
  }
  if (snapshot.connection == PrinterConnectionState::kConnecting && !snapshot.wifi_connected) {
    return "syncing";
  }
  if (snapshot.connection == PrinterConnectionState::kError || snapshot.has_error ||
      snapshot.lifecycle == PrintLifecycleState::kError) {
    return "failed";
  }
  if (snapshot.connection == PrinterConnectionState::kBooting) {
    return "booting";
  }
  if (snapshot.connection == PrinterConnectionState::kReadyForLanConnect) {
    return "ready";
  }
  if (!snapshot.ui_status.empty()) {
    return snapshot.ui_status;
  }

  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPreparing:
      return "preparing";
    case PrintLifecycleState::kPrinting:
      return "printing";
    case PrintLifecycleState::kPaused:
      return "paused";
    case PrintLifecycleState::kFinished:
      return "done";
    case PrintLifecycleState::kIdle:
      return "idle";
    case PrintLifecycleState::kUnknown:
    default:
      return snapshot.wifi_connected ? "waiting..." : "offline";
  }
}

std::string setup_access_text(const PrinterSnapshot& snapshot) {
  std::string text;
  if (!snapshot.setup_ap_ssid.empty()) {
    text = "AP: " + snapshot.setup_ap_ssid;
  }
  if (!snapshot.setup_ap_password.empty()) {
    if (!text.empty()) {
      text += "\n";
    }
    text += "PW: " + snapshot.setup_ap_password;
  }

  const std::string ip = snapshot.setup_ap_ip.empty() ? "192.168.4.1" : snapshot.setup_ap_ip;
  if (!ip.empty()) {
    if (!text.empty()) {
      text += "\n";
    }
    text += "Open " + ip;
  }

  return text;
}

std::string station_portal_text(const PrinterSnapshot& snapshot) {
  if (!snapshot.wifi_ip.empty()) {
    return "Open " + snapshot.wifi_ip;
  }
  return "Open the portal on your Wi-Fi IP";
}

std::string detail_text(const PrinterSnapshot& snapshot) {
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    if (snapshot.setup_ap_active) {
      return setup_access_text(snapshot);
    }
    if (snapshot.wifi_connected) {
      return station_portal_text(snapshot);
    }
    return {};
  }
  if (!snapshot.wifi_connected) {
    if (snapshot.setup_ap_active) {
      return setup_access_text(snapshot);
    }
    return {};
  }
  if (snapshot.has_error && !snapshot.detail.empty()) {
    return snapshot.detail;
  }
  if (!snapshot.job_name.empty() &&
      (snapshot.lifecycle == PrintLifecycleState::kPreparing ||
       snapshot.lifecycle == PrintLifecycleState::kPrinting ||
       snapshot.lifecycle == PrintLifecycleState::kPaused)) {
    return snapshot.job_name;
  }
  if (!snapshot.detail.empty() && snapshot.detail != snapshot.stage &&
      snapshot.detail != "Connected to local Bambu MQTT" &&
      snapshot.detail != "Printer version info received" &&
      snapshot.detail != "Status payload received") {
    return snapshot.detail;
  }
  return {};
}

std::string layer_text(const PrinterSnapshot& snapshot) {
  char buffer[32] = {};
  if (snapshot.total_layers > 0) {
    std::snprintf(buffer, sizeof(buffer), "Layer: %u / %u", snapshot.current_layer,
                  snapshot.total_layers);
  } else if (snapshot.current_layer > 0) {
    std::snprintf(buffer, sizeof(buffer), "Layer: %u / --", snapshot.current_layer);
  } else {
    std::snprintf(buffer, sizeof(buffer), "Layer: -- / --");
  }
  return buffer;
}

std::string remaining_text(const PrinterSnapshot& snapshot) {
  if (snapshot.ui_status == "done" || snapshot.lifecycle == PrintLifecycleState::kFinished) {
    return "Done";
  }
  if (snapshot.remaining_seconds == 0) {
    return "--m";
  }

  const uint32_t minutes_total = snapshot.remaining_seconds / 60U;
  const uint32_t hours = minutes_total / 60U;
  const uint32_t minutes = minutes_total % 60U;
  char buffer[24] = {};
  if (hours > 0U) {
    std::snprintf(buffer, sizeof(buffer), "%uh %um", static_cast<unsigned int>(hours),
                  static_cast<unsigned int>(minutes));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%um", static_cast<unsigned int>(minutes));
  }
  return buffer;
}

std::string preview_note_text(const PrinterSnapshot& snapshot) {
  if (snapshot.preview_blob && !snapshot.preview_blob->empty()) {
    return {};  // note hidden when cover is loaded — title takes over
  }
  if (!snapshot.preview_url.empty()) {
    return "Loading cloud cover";
  }
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return "Set up printer";
  }
  if (!snapshot.wifi_connected) {
    return "Printer offline";
  }
  if (!snapshot.cloud_detail.empty() && !snapshot.cloud_connected) {
    return "Connecting to cloud";
  }

  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPreparing:
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPaused:
      return "Preparing cover";
    case PrintLifecycleState::kFinished:
      return "Last print done";
    case PrintLifecycleState::kError:
      return "Cover unavailable";
    case PrintLifecycleState::kIdle:
    case PrintLifecycleState::kUnknown:
    default:
      return "No active print";
  }
}

std::string preview_subnote_text(const PrinterSnapshot& snapshot) {
  if (snapshot.print_active && !snapshot.job_name.empty()) {
    return snapshot.job_name;
  }
  if (!snapshot.preview_title.empty()) {
    return snapshot.preview_title;
  }
  if (!snapshot.job_name.empty()) {
    return snapshot.job_name;
  }
  if (!snapshot.cloud_detail.empty()) {
    return snapshot.cloud_detail;
  }
  return snapshot.preview_hint;
}

std::string camera_note_text(const PrinterSnapshot& snapshot) {
  if (snapshot.camera_blob && !snapshot.camera_blob->empty()) {
    return {};  // note hidden when snapshot is loaded — title/subnote takes over
  }
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return "Set up printer";
  }
  if (!snapshot.wifi_connected) {
    return "Camera offline";
  }
  if (!snapshot.camera_detail.empty()) {
    return snapshot.camera_detail;
  }
  return "Tap for new image";
}

std::string camera_subnote_text(const PrinterSnapshot& snapshot) {
  if (!snapshot.job_name.empty()) {
    return snapshot.job_name;
  }
  if (snapshot.camera_blob && !snapshot.camera_blob->empty()) {
    return "Tap to refresh now";
  }
  return "Auto-refresh every 2s";
}

const char* mdi_battery_symbol(const PrinterSnapshot& snapshot) {
  if (!snapshot.usb_present && snapshot.battery_percent == 0U && !snapshot.charging) {
    return kMdiBatteryAlert;
  }

  if (snapshot.charging) {
    if (snapshot.battery_percent >= 95U) return kMdiBatteryCharging100;
    if (snapshot.battery_percent >= 85U) return kMdiBatteryCharging90;
    if (snapshot.battery_percent >= 75U) return kMdiBatteryCharging80;
    if (snapshot.battery_percent >= 65U) return kMdiBatteryCharging70;
    if (snapshot.battery_percent >= 55U) return kMdiBatteryCharging60;
    if (snapshot.battery_percent >= 45U) return kMdiBatteryCharging50;
    if (snapshot.battery_percent >= 35U) return kMdiBatteryCharging40;
    if (snapshot.battery_percent >= 25U) return kMdiBatteryCharging30;
    if (snapshot.battery_percent >= 15U) return kMdiBatteryCharging20;
    if (snapshot.battery_percent >= 5U) return kMdiBatteryCharging10;
    return kMdiBatteryCharging0;
  }

  if (snapshot.battery_percent >= 95U) return kMdiBattery100;
  if (snapshot.battery_percent >= 85U) return kMdiBattery90;
  if (snapshot.battery_percent >= 75U) return kMdiBattery80;
  if (snapshot.battery_percent >= 65U) return kMdiBattery70;
  if (snapshot.battery_percent >= 55U) return kMdiBattery60;
  if (snapshot.battery_percent >= 45U) return kMdiBattery50;
  if (snapshot.battery_percent >= 35U) return kMdiBattery40;
  if (snapshot.battery_percent >= 25U) return kMdiBattery30;
  if (snapshot.battery_percent >= 15U) return kMdiBattery20;
  return kMdiBattery10;
}

std::string battery_icon_text(const PrinterSnapshot& snapshot) {
  return mdi_battery_symbol(snapshot);
}

std::string battery_pct_text(const PrinterSnapshot& snapshot) {
  if (!snapshot.usb_present && snapshot.battery_percent == 0U && !snapshot.charging) {
    return "--%";
  }

  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "%u%%", snapshot.battery_percent);
  return buffer;
}

std::string short_duration_text(uint32_t total_seconds) {
  const uint32_t minutes = total_seconds / 60U;
  const uint32_t seconds = total_seconds % 60U;
  char buffer[24] = {};
  if (minutes > 0U) {
    std::snprintf(buffer, sizeof(buffer), "%um %us", static_cast<unsigned int>(minutes),
                  static_cast<unsigned int>(seconds));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%us", static_cast<unsigned int>(seconds));
  }
  return buffer;
}

bool should_show_logo(const PrinterSnapshot& snapshot) {
  if (!snapshot.wifi_connected) {
    return false;
  }

  switch (snapshot.connection) {
    case PrinterConnectionState::kBooting:
    case PrinterConnectionState::kWaitingForCredentials:
    case PrinterConnectionState::kConnecting:
      return false;
    case PrinterConnectionState::kReadyForLanConnect:
    case PrinterConnectionState::kOnline:
    case PrinterConnectionState::kError:
    default:
      return true;
  }
}

void dot_pulse_exec_cb(void* obj, int32_t val) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(val), 0);
}

void start_dot_pulse(lv_obj_t* dot) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, dot);
  lv_anim_set_exec_cb(&a, dot_pulse_exec_cb);
  lv_anim_set_values(&a, kDotPulseOpaMin, kDotPulseOpaMax);
  lv_anim_set_duration(&a, kDotPulseDurationMs);
  lv_anim_set_reverse_duration(&a, kDotPulseDurationMs);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

// Micro-interaction: uniform scale on card tap (256 = 100% in LVGL 9)
void card_scale_exec_cb(void* obj, int32_t val) {
  lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), val, 0);
}

// Cascading reveal: vertical slide-in per card
void card_reveal_y_exec_cb(void* obj, int32_t val) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), val, 0);
}

// Cascading reveal: fade-in per card
void card_reveal_opa_exec_cb(void* obj, int32_t val) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(val), 0);
}

}  // namespace

void Ui::set_display_rotation(DisplayRotation rotation) {
  if (initialized_) {
    return;
  }
  display_rotation_ = rotation;
}

esp_err_t Ui::initialize() {
  if (initialized_) {
    return ESP_OK;
  }

  portal_hint_boot_ms_ = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);

  bsp_display_cfg_t display_cfg = {
      .lv_adapter_cfg = []() {
        esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
        // Pin LVGL render task to core 1 so WiFi/BT on core 0 can't interrupt rendering.
        cfg.task_core_id = 1;
        return cfg;
      }(),
      .rotation = ESP_LV_ADAPTER_ROTATE_0,
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC,
      .touch_flags = {
          .swap_xy = 0,
          .mirror_x = 1,
          .mirror_y = 1,
      },
  };
  apply_touch_rotation_flags(display_rotation_, &display_cfg);

  display_ = bsp_display_start_with_config(&display_cfg);
  if (display_ == nullptr) {
    ESP_LOGE(kTag, "bsp_display_start failed");
    return ESP_FAIL;
  }

  ESP_RETURN_ON_ERROR(bsp_display_rotation_set(bsp_rotation_for(display_rotation_)), kTag,
                      "apply display rotation failed");

  user_brightness_percent_ = -1;
  applied_brightness_percent_ = -1;
  screen_power_mode_ = ScreenPowerMode::kAwake;
  last_activity_tick_ms_.store(lv_tick_get());
  set_brightness_percent(kDefaultBrightnessPercent);
  ESP_RETURN_ON_ERROR(build_dashboard(), kTag, "build_dashboard failed");

  // With LV_SCROLL_SNAP_NONE the pager decelerates freely after finger release.
  // scroll_throw=90 shrinks velocity to ~10% per tick, reaching zero in ~3 ticks
  // (~30 ms). SCROLL_END fires almost immediately and our single set_active_page()
  // animation takes over cleanly — no competing LVGL snap animations.
  {
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev != nullptr) {
      if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_indev_set_scroll_throw(indev, 90);
        break;
      }
      indev = lv_indev_get_next(indev);
    }
  }

  ring_anim_timer_ = lv_timer_create(&Ui::ring_timer_cb, kRingAnimationTickMs, this);
  lv_timer_pause(ring_anim_timer_);  // Starts paused; resumed only for ambient sweep.

  initialized_ = true;
  ESP_LOGI(kTag, "UI ready with YAML-style pager layout (rotation=%s)",
           to_string(display_rotation_));
  return ESP_OK;
}

void Ui::set_arc_color_scheme(const ArcColorScheme& colors) {
  if (!initialized_) {
    arc_colors_ = colors;
    return;
  }

  LvglLockGuard lock(200, "set_arc_color");
  if (!lock.locked()) {
    return;
  }

  arc_colors_ = colors;
  if (last_snapshot_.ui_status.empty() && last_snapshot_.stage.empty() &&
      last_snapshot_.detail.empty()) {
    return;
  }
  apply_ring_visual_locked(last_snapshot_);
}

bool Ui::consume_camera_refresh_request() {
  std::lock_guard<std::mutex> lock(camera_refresh_mutex_);
  const bool requested = camera_refresh_requested_;
  camera_refresh_requested_ = false;
  return requested;
}

bool Ui::consume_chamber_light_toggle_request() {
  return chamber_light_toggle_requested_.exchange(false);
}

bool Ui::consume_portal_unlock_request() {
  return portal_unlock_requested_.exchange(false);
}

void Ui::set_portal_access_state(bool lock_enabled, bool request_authorized,
                                 bool session_active, bool pin_active,
                                 const std::string& pin_code, uint32_t pin_remaining_s,
                                 uint32_t session_remaining_s) {
  if (!initialized_) {
    return;
  }

  // Store portal data without LVGL lock.  The actual text computation and
  // visual update happen inside apply_snapshot_locked() which already holds
  // the lock.  This eliminates a separate lock acquisition per tick.
  portal_lock_enabled_ = lock_enabled;
  portal_request_authorized_ = request_authorized;
  portal_session_active_ = session_active;
  portal_pin_active_ = pin_active;
  portal_pin_code_ = pin_code;
  portal_pin_remaining_s_ = pin_remaining_s;
  portal_session_remaining_s_ = session_remaining_s;
}

void Ui::compute_portal_texts_locked() {
  const bool provisioning_context =
      last_snapshot_.setup_ap_active ||
      last_snapshot_.connection == PrinterConnectionState::kWaitingForCredentials;
  const bool station_portal_available =
      !last_snapshot_.setup_ap_active && last_snapshot_.wifi_connected && !last_snapshot_.wifi_ip.empty();

  if (portal_pin_active_) {
    portal_hint_text_ = portal_request_authorized_ ? "Web Config PIN active on the display"
                                                   : "Enter the PIN shown on the display";
    portal_overlay_title_text_ = "WEB CONFIG PIN";
    portal_overlay_value_text_ = portal_pin_code_;
    portal_overlay_detail_text_ =
        "Valid for " + short_duration_text(std::max<uint32_t>(portal_pin_remaining_s_, 1U)) +
        ". Enter it in the browser.";
  } else {
    portal_overlay_title_text_.clear();
    portal_overlay_value_text_.clear();
    portal_overlay_detail_text_.clear();
    if (portal_session_active_ && portal_request_authorized_) {
      portal_hint_text_ =
          "Web Config unlocked for " +
          short_duration_text(std::max<uint32_t>(portal_session_remaining_s_, 1U));
    } else if (provisioning_context) {
      portal_hint_text_.clear();
    } else if (!portal_lock_enabled_) {
      portal_hint_text_ =
          station_portal_available ? ("Open " + last_snapshot_.wifi_ip) : "Web Config open";
    } else if (portal_hint_boot_ms_ != 0 &&
               static_cast<uint64_t>(esp_timer_get_time() / 1000ULL) <
                   portal_hint_boot_ms_ + kPortalHintIntroMs) {
      portal_hint_text_ = station_portal_available
                              ? ("Open " + last_snapshot_.wifi_ip + " | Hold for PIN")
                              : "Hold for PIN";
    } else {
      portal_hint_text_.clear();
    }
  }

  update_portal_access_visuals_locked();
}

// --- Page 0: printer cards ---

void Ui::printer_card_click_cb(lv_event_t* event) {
  auto* self = static_cast<Ui*>(lv_event_get_user_data(event));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
  for (const auto& cw : self->page0_cards_) {
    if (cw.card == target) {
      self->pending_printer_switch_ = cw.profile_index;
      self->note_activity(true);

      // Micro-interaction: quick scale bounce (100% → 91% → 100%)
      lv_anim_t sa;
      lv_anim_init(&sa);
      lv_anim_set_var(&sa, target);
      lv_anim_set_exec_cb(&sa, card_scale_exec_cb);
      lv_anim_set_values(&sa, 256, 233);
      lv_anim_set_duration(&sa, 75);
      lv_anim_set_reverse_duration(&sa, 110);
      lv_anim_set_path_cb(&sa, lv_anim_path_ease_out);
      lv_anim_start(&sa);
      break;
    }
  }
}

int Ui::consume_printer_switch_request() {
  int val = pending_printer_switch_;
  pending_printer_switch_ = -1;
  return val;
}

void Ui::apply_page0_parallax() {
  if (page0_title_ == nullptr || page0_card_list_ == nullptr || pager_ == nullptr) {
    return;
  }

  int scroll_x = lv_obj_get_scroll_x(pager_);
  if (scroll_x < 0) scroll_x = 0;

  const int page_w = board::kDisplayWidth;
  const int clamped = (scroll_x > page_w) ? page_w : scroll_x;

  // Page 0 content: fade out + shift up as we scroll away
  const int32_t title_y = static_cast<int32_t>(kParallaxTitleMaxY * clamped / page_w);
  const int32_t cards_y = static_cast<int32_t>(kParallaxCardsMaxY * clamped / page_w);
  const lv_opa_t page0_opa = static_cast<lv_opa_t>(255 - 255 * clamped / page_w);

  lv_obj_set_style_translate_y(page0_title_, title_y, 0);
  lv_obj_set_style_translate_y(page0_card_list_, cards_y, 0);
  // NOTE: lv_obj_set_style_opa() on large containers forces LVGL to render the entire
  // widget tree into a temporary offscreen layer before blending it at the given opacity.
  // With LV_DRAW_LAYER_SIMPLE_BUF_SIZE=24KB on a 466px-wide display that means ~14 layer
  // passes for the card list alone — triggered every LV_EVENT_SCROLL. This reduces scroll
  // fps to ~4. Use only translate_y for the parallax shift; no opa on large containers.
  if (page0_empty_note_ != nullptr) {
    // empty_note is a single small label — opa is cheap here
    lv_obj_set_style_opa(page0_empty_note_, page0_opa, 0);
  }

  // Overlay (arc, progress, battery): fade in as we leave page 0
  const lv_opa_t overlay_opa = static_cast<lv_opa_t>(255 * clamped / page_w);

  // fixed_overlay_ contains the full-screen arc (466×466 px).  Setting opa to any
  // fractional value forces LVGL to render the entire widget tree into a 24 KB
  // offscreen-layer buffer → ~18 layer-passes per scroll event → measurable stutter.
  // Instead we snap to binary visibility: hidden while page 0 dominates, fully
  // opaque once we cross the midpoint toward page 1.  Exactly 0 and LV_OPA_COVER
  // are both "fully decided" → LVGL skips the layer path entirely.
  if (clamped < page_w / 2) {
    set_hidden(fixed_overlay_, true);
  } else {
    set_hidden(fixed_overlay_, false);
    lv_obj_set_style_opa(fixed_overlay_, LV_OPA_COVER, 0);
  }

  // Small text labels: opa fade is cheap (entire label fits in the 24 KB layer buffer
  // in a single pass), so the visual cross-fade is worth keeping.
  lv_obj_set_style_opa(progress_label_, overlay_opa, 0);
  lv_obj_set_style_opa(battery_icon_label_, overlay_opa, 0);
  lv_obj_set_style_opa(battery_pct_label_, overlay_opa, 0);
}

void Ui::update_printer_cards(const std::vector<PrinterCardInfo>& cards) {
  if (!initialized_) {
    return;
  }

  // Only rebuild (and trigger the reveal animation) when the card data has
  // actually changed — the caller runs every main-loop tick, so without this
  // guard the animation restarts hundreds of times per second.
  const auto cards_equal = [](const std::vector<PrinterCardInfo>& a,
                               const std::vector<PrinterCardInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i].index != b[i].index ||
          a[i].active != b[i].active ||
          a[i].connected != b[i].connected ||
          a[i].name != b[i].name ||
          a[i].model != b[i].model ||
          a[i].host != b[i].host) {
        return false;
      }
    }
    return true;
  };
  if (cards_equal(cards, last_printer_cards_)) {
    return;
  }
  last_printer_cards_ = cards;

  LvglLockGuard lock(200, "update_cards");
  if (!lock.locked()) {
    return;
  }

  rebuild_printer_cards_locked(cards);
}

void Ui::rebuild_printer_cards_locked(const std::vector<PrinterCardInfo>& cards) {
  // Remove old card widgets
  for (auto& cw : page0_cards_) {
    if (cw.card != nullptr) {
      lv_obj_delete(cw.card);
    }
  }
  page0_cards_.clear();

  const bool empty = cards.empty();
  set_hidden(page0_card_list_, empty);
  set_hidden(page0_empty_note_, !empty);
  if (empty) {
    return;
  }

  const lv_font_t* font_name = &dosis_20;
  const lv_font_t* font_detail = &lv_font_montserrat_14;

  int card_idx = 0;
  for (const auto& info : cards) {
    // Card container — glasmorphism-lite: semi-transparent bg + shadow elevation
    lv_obj_t* card = lv_obj_create(page0_card_list_);
    lv_obj_set_size(card, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 72, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card, 195, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Shadow for 3D depth on OLED
    lv_obj_set_style_shadow_width(card, 20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_offset_y(card, 6, 0);

    if (info.active) {
      lv_obj_set_style_border_color(card, lv_color_hex(0x00CC66), 0);
      lv_obj_set_style_border_width(card, 2, 0);
      lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    } else {
      // Subtle border to define card edges on dark background
      lv_obj_set_style_border_color(card, lv_color_hex(0x303030), 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    }

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, &Ui::printer_card_click_cb, LV_EVENT_CLICKED, this);

    // Status dot — small colored circle in top-right
    lv_obj_t* dot = lv_obj_create(card);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, info.connected ? lv_color_hex(0x00CC66) : lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    if (info.connected) {
      start_dot_pulse(dot);
    }

    // Printer name (bold, larger)
    lv_obj_t* name_lbl = lv_label_create(card);
    const std::string display_name = info.name.empty() ? info.model : info.name;
    set_label_text_if_changed(name_lbl, display_name);
    lv_obj_set_width(name_lbl, 300);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name_lbl, font_name, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Model
    lv_obj_t* model_lbl = lv_label_create(card);
    set_label_text_if_changed(model_lbl, info.model.empty() ? "Unknown" : info.model);
    lv_obj_set_width(model_lbl, 300);
    lv_label_set_long_mode(model_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(model_lbl, font_detail, 0);
    lv_obj_set_style_text_color(model_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align_to(model_lbl, name_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    // Host IP
    lv_obj_t* host_lbl = lv_label_create(card);
    set_label_text_if_changed(host_lbl, info.host.empty() ? "No local IP" : info.host);
    lv_obj_set_width(host_lbl, 300);
    lv_label_set_long_mode(host_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(host_lbl, font_detail, 0);
    lv_obj_set_style_text_color(host_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align_to(host_lbl, model_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    PrinterCardWidgets cw;
    cw.card = card;
    cw.name_label = name_lbl;
    cw.model_label = model_lbl;
    cw.host_label = host_lbl;
    cw.status_dot = dot;
    cw.profile_index = info.index;
    page0_cards_.push_back(cw);

    // Cascading reveal: start hidden below, fade + slide in with staggered delay
    lv_obj_set_style_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(card, kCardRevealYStart, 0);
    const uint32_t reveal_delay = static_cast<uint32_t>(card_idx) * kCardRevealStaggerMs;

    lv_anim_t ry;
    lv_anim_init(&ry);
    lv_anim_set_var(&ry, card);
    lv_anim_set_exec_cb(&ry, card_reveal_y_exec_cb);
    lv_anim_set_values(&ry, kCardRevealYStart, 0);
    lv_anim_set_duration(&ry, kCardRevealDurationMs);
    lv_anim_set_delay(&ry, reveal_delay);
    LV_ANIM_SET_EASE_OUT_BACK(&ry);
    lv_anim_set_path_cb(&ry, lv_anim_path_custom_bezier3);
    lv_anim_start(&ry);

    lv_anim_t ro;
    lv_anim_init(&ro);
    lv_anim_set_var(&ro, card);
    lv_anim_set_exec_cb(&ro, card_reveal_opa_exec_cb);
    lv_anim_set_values(&ro, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ro, kCardRevealDurationMs);
    lv_anim_set_delay(&ro, reveal_delay);
    lv_anim_set_path_cb(&ro, lv_anim_path_ease_out);
    lv_anim_start(&ro);

    ++card_idx;
  }
}

void Ui::replay_card_animations_locked() {
  int card_idx = 0;
  for (const PrinterCardWidgets& cw : page0_cards_) {
    lv_obj_set_style_opa(cw.card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(cw.card, kCardRevealYStart, 0);
    const uint32_t reveal_delay = static_cast<uint32_t>(card_idx) * kCardRevealStaggerMs;

    lv_anim_t ry;
    lv_anim_init(&ry);
    lv_anim_set_var(&ry, cw.card);
    lv_anim_set_exec_cb(&ry, card_reveal_y_exec_cb);
    lv_anim_set_values(&ry, kCardRevealYStart, 0);
    lv_anim_set_duration(&ry, kCardRevealDurationMs);
    lv_anim_set_delay(&ry, reveal_delay);
    LV_ANIM_SET_EASE_OUT_BACK(&ry);
    lv_anim_set_path_cb(&ry, lv_anim_path_custom_bezier3);
    lv_anim_start(&ry);

    lv_anim_t ro;
    lv_anim_init(&ro);
    lv_anim_set_var(&ro, cw.card);
    lv_anim_set_exec_cb(&ro, card_reveal_opa_exec_cb);
    lv_anim_set_values(&ro, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ro, kCardRevealDurationMs);
    lv_anim_set_delay(&ro, reveal_delay);
    lv_anim_set_path_cb(&ro, lv_anim_path_ease_out);
    lv_anim_start(&ro);

    ++card_idx;
  }
}

void Ui::apply_snapshot(const PrinterSnapshot& snapshot) {
  if (!initialized_) {
    return;
  }

  // Pre-decode preview PNG outside LVGL lock.  decode_preview_png() is pure
  // computation (heap alloc + libpng) that can block the CPU for 200-500 ms
  // on a typical cover image.  Holding the LVGL mutex that long starves the
  // LVGL worker and eventually freezes the display.
  std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw;
  lv_image_dsc_t pre_decoded_dsc{};
  const bool preview_blob_changed =
      snapshot.preview_blob && !snapshot.preview_blob->empty() &&
      last_preview_blob_.get() != snapshot.preview_blob.get();
  // Also pre-decode when the blob exists but hasn't been decoded yet (e.g.
  // blob arrived while another page was active, then user scrolled to preview).
  const bool needs_first_decode =
      !preview_blob_changed && snapshot.preview_blob &&
      !snapshot.preview_blob->empty() &&
      (!last_preview_raw_ || last_preview_raw_->empty());
  if (preview_blob_changed || needs_first_decode) {
    decode_preview_png(snapshot.preview_blob, &pre_decoded_raw, &pre_decoded_dsc);
  }

  LvglLockGuard lock(500, "apply_snapshot");
  if (!lock.locked()) {
    return;
  }

  const PrinterSnapshot previous_snapshot = last_snapshot_;
  last_snapshot_ = snapshot;
  if (scrolling_) {
    deferred_snapshot_ = snapshot;
    deferred_snapshot_pending_ = true;
    return;
  }

  apply_snapshot_locked(snapshot, false,
                        std::move(pre_decoded_raw), &pre_decoded_dsc);
}

void Ui::apply_ring_visual_locked(const PrinterSnapshot& snapshot) {
  const int progress = std::clamp(static_cast<int>(snapshot.progress_percent + 0.5f), 0, 100);
  const RingVisual ring = lifecycle_ring_visual(snapshot, arc_colors_);
  const uint32_t text_hex = stable_status_text_hex(snapshot, arc_colors_);

  // --- Manage lv_anim transitions ---
  const auto anim_kind_u8 = static_cast<uint8_t>(ring.anim_kind);
  if (anim_kind_u8 != active_ring_anim_kind_) {
    // Stop any running ring animations.
    stop_ring_animations_locked();

    active_ring_anim_kind_ = anim_kind_u8;

    switch (ring.anim_kind) {
      case RingAnimKind::kFilamentLoad:
      case RingAnimKind::kFilamentUnload: {
        const bool loading = (ring.anim_kind == RingAnimKind::kFilamentLoad);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, status_arc_);
        lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
          lv_arc_set_value(static_cast<lv_obj_t*>(obj), v);
        });
        lv_anim_set_values(&a, loading ? 0 : 100, loading ? 100 : 0);
        lv_anim_set_duration(&a, 2000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 300);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        break;
      }
      case RingAnimKind::kPulseBoth:
      case RingAnimKind::kPulseIndicator: {
        const uint16_t depth = static_cast<uint16_t>(
            std::min<uint32_t>(kRingPulseDepthPercent, 100U) * 255U / 100U);
        const int32_t min_scale = static_cast<int32_t>(255 > depth ? 255 - depth : 0);
        pulse_base_hex_ = ring.pulse_base_hex;
        pulse_both_parts_ = (ring.anim_kind == RingAnimKind::kPulseBoth);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_exec_cb(&a, pulse_anim_exec_cb);
        lv_anim_set_values(&a, min_scale, 255);
        lv_anim_set_duration(&a, ring.pulse_period_ms / 2);
        lv_anim_set_reverse_duration(&a, ring.pulse_period_ms / 2);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        break;
      }
      case RingAnimKind::kAmbientSweep:
        // Future: slow rotation animation. For now just static.
        break;
      case RingAnimKind::kNone:
        break;
    }
  } else if (ring.anim_kind == RingAnimKind::kPulseBoth ||
             ring.anim_kind == RingAnimKind::kPulseIndicator) {
    // Same animation kind but base color might have changed (e.g. color scheme switch).
    pulse_base_hex_ = ring.pulse_base_hex;
    pulse_both_parts_ = (ring.anim_kind == RingAnimKind::kPulseBoth);
  }

  // --- Apply static properties ---
  // For non-filament states, set the arc value from progress.
  if (ring.anim_kind != RingAnimKind::kFilamentLoad &&
      ring.anim_kind != RingAnimKind::kFilamentUnload) {
    const int displayed_value = (ring.value_override >= 0) ? ring.value_override : progress;
    if (lv_arc_get_value(status_arc_) != displayed_value) {
      lv_arc_set_value(status_arc_, displayed_value);
    }
  }

  // Set colors for non-pulse states (pulse callback handles colors itself).
  if (ring.anim_kind != RingAnimKind::kPulseBoth &&
      ring.anim_kind != RingAnimKind::kPulseIndicator) {
    if (last_ring_main_hex_ != ring.main_hex) {
      lv_obj_set_style_arc_color(status_arc_, lv_color_hex(ring.main_hex), LV_PART_MAIN);
      last_ring_main_hex_ = ring.main_hex;
    }
    if (last_ring_indicator_hex_ != ring.indicator_hex) {
      lv_obj_set_style_arc_color(status_arc_, lv_color_hex(ring.indicator_hex), LV_PART_INDICATOR);
      last_ring_indicator_hex_ = ring.indicator_hex;
    }
  }

  if (last_ring_text_hex_ != text_hex) {
    const lv_color_t text_color = lv_color_hex(text_hex);
    lv_obj_set_style_text_color(progress_label_, text_color, 0);
    lv_obj_set_style_text_color(status_label_, text_color, 0);
    last_ring_text_hex_ = text_hex;
  }
  // Reset arc rotation to top (270°) for non-sweep states.
  if (ring.anim_kind != RingAnimKind::kAmbientSweep) {
    lv_arc_set_rotation(status_arc_, 270);
  }

  // Resume/pause the ring timer — only needed for ambient sweep rotation.
  if (ring_anim_timer_ != nullptr) {
    if (ring.anim_kind == RingAnimKind::kAmbientSweep) {
      lv_timer_resume(ring_anim_timer_);
    } else {
      lv_timer_pause(ring_anim_timer_);
    }
  }
}

void Ui::stop_ring_animations_locked() {
  // Stop filament value animation (var = status_arc_).
  lv_anim_delete(status_arc_, nullptr);
  // Stop pulse color animation (var = this).
  lv_anim_delete(this, pulse_anim_exec_cb);
  active_ring_anim_kind_ = static_cast<uint8_t>(RingAnimKind::kNone);
}

void Ui::pulse_anim_exec_cb(void* var, int32_t scale) {
  auto* ui = static_cast<Ui*>(var);
  if (ui == nullptr || ui->status_arc_ == nullptr) return;
  const uint32_t hex = scale_color(ui->pulse_base_hex_, static_cast<uint16_t>(scale));
  if (ui->pulse_both_parts_) {
    if (ui->last_ring_main_hex_ != hex) {
      lv_obj_set_style_arc_color(ui->status_arc_, lv_color_hex(hex), LV_PART_MAIN);
      ui->last_ring_main_hex_ = hex;
    }
  }
  if (ui->last_ring_indicator_hex_ != hex) {
    lv_obj_set_style_arc_color(ui->status_arc_, lv_color_hex(hex), LV_PART_INDICATOR);
    ui->last_ring_indicator_hex_ = hex;
  }
}

bool Ui::ensure_preview_image_loaded_locked(
    bool force_reload,
    std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw,
    const lv_image_dsc_t* pre_decoded_dsc) {
  if (force_reload) {
    release_preview_image_locked();
  }

  if (last_preview_raw_ && !last_preview_raw_->empty()) {
    lv_image_set_src(page2_image_, &preview_image_dsc_);
    return true;
  }

  // Use pre-decoded data when available (decoded outside LVGL lock).
  if (pre_decoded_raw && !pre_decoded_raw->empty() && pre_decoded_dsc != nullptr) {
    last_preview_raw_ = std::move(pre_decoded_raw);
    preview_image_dsc_ = *pre_decoded_dsc;
    lv_image_set_src(page2_image_, &preview_image_dsc_);
    return true;
  }

  // No fallback decode under lock — pre-decode happens outside the LVGL
  // lock in apply_snapshot().  If we reach here without decoded data the
  // image will appear on the next snapshot tick (~500 ms).
  return false;
}

void Ui::release_preview_image_locked() {
  if (page2_image_ != nullptr) {
    lv_image_set_src(page2_image_, nullptr);
  }
  lv_image_cache_drop(&preview_image_dsc_);
  last_preview_raw_.reset();
  std::memset(&preview_image_dsc_, 0, sizeof(preview_image_dsc_));
}

void Ui::apply_snapshot_locked(const PrinterSnapshot& snapshot, bool force_ring_refresh,
                               std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw,
                               const lv_image_dsc_t* pre_decoded_dsc) {
  deferred_snapshot_pending_ = false;
  update_page_availability_locked(snapshot);

  bool wake_due_to_state_change = false;
  if (!snapshot.ui_status.empty() && snapshot.ui_status != last_ui_status_) {
    wake_due_to_state_change = true;
    last_ui_status_ = snapshot.ui_status;
  }
  if (snapshot.print_active != last_print_active_) {
    wake_due_to_state_change = true;
    last_print_active_ = snapshot.print_active;
  }
  if (wake_due_to_state_change) {
    note_activity(true);
  }

  const int progress = std::clamp(static_cast<int>(snapshot.progress_percent + 0.5f), 0, 100);

  // Always apply ring visual — it manages lv_anim transitions internally
  // and only restarts animations when the kind actually changes.
  apply_ring_visual_locked(snapshot);

  char progress_buffer[8] = {};
  std::snprintf(progress_buffer, sizeof(progress_buffer), "%d%%", progress);
  set_label_text_if_changed(progress_label_, progress_buffer);
  const std::string status_text = lifecycle_label(snapshot);
  set_label_text_if_changed(status_label_, status_text);

  const std::string detail = detail_text(snapshot);

  // [DIAG] Log what the display is actually showing — on change only.
  if (status_text != last_diag_status_ || detail != last_diag_detail_ ||
      snapshot.stage != last_diag_stage_) {
    last_diag_status_ = status_text;
    last_diag_detail_ = detail;
    last_diag_stage_ = snapshot.stage;
    ESP_LOGI(kTag, "[DIAG] display: status=%s stage=%s detail=%.60s lifecycle=%s",
             status_text.c_str(), snapshot.stage.c_str(),
             detail.empty() ? "(-)" : detail.c_str(),
             to_string(snapshot.lifecycle));
  }
  detail_visible_ = !detail.empty();
  if (detail_visible_) {
    const lv_label_long_mode_t desired_mode =
        snapshot.has_error ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_WRAP;
    if (lv_label_get_long_mode(detail_label_) != desired_mode) {
      lv_label_set_long_mode(detail_label_, desired_mode);
    }
    set_label_text_if_changed(detail_label_, detail);
  }

  const std::string layer = layer_text(snapshot);
  set_label_text_if_changed(layer_label_, layer);

  const std::string remaining = remaining_text(snapshot);
  set_label_text_if_changed(remaining_label_, remaining);

  char temp_buffer[24] = {};
  if (snapshot.nozzle_temp_known || snapshot.nozzle_temp_c > 0.0f) {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "%.0f%s", snapshot.nozzle_temp_c, kDegreeC);
  } else {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "--%s", kDegreeC);
  }
  set_label_text_if_changed(nozzle_value_label_, temp_buffer);

  if (snapshot.bed_temp_known || snapshot.bed_temp_c > 0.0f) {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "%.0f%s", snapshot.bed_temp_c, kDegreeC);
  } else {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "--%s", kDegreeC);
  }
  set_label_text_if_changed(bed_value_label_, temp_buffer);

  const std::string nozzle_aux =
      optional_temperature_text("Other nozzle", snapshot.secondary_nozzle_temp_c,
                                snapshot.secondary_nozzle_temp_known);
  nozzle_aux_visible_ = !nozzle_aux.empty();
  if (nozzle_aux_visible_) {
    set_label_text_if_changed(nozzle_aux_label_, nozzle_aux);
  }

  const std::string bed_aux =
      optional_temperature_text("Chamber", snapshot.chamber_temp_c, snapshot.chamber_temp_known);
  bed_aux_visible_ = !bed_aux.empty();
  if (bed_aux_visible_) {
    set_label_text_if_changed(bed_aux_label_, bed_aux);
  }

  const std::string battery_icon = battery_icon_text(snapshot);
  const std::string battery_pct = battery_pct_text(snapshot);
  const bool show_battery = snapshot.battery_present;
  set_label_text_if_changed(battery_icon_label_, show_battery ? battery_icon : "");
  set_label_text_if_changed(battery_pct_label_, show_battery ? battery_pct : "");

  // --- AMS page rendering ---
  const uint8_t ams_count = snapshot.ams ? snapshot.ams->count : 0;
  if (ams_page_available_ && ams_count > 0) {
    const AmsUnitInfo& unit = snapshot.ams->units[0];

    for (int i = 0; i < kMaxAmsTrays; ++i) {
      const AmsTrayInfo& tray = unit.trays[i];
      if (tray.present) {
        const uint32_t rgba = tray.color_rgba;
        const uint32_t rgb = (rgba >> 8) & 0x00FFFFFF;
        lv_obj_set_style_bg_color(ams_tray_rect_[i], lv_color_hex(rgb), 0);
        lv_obj_set_style_bg_opa(ams_tray_rect_[i], LV_OPA_COVER, 0);
        // Active tray: 2px black border + 2px white outline (double ring)
        if (tray.active) {
          lv_obj_set_style_border_width(ams_tray_rect_[i], 2, 0);
          lv_obj_set_style_border_color(ams_tray_rect_[i], lv_color_hex(0x000000), 0);
          lv_obj_set_style_outline_width(ams_tray_rect_[i], 2, 0);
          lv_obj_set_style_outline_color(ams_tray_rect_[i], lv_color_hex(0xFFFFFF), 0);
          lv_obj_set_style_outline_opa(ams_tray_rect_[i], LV_OPA_COVER, 0);
          lv_obj_set_style_outline_pad(ams_tray_rect_[i], 0, 0);
        } else {
          lv_obj_set_style_border_width(ams_tray_rect_[i], 1, 0);
          lv_obj_set_style_border_color(ams_tray_rect_[i], lv_color_hex(0x555555), 0);
          lv_obj_set_style_outline_width(ams_tray_rect_[i], 0, 0);
        }
        set_label_text_if_changed(ams_tray_type_[i],
                                  tray.material_type.empty() ? "--" : tray.material_type);
        // Slot label contrast: light text on dark fill, dark text on light fill
        const bool is_dark = ((rgb >> 16) & 0xFF) * 299 +
                             ((rgb >> 8) & 0xFF) * 587 +
                             (rgb & 0xFF) * 114 < 128000;
        lv_obj_set_style_text_color(ams_tray_type_[i],
            lv_color_hex(is_dark ? 0xFFFFFF : 0x000000), 0);

        // Filament remaining fill level
        if (tray.remain_pct >= 0) {
          const int rect_h = 140;
          const int empty_h = rect_h - (rect_h * tray.remain_pct / 100);
          lv_obj_set_height(ams_tray_fill_[i], empty_h);
          lv_obj_align(ams_tray_fill_[i], LV_ALIGN_TOP_MID, 0, 0);
          lv_obj_clear_flag(ams_tray_fill_[i], LV_OBJ_FLAG_HIDDEN);
          char pct_buf[8];
          std::snprintf(pct_buf, sizeof(pct_buf), "%d%%", tray.remain_pct);
          set_label_text_if_changed(ams_tray_pct_[i], pct_buf);
          lv_obj_set_style_text_color(ams_tray_pct_[i],
              lv_color_hex(is_dark ? 0xFFFFFF : 0x000000), 0);
          lv_obj_clear_flag(ams_tray_pct_[i], LV_OBJ_FLAG_HIDDEN);
          lv_obj_align_to(ams_tray_pct_[i], ams_tray_rect_[i],
                          LV_ALIGN_BOTTOM_MID, 0, -10);
        } else {
          lv_obj_add_flag(ams_tray_fill_[i], LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(ams_tray_pct_[i], LV_OBJ_FLAG_HIDDEN);
        }
      } else {
        lv_obj_set_style_bg_color(ams_tray_rect_[i], lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_opa(ams_tray_rect_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ams_tray_rect_[i], 1, 0);
        lv_obj_set_style_border_color(ams_tray_rect_[i], lv_color_hex(0x555555), 0);
        lv_obj_set_style_outline_width(ams_tray_rect_[i], 0, 0);
        set_label_text_if_changed(ams_tray_type_[i], "Empty");
        lv_obj_add_flag(ams_tray_fill_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ams_tray_pct_[i], LV_OBJ_FLAG_HIDDEN);
      }
    }

    // Humidity: direct percentage from printer
    char hum_buf[16] = {};
    if (unit.humidity_pct >= 0) {
      std::snprintf(hum_buf, sizeof(hum_buf), "%d%%", unit.humidity_pct);
    } else {
      std::snprintf(hum_buf, sizeof(hum_buf), "--%% ");
    }
    set_label_text_if_changed(ams_humidity_label_, hum_buf);

    char temp_buf[24] = {};
    if (unit.temperature_c > 0.0f) {
      std::snprintf(temp_buf, sizeof(temp_buf), "%.0f%s", unit.temperature_c, kDegreeC);
    } else {
      std::snprintf(temp_buf, sizeof(temp_buf), "--%s", kDegreeC);
    }
    set_label_text_if_changed(ams_temp_label_, temp_buf);

    set_hidden(ams_tray_row_, false);
    set_hidden(ams_note_, true);
  } else if (!ams_page_available_) {
    // Page is hidden, nothing to do.
  } else {
    set_hidden(ams_tray_row_, true);
    set_hidden(ams_note_, false);
  }

  const std::string preview_note = preview_note_text(snapshot);
  const std::string preview_subnote = preview_subnote_text(snapshot);
  const std::string camera_note = camera_note_text(snapshot);
  const std::string camera_subnote = camera_subnote_text(snapshot);
  bool has_preview_image = false;
  if (snapshot.preview_blob && !snapshot.preview_blob->empty()) {
    const bool preview_blob_changed = last_preview_blob_.get() != snapshot.preview_blob.get();
    last_preview_blob_ = snapshot.preview_blob;
    if (active_page_ == 3) {
      has_preview_image = ensure_preview_image_loaded_locked(
          preview_blob_changed, std::move(pre_decoded_raw), pre_decoded_dsc);
    } else if (preview_blob_changed) {
      release_preview_image_locked();
    }
  } else {
    if (last_preview_blob_ || last_preview_raw_) {
      release_preview_image_locked();
    }
    last_preview_blob_.reset();
  }
  const bool has_page2_image = has_preview_image;
  preview_image_visible_ = has_page2_image;

  if (preview_text_image_mode_ != has_page2_image) {
    if (has_page2_image) {
      // Note is hidden when cover is loaded; subnote (title) moves to former note position.
      lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, kPage2NoteWithImageY);
    } else {
      lv_obj_align(page2_note_, LV_ALIGN_CENTER, 0, -14);
      lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, 18);
    }
    preview_text_image_mode_ = has_page2_image;
  }
  set_hidden(page2_note_, preview_note.empty());
  if (!preview_note.empty()) {
    set_label_text_if_changed(page2_note_, preview_note);
  }
  set_hidden(page2_subnote_, preview_subnote.empty());
  if (!preview_subnote.empty()) {
    set_label_text_if_changed(page2_subnote_, preview_subnote);
  }

  bool has_camera_image =
      camera_slot_initialized_ && camera_blobs_[active_camera_slot_] &&
      !camera_blobs_[active_camera_slot_]->empty();
  if (snapshot.camera_blob && !snapshot.camera_blob->empty() && snapshot.camera_width > 0U &&
      snapshot.camera_height > 0U) {
    const bool camera_blob_changed =
        !camera_slot_initialized_ || camera_blobs_[active_camera_slot_].get() != snapshot.camera_blob.get();
    if (camera_blob_changed ||
        last_camera_width_ != snapshot.camera_width || last_camera_height_ != snapshot.camera_height) {
      const uint8_t next_slot =
          camera_slot_initialized_ ? static_cast<uint8_t>((active_camera_slot_ + 1U) % 2U) : 0U;
      lv_image_cache_drop(&camera_image_dscs_[next_slot]);
      lv_image_dsc_t next_dsc = {};
      if (configure_camera_rgb565(snapshot.camera_blob, snapshot.camera_width, snapshot.camera_height,
                                  &next_dsc)) {
        camera_blobs_[next_slot] = snapshot.camera_blob;
        camera_image_dscs_[next_slot] = next_dsc;
        active_camera_slot_ = next_slot;
        camera_slot_initialized_ = true;
        last_camera_width_ = snapshot.camera_width;
        last_camera_height_ = snapshot.camera_height;
        lv_image_set_src(page3_image_, &camera_image_dscs_[active_camera_slot_]);
      } else {
        camera_blobs_[next_slot].reset();
        std::memset(&camera_image_dscs_[next_slot], 0, sizeof(camera_image_dscs_[next_slot]));
        if (!camera_slot_initialized_) {
          active_camera_slot_ = 0;
        }
        last_camera_width_ = 0;
        last_camera_height_ = 0;
      }
    }
    has_camera_image =
        camera_slot_initialized_ && camera_blobs_[active_camera_slot_] &&
        !camera_blobs_[active_camera_slot_]->empty();
  } else {
    if (camera_slot_initialized_ && page3_image_ != nullptr) {
      lv_image_set_src(page3_image_, nullptr);
    }
    lv_image_cache_drop(&camera_image_dscs_[0]);
    lv_image_cache_drop(&camera_image_dscs_[1]);
    camera_blobs_[0].reset();
    camera_blobs_[1].reset();
    camera_slot_initialized_ = false;
    active_camera_slot_ = 0;
    last_camera_width_ = 0;
    last_camera_height_ = 0;
    std::memset(&camera_image_dscs_[0], 0, sizeof(camera_image_dscs_[0]));
    std::memset(&camera_image_dscs_[1], 0, sizeof(camera_image_dscs_[1]));
    has_camera_image = false;
  }
  camera_image_visible_ = has_camera_image;

  if (camera_text_image_mode_ != has_camera_image) {
    if (has_camera_image) {
      // Note is hidden when snapshot is loaded; subnote moves to former note position.
      lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, kPage3NoteWithImageY);
    } else {
      lv_obj_align(page3_note_, LV_ALIGN_CENTER, 0, 0);
      lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, 28);
    }
    camera_text_image_mode_ = has_camera_image;
  }
  set_hidden(page3_note_, camera_note.empty());
  if (!camera_note.empty()) {
    set_label_text_if_changed(page3_note_, camera_note);
  }
  set_hidden(page3_subnote_, camera_subnote.empty());
  if (!camera_subnote.empty()) {
    set_label_text_if_changed(page3_subnote_, camera_subnote);
  }

  show_logo_ = should_show_logo(snapshot);
  const bool chamber_light_clickable = snapshot.chamber_light_supported;
  if (logo_clickable_ != chamber_light_clickable) {
    set_clickable(logo_badge_, chamber_light_clickable);
    logo_clickable_ = chamber_light_clickable;
  }

  const bool logo_recolor_enabled =
      snapshot.chamber_light_supported && snapshot.chamber_light_state_known &&
      !snapshot.chamber_light_on;
  const uint32_t logo_recolor_hex = logo_recolor_enabled ? 0x7A7A7A : 0U;
  if (logo_recolor_enabled != logo_recolor_enabled_ ||
      (logo_recolor_enabled && logo_recolor_hex != logo_recolor_hex_)) {
    if (logo_recolor_enabled) {
      lv_obj_set_style_image_recolor(logo_image_, lv_color_hex(logo_recolor_hex), 0);
      lv_obj_set_style_image_recolor_opa(logo_image_, LV_OPA_COVER, 0);
    } else {
      lv_obj_set_style_image_recolor_opa(logo_image_, LV_OPA_TRANSP, 0);
    }
    logo_recolor_enabled_ = logo_recolor_enabled;
    logo_recolor_hex_ = logo_recolor_hex;
  }
  compute_portal_texts_locked();
  apply_page_visibility();
}

void Ui::ring_timer_cb(lv_timer_t* timer) {
  if (timer == nullptr) {
    return;
  }

  auto* ui = static_cast<Ui*>(lv_timer_get_user_data(timer));
  if (ui == nullptr) {
    return;
  }

  ui->handle_ring_timer();
}

void Ui::handle_ring_timer() {
  // With lv_anim_t driving all animations, this timer is only needed for
  // the ambient sweep rotation during printing.  For all other states the
  // timer is paused by apply_ring_visual_locked().
  if (!initialized_ || status_arc_ == nullptr ||
      screen_power_mode_ == ScreenPowerMode::kOff || scrolling_) {
    return;
  }

  if (active_ring_anim_kind_ == static_cast<uint8_t>(RingAnimKind::kAmbientSweep)) {
    const uint32_t phase = lv_tick_get() % kPrintRotPeriodMs;
    const int32_t angle = 270 + static_cast<int32_t>((phase * 360U) / kPrintRotPeriodMs);
    lv_arc_set_rotation(status_arc_, angle % 360);
  }
}

esp_err_t Ui::build_dashboard() {
  LvglLockGuard lock(3000, "build_dashboard");
  if (!lock.locked()) {
    return ESP_ERR_TIMEOUT;
  }

  screen_ = lv_screen_active();
  lv_obj_set_style_bg_color(screen_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);

  const lv_font_t* dosis20 = &dosis_20;
  const lv_font_t* dosis32 = &dosis_32;
  const lv_font_t* dosis40 = &dosis_40;
  const lv_font_t* info20 = &lv_font_montserrat_20;
  const lv_font_t* mdi30 = &mdi_30;
  const lv_font_t* mdi40 = &mdi_40;

  pager_ = lv_obj_create(screen_);
  lv_obj_set_size(pager_, board::kDisplayWidth, board::kDisplayHeight);
  lv_obj_center(pager_);
  make_transparent(pager_);
  apply_display_rotation_visual_offset(pager_, display_rotation_);
  lv_obj_set_style_pad_column(pager_, 0, 0);
  lv_obj_set_style_pad_row(pager_, 0, 0);
  enable_touch_bubble(pager_);
  lv_obj_set_flex_flow(pager_, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_dir(pager_, LV_DIR_HOR);
  // LV_SCROLL_SNAP_NONE: we handle page snapping ourselves in handle_pager_event.
  // LV_SCROLL_SNAP_CENTER would make LVGL launch its own snap animation that conflicts
  // with our set_active_page() call, causing double-animation jitter and page-skip bugs.
  lv_obj_set_scroll_snap_x(pager_, LV_SCROLL_SNAP_NONE);
  lv_obj_set_scrollbar_mode(pager_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(pager_, &Ui::pager_event_cb, LV_EVENT_ALL, this);

  auto create_page = [](lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, board::kDisplayWidth, board::kDisplayHeight);
    make_transparent(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
  };

  page0_ = create_page(pager_);
  ams_page_ = create_page(pager_);
  page1_ = create_page(pager_);
  page2_ = create_page(pager_);
  page3_ = create_page(pager_);
  enable_touch_bubble(page0_);
  enable_touch_bubble(ams_page_);
  enable_touch_bubble(page1_);
  enable_touch_bubble(page2_);
  enable_touch_bubble(page3_);

  // --- Page 0: printer selection ---
  page0_title_ = lv_label_create(page0_);
  set_label_text_if_changed(page0_title_, "Printers");
  lv_obj_set_width(page0_title_, 320);
  lv_label_set_long_mode(page0_title_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page0_title_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page0_title_, dosis32, 0);
  lv_obj_set_style_text_color(page0_title_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(page0_title_, LV_ALIGN_TOP_MID, 0, 60);

  page0_card_list_ = lv_obj_create(page0_);
  lv_obj_set_size(page0_card_list_, 380, 300);
  lv_obj_align(page0_card_list_, LV_ALIGN_CENTER, 0, 20);
  make_transparent(page0_card_list_);
  lv_obj_set_flex_flow(page0_card_list_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(page0_card_list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(page0_card_list_, 10, 0);
  lv_obj_set_style_pad_all(page0_card_list_, 0, 0);
  lv_obj_set_scroll_dir(page0_card_list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(page0_card_list_, LV_SCROLLBAR_MODE_OFF);

  page0_empty_note_ = lv_label_create(page0_);
  set_label_text_if_changed(page0_empty_note_, "No printers configured.\nUse the web portal to add printers.");
  lv_obj_set_width(page0_empty_note_, 320);
  lv_label_set_long_mode(page0_empty_note_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page0_empty_note_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page0_empty_note_, info20, 0);
  lv_obj_set_style_text_color(page0_empty_note_, lv_color_hex(0x666666), 0);
  lv_obj_align(page0_empty_note_, LV_ALIGN_CENTER, 0, 20);
  lv_obj_add_flag(page0_empty_note_, LV_OBJ_FLAG_HIDDEN);

  // --- AMS page (page index 1) ---
  // Pure LVGL spool visualization (no bitmap image).
  // Layout: gray shelf + dark base background, 4 pill-shaped spool rects,
  //         humidity pill indicator below.

  // Gray shelf background (behind upper half of pills)
  lv_obj_t* ams_shelf = lv_obj_create(ams_page_);
  lv_obj_set_size(ams_shelf, 359, 110);
  lv_obj_set_style_radius(ams_shelf, 20, 0);
  lv_obj_set_style_bg_color(ams_shelf, lv_color_hex(0x565656), 0);
  lv_obj_set_style_bg_opa(ams_shelf, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ams_shelf, 0, 0);
  lv_obj_align(ams_shelf, LV_ALIGN_CENTER, 0, -47);
  lv_obj_clear_flag(ams_shelf, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ams_shelf, LV_OBJ_FLAG_CLICKABLE);
  enable_touch_bubble(ams_shelf);

  // Dark base behind lower half of pills (solid, hides shelf's bottom corners)
  lv_obj_t* ams_base = lv_obj_create(ams_page_);
  lv_obj_set_size(ams_base, 385, 103);
  lv_obj_set_style_radius(ams_base, 0, 0);
  lv_obj_set_style_bg_color(ams_base, lv_color_hex(0x1F1F1F), 0);
  lv_obj_set_style_bg_opa(ams_base, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ams_base, 0, 0);
  lv_obj_align(ams_base, LV_ALIGN_CENTER, 0, 38);
  lv_obj_clear_flag(ams_base, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ams_base, LV_OBJ_FLAG_CLICKABLE);
  enable_touch_bubble(ams_base);

  ams_tray_row_ = lv_obj_create(ams_page_);
  lv_obj_set_size(ams_tray_row_, 420, LV_SIZE_CONTENT);
  make_transparent(ams_tray_row_);
  lv_obj_set_flex_flow(ams_tray_row_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ams_tray_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ams_tray_row_, 6, 0);
  lv_obj_align(ams_tray_row_, LV_ALIGN_CENTER, 0, -19);
  lv_obj_clear_flag(ams_tray_row_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(ams_tray_row_);

  for (int i = 0; i < kMaxAmsTrays; ++i) {
    // Column: type label on top, colored rect, color name below
    ams_tray_col_[i] = lv_obj_create(ams_tray_row_);
    lv_obj_set_size(ams_tray_col_[i], 76, LV_SIZE_CONTENT);
    make_transparent(ams_tray_col_[i]);
    lv_obj_set_flex_flow(ams_tray_col_[i], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ams_tray_col_[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ams_tray_col_[i], 4, 0);
    lv_obj_set_style_pad_all(ams_tray_col_[i], 0, 0);
    lv_obj_clear_flag(ams_tray_col_[i], LV_OBJ_FLAG_SCROLLABLE);

    // Pill-shaped spool rectangle
    ams_tray_rect_[i] = lv_obj_create(ams_tray_col_[i]);
    lv_obj_set_size(ams_tray_rect_[i], 72, 140);
    lv_obj_set_style_radius(ams_tray_rect_[i], 40, 0);
    lv_obj_set_style_bg_color(ams_tray_rect_[i], lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(ams_tray_rect_[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ams_tray_rect_[i], 1, 0);
    lv_obj_set_style_border_color(ams_tray_rect_[i], lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(ams_tray_rect_[i], LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(ams_tray_rect_[i], 0, 0);
    lv_obj_set_style_pad_all(ams_tray_rect_[i], 0, 0);
    lv_obj_set_style_clip_corner(ams_tray_rect_[i], true, 0);
    lv_obj_clear_flag(ams_tray_rect_[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ams_tray_rect_[i], LV_OBJ_FLAG_CLICKABLE);

    // Material type label inside pill upper area (e.g. "PLA")
    ams_tray_type_[i] = lv_label_create(ams_tray_rect_[i]);
    set_label_text_if_changed(ams_tray_type_[i], "--");
    lv_obj_set_width(ams_tray_type_[i], 68);
    lv_label_set_long_mode(ams_tray_type_[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ams_tray_type_[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ams_tray_type_[i], dosis20, 0);
    lv_obj_set_style_text_color(ams_tray_type_[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ams_tray_type_[i], LV_ALIGN_TOP_MID, 0, 10);

    // Dark overlay covering the empty portion from top (fill level indicator)
    ams_tray_fill_[i] = lv_obj_create(ams_tray_rect_[i]);
    lv_obj_set_size(ams_tray_fill_[i], 72, 0);
    lv_obj_set_style_radius(ams_tray_fill_[i], 0, 0);
    lv_obj_set_style_bg_color(ams_tray_fill_[i], lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ams_tray_fill_[i], LV_OPA_40, 0);
    lv_obj_set_style_border_width(ams_tray_fill_[i], 0, 0);
    lv_obj_align(ams_tray_fill_[i], LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(ams_tray_fill_[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ams_tray_fill_[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ams_tray_fill_[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Dark overlay on top of pills (opa 160) — creates "rolls inside box" effect
  lv_obj_t* ams_overlay = lv_obj_create(ams_page_);
  lv_obj_set_size(ams_overlay, 385, 103);
  lv_obj_set_style_radius(ams_overlay, 0, 0);
  lv_obj_set_style_bg_color(ams_overlay, lv_color_hex(0x1F1F1F), 0);
  lv_obj_set_style_bg_opa(ams_overlay, 160, 0);
  lv_obj_set_style_border_width(ams_overlay, 0, 0);
  lv_obj_align(ams_overlay, LV_ALIGN_CENTER, 0, 38);
  lv_obj_clear_flag(ams_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ams_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(ams_overlay, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Percentage labels on topmost layer (above overlay)
  for (int i = 0; i < kMaxAmsTrays; ++i) {
    ams_tray_pct_[i] = lv_label_create(ams_page_);
    set_label_text_if_changed(ams_tray_pct_[i], "");
    lv_obj_set_style_text_font(ams_tray_pct_[i], info20, 0);
    lv_obj_set_style_text_color(ams_tray_pct_[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_flag(ams_tray_pct_[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ams_tray_pct_[i], LV_OBJ_FLAG_IGNORE_LAYOUT);
  }

  // Humidity indicator in a pill-shaped bordered container
  lv_obj_t* hum_pill = lv_obj_create(ams_page_);
  lv_obj_set_size(hum_pill, 139, 50);
  lv_obj_set_style_radius(hum_pill, 25, 0);
  lv_obj_set_style_bg_opa(hum_pill, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(hum_pill, lv_color_hex(0x9B9B9B), 0);
  lv_obj_set_style_border_opa(hum_pill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hum_pill, 1, 0);
  lv_obj_set_style_pad_all(hum_pill, 0, 0);
  lv_obj_align(hum_pill, LV_ALIGN_CENTER, 0, 154);
  lv_obj_clear_flag(hum_pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hum_pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hum_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hum_pill, 8, 0);

  // Water drop icon (simple circle as placeholder)
  ams_humidity_drop_ = lv_obj_create(hum_pill);
  lv_obj_set_size(ams_humidity_drop_, 14, 14);
  lv_obj_set_style_radius(ams_humidity_drop_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ams_humidity_drop_, lv_color_hex(0x4A90D9), 0);
  lv_obj_set_style_bg_opa(ams_humidity_drop_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ams_humidity_drop_, 0, 0);
  lv_obj_clear_flag(ams_humidity_drop_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ams_humidity_drop_, LV_OBJ_FLAG_CLICKABLE);

  ams_humidity_label_ = lv_label_create(hum_pill);
  set_label_text_if_changed(ams_humidity_label_, "--");
  lv_obj_set_style_text_font(ams_humidity_label_, dosis20, 0);
  lv_obj_set_style_text_color(ams_humidity_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_set_style_text_align(ams_humidity_label_, LV_TEXT_ALIGN_CENTER, 0);

  ams_temp_label_ = lv_label_create(hum_pill);
  set_label_text_if_changed(ams_temp_label_, "--\xC2\xB0""C");
  lv_obj_set_style_text_font(ams_temp_label_, dosis20, 0);
  lv_obj_set_style_text_color(ams_temp_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_set_style_text_align(ams_temp_label_, LV_TEXT_ALIGN_CENTER, 0);

  ams_note_ = lv_label_create(ams_page_);
  set_label_text_if_changed(ams_note_, "No AMS connected");
  lv_obj_set_width(ams_note_, 280);
  lv_label_set_long_mode(ams_note_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(ams_note_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(ams_note_, dosis20, 0);
  lv_obj_set_style_text_color(ams_note_, lv_color_hex(0x666666), 0);
  lv_obj_align(ams_note_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(ams_note_, LV_OBJ_FLAG_HIDDEN);

  // AMS page starts hidden until data says there's an AMS
  lv_obj_add_flag(ams_page_, LV_OBJ_FLAG_HIDDEN);
  ams_page_available_ = false;

  fixed_overlay_ = lv_obj_create(screen_);
  lv_obj_set_size(fixed_overlay_, board::kDisplayWidth, board::kDisplayHeight);
  lv_obj_center(fixed_overlay_);
  make_transparent(fixed_overlay_);
  apply_display_rotation_visual_offset(fixed_overlay_, display_rotation_);
  lv_obj_clear_flag(fixed_overlay_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(fixed_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(fixed_overlay_);
  lv_obj_move_foreground(fixed_overlay_);

  status_arc_ = lv_arc_create(fixed_overlay_);
  lv_obj_set_size(status_arc_, board::kDisplayWidth, board::kDisplayHeight);
  lv_obj_remove_flag(status_arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_rotation(status_arc_, 270);
  lv_arc_set_bg_angles(status_arc_, 0, 360);
  lv_arc_set_range(status_arc_, 0, 100);
  lv_arc_set_value(status_arc_, 0);
  lv_obj_center(status_arc_);
  lv_obj_set_style_arc_width(status_arc_, kRingStrokeWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_width(status_arc_, kRingStrokeWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(status_arc_, lv_color_hex(0x101010), LV_PART_MAIN);
  lv_obj_set_style_arc_color(status_arc_, lv_color_hex(arc_colors_.printing), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(status_arc_, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(status_arc_, true, LV_PART_INDICATOR);
  lv_obj_set_style_opa(status_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_clear_flag(status_arc_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(status_arc_);

  progress_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(progress_label_, "--%");
  lv_obj_set_style_text_font(progress_label_, dosis40, 0);
  lv_obj_set_style_text_color(progress_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(progress_label_, LV_ALIGN_CENTER, 0, -178);
  apply_display_rotation_visual_offset(progress_label_, display_rotation_);
  lv_obj_move_foreground(progress_label_);

  battery_icon_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(battery_icon_label_, kMdiBattery100);
  lv_obj_set_style_text_font(battery_icon_label_, mdi30, 0);
  lv_obj_set_style_text_color(battery_icon_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(battery_icon_label_, LV_ALIGN_CENTER, -20, -140);
  apply_display_rotation_visual_offset(battery_icon_label_, display_rotation_);
  lv_obj_move_foreground(battery_icon_label_);

  battery_pct_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(battery_pct_label_, "--%");
  lv_obj_set_style_text_font(battery_pct_label_, dosis20, 0);
  lv_obj_set_style_text_color(battery_pct_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(battery_pct_label_, LV_ALIGN_CENTER, 20, -140);
  apply_display_rotation_visual_offset(battery_pct_label_, display_rotation_);
  lv_obj_move_foreground(battery_pct_label_);

  badge_slot_ = lv_obj_create(page1_);
  make_transparent(badge_slot_);
  lv_obj_set_size(badge_slot_, 86, 86);
  lv_obj_align(badge_slot_, LV_ALIGN_CENTER, 0, -7);
  lv_obj_clear_flag(badge_slot_, LV_OBJ_FLAG_SCROLLABLE);

  sync_spinner_ = lv_spinner_create(badge_slot_);
  lv_spinner_set_anim_params(sync_spinner_, 2000, 60);
  lv_obj_set_size(sync_spinner_, 86, 86);
  make_transparent(sync_spinner_);
  lv_obj_set_style_arc_width(sync_spinner_, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_width(sync_spinner_, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sync_spinner_, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_arc_color(sync_spinner_, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(sync_spinner_, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(sync_spinner_, true, LV_PART_INDICATOR);
  lv_obj_center(sync_spinner_);

  sync_label_ = lv_label_create(badge_slot_);
  set_label_text_if_changed(sync_label_, kMdiSync);
  lv_obj_set_style_text_font(sync_label_, mdi40, 0);
  lv_obj_set_style_text_color(sync_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(sync_label_);

  logo_badge_ = lv_obj_create(badge_slot_);
  lv_obj_set_size(logo_badge_, 120, 120);
  lv_obj_set_style_radius(logo_badge_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(logo_badge_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(logo_badge_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(logo_badge_, 0, 0);
  lv_obj_center(logo_badge_);
  lv_obj_clear_flag(logo_badge_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(logo_badge_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(logo_badge_);
  lv_obj_add_event_cb(logo_badge_, &Ui::logo_event_cb, LV_EVENT_CLICKED, this);

  logo_image_ = lv_image_create(logo_badge_);
  lv_image_set_src(logo_image_, &bambuicon_small);
  lv_image_set_scale(logo_image_, 183);
  lv_image_set_antialias(logo_image_, true);
  lv_obj_set_style_image_recolor_opa(logo_image_, LV_OPA_TRANSP, 0);
  lv_obj_center(logo_image_);
  lv_obj_clear_flag(logo_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(logo_image_, LV_OBJ_FLAG_SCROLLABLE);

  status_label_ = lv_label_create(page1_);
  set_label_text_if_changed(status_label_, "waiting...");
  lv_obj_set_style_text_font(status_label_, dosis32, 0);
  lv_obj_set_style_text_color(status_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, -86);

  detail_label_ = lv_label_create(page1_);
  set_label_text_if_changed(detail_label_, "Waiting for printer data");
  lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_label_, 320);
  lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(detail_label_, info20, 0);
  lv_obj_set_style_text_color(detail_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(detail_label_, LV_ALIGN_CENTER, 0, 114);

  layer_label_ = lv_label_create(page1_);
  set_label_text_if_changed(layer_label_, "Layer: -- / --");
  lv_obj_set_style_text_font(layer_label_, dosis32, 0);
  lv_obj_set_style_text_color(layer_label_, lv_color_hex(0xDDDDDD), 0);
  lv_obj_align(layer_label_, LV_ALIGN_CENTER, 0, 70);

  nozzle_prefix_label_ = lv_label_create(page1_);
  set_label_text_if_changed(nozzle_prefix_label_, kMdiNozzle);
  lv_obj_set_style_text_font(nozzle_prefix_label_, mdi40, 0);
  lv_obj_set_style_text_color(nozzle_prefix_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(nozzle_prefix_label_, LV_ALIGN_CENTER, -182, -10);

  nozzle_value_label_ = lv_label_create(page1_);
  set_label_text_if_changed(nozzle_value_label_, "--°C");
  lv_obj_set_style_text_font(nozzle_value_label_, dosis32, 0);
  lv_obj_set_style_text_color(nozzle_value_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(nozzle_value_label_, LV_ALIGN_CENTER, -132, -10);
  set_label_text_if_changed(nozzle_value_label_, std::string("--") + kDegreeC);

  nozzle_aux_label_ = lv_label_create(page1_);
  set_label_text_if_changed(nozzle_aux_label_, "");
  lv_obj_set_width(nozzle_aux_label_, 170);
  lv_label_set_long_mode(nozzle_aux_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(nozzle_aux_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(nozzle_aux_label_, dosis20, 0);
  lv_obj_set_style_text_color(nozzle_aux_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(nozzle_aux_label_, LV_ALIGN_CENTER, -132, kAuxTempRowY);
  lv_obj_add_flag(nozzle_aux_label_, LV_OBJ_FLAG_HIDDEN);

  bed_prefix_label_ = lv_label_create(page1_);
  set_label_text_if_changed(bed_prefix_label_, kMdiBed);
  lv_obj_set_style_text_font(bed_prefix_label_, mdi40, 0);
  lv_obj_set_style_text_color(bed_prefix_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(bed_prefix_label_, LV_ALIGN_CENTER, 182, -10);

  bed_value_label_ = lv_label_create(page1_);
  set_label_text_if_changed(bed_value_label_, "--°C");
  lv_obj_set_style_text_font(bed_value_label_, dosis32, 0);
  lv_obj_set_style_text_color(bed_value_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_width(bed_value_label_, 96);
  lv_obj_set_style_text_align(bed_value_label_, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(bed_value_label_, LV_ALIGN_CENTER, 108, -10);
  set_label_text_if_changed(bed_value_label_, std::string("--") + kDegreeC);

  bed_aux_label_ = lv_label_create(page1_);
  set_label_text_if_changed(bed_aux_label_, "");
  lv_obj_set_width(bed_aux_label_, 170);
  lv_label_set_long_mode(bed_aux_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(bed_aux_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(bed_aux_label_, dosis20, 0);
  lv_obj_set_style_text_color(bed_aux_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(bed_aux_label_, LV_ALIGN_CENTER, 132, kAuxTempRowY);
  lv_obj_add_flag(bed_aux_label_, LV_OBJ_FLAG_HIDDEN);

  remaining_row_ = lv_obj_create(page1_);
  make_transparent(remaining_row_);
  lv_obj_set_size(remaining_row_, 280, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(remaining_row_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(remaining_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(remaining_row_, LV_ALIGN_CENTER, 0, kRemainingRowY);
  lv_obj_clear_flag(remaining_row_, LV_OBJ_FLAG_SCROLLABLE);

  remaining_prefix_label_ = lv_label_create(remaining_row_);
  set_label_text_if_changed(remaining_prefix_label_, kMdiClock);
  lv_obj_set_style_text_font(remaining_prefix_label_, mdi40, 0);
  lv_obj_set_style_text_color(remaining_prefix_label_, lv_color_hex(0x87CEEB), 0);
  lv_obj_set_style_pad_right(remaining_prefix_label_, 8, 0);

  remaining_label_ = lv_label_create(remaining_row_);
  set_label_text_if_changed(remaining_label_, "--m");
  lv_obj_set_style_text_font(remaining_label_, dosis40, 0);
  lv_obj_set_style_text_color(remaining_label_, lv_color_hex(0x87CEEB), 0);

  portal_hint_label_ = lv_label_create(page1_);
  set_label_text_if_changed(portal_hint_label_, "");
  lv_obj_set_width(portal_hint_label_, 320);
  lv_label_set_long_mode(portal_hint_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(portal_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(portal_hint_label_, info20, 0);
  lv_obj_set_style_text_color(portal_hint_label_, lv_color_hex(0x64748B), 0);
  lv_obj_align(portal_hint_label_, LV_ALIGN_CENTER, 0, 114);
  lv_obj_add_flag(portal_hint_label_, LV_OBJ_FLAG_HIDDEN);

  brightness_overlay_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(brightness_overlay_, "80%");
  lv_obj_set_style_text_font(brightness_overlay_, dosis40, 0);
  lv_obj_set_style_text_color(brightness_overlay_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(brightness_overlay_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(brightness_overlay_, LV_OPA_60, 0);
  lv_obj_set_style_pad_hor(brightness_overlay_, 20, 0);
  lv_obj_set_style_pad_ver(brightness_overlay_, 10, 0);
  lv_obj_set_style_radius(brightness_overlay_, 16, 0);
  lv_obj_align(brightness_overlay_, LV_ALIGN_CENTER, 0, 0);
  apply_display_rotation_visual_offset(brightness_overlay_, display_rotation_);
  lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(brightness_overlay_);

  portal_overlay_card_ = lv_obj_create(lv_layer_top());
  lv_obj_set_size(portal_overlay_card_, 280, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(portal_overlay_card_, 22, 0);
  lv_obj_set_style_bg_color(portal_overlay_card_, lv_color_hex(0x071018), 0);
  lv_obj_set_style_bg_opa(portal_overlay_card_, LV_OPA_90, 0);
  lv_obj_set_style_border_color(portal_overlay_card_, lv_color_hex(0xF0A64B), 0);
  lv_obj_set_style_border_width(portal_overlay_card_, 2, 0);
  lv_obj_set_style_pad_hor(portal_overlay_card_, 22, 0);
  lv_obj_set_style_pad_ver(portal_overlay_card_, 18, 0);
  lv_obj_set_style_pad_row(portal_overlay_card_, 8, 0);
  lv_obj_set_layout(portal_overlay_card_, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(portal_overlay_card_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(portal_overlay_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_center(portal_overlay_card_);
  apply_display_rotation_visual_offset(portal_overlay_card_, display_rotation_);
  lv_obj_clear_flag(portal_overlay_card_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(portal_overlay_card_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(portal_overlay_card_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(portal_overlay_card_);

  portal_overlay_title_ = lv_label_create(portal_overlay_card_);
  set_label_text_if_changed(portal_overlay_title_, "WEB CONFIG PIN");
  lv_obj_set_style_text_font(portal_overlay_title_, dosis20, 0);
  lv_obj_set_style_text_color(portal_overlay_title_, lv_color_hex(0xF8FAFC), 0);

  portal_overlay_value_ = lv_label_create(portal_overlay_card_);
  set_label_text_if_changed(portal_overlay_value_, "000000");
  lv_obj_set_style_text_font(portal_overlay_value_, dosis40, 0);
  lv_obj_set_style_text_color(portal_overlay_value_, lv_color_hex(0xF0A64B), 0);

  portal_overlay_detail_ = lv_label_create(portal_overlay_card_);
  set_label_text_if_changed(portal_overlay_detail_, "");
  lv_obj_set_width(portal_overlay_detail_, 236);
  lv_label_set_long_mode(portal_overlay_detail_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(portal_overlay_detail_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(portal_overlay_detail_, dosis20, 0);
  lv_obj_set_style_text_color(portal_overlay_detail_, lv_color_hex(0xCBD5E1), 0);

  page2_shell_ = nullptr;

  page2_image_ = lv_image_create(page2_);
  lv_obj_set_size(page2_image_, kPage2PreviewSize, kPage2PreviewSize);
  lv_image_set_inner_align(page2_image_, LV_IMAGE_ALIGN_CONTAIN);
  lv_obj_align(page2_image_, LV_ALIGN_CENTER, 0, kPage2PreviewYOffset);
  lv_obj_add_flag(page2_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(page2_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(page2_image_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(page2_image_);

  page2_note_ = lv_label_create(page2_);
  set_label_text_if_changed(page2_note_, "No cover image yet");
  lv_obj_set_width(page2_note_, 280);
  lv_label_set_long_mode(page2_note_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page2_note_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page2_note_, dosis20, 0);
  lv_obj_set_style_text_color(page2_note_, lv_color_hex(0x888888), 0);
  lv_obj_align(page2_note_, LV_ALIGN_CENTER, 0, -14);
  enable_touch_bubble(page2_note_);

  page2_subnote_ = lv_label_create(page2_);
  set_label_text_if_changed(page2_subnote_, "");
  lv_obj_set_width(page2_subnote_, 320);
  lv_label_set_long_mode(page2_subnote_, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(page2_subnote_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page2_subnote_, info20, 0);
  lv_obj_set_style_text_color(page2_subnote_, lv_color_hex(0x888888), 0);
  lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, 18);
  lv_obj_add_flag(page2_subnote_, LV_OBJ_FLAG_HIDDEN);
  enable_touch_bubble(page2_subnote_);

  page3_image_ = lv_image_create(page3_);
  lv_obj_set_size(page3_image_, board::kDisplayWidth, kPage3CameraHeight);
  lv_image_set_inner_align(page3_image_, LV_IMAGE_ALIGN_CENTER);
  lv_obj_align(page3_image_, LV_ALIGN_CENTER, 0, kPage3CameraYOffset);
  lv_obj_add_flag(page3_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(page3_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(page3_image_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(page3_image_);

  page3_note_ = lv_label_create(page3_);
  set_label_text_if_changed(page3_note_, "Tap for new image");
  lv_obj_set_width(page3_note_, 320);
  lv_label_set_long_mode(page3_note_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page3_note_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page3_note_, info20, 0);
  lv_obj_set_style_text_color(page3_note_, lv_color_hex(0x888888), 0);
  lv_obj_align(page3_note_, LV_ALIGN_CENTER, 0, 0);
  enable_touch_bubble(page3_note_);

  page3_subnote_ = lv_label_create(page3_);
  set_label_text_if_changed(page3_subnote_, "Auto-refresh every 2s");
  lv_obj_set_width(page3_subnote_, 320);
  lv_label_set_long_mode(page3_subnote_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page3_subnote_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page3_subnote_, info20, 0);
  lv_obj_set_style_text_color(page3_subnote_, lv_color_hex(0x888888), 0);
  lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, 28);
  lv_obj_add_flag(page3_subnote_, LV_OBJ_FLAG_HIDDEN);
  enable_touch_bubble(page3_subnote_);

  lv_obj_add_event_cb(screen_, &Ui::screen_event_cb, LV_EVENT_ALL, this);
  lv_obj_update_layout(pager_);
  lv_obj_scroll_to_view(page1_, LV_ANIM_OFF);

  active_page_ = 2;
  scrolling_ = false;
  deferred_snapshot_pending_ = false;
  detail_visible_ = true;
  show_logo_ = false;
  preview_text_image_mode_ = false;
  camera_text_image_mode_ = false;
  apply_page_visibility();
  update_portal_access_visuals_locked();

  return ESP_OK;
}

void Ui::apply_page_visibility() {
  // Page content labels are children of their respective page objects which live
  // inside the horizontally-scrolling pager.  LVGL clips children that are
  // outside the visible viewport, so we do NOT need to hide page-1/2/3 content
  // during a scroll gesture.  Keeping them visible produces a smooth gallery-
  // like swipe transition where the departing and arriving pages render side by
  // side while the user's finger drags.
  const bool on_page1 = !scrolling_ ? (active_page_ == 2) : true;
  const bool on_page2 = !scrolling_ ? (active_page_ == 3) : true;
  const bool on_page3 = !scrolling_ ? (active_page_ == 4) : true;
  const bool settled_page1 = !scrolling_ && active_page_ == 2;
  const bool portal_hint_has_priority = portal_pin_active_ || portal_session_active_;
  const bool show_portal_hint =
      settled_page1 && !portal_hint_text_.empty() &&
      (portal_hint_has_priority || !detail_visible_);

  set_hidden(ams_page_, !ams_page_available_);
  set_hidden(page2_, !preview_page_available_);
  set_hidden(page3_, !camera_page_available_);

  // Page 1 detail labels
  set_hidden(status_label_, !on_page1);
  set_hidden(detail_label_, !on_page1 || !detail_visible_ || show_portal_hint);
  set_hidden(layer_label_, !on_page1);
  set_hidden(nozzle_prefix_label_, !on_page1);
  set_hidden(nozzle_value_label_, !on_page1);
  set_hidden(nozzle_aux_label_, !on_page1 || !nozzle_aux_visible_);
  set_hidden(bed_prefix_label_, !on_page1);
  set_hidden(bed_value_label_, !on_page1);
  set_hidden(bed_aux_label_, !on_page1 || !bed_aux_visible_);
  set_hidden(remaining_row_, !on_page1);
  set_hidden(badge_slot_, !on_page1);
  set_hidden(portal_hint_label_, !show_portal_hint);
  set_hidden(page2_image_, !on_page2 || !preview_image_visible_);
  set_hidden(page3_image_, !on_page3 || !camera_image_visible_);

  // Overlay + page0 opacity are driven by apply_page0_parallax.
  // When not scrolling, snap to final state.
  if (!scrolling_) {
    apply_page0_parallax();
  }

  apply_logo_visibility();
  update_portal_access_visuals_locked();
}

void Ui::apply_logo_visibility() {
  // badge_slot_ is a child of page1_ so it clips naturally during scroll.
  const bool show_badge_slot = scrolling_ || active_page_ == 2;
  if (!show_badge_slot) {
    set_hidden(logo_badge_, true);
    set_hidden(sync_spinner_, true);
    set_hidden(sync_label_, true);
    return;
  }

  set_hidden(logo_badge_, !show_logo_);
  set_hidden(sync_spinner_, show_logo_);
  set_hidden(sync_label_, show_logo_);
}

void Ui::update_page_availability_locked(const PrinterSnapshot& snapshot) {
  const bool preview_available = snapshot.preview_page_available;
  const bool camera_available = snapshot.camera_page_available;
  const bool ams_available = snapshot.ams && snapshot.ams->count > 0;
  const bool availability_changed =
      preview_page_available_ != preview_available || camera_page_available_ != camera_available ||
      ams_page_available_ != ams_available;

  preview_page_available_ = preview_available;
  camera_page_available_ = camera_available;
  ams_page_available_ = ams_available;

  if (!availability_changed) {
    return;
  }

  set_hidden(ams_page_, !ams_page_available_);
  set_hidden(page2_, !preview_page_available_);
  set_hidden(page3_, !camera_page_available_);

  if (!preview_page_available_) {
    release_preview_image_locked();
    preview_image_visible_ = false;
  }

  if (!camera_page_available_) {
    if (camera_slot_initialized_ && page3_image_ != nullptr) {
      lv_image_set_src(page3_image_, nullptr);
    }
    lv_image_cache_drop(&camera_image_dscs_[0]);
    lv_image_cache_drop(&camera_image_dscs_[1]);
    camera_blobs_[0].reset();
    camera_blobs_[1].reset();
    camera_slot_initialized_ = false;
    active_camera_slot_ = 0;
    last_camera_width_ = 0;
    last_camera_height_ = 0;
    std::memset(&camera_image_dscs_[0], 0, sizeof(camera_image_dscs_[0]));
    std::memset(&camera_image_dscs_[1], 0, sizeof(camera_image_dscs_[1]));
    camera_image_visible_ = false;
  }

  active_page_ = clamp_enabled_page(active_page_);
  lv_obj_update_layout(pager_);
  if (lv_obj_t* target_page = page_object(active_page_); target_page != nullptr) {
    lv_obj_scroll_to_view(target_page, LV_ANIM_OFF);
  }
  scrolling_ = false;
  apply_page0_parallax();
  // Ring timer resume is handled by apply_ring_visual_locked via apply_snapshot.
}

bool Ui::page_enabled(int page) const {
  switch (page) {
    case 0:
      return true;
    case 1:
      return ams_page_available_;
    case 2:
      return true;
    case 3:
      return preview_page_available_;
    case 4:
      return camera_page_available_;
    default:
      return false;
  }
}

lv_obj_t* Ui::page_object(int page) const {
  switch (page) {
    case 0:
      return page0_;
    case 1:
      return ams_page_;
    case 2:
      return page1_;
    case 3:
      return page2_;
    case 4:
      return page3_;
    default:
      return nullptr;
  }
}

int Ui::next_enabled_page(int page, int direction) const {
  int candidate = page + direction;
  while (candidate >= 0 && candidate <= 4) {
    if (page_enabled(candidate)) {
      return candidate;
    }
    candidate += direction;
  }
  return page;
}

int Ui::clamp_enabled_page(int page) const {
  if (page_enabled(page)) {
    return page;
  }

  for (int candidate = 0; candidate <= 4; ++candidate) {
    if (page_enabled(candidate)) {
      return candidate;
    }
  }

  return 0;
}

int Ui::nearest_enabled_page_for_scroll() const {
  lv_obj_update_layout(pager_);
  int scroll_x = lv_obj_get_scroll_x(pager_);
  if (scroll_x < 0) {
    scroll_x = -scroll_x;
  }

  // With LV_SCROLL_SNAP_NONE + scroll_throw=90, the throw decays in ~3 ticks so
  // scroll_x already reflects the post-throw resting position when SCROLL_END fires.
  // Simply pick whichever page center is closest to the viewport center.
  const int viewport_center = scroll_x + (board::kDisplayWidth / 2);
  int best_page = clamp_enabled_page(active_page_);
  int best_distance = INT32_MAX;

  for (int page = 0; page <= 4; ++page) {
    if (!page_enabled(page)) {
      continue;
    }

    lv_obj_t* object = page_object(page);
    if (object == nullptr) {
      continue;
    }

    const int page_center = lv_obj_get_x(object) + (board::kDisplayWidth / 2);
    const int distance = std::abs(page_center - viewport_center);
    if (distance < best_distance) {
      best_distance = distance;
      best_page = page;
    }
  }

  return best_page;
}

void Ui::set_active_page(int page) {
  const int clamped_page = clamp_enabled_page(page);
  const int previous_page = active_page_;
  lv_obj_update_layout(pager_);
  if (lv_obj_t* target_page = page_object(clamped_page); target_page != nullptr) {
    // LV_ANIM_ON with ease_in_out causes visible stutter: the ease-in phase is
    // intentionally slow. The drag has already moved the pager visually — snap to
    // final position instantly in one frame.
    lv_obj_scroll_to_view(target_page, LV_ANIM_OFF);
  }
  if (previous_page == 3 && clamped_page != 3) {
    release_preview_image_locked();
    preview_image_visible_ = false;
  }
  active_page_ = clamped_page;
  if (clamped_page == 0 && previous_page != 0) {
    replay_card_animations_locked();
  }
  if (active_page_ == 3) {
    preview_image_visible_ = ensure_preview_image_loaded_locked(false);
  }
  scrolling_ = false;
  apply_page0_parallax();
  // Ring timer resume is handled by apply_ring_visual_locked below.
  apply_page_visibility();
  if (deferred_snapshot_pending_) {
    apply_snapshot_locked(deferred_snapshot_, true);
  } else if (previous_page != clamped_page) {
    apply_snapshot_locked(last_snapshot_, true);
  }
}

void Ui::handle_pager_event(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);

  if (code == LV_EVENT_SCROLL) {
    apply_page0_parallax();
    return;
  }

  if (code != LV_EVENT_SCROLL_BEGIN && code != LV_EVENT_SCROLL_END) {
    return;
  }

  if (code == LV_EVENT_SCROLL_BEGIN) {
    scrolling_ = true;
    if (ring_anim_timer_ != nullptr) {
      lv_timer_pause(ring_anim_timer_);
    }

    // Eagerly load images for adjacent pages so they are visible during the
    // scroll transition.  ensure_preview_image_loaded_locked() uses the cached
    // decoded buffer when available (fast pointer assignment), or falls back to
    // decoding under lock for the first load.
    if (preview_page_available_ && !preview_image_visible_) {
      preview_image_visible_ = ensure_preview_image_loaded_locked(false);
      if (preview_image_visible_ && !preview_text_image_mode_) {
        // Reposition text so the title doesn't sit on top of the image.
        set_hidden(page2_note_, true);
        lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, kPage2NoteWithImageY);
        preview_text_image_mode_ = true;
      }
    }

    apply_page_visibility();
    return;
  }

  int scroll_x = lv_obj_get_scroll_x(pager_);
  if (scroll_x < 0) {
    scroll_x = -scroll_x;
  }

  (void)scroll_x;
  set_active_page(nearest_enabled_page_for_scroll());
}

void Ui::handle_screen_event(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
      code != LV_EVENT_PRESS_LOST && code != LV_EVENT_LONG_PRESSED) {
    return;
  }

  lv_indev_t* indev = lv_indev_get_act();
  if (indev == nullptr) {
    return;
  }

  lv_point_t point = {};
  lv_indev_get_point(indev, &point);

  if (code == LV_EVENT_PRESSED) {
    if (screen_power_mode_ == ScreenPowerMode::kOff) {
      // First touch wakes the screen; a second touch performs UI actions.
      note_activity(true);
      gesture_active_ = false;
      swipe_switched_ = false;
      overlay_visible_ = false;
      return;
    }

    note_activity(false);
    gesture_active_ = true;
    swipe_switched_ = false;
    overlay_visible_ = false;
    gesture_start_x_ = point.x;
    gesture_start_y_ = point.y;
    gesture_start_brightness_ = user_brightness_percent_;
    return;
  }

  if (code == LV_EVENT_PRESSING && gesture_active_) {
    note_activity(false);
    const int dx = static_cast<int>(point.x - gesture_start_x_);
    const int dy = static_cast<int>(gesture_start_y_ - point.y);
    const int abs_dx = std::abs(dx);
    const int abs_dy = std::abs(dy);

    if (abs_dy < 12 || abs_dy < abs_dx) {
      return;
    }

    const float delta = static_cast<float>(dy) * (100.0f / 250.0f);
    const int new_brightness =
        std::clamp(gesture_start_brightness_ + static_cast<int>(std::lround(delta)),
                   kManualMinBrightnessPercent, 100);
    set_brightness_percent(new_brightness);

    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%d%%", user_brightness_percent_);
    set_label_text_if_changed(brightness_overlay_, buffer);
    lv_obj_clear_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
    overlay_visible_ = true;
    return;
  }

  if (code == LV_EVENT_LONG_PRESSED && gesture_active_) {
    note_activity(false);
    if (!overlay_visible_ && !scrolling_) {
      portal_unlock_requested_.store(true);
    }
    return;
  }

  if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && gesture_active_) {
    note_activity(false);
    const int dx = static_cast<int>(point.x - gesture_start_x_);
    const int dy = static_cast<int>(gesture_start_y_ - point.y);
    const int abs_dx = std::abs(dx);
    const int abs_dy = std::abs(dy);

    gesture_active_ = false;
    swipe_switched_ = false;
    if (overlay_visible_) {
      lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
      overlay_visible_ = false;
      return;
    }

    // Horizontal page swiping is handled by the LVGL pager (flex-row +
    // LV_SCROLL_SNAP_NONE → handle_pager_event snaps to nearest page on SCROLL_END).
    // Only handle taps here (camera refresh on page 3).
    if (active_page_ == 4 && camera_page_available_ && abs_dx < 12 && abs_dy < 12) {
      std::lock_guard<std::mutex> lock(camera_refresh_mutex_);
      camera_refresh_requested_ = true;
    }
  }
}

void Ui::update_portal_access_visuals_locked() {
  const bool show_page1 = !scrolling_ && active_page_ == 2;
  const bool portal_hint_has_priority = portal_pin_active_ || portal_session_active_;
  const bool show_hint = portal_hint_label_ != nullptr && show_page1 && !portal_hint_text_.empty() &&
                         (portal_hint_has_priority || !detail_visible_);
  set_hidden(portal_hint_label_, !show_hint);
  if (show_hint) {
    set_label_text_if_changed(portal_hint_label_, portal_hint_text_);
    set_hidden(detail_label_, true);
  } else if (detail_label_ != nullptr && show_page1 && detail_visible_) {
    set_hidden(detail_label_, false);
  }

  const bool show_overlay = portal_overlay_card_ != nullptr && portal_pin_active_;
  set_hidden(portal_overlay_card_, !show_overlay);
  if (!show_overlay) {
    return;
  }

  set_label_text_if_changed(portal_overlay_title_, portal_overlay_title_text_);
  set_label_text_if_changed(portal_overlay_value_, portal_overlay_value_text_);
  set_label_text_if_changed(portal_overlay_detail_, portal_overlay_detail_text_);
}

void Ui::handle_logo_event(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }

  if (scrolling_ || !show_logo_ || !last_snapshot_.chamber_light_supported) {
    return;
  }

  note_activity(false);
  chamber_light_toggle_requested_.store(true);
}

void Ui::set_brightness_percent(int brightness_percent) {
  const int clamped = std::clamp(brightness_percent, 0, 100);
  if (user_brightness_percent_ == clamped) {
    return;
  }

  user_brightness_percent_ = clamped;
  apply_brightness_policy();
}

void Ui::note_activity(bool wake_display_now) {
  last_activity_tick_ms_.store(lv_tick_get());
  if (wake_display_now) {
    wake_display();
  }
}

void Ui::wake_display() {
  if (screen_power_mode_ == ScreenPowerMode::kAwake) {
    return;
  }

  const bool was_off = screen_power_mode_ == ScreenPowerMode::kOff;
  screen_power_mode_ = ScreenPowerMode::kAwake;
  apply_brightness_policy();
  if (was_off) {
    esp_lv_adapter_resume();
  }
}

void Ui::request_wake_display() {
  wake_display();
}

void Ui::set_battery_display_policy(const BatteryDisplayPolicy& policy) {
  battery_display_policy_ = policy;
}

void Ui::apply_brightness_policy() {
  int target_brightness = user_brightness_percent_;
  if (screen_power_mode_ == ScreenPowerMode::kDimmed) {
    if (battery_display_policy_.dim_brightness_percent > 0) {
      target_brightness = std::clamp(battery_display_policy_.dim_brightness_percent, 1, 100);
    } else {
      target_brightness = std::max(8, std::min(18, std::max(1, user_brightness_percent_ / 3)));
    }
  } else if (screen_power_mode_ == ScreenPowerMode::kOff) {
    target_brightness = 0;
  }

  if (applied_brightness_percent_ == target_brightness) {
    return;
  }

  applied_brightness_percent_ = target_brightness;
  bsp_display_brightness_set(target_brightness);
}

void Ui::update_power_save(bool on_battery, bool print_active) {
  const uint32_t now = lv_tick_get();
  const uint32_t idle_ms = now - last_activity_tick_ms_.load();

  // While the LVGL worker is paused (screen off), LVGL touch events are not
  // processed.  Poll the raw touch-interrupt GPIO so a finger press can still
  // wake the display.  CST9217 pulls INT (GPIO 11) low on contact.
  if (screen_power_mode_ == ScreenPowerMode::kOff &&
      gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
    note_activity(true);  // updates last_activity_tick_ms_ + calls wake_display()
    return;               // re-evaluate on next call with fresh idle_ms
  }

  const uint32_t dim_timeout = (print_active
      ? battery_display_policy_.dim_timeout_active_s
      : battery_display_policy_.dim_timeout_idle_s) * 1000U;
  const uint32_t off_timeout = (print_active
      ? battery_display_policy_.off_timeout_active_s
      : battery_display_policy_.off_timeout_idle_s) * 1000U;

  ScreenPowerMode target_mode = ScreenPowerMode::kAwake;
  if (on_battery || battery_display_policy_.usb_power_save_enabled) {
    if (battery_display_policy_.screen_off_enabled && idle_ms >= off_timeout) {
      target_mode = ScreenPowerMode::kOff;
    } else if (battery_display_policy_.dim_enabled && idle_ms >= dim_timeout) {
      target_mode = ScreenPowerMode::kDimmed;
    }
  }

  if (screen_power_mode_ != target_mode) {
    const bool was_off = screen_power_mode_ == ScreenPowerMode::kOff;
    const bool going_off = target_mode == ScreenPowerMode::kOff;
    screen_power_mode_ = target_mode;
    apply_brightness_policy();

    // Pause the LVGL worker when the screen is off so lv_timer_handler() does
    // not run.  This prevents the flush path from blocking indefinitely on a TE
    // signal (GPIO13 edge via portMAX_DELAY semaphore) while the display panel
    // may have stopped producing TE edges at brightness 0.
    if (going_off && !was_off) {
      esp_lv_adapter_pause(1000);
    } else if (was_off && !going_off) {
      esp_lv_adapter_resume();
    }
  }
}

bool Ui::is_low_power_mode_active() const {
  return screen_power_mode_ != ScreenPowerMode::kAwake;
}

void Ui::pager_event_cb(lv_event_t* event) {
  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_pager_event(event);
  }
}

void Ui::screen_event_cb(lv_event_t* event) {
  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_screen_event(event);
  }
}

void Ui::logo_event_cb(lv_event_t* event) {
  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_logo_event(event);
  }
}

}  // namespace printsphere
