#pragma once

// Host-build stub for ESP-IDFs <mqtt_client.h>
// Only the type aliases and enums referenced by headers under test are provided

#include <cstdint>

typedef const char* esp_event_base_t;
typedef void* esp_mqtt_client_handle_t;
typedef void* esp_mqtt_event_handle_t;

enum {
  MQTT_CONNECTION_ACCEPTED = 0,
};
