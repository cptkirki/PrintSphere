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
constexpr int kPage2NoteWithImageY = 156;
constexpr int kPage2SubnoteWithImageY = 188;
constexpr int kPage3CameraWidth = 400;
constexpr int kPage3CameraHeight = 224;
constexpr int kPage3CameraYOffset = 20;
constexpr int kPage3NoteWithImageY = 150;
constexpr int kPage3SubnoteWithImageY = 182;
constexpr int kAuxTempRowY = 28;
constexpr int kSwipeThresholdPx = 24;
constexpr int kRotatedVisualOffsetX = -6;
constexpr int kRotatedVisualOffsetY = 12;
constexpr int kManualMinBrightnessPercent = 4;
constexpr uint32_t kRingAnimationTickMs = 220U;
constexpr uint8_t kRingPulseDepthPercent = 35U;
constexpr uint32_t kBatteryDimTimeoutIdleMs = 20000U;
constexpr uint32_t kBatteryOffTimeoutIdleMs = 60000U;
constexpr uint32_t kBatteryDimTimeoutActiveMs = 30000U;
constexpr uint32_t kBatteryOffTimeoutActiveMs = 120000U;
constexpr uint64_t kPortalHintIntroMs = 5ULL * 60ULL * 1000ULL;
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
  explicit LvglLockGuard(uint32_t timeout_ms) : locked_(bsp_display_lock(timeout_ms) == ESP_OK) {}
  ~LvglLockGuard() {
    if (locked_) {
      bsp_display_unlock();
    }
  }

  bool locked() const { return locked_; }

 private:
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

uint32_t pulse_between(uint32_t base_color, uint32_t period_ms, uint8_t pulse_depth_percent) {
  if (period_ms == 0U) {
    return base_color;
  }

  const uint32_t phase = lv_tick_get() % period_ms;
  const uint32_t half = std::max<uint32_t>(period_ms / 2U, 1U);
  uint16_t wave = 0U;
  if (phase < half) {
    wave = static_cast<uint16_t>((phase * 255U) / half);
  } else {
    const uint32_t down_phase = std::min<uint32_t>(phase - half, half);
    wave = static_cast<uint16_t>(255U - ((down_phase * 255U) / half));
  }

  const uint16_t depth = static_cast<uint16_t>(
      std::min<uint32_t>(pulse_depth_percent, 100U) * 255U / 100U);
  const uint16_t min_scale = static_cast<uint16_t>(255U > depth ? 255U - depth : 0U);
  const uint16_t scale = static_cast<uint16_t>(
      min_scale + ((static_cast<uint32_t>(255U - min_scale) * wave + 127U) / 255U));
  return scale_color(base_color, scale);
}

struct RingVisual {
  uint32_t main_hex = kRingBaseDark;
  uint32_t indicator_hex = 0xFFFFFF;
  int value_override = -1;
  bool animated = false;
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
    const uint32_t cycle = 2300U;
    const uint32_t t = lv_tick_get() % cycle;
    const bool is_loading =
        stage_contains(stage, "filament_loading") || stage_contains(stage, "changing_filament");
    int value = 0;
    if (t < 2000U) {
      const float f = static_cast<float>(t) / 2000.0f;
      value = is_loading ? static_cast<int>(std::lround(f * 100.0f))
                         : static_cast<int>(std::lround((1.0f - f) * 100.0f));
    } else {
      value = is_loading ? 100 : 0;
    }
    visual.main_hex = kRingBaseDark;
    visual.indicator_hex = colors.filament;
    visual.value_override = value;
    visual.animated = true;
    return visual;
  }

  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    visual.main_hex = colors.setup;
    visual.indicator_hex = colors.setup;
    return visual;
  }
  if (snapshot.connection == PrinterConnectionState::kError || snapshot.has_error ||
      snapshot.lifecycle == PrintLifecycleState::kError) {
    const uint32_t c = pulse_between(colors.error, 1600U, kRingPulseDepthPercent);
    visual.main_hex = c;
    visual.indicator_hex = c;
    visual.animated = true;
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
    visual.indicator_hex = pulse_between(colors.preheat, 1400U, kRingPulseDepthPercent);
    visual.animated = true;
    return visual;
  }

  if (stage_contains(stage, "heatbed_preheating") || stage_contains(stage, "nozzle_preheating") ||
      stage_contains(stage, "heating_hotend") || stage_contains(stage, "heating_chamber") ||
      stage_contains(stage, "waiting_for_heatbed_temperature") ||
      stage_contains(stage, "thermal_preconditioning") || stage_contains(stage, "preheat") ||
      status == "prepare" || snapshot.ui_status == "preparing" ||
      snapshot.ui_status == "preheating" ||
      snapshot.lifecycle == PrintLifecycleState::kPreparing) {
    const uint32_t c = pulse_between(colors.preheat, 1400U, kRingPulseDepthPercent);
    visual.main_hex = c;
    visual.indicator_hex = c;
    visual.animated = true;
    return visual;
  }

  if (stage_contains(stage, "cleaning_nozzle_tip") || stage_contains(stage, "clean") ||
      snapshot.ui_status == "clean nozzle") {
    const uint32_t c = pulse_between(colors.clean, 1200U, kRingPulseDepthPercent);
    visual.main_hex = c;
    visual.indicator_hex = c;
    visual.animated = true;
    return visual;
  }

  if (stage_contains(stage, "auto_bed_leveling") || stage_contains(stage, "bed_level") ||
      stage_contains(stage, "level") || stage_contains(stage, "measuring_surface") ||
      snapshot.ui_status == "bed level") {
    const uint32_t c = pulse_between(colors.level, 1400U, kRingPulseDepthPercent);
    visual.main_hex = c;
    visual.indicator_hex = c;
    visual.animated = true;
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

bool ring_visual_is_animated(const PrinterSnapshot& snapshot, const ArcColorScheme& colors) {
  return lifecycle_ring_visual(snapshot, colors).animated;
}

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
    return "Cloud cover loaded";
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
    return "Camera snapshot";
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
      .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
      .rotation = ESP_LV_ADAPTER_ROTATE_0,
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
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
  ring_anim_timer_ = lv_timer_create(&Ui::ring_timer_cb, kRingAnimationTickMs, this);

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

  LvglLockGuard lock(200);
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

void Ui::set_portal_access_state(bool request_authorized, bool session_active, bool pin_active,
                                 const std::string& pin_code, uint32_t pin_remaining_s,
                                 uint32_t session_remaining_s) {
  if (!initialized_) {
    return;
  }

  LvglLockGuard lock(200);
  if (!lock.locked()) {
    return;
  }

  portal_request_authorized_ = request_authorized;
  portal_session_active_ = session_active;
  portal_pin_active_ = pin_active;
  portal_pin_code_ = pin_code;
  portal_pin_remaining_s_ = pin_remaining_s;
  portal_session_remaining_s_ = session_remaining_s;
  const bool provisioning_context =
      last_snapshot_.setup_ap_active ||
      last_snapshot_.connection == PrinterConnectionState::kWaitingForCredentials;
  const bool station_portal_available =
      !last_snapshot_.setup_ap_active && last_snapshot_.wifi_connected && !last_snapshot_.wifi_ip.empty();

  if (portal_pin_active_) {
    portal_hint_text_ = request_authorized ? "Web Config PIN active on the display"
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
    if (portal_session_active_ && request_authorized) {
      portal_hint_text_ =
          "Web Config unlocked for " +
          short_duration_text(std::max<uint32_t>(portal_session_remaining_s_, 1U));
    } else if (provisioning_context) {
      portal_hint_text_.clear();
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

void Ui::apply_snapshot(const PrinterSnapshot& snapshot) {
  if (!initialized_) {
    return;
  }

  LvglLockGuard lock(200);
  if (!lock.locked()) {
    return;
  }

  const PrinterSnapshot previous_snapshot = last_snapshot_;
  const bool was_animated = ring_animation_active_;
  last_snapshot_ = snapshot;
  if (scrolling_) {
    deferred_snapshot_ = snapshot;
    deferred_snapshot_pending_ = true;
    return;
  }

  const bool animation_state_changed =
      ring_visual_is_animated(snapshot, arc_colors_) != was_animated ||
      snapshot.raw_status != previous_snapshot.raw_status ||
      snapshot.raw_stage != previous_snapshot.raw_stage ||
      snapshot.connection != previous_snapshot.connection ||
      snapshot.lifecycle != previous_snapshot.lifecycle ||
      snapshot.has_error != previous_snapshot.has_error ||
      snapshot.wifi_connected != previous_snapshot.wifi_connected ||
      snapshot.ui_status != previous_snapshot.ui_status ||
      std::lround(snapshot.progress_percent) != std::lround(previous_snapshot.progress_percent);

  apply_snapshot_locked(snapshot, animation_state_changed);
}

void Ui::apply_ring_visual_locked(const PrinterSnapshot& snapshot) {
  const int progress = std::clamp(static_cast<int>(snapshot.progress_percent + 0.5f), 0, 100);
  const RingVisual ring = lifecycle_ring_visual(snapshot, arc_colors_);
  const uint32_t text_hex = stable_status_text_hex(snapshot, arc_colors_);
  const int displayed_value = (ring.value_override >= 0) ? ring.value_override : progress;

  if (lv_arc_get_value(status_arc_) != displayed_value) {
    lv_arc_set_value(status_arc_, displayed_value);
  }

  if (last_ring_main_hex_ != ring.main_hex) {
    lv_obj_set_style_arc_color(status_arc_, lv_color_hex(ring.main_hex), LV_PART_MAIN);
    last_ring_main_hex_ = ring.main_hex;
  }
  if (last_ring_indicator_hex_ != ring.indicator_hex) {
    lv_obj_set_style_arc_color(status_arc_, lv_color_hex(ring.indicator_hex), LV_PART_INDICATOR);
    last_ring_indicator_hex_ = ring.indicator_hex;
  }
  if (last_ring_text_hex_ != text_hex) {
    const lv_color_t text_color = lv_color_hex(text_hex);
    lv_obj_set_style_text_color(progress_label_, text_color, 0);
    lv_obj_set_style_text_color(status_label_, text_color, 0);
    last_ring_text_hex_ = text_hex;
  }
}

bool Ui::ensure_preview_image_loaded_locked(bool force_reload) {
  if (force_reload) {
    release_preview_image_locked();
  }

  if (last_preview_raw_ && !last_preview_raw_->empty()) {
    lv_image_set_src(page2_image_, &preview_image_dsc_);
    return true;
  }

  if (!last_preview_blob_ || last_preview_blob_->empty()) {
    return false;
  }

  if (!decode_preview_png(last_preview_blob_, &last_preview_raw_, &preview_image_dsc_)) {
    std::memset(&preview_image_dsc_, 0, sizeof(preview_image_dsc_));
    last_preview_raw_.reset();
    return false;
  }

  lv_image_set_src(page2_image_, &preview_image_dsc_);
  return true;
}

void Ui::release_preview_image_locked() {
  if (page2_image_ != nullptr) {
    lv_image_set_src(page2_image_, nullptr);
  }
  lv_image_cache_drop(&preview_image_dsc_);
  last_preview_raw_.reset();
  std::memset(&preview_image_dsc_, 0, sizeof(preview_image_dsc_));
}

void Ui::apply_snapshot_locked(const PrinterSnapshot& snapshot, bool force_ring_refresh) {
  deferred_snapshot_pending_ = false;
  update_page_availability_locked(snapshot);

  if (!snapshot.ui_status.empty() &&
      (snapshot.ui_status != last_ui_status_ || snapshot.print_active != last_print_active_)) {
    note_activity(true);
    last_ui_status_ = snapshot.ui_status;
    last_print_active_ = snapshot.print_active;
  }

  ring_animation_active_ = ring_visual_is_animated(snapshot, arc_colors_);
  const int progress = std::clamp(static_cast<int>(snapshot.progress_percent + 0.5f), 0, 100);
  if (!ring_animation_active_ || force_ring_refresh) {
    apply_ring_visual_locked(snapshot);
  }

  char progress_buffer[8] = {};
  std::snprintf(progress_buffer, sizeof(progress_buffer), "%d%%", progress);
  set_label_text_if_changed(progress_label_, progress_buffer);
  set_label_text_if_changed(status_label_, lifecycle_label(snapshot));

  const std::string detail = detail_text(snapshot);
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
  set_label_text_if_changed(battery_icon_label_, battery_icon);
  set_label_text_if_changed(battery_pct_label_, battery_pct);

  const std::string preview_note = preview_note_text(snapshot);
  const std::string preview_subnote = preview_subnote_text(snapshot);
  const std::string camera_note = camera_note_text(snapshot);
  const std::string camera_subnote = camera_subnote_text(snapshot);
  bool has_preview_image = false;
  if (snapshot.preview_blob && !snapshot.preview_blob->empty()) {
    const bool preview_blob_changed = last_preview_blob_.get() != snapshot.preview_blob.get();
    last_preview_blob_ = snapshot.preview_blob;
    if (active_page_ == 1) {
      has_preview_image = ensure_preview_image_loaded_locked(preview_blob_changed);
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
      lv_obj_align(page2_note_, LV_ALIGN_CENTER, 0, kPage2NoteWithImageY);
      lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, kPage2SubnoteWithImageY);
    } else {
      lv_obj_align(page2_note_, LV_ALIGN_CENTER, 0, 0);
      lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, 28);
    }
    preview_text_image_mode_ = has_page2_image;
  }
  set_label_text_if_changed(page2_note_, preview_note);
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
      lv_obj_align(page3_note_, LV_ALIGN_CENTER, 0, kPage3NoteWithImageY);
      lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, kPage3SubnoteWithImageY);
    } else {
      lv_obj_align(page3_note_, LV_ALIGN_CENTER, 0, 0);
      lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, 28);
    }
    camera_text_image_mode_ = has_camera_image;
  }
  set_label_text_if_changed(page3_note_, camera_note);
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
  if (!initialized_ || !ring_animation_active_ || status_arc_ == nullptr ||
      screen_power_mode_ == ScreenPowerMode::kOff || scrolling_) {
    return;
  }

  apply_ring_visual_locked(last_snapshot_);
}

esp_err_t Ui::build_dashboard() {
  LvglLockGuard lock(1000);
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
  lv_obj_set_scroll_snap_x(pager_, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_scrollbar_mode(pager_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(pager_, &Ui::pager_event_cb, LV_EVENT_ALL, this);

  auto create_page = [](lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, board::kDisplayWidth, board::kDisplayHeight);
    make_transparent(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
  };

  page1_ = create_page(pager_);
  page2_ = create_page(pager_);
  page3_ = create_page(pager_);
  enable_touch_bubble(page1_);
  enable_touch_bubble(page2_);
  enable_touch_bubble(page3_);

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
  lv_obj_align(page2_note_, LV_ALIGN_CENTER, 0, 0);
  enable_touch_bubble(page2_note_);

  page2_subnote_ = lv_label_create(page2_);
  set_label_text_if_changed(page2_subnote_, "");
  lv_obj_set_width(page2_subnote_, 320);
  lv_label_set_long_mode(page2_subnote_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page2_subnote_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page2_subnote_, info20, 0);
  lv_obj_set_style_text_color(page2_subnote_, lv_color_hex(0x888888), 0);
  lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, 28);
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
  lv_obj_scroll_to_x(pager_, 0, LV_ANIM_OFF);

  active_page_ = 0;
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
  const bool show_page1 = !scrolling_ && active_page_ == 0;
  const bool show_page2 = !scrolling_ && active_page_ == 1;
  const bool show_page3 = !scrolling_ && active_page_ == 2;
  const bool portal_hint_has_priority = portal_pin_active_ || portal_session_active_;
  const bool show_portal_hint =
      show_page1 && !portal_hint_text_.empty() &&
      (portal_hint_has_priority || !detail_visible_);

  set_hidden(page2_, !preview_page_available_);
  set_hidden(page3_, !camera_page_available_);
  set_hidden(status_label_, !show_page1);
  set_hidden(detail_label_, !show_page1 || !detail_visible_ || show_portal_hint);
  set_hidden(layer_label_, !show_page1);
  set_hidden(nozzle_prefix_label_, !show_page1);
  set_hidden(nozzle_value_label_, !show_page1);
  set_hidden(nozzle_aux_label_, !show_page1 || !nozzle_aux_visible_);
  set_hidden(bed_prefix_label_, !show_page1);
  set_hidden(bed_value_label_, !show_page1);
  set_hidden(bed_aux_label_, !show_page1 || !bed_aux_visible_);
  set_hidden(remaining_row_, !show_page1);
  set_hidden(badge_slot_, !show_page1);
  set_hidden(portal_hint_label_, !show_portal_hint);
  set_hidden(page2_image_, !show_page2 || !preview_image_visible_);
  set_hidden(page3_image_, !show_page3 || !camera_image_visible_);

  apply_logo_visibility();
  update_portal_access_visuals_locked();
}

void Ui::apply_logo_visibility() {
  const bool show_badge_slot = !scrolling_ && active_page_ == 0;
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
  const bool availability_changed =
      preview_page_available_ != preview_available || camera_page_available_ != camera_available;

  preview_page_available_ = preview_available;
  camera_page_available_ = camera_available;

  if (!availability_changed) {
    return;
  }

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
}

bool Ui::page_enabled(int page) const {
  switch (page) {
    case 0:
      return true;
    case 1:
      return preview_page_available_;
    case 2:
      return camera_page_available_;
    default:
      return false;
  }
}

lv_obj_t* Ui::page_object(int page) const {
  switch (page) {
    case 0:
      return page1_;
    case 1:
      return page2_;
    case 2:
      return page3_;
    default:
      return nullptr;
  }
}

int Ui::next_enabled_page(int page, int direction) const {
  int candidate = page + direction;
  while (candidate >= 0 && candidate <= 2) {
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

  for (int candidate = 0; candidate <= 2; ++candidate) {
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

  const int viewport_center = scroll_x + (board::kDisplayWidth / 2);
  int best_page = clamp_enabled_page(active_page_);
  int best_distance = INT32_MAX;

  for (int page = 0; page <= 2; ++page) {
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
    lv_obj_scroll_to_view(target_page, LV_ANIM_OFF);
  }
  if (previous_page == 1 && clamped_page != 1) {
    release_preview_image_locked();
    preview_image_visible_ = false;
  }
  active_page_ = clamped_page;
  if (active_page_ == 1) {
    preview_image_visible_ = ensure_preview_image_loaded_locked(false);
  }
  scrolling_ = false;
  apply_page_visibility();
  if (deferred_snapshot_pending_) {
    apply_snapshot_locked(deferred_snapshot_, true);
  } else if (previous_page != clamped_page) {
    apply_snapshot_locked(last_snapshot_, true);
  }
}

void Ui::handle_pager_event(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_SCROLL_BEGIN && code != LV_EVENT_SCROLL_END && code != LV_EVENT_RELEASED) {
    return;
  }

  if (code == LV_EVENT_SCROLL_BEGIN) {
    scrolling_ = true;
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

    if (abs_dx >= kSwipeThresholdPx && abs_dx > abs_dy + 8) {
      if (dx < 0) {
        const int next_page = next_enabled_page(active_page_, 1);
        if (next_page != active_page_) {
          set_active_page(next_page);
        }
      } else if (dx > 0) {
        const int previous_page = next_enabled_page(active_page_, -1);
        if (previous_page != active_page_) {
          set_active_page(previous_page);
        }
      }
    } else if (active_page_ == 2 && camera_page_available_ && abs_dx < 12 && abs_dy < 12) {
      std::lock_guard<std::mutex> lock(camera_refresh_mutex_);
      camera_refresh_requested_ = true;
    }
  }
}

void Ui::update_portal_access_visuals_locked() {
  const bool show_page1 = !scrolling_ && active_page_ == 0;
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

  screen_power_mode_ = ScreenPowerMode::kAwake;
  apply_brightness_policy();
}

void Ui::apply_brightness_policy() {
  int target_brightness = user_brightness_percent_;
  if (screen_power_mode_ == ScreenPowerMode::kDimmed) {
    target_brightness = std::max(8, std::min(18, std::max(1, user_brightness_percent_ / 3)));
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
  const uint32_t dim_timeout = print_active ? kBatteryDimTimeoutActiveMs : kBatteryDimTimeoutIdleMs;
  const uint32_t off_timeout = print_active ? kBatteryOffTimeoutActiveMs : kBatteryOffTimeoutIdleMs;

  ScreenPowerMode target_mode = ScreenPowerMode::kAwake;
  if (on_battery) {
    if (idle_ms >= off_timeout) {
      target_mode = ScreenPowerMode::kOff;
    } else if (idle_ms >= dim_timeout) {
      target_mode = ScreenPowerMode::kDimmed;
    }
  }

  if (screen_power_mode_ != target_mode) {
    screen_power_mode_ = target_mode;
    apply_brightness_policy();
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
