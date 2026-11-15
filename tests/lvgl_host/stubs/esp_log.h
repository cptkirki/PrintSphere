#pragma once
#include "esp_timer.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
inline uint32_t esp_log_timestamp() { return static_cast<uint32_t>(host_ticks.load()); }
