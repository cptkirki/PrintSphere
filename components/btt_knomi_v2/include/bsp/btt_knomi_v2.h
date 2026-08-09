#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "esp_codec_dev.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "bsp/display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_CAPS_DISPLAY 1
#define BSP_CAPS_TOUCH 1
#define BSP_CAPS_BUTTONS 0
#define BSP_CAPS_AUDIO 0
#define BSP_CAPS_AUDIO_SPEAKER 0
#define BSP_CAPS_AUDIO_MIC 0
#define BSP_CAPS_SDCARD 0
#define BSP_CAPS_IMU 0

#define BSP_I2C_SCL (GPIO_NUM_1)
#define BSP_I2C_SDA (GPIO_NUM_2)
#define BSP_I2C_NUM (0)

#define BSP_LCD_SPI_NUM (SPI2_HOST)
#define BSP_LCD_SPI_MOSI (GPIO_NUM_14)
#define BSP_LCD_SPI_SCLK (GPIO_NUM_18)
#define BSP_LCD_CS (GPIO_NUM_20)
#define BSP_LCD_DC (GPIO_NUM_19)
#define BSP_LCD_RST (GPIO_NUM_21)
#define BSP_LCD_BACKLIGHT (GPIO_NUM_12)

#define BSP_LCD_TOUCH_INT (GPIO_NUM_17)
#define BSP_LCD_TOUCH_RST (GPIO_NUM_16)

typedef struct {
    esp_lv_adapter_config_t lv_adapter_cfg;
    esp_lv_adapter_rotation_t rotation;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
    struct {
        unsigned int swap_xy;
        unsigned int mirror_x;
        unsigned int mirror_y;
    } touch_flags;
} bsp_display_cfg_t;

esp_err_t bsp_i2c_init(void);
esp_err_t bsp_i2c_deinit(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

esp_err_t bsp_spiffs_mount(void);
esp_err_t bsp_spiffs_unmount(void);

extern sdmmc_card_t *bsp_sdcard;
esp_err_t bsp_sdcard_mount(void);
esp_err_t bsp_sdcard_unmount(void);

lv_display_t *bsp_display_start(void);
lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg);
lv_indev_t *bsp_display_get_input_dev(void);
esp_err_t bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
esp_err_t bsp_audio_init(const void *i2s_config);

#ifdef __cplusplus
}
#endif
