#pragma once

#include <cstdint>

#include "esp_err.h"

namespace printsphere {

struct PowerSnapshot {
  bool available = false;
  bool battery_present = false;
  uint8_t battery_percent = 0;
  bool charging = false;
  bool usb_present = false;
  float temperature_c = 0.0f;
};

class PmuManager {
 public:
  esp_err_t initialize();
  PowerSnapshot sample() const;
  bool is_ready() const { return initialized_; }

 private:
  bool initialized_ = false;
};

}  // namespace printsphere
