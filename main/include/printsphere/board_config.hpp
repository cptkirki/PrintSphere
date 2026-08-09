#pragma once

#include "driver/gpio.h"

namespace printsphere::board {

#if defined(PRINTSPHERE_HW_VARIANT_AMOLED_1_75)
constexpr int kDisplayWidth = 466;
constexpr int kDisplayHeight = 466;

constexpr gpio_num_t kI2cScl = GPIO_NUM_14;
constexpr gpio_num_t kI2cSda = GPIO_NUM_15;

constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_11;
constexpr gpio_num_t kTouchReset = GPIO_NUM_40;

constexpr gpio_num_t kQspiClk = GPIO_NUM_38;
constexpr gpio_num_t kQspiData0 = GPIO_NUM_4;
constexpr gpio_num_t kQspiData1 = GPIO_NUM_5;
constexpr gpio_num_t kQspiData2 = GPIO_NUM_6;
constexpr gpio_num_t kQspiData3 = GPIO_NUM_7;

constexpr int kAxp2101Address = 0x34;
constexpr char kBoardName[] = "Waveshare ESP32-S3 AMOLED 1.75";
#elif defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;

constexpr gpio_num_t kI2cScl = GPIO_NUM_7;
constexpr gpio_num_t kI2cSda = GPIO_NUM_15;

constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_16;
constexpr gpio_num_t kTouchReset = GPIO_NUM_NC;

constexpr gpio_num_t kQspiClk = GPIO_NUM_41;
constexpr gpio_num_t kQspiData0 = GPIO_NUM_5;
constexpr gpio_num_t kQspiData1 = GPIO_NUM_45;
constexpr gpio_num_t kQspiData2 = GPIO_NUM_48;
constexpr gpio_num_t kQspiData3 = GPIO_NUM_47;

constexpr int kAxp2101Address = 0;
constexpr char kBoardName[] = "ESP32-S3-Touch-LCD-2.8C";
#elif defined(PRINTSPHERE_HW_VARIANT_KNOMI_V2)
constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr gpio_num_t kI2cScl = GPIO_NUM_1;
constexpr gpio_num_t kI2cSda = GPIO_NUM_2;

constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_17;
constexpr gpio_num_t kTouchReset = GPIO_NUM_16;

constexpr gpio_num_t kSpiSclk = GPIO_NUM_18;
constexpr gpio_num_t kSpiMosi = GPIO_NUM_14;
constexpr gpio_num_t kSpiCs = GPIO_NUM_20;
constexpr gpio_num_t kSpiDc = GPIO_NUM_19;
constexpr gpio_num_t kLcdRst = GPIO_NUM_21;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_12;

constexpr int kAxp2101Address = 0;
constexpr char kBoardName[] = "BTT Knomi v2";
#else
#error "Unknown PrintSphere hardware variant"
#endif

}  // namespace printsphere::board
