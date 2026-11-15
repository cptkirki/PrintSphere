#pragma once
#include "lvgl.h"
#include "esp_err.h"
inline constexpr int ESP_LV_ADAPTER_DEFAULT_STACK_SIZE=6144, ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY=5,
 ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS=2, ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS=1,
 ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS=100, ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED=0,
 ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS=60000, ESP_LV_ADAPTER_ROTATE_0=0,
 ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL=1, ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC=5, BSP_LCD_TOUCH_INT=11;
enum bsp_display_rotation_t { BSP_DISPLAY_ROTATE_0, BSP_DISPLAY_ROTATE_90, BSP_DISPLAY_ROTATE_180, BSP_DISPLAY_ROTATE_270 };
struct esp_lv_adapter_config_t {
 int task_stack_size, task_priority, task_core_id, tick_period_ms, task_min_delay_ms, task_max_delay_ms;
 bool stack_in_psram;
 struct { bool enable; int mode, idle_timeout_ms; struct { int reserved; } callbacks; } auto_sleep;
};
struct bsp_display_cfg_t {
 esp_lv_adapter_config_t lv_adapter_cfg;
 int rotation, tear_avoid_mode;
 struct { bool swap_xy, mirror_x, mirror_y; } touch_flags;
};
lv_display_t* bsp_display_start_with_config(bsp_display_cfg_t*);
int bsp_display_lock(uint32_t);
void bsp_display_unlock();
int bsp_display_brightness_set(int);
inline int bsp_display_rotation_set(bsp_display_rotation_t) { return ESP_OK; }
inline int esp_lv_adapter_pause(int) { return ESP_OK; }
inline int esp_lv_adapter_resume() { return ESP_OK; }
