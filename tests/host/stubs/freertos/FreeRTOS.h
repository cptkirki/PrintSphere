#pragma once

// Host-build stub for FreeRTOS.h
// Only the type aliases referenced by data-only headers under test are provided

#include <cstdint>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFU)
#define pdPASS        ((BaseType_t)1)
#define pdFAIL        ((BaseType_t)0)
#define pdTRUE        ((BaseType_t)1)
#define pdFALSE       ((BaseType_t)0)

inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
