#pragma once

// Host-build stub for ESP-IDFs <esp_log.h>
// All macros are no-ops

#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)

#define ESP_EARLY_LOGE(tag, ...) ((void)0)
#define ESP_EARLY_LOGW(tag, ...) ((void)0)
#define ESP_EARLY_LOGI(tag, ...) ((void)0)
#define ESP_EARLY_LOGD(tag, ...) ((void)0)
#define ESP_EARLY_LOGV(tag, ...) ((void)0)

inline void esp_log_level_set(const char*, int) {}

enum {
  ESP_LOG_NONE,
  ESP_LOG_ERROR,
  ESP_LOG_WARN,
  ESP_LOG_INFO,
  ESP_LOG_DEBUG,
  ESP_LOG_VERBOSE,
};
