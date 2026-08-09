#include "bsp/btt_knomi_v2.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter_input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

static const char *TAG = "BTT-Knomi-v2";

#define BSP_LCD_SPI_PCLK_HZ (40 * 1000 * 1000)
#define BSP_LCD_DRAW_BUFFER_HEIGHT 40
#define BSP_AW9364_MAX_LEVEL 16

static i2c_master_bus_handle_t s_i2c_handle = NULL;
static bool s_i2c_initialized = false;
static bool s_backlight_initialized = false;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_display = NULL;
static lv_indev_t *s_indev = NULL;
static uint8_t s_brightness_percent = 0;
static int8_t s_aw9364_level = -1;

sdmmc_card_t *bsp_sdcard = NULL;

static esp_err_t apply_touch_flags(const bsp_display_cfg_t *cfg) {
    if (s_touch_handle == NULL || cfg == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_swap_xy(s_touch_handle, cfg->touch_flags.swap_xy != 0), TAG,
                        "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_x(s_touch_handle, cfg->touch_flags.mirror_x != 0), TAG,
                        "mirror_x failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_y(s_touch_handle, cfg->touch_flags.mirror_y != 0), TAG,
                        "mirror_y failed");
    return ESP_OK;
}

esp_err_t bsp_i2c_init(void) {
    if (s_i2c_initialized) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_NUM,
        .scl_io_num = BSP_I2C_SCL,
        .sda_io_num = BSP_I2C_SDA,
        .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_handle), TAG, "i2c init failed");
    s_i2c_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void) {
    if (!s_i2c_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(i2c_del_master_bus(s_i2c_handle), TAG, "i2c deinit failed");
    s_i2c_handle = NULL;
    s_i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
    if (bsp_i2c_init() != ESP_OK) {
        return NULL;
    }
    return s_i2c_handle;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io) {
    if (s_panel_handle != NULL) {
        if (ret_panel != NULL) {
            *ret_panel = s_panel_handle;
        }
        if (ret_io != NULL) {
            *ret_io = s_panel_io;
        }
        return ESP_OK;
    }

    const int max_transfer_sz =
        (config != NULL && config->max_transfer_sz > 0)
            ? config->max_transfer_sz
            : (BSP_LCD_H_RES * BSP_LCD_DRAW_BUFFER_HEIGHT * BSP_LCD_BITS_PER_PIXEL / 8);

    ESP_LOGI(TAG, "Initialize SPI bus for GC9A01");
    const spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(BSP_LCD_SPI_SCLK, BSP_LCD_SPI_MOSI,
                                                                max_transfer_sz);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_config =
        GC9A01_PANEL_IO_SPI_CONFIG(BSP_LCD_CS, BSP_LCD_DC, NULL, NULL);
    io_config.pclk_hz = BSP_LCD_SPI_PCLK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_panel_io),
        TAG, "panel io failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(s_panel_io, &panel_config, &s_panel_handle), TAG,
                        "gc9a01 create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");
    // Match BTT firmware: invert on for Knomi GC9A01 panels.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, true), TAG, "invert failed");
    // Default MADCTL; final orientation is applied by bsp_display_rotation_set().
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, false), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, true, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "disp on failed");

    if (ret_panel != NULL) {
        *ret_panel = s_panel_handle;
    }
    if (ret_io != NULL) {
        *ret_io = s_panel_io;
    }
    return ESP_OK;
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg) {
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const bool use_psram = psram_total > 0;
    const uint32_t buffer_height = use_psram ? BSP_LCD_DRAW_BUFFER_HEIGHT : 20;

    const bsp_display_config_t disp_config = {
        .max_transfer_sz = BSP_LCD_H_RES * (int)buffer_height * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    if (bsp_display_new(&disp_config, NULL, NULL) != ESP_OK) {
        return NULL;
    }

    // SPI panels use the generic adapter path; TE sync is unavailable.
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel_handle,
        .panel_io = s_panel_io,
        .profile =
            {
                .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
                .rotation = cfg->rotation,
                .hor_res = BSP_LCD_H_RES,
                .ver_res = BSP_LCD_V_RES,
                .buffer_height = buffer_height,
                .use_psram = use_psram,
                .enable_ppa_accel = false,
                .require_double_buffer = use_psram,
            },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    return esp_lv_adapter_register_display(&disp_cfg);
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp) {
    ESP_RETURN_ON_FALSE(bsp_i2c_get_handle() != NULL, NULL, TAG, "i2c handle unavailable");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_config.scl_speed_hz = 100000;

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels =
            {
                .reset = 0,
                .interrupt = 0,
            },
        .flags =
            {
                .swap_xy = cfg->touch_flags.swap_xy,
                .mirror_x = cfg->touch_flags.mirror_x,
                .mirror_y = cfg->touch_flags.mirror_y,
            },
    };

    if (esp_lcd_new_panel_io_i2c(s_i2c_handle, &tp_io_config, &tp_io) != ESP_OK) {
        return NULL;
    }
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_touch_handle) != ESP_OK) {
        return NULL;
    }

    const esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_touch_handle);
    return esp_lv_adapter_register_touch(&touch_cfg);
}

lv_display_t *bsp_display_start(void) {
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags =
            {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg != NULL, NULL, TAG, "cfg is null");
    if (s_display != NULL) {
        return s_display;
    }

    if (esp_lv_adapter_init(&cfg->lv_adapter_cfg) != ESP_OK) {
        return NULL;
    }
    s_display = bsp_display_lcd_init(cfg);
    if (s_display == NULL) {
        return NULL;
    }
    s_indev = bsp_display_indev_init(cfg, s_display);
    if (s_indev == NULL) {
        return NULL;
    }
    if (bsp_display_brightness_init() != ESP_OK) {
        return NULL;
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    (void)apply_touch_flags(cfg);
    return s_display;
}

lv_indev_t *bsp_display_get_input_dev(void) {
    return s_indev;
}

esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation) {
    if (s_panel_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // esp_lv_adapter does not rotate ESP_LV_ADAPTER_PANEL_IF_OTHER / SPI panels.
    // Orientation must be applied via GC9A01 MADCTL (swap_xy + mirror).
    // Baseline 0° matches BTT firmware: mirror X only.
    bool swap_xy = false;
    bool mirror_x = true;
    bool mirror_y = false;
    switch (rotation) {
        case BSP_DISPLAY_ROTATE_90:
            swap_xy = true;
            mirror_x = true;
            mirror_y = true;
            break;
        case BSP_DISPLAY_ROTATE_180:
            swap_xy = false;
            mirror_x = false;
            mirror_y = true;
            break;
        case BSP_DISPLAY_ROTATE_270:
            swap_xy = true;
            mirror_x = false;
            mirror_y = false;
            break;
        case BSP_DISPLAY_ROTATE_0:
        default:
            break;
    }

    ESP_LOGI(TAG, "Set panel rotation %d (swap_xy=%d mirror_x=%d mirror_y=%d)", (int)rotation,
             (int)swap_xy, (int)mirror_x, (int)mirror_y);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, swap_xy), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, mirror_x, mirror_y), TAG,
                        "mirror failed");

    // Keep LVGL logical orientation at 0° — pixel transform is panel-side only.
    if (s_display != NULL) {
        ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(1000), TAG, "display lock failed");
        lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_0);
        esp_lv_adapter_unlock();
    }
    return ESP_OK;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms) {
    return esp_lv_adapter_lock((int32_t)timeout_ms);
}

void bsp_display_unlock(void) {
    esp_lv_adapter_unlock();
}

// AW9364 single-wire pulse brightness (ported from BTT KNOMI firmware).
static void aw9364_set_level(int8_t level) {
    if (level > BSP_AW9364_MAX_LEVEL) {
        level = BSP_AW9364_MAX_LEVEL;
    }
    if (level < 0) {
        level = 0;
    }
    if (s_aw9364_level == level) {
        return;
    }

    if (level == 0) {
        gpio_set_level(BSP_LCD_BACKLIGHT, 0);
        vTaskDelay(pdMS_TO_TICKS(3));
        s_aw9364_level = 0;
        return;
    }

    if (s_aw9364_level <= 0) {
        gpio_set_level(BSP_LCD_BACKLIGHT, 1);
        ets_delay_us(25);
        s_aw9364_level = BSP_AW9364_MAX_LEVEL;
    }

    if (s_aw9364_level < level) {
        s_aw9364_level = (int8_t)(s_aw9364_level + BSP_AW9364_MAX_LEVEL);
    }

    const int8_t pulses = (int8_t)(s_aw9364_level - level);
    for (int8_t i = 0; i < pulses; ++i) {
        gpio_set_level(BSP_LCD_BACKLIGHT, 0);
        ets_delay_us(1);
        gpio_set_level(BSP_LCD_BACKLIGHT, 1);
        ets_delay_us(1);
    }
    s_aw9364_level = level;
}

esp_err_t bsp_display_brightness_init(void) {
    if (s_backlight_initialized) {
        return ESP_OK;
    }
    const gpio_config_t gpio_config_backlight = {
        .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_config_backlight), TAG, "backlight gpio config failed");
    gpio_set_level(BSP_LCD_BACKLIGHT, 0);
    vTaskDelay(pdMS_TO_TICKS(3));
    s_aw9364_level = 0;
    s_backlight_initialized = true;
    return bsp_display_brightness_set(70);
}

esp_err_t bsp_display_brightness_set(int brightness_percent) {
    ESP_RETURN_ON_FALSE(s_backlight_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "backlight not initialized");
    const int clamped = brightness_percent < 0 ? 0 : (brightness_percent > 100 ? 100 : brightness_percent);
    s_brightness_percent = (uint8_t)clamped;
    // Map 0–100% onto AW9364 levels 0–16.
    const int8_t level =
        clamped == 0 ? 0 : (int8_t)((clamped * BSP_AW9364_MAX_LEVEL + 99) / 100);
    aw9364_set_level(level);
    return ESP_OK;
}

int bsp_display_brightness_get(void) {
    return s_brightness_percent;
}

esp_err_t bsp_display_backlight_on(void) {
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_backlight_off(void) {
    return bsp_display_brightness_set(0);
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void) {
    ESP_LOGW(TAG, "Audio codec is not available on Knomi v2");
    return NULL;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void) {
    return NULL;
}

esp_err_t bsp_audio_init(const void *i2s_config) {
    (void)i2s_config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_spiffs_mount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_spiffs_unmount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_mount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_unmount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}
