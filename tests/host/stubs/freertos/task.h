#pragma once

// Host-build stub for freertos/task.h

#include "freertos/FreeRTOS.h"

typedef void* TaskHandle_t;

inline BaseType_t xTaskCreate(void (*)(void*), const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*) {
  return pdPASS;
}

inline void vTaskDelay(TickType_t) {}
inline void vTaskDelete(TaskHandle_t) {}
inline TickType_t xTaskGetTickCount() { return 0; }
inline void xTaskNotifyGive(TaskHandle_t) {}
