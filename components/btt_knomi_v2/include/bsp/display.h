#pragma once

#include "driver/gpio.h"
#include "esp_lcd_types.h"

#define ESP_LCD_COLOR_FORMAT_RGB565 (1)

#define BSP_LCD_COLOR_FORMAT (ESP_LCD_COLOR_FORMAT_RGB565)
#define BSP_LCD_BIGENDIAN (0)
#define BSP_LCD_BITS_PER_PIXEL (16)
#define BSP_LCD_COLOR_SPACE (ESP_LCD_COLOR_SPACE_RGB)
#define BSP_LCD_H_RES (240)
#define BSP_LCD_V_RES (240)
#define BSP_LCD_TE_GPIO (GPIO_NUM_NC)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int max_transfer_sz;
} bsp_display_config_t;

typedef enum {
    BSP_DISPLAY_ROTATE_0 = 0,
    BSP_DISPLAY_ROTATE_90 = 1,
    BSP_DISPLAY_ROTATE_180 = 2,
    BSP_DISPLAY_ROTATE_270 = 3,
} bsp_display_rotation_t;

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io);

esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
int bsp_display_brightness_get(void);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_display_backlight_off(void);
esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation);

#ifdef __cplusplus
}
#endif
