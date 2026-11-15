#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst9217.h"

#include "esp_codec_dev_defaults.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "bsp_err_check.h"
#include "bsp/display.h"
#include "bsp/touch.h"

static const char *TAG = "ESP32-S3-Touch-AMOLED-1.75";

#define BSP_LCD_DMA_STAGING_ROWS (4)
static i2c_master_bus_handle_t i2c_handle = NULL; // I2C Handle
static bool i2c_initialized = false;
static esp_io_expander_handle_t io_expander = NULL; // IO expander tca9554 handle
static lv_indev_t *disp_indev = NULL;
sdmmc_card_t *bsp_sdcard = NULL; // Global uSD card handler
static esp_lcd_touch_handle_t tp = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL; // LCD panel handle
static esp_lcd_panel_handle_t adapter_panel_handle = NULL; // Rotation-aware panel handle for LVGL
static esp_lcd_panel_io_handle_t io_handle = NULL;
uint8_t brightness;
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL; /* Codec data interface */

#define BSP_ES7210_CODEC_ADDR ES7210_CODEC_DEFAULT_ADDR
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

static const co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_handle_t target;
    bsp_display_rotation_t rotation;
    // Small DMA-capable internal-RAM buffer used as a staging area when the
    // source bitmap lives in PSRAM (or when rotation produces an output too
    // large to fit in internal heap as a single chunk). Sized for two slots
    // of `BSP_LCD_DMA_STAGING_ROWS` rows each so DMA can ping-pong while the
    // CPU prepares the next slot.
    uint16_t *rotation_buffer;
    size_t rotation_buffer_pixels;
    // Large rotation scratch placed in PSRAM. Holds the rotated full-screen
    // bitmap for 90/270 deg rendering. Internal heap is too small for the
    // 466*466*2 byte working set at full-screen flush. Output is then pushed
    // out to the panel in DMA-sized chunks via `rotation_buffer`.
    uint16_t *rotated_buffer;
    size_t rotated_buffer_pixels;
    bool te_sync_mode;
    uint8_t staging_slot;
} bsp_rotation_panel_t;

static bsp_rotation_panel_t rotation_panel = {0};

static bsp_rotation_panel_t *bsp_rotation_panel_from_base(esp_lcd_panel_t *panel)
{
    return panel ? (bsp_rotation_panel_t *)panel->user_data : NULL;
}

static esp_err_t bsp_rotation_panel_reset(esp_lcd_panel_t *panel)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_reset(ctx->target) : ESP_ERR_INVALID_STATE;
}

static esp_err_t bsp_rotation_panel_init(esp_lcd_panel_t *panel)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_init(ctx->target) : ESP_ERR_INVALID_STATE;
}

static esp_err_t bsp_rotation_panel_del(esp_lcd_panel_t *panel)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    if (!ctx || !ctx->target)
    {
        return ESP_ERR_INVALID_STATE;
    }

    free(ctx->rotation_buffer);
    ctx->rotation_buffer = NULL;
    ctx->rotation_buffer_pixels = 0;
    free(ctx->rotated_buffer);
    ctx->rotated_buffer = NULL;
    ctx->rotated_buffer_pixels = 0;
    ctx->te_sync_mode = false;
    ctx->staging_slot = 0;
    return esp_lcd_panel_del(ctx->target);
}

// Ensure the large PSRAM scratch used for the rotated frame is sized to fit
// `pixels` (RGB565). PSRAM is plentiful (8 MB on this board) and cheaper than
// the internal-RAM scratch which can't hold a full-screen rotated bitmap.
static bool bsp_rotation_panel_ensure_rotated_buffer(bsp_rotation_panel_t *ctx, size_t pixels)
{
    if (ctx->rotated_buffer_pixels >= pixels && ctx->rotated_buffer != NULL)
    {
        return true;
    }

    free(ctx->rotated_buffer);
    ctx->rotated_buffer = heap_caps_malloc(pixels * sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->rotated_buffer == NULL)
    {
        // Last-ditch fallback to internal heap (small displays / low-PSRAM
        // configurations). Will silently fail for full-screen flushes on this
        // board, but keeps small partial-flush rotations working.
        ctx->rotated_buffer = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    ctx->rotated_buffer_pixels = ctx->rotated_buffer ? pixels : 0;
    return ctx->rotated_buffer != NULL;
}

static void bsp_rotation_panel_rotate_region_90(const uint16_t *src, uint16_t *dst,
                                                int src_w, int src_h)
{
    // Write the rotated output row-contiguously. On PSRAM this is much cheaper
    // than the naive source-row loop, which writes one pixel every destination row.
    for (int dst_y = 0; dst_y < src_w; ++dst_y)
    {
        const uint16_t *src_col = src + dst_y;
        uint16_t *dst_row = dst + ((size_t)dst_y * src_h);
        for (int dst_x = 0; dst_x < src_h; ++dst_x)
        {
            dst_row[dst_x] = src_col[(size_t)(src_h - 1 - dst_x) * src_w];
        }
    }
}

static void bsp_rotation_panel_rotate_region_270(const uint16_t *src, uint16_t *dst,
                                                 int src_w, int src_h)
{
    // Same layout strategy as 90 deg: contiguous writes into the rotated buffer.
    for (int dst_y = 0; dst_y < src_w; ++dst_y)
    {
        const int src_x = src_w - 1 - dst_y;
        uint16_t *dst_row = dst + ((size_t)dst_y * src_h);
        for (int dst_x = 0; dst_x < src_h; ++dst_x)
        {
            dst_row[dst_x] = src[((size_t)dst_x * src_w) + src_x];
        }
    }
}

#include "bsp_dma_staging.h"

static esp_err_t bsp_rotation_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                                int x_end, int y_end, const void *color_data)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    if (!ctx || !ctx->target)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (ctx->rotation != BSP_DISPLAY_ROTATE_90 && ctx->rotation != BSP_DISPLAY_ROTATE_270)
    {
        if (color_data && esp_ptr_external_ram(color_data))
        {
            return bsp_rotation_panel_draw_staged(ctx, x_start, y_start, x_end, y_end,
                                                 (const uint16_t *)color_data);
        }
        return esp_lcd_panel_draw_bitmap(ctx->target, x_start, y_start, x_end, y_end, color_data);
    }

    const int src_w = x_end - x_start;
    const int src_h = y_end - y_start;
    if (src_w <= 0 || src_h <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t pixels = (size_t)src_w * src_h;
    if (!bsp_rotation_panel_ensure_rotated_buffer(ctx, pixels))
    {
        ESP_LOGE(TAG, "Unable to allocate %" PRIu32 " px rotation buffer", (uint32_t)pixels);
        return ESP_ERR_NO_MEM;
    }

    const uint16_t *src = (const uint16_t *)color_data;
    int dst_x_start = 0;
    int dst_y_start = 0;
    int dst_x_end = 0;
    int dst_y_end = 0;
    if (ctx->rotation == BSP_DISPLAY_ROTATE_90)
    {
        bsp_rotation_panel_rotate_region_90(src, ctx->rotated_buffer, src_w, src_h);
        dst_x_start = BSP_LCD_H_RES - y_end;
        dst_x_end = BSP_LCD_H_RES - y_start;
        dst_y_start = x_start;
        dst_y_end = x_end;
    }
    else
    {
        bsp_rotation_panel_rotate_region_270(src, ctx->rotated_buffer, src_w, src_h);
        dst_x_start = y_start;
        dst_x_end = y_end;
        dst_y_start = BSP_LCD_V_RES - x_end;
        dst_y_end = BSP_LCD_V_RES - x_start;
    }

    // The rotated buffer lives in PSRAM, so it cannot be handed to the SPI DMA
    // engine directly. Re-use the staged draw path which copies row-chunks
    // through the small internal-DMA `rotation_buffer`. Output dimensions
    // after rotation are width=src_h, height=src_w.
    return bsp_rotation_panel_draw_staged(ctx, dst_x_start, dst_y_start, dst_x_end, dst_y_end,
                                          ctx->rotated_buffer);
}

static esp_err_t bsp_rotation_panel_mirror(esp_lcd_panel_t *panel, bool x_axis, bool y_axis)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_mirror(ctx->target, x_axis, y_axis)
                              : ESP_ERR_INVALID_STATE;
}

static esp_err_t bsp_rotation_panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_swap_xy(ctx->target, swap_axes)
                              : ESP_ERR_INVALID_STATE;
}

static esp_err_t bsp_rotation_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_set_gap(ctx->target, x_gap, y_gap)
                              : ESP_ERR_INVALID_STATE;
}

static esp_err_t bsp_rotation_panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_invert_color(ctx->target, invert_color_data)
                              : ESP_ERR_INVALID_STATE;
}

static esp_err_t bsp_rotation_panel_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_disp_on_off(ctx->target, on_off)
                              : ESP_ERR_INVALID_STATE;
}

static esp_err_t bsp_rotation_panel_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    bsp_rotation_panel_t *ctx = bsp_rotation_panel_from_base(panel);
    return ctx && ctx->target ? esp_lcd_panel_disp_sleep(ctx->target, sleep)
                              : ESP_ERR_INVALID_STATE;
}

static esp_lcd_panel_handle_t bsp_rotation_panel_wrap(esp_lcd_panel_handle_t target)
{
    rotation_panel.base.reset = bsp_rotation_panel_reset;
    rotation_panel.base.init = bsp_rotation_panel_init;
    rotation_panel.base.del = bsp_rotation_panel_del;
    rotation_panel.base.draw_bitmap = bsp_rotation_panel_draw_bitmap;
    rotation_panel.base.mirror = bsp_rotation_panel_mirror;
    rotation_panel.base.swap_xy = bsp_rotation_panel_swap_xy;
    rotation_panel.base.set_gap = bsp_rotation_panel_set_gap;
    rotation_panel.base.invert_color = bsp_rotation_panel_invert_color;
    rotation_panel.base.disp_on_off = bsp_rotation_panel_disp_on_off;
    rotation_panel.base.disp_sleep = bsp_rotation_panel_disp_sleep;
    rotation_panel.base.user_data = &rotation_panel;
    rotation_panel.target = target;
    rotation_panel.rotation = BSP_DISPLAY_ROTATE_0;
    rotation_panel.te_sync_mode = false;
    rotation_panel.staging_slot = 0;
    return &rotation_panel.base;
}

#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

/**************************************************************************************************
 *
 * I2C Function
 *
 **************************************************************************************************/
esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized)
    {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

esp_err_t bsp_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};

    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

#if !CONFIG_FATFS_LONG_FILENAMES
    ESP_LOGW(TAG, "Warning: Long filenames on SD card are disabled in menuconfig!");
#endif

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
}

esp_err_t bsp_sdcard_unmount(void)
{
    return esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
}

/**************************************************************************************************
 *
 * I2S Audio Function
 *
 **************************************************************************************************/
esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    esp_err_t ret = ESP_FAIL;
    if (i2s_tx_chan && i2s_rx_chan) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    BSP_ERROR_CHECK_RETURN_ERR(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *p_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        p_i2s_cfg = i2s_config;
    }

    if (i2s_tx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_chan), err, TAG, "I2S enabling failed");
    }
    if (i2s_rx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_chan), err, TAG, "I2S enabling failed");
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK_GOTO(i2s_data_if, err);

    return ESP_OK;

err:
    if (i2s_tx_chan) {
        i2s_del_channel(i2s_tx_chan);
    }
    if (i2s_rx_chan) {
        i2s_del_channel(i2s_rx_chan);
    }

    return ret;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    BSP_NULL_CHECK(es8311_dev, NULL);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }
    assert(i2s_data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = BSP_ES7210_CODEC_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    BSP_NULL_CHECK(es7210_dev, NULL);

    esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es7210_dev_cfg);
}

#define LCD_CMD_BITS (8)
#define LCD_PARAM_BITS (8)
#define LCD_LEDC_CH (CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH)
#define LVGL_TICK_PERIOD_MS (CONFIG_BSP_DISPLAY_LVGL_TICK)
#define LVGL_MAX_SLEEP_MS (CONFIG_BSP_DISPLAY_LVGL_MAX_SLEEP)
#define BSP_LCD_QSPI_PCLK_HZ (80 * 1000 * 1000)
// NOTE: under TE_SYNC the adapter forces LV_DISPLAY_RENDER_MODE_FULL and
// allocates a full-frame draw buffer regardless of buffer_height (see
// display_manager_pick_render_mode / display_manager_default_buffer_pixels).
// These heights therefore only size max_transfer_sz and the partial-buffer
// fallback path when tear mode ends up as NONE.
#define LVGL_BUFFER_HEIGHT_PSRAM (24)
#define LVGL_BUFFER_HEIGHT_INTERNAL (12)

esp_err_t bsp_display_brightness_init(void)
{
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (panel_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel handle is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (brightness_percent < 0 || brightness_percent > 100)
    {
        ESP_LOGE(TAG, "Invalid brightness percentage. Should be between 0 and 100.");
        return ESP_ERR_INVALID_ARG;
    }

    // SPI panel IO is not thread-safe for the same device. Serialize command
    // writes with LVGL's whole flush, including every DMA staging chunk.
    // This mutex is recursive, so calls from an LVGL callback are safe too.
    esp_err_t ret = bsp_display_lock(250);
    if (ret != ESP_OK) return ret;

    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    uint8_t param = (uint8_t)(brightness_percent * 255 / 100);
    ret = esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &param, 1);
    if (ret == ESP_OK) brightness = param;
    bsp_display_unlock();
    return ret;
}

int bsp_display_brightness_get(void)
{
    if (panel_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel handle is not initialized");
        return -1;
    }

    return brightness * 100 / 255;
}

esp_err_t bsp_display_backlight_off(void)
{
    ESP_LOGI(TAG, "Backlight off");
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    ESP_LOGI(TAG, "Backlight on");
    return bsp_display_brightness_set(100);
}
#if LVGL_VERSION_MAJOR >= 9
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}
#else
static void bsp_lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}
#endif
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    assert(config != NULL && config->max_transfer_sz > 0);

    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK,
                                                                 BSP_LCD_DATA0,
                                                                 BSP_LCD_DATA1,
                                                                 BSP_LCD_DATA2,
                                                                 BSP_LCD_DATA3,
                                                                 config->max_transfer_sz);
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_config.pclk_hz = BSP_LCD_QSPI_PCLK_HZ;
    io_config.trans_queue_depth = CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH;
    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &io_handle));
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io_handle, &panel_config, &panel_handle));
    esp_lcd_panel_set_gap(panel_handle, 0x06, 0);
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    if (ret_panel)
    {
        *ret_panel = panel_handle;
    }
    if (ret_io)
    {
        *ret_io = io_handle;
    }
    return ret;
}

esp_err_t bsp_touch_new(const bsp_display_cfg_t *cfg, esp_lcd_touch_handle_t *ret_touch)
{
    assert(cfg != NULL);
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST, // Shared with LCD reset
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = cfg->touch_flags.swap_xy,
            .mirror_x = cfg->touch_flags.mirror_x,
            .mirror_y = cfg->touch_flags.mirror_y,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
    return esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, ret_touch);
}

/**************************************************************************************************
 *
 * IO Expander Function
 *
 **************************************************************************************************/
esp_io_expander_handle_t bsp_io_expander_init(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
    if (!io_expander)
    {
        BSP_ERROR_CHECK_RETURN_NULL(esp_io_expander_new_i2c_tca9554(i2c_handle, BSP_IO_EXPANDER_I2C_ADDRESS, &io_expander));
    }
    return io_expander;
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const bool psram_available = psram_total > 0;
    const esp_lv_adapter_tear_avoid_mode_t requested_tear_mode = cfg->tear_avoid_mode;
    esp_lv_adapter_tear_avoid_mode_t tear_mode = cfg->tear_avoid_mode;
    if (tear_mode != ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE &&
        tear_mode != ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC) {
        ESP_LOGW(TAG, "SPI/QSPI display mode %d is not supported by this panel path; using NONE",
                 tear_mode);
        tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    }

    const bool use_te_sync = (tear_mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC);
    const bool use_psram = psram_available;

    const uint32_t buffer_height = use_psram ? LVGL_BUFFER_HEIGHT_PSRAM : LVGL_BUFFER_HEIGHT_INTERNAL;
    const size_t max_transfer_sz = BSP_LCD_H_RES * buffer_height * BSP_LCD_BITS_PER_PIXEL / 8;
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = max_transfer_sz,
    };

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, &panel_handle, &io_handle));
    adapter_panel_handle = bsp_rotation_panel_wrap(panel_handle);
    rotation_panel.te_sync_mode = use_te_sync;

    ESP_LOGI(TAG, "LVGL display buffers: psram=%s lvgl_psram=%s height=%" PRIu32 " max_transfer=%u qspi=%uMHz queue_depth=%d tear_mode=%d requested_tear_mode=%d double_buffer=%s dma_stage_rows=%d",
             psram_available ? "yes" : "no",
             use_psram ? "yes" : "no",
             buffer_height,
             (unsigned int)max_transfer_sz,
             (unsigned int)(BSP_LCD_QSPI_PCLK_HZ / 1000000),
             CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH,
             tear_mode,
             requested_tear_mode,
             (use_psram && !use_te_sync) ? "yes" : "no",
             BSP_LCD_DMA_STAGING_ROWS);

    ESP_LOGD(TAG, "Add LCD screen");
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = adapter_panel_handle,
        .panel_io = io_handle,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = cfg->rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = buffer_height,
            .use_psram = use_psram,
            .enable_ppa_accel = false,
            .require_double_buffer = use_psram && !use_te_sync,
        },
        .tear_avoid_mode = tear_mode,
        .te_sync = use_te_sync
            ? (esp_lv_adapter_te_sync_config_t){
                .gpio_num = BSP_LCD_TE_GPIO,
                .time_tvdl_ms = 0,
                .time_tvdh_ms = 0,
                .bus_freq_hz = BSP_LCD_QSPI_PCLK_HZ,
                .data_lines = 4,
                .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
                .intr_type = GPIO_INTR_DISABLE,
                .refresh_window_percent = 0,
            }
            : ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };

    if (use_psram) {
        ESP_LOGI(TAG, "Using PSRAM LVGL draw buffers with internal SPI DMA staging");
    } else {
        ESP_LOGW(TAG, "PSRAM not available, using single internal LVGL buffer");
    }

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp)
    {
        return NULL;
    }

#if LVGL_VERSION_MAJOR >= 9
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#else
    lv_disp_t *disp_v8 = (lv_disp_t *)disp;
    if (disp_v8 && disp_v8->driver)
    {
        disp_v8->driver->rounder_cb = bsp_lvgl_rounder_cb;
    }
#endif

    return disp;
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp)
{
    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(cfg, &tp));
    assert(tp);

    const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);

    return esp_lv_adapter_register_touch(&touch_cfg);
}
/**********************************************************************************************************
 *
 * Display Function
 *
 **********************************************************************************************************/
lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1}};
    return bsp_display_start_with_config(&cfg);
}


lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(esp_lv_adapter_init(&cfg->lv_adapter_cfg));

    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);

    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(cfg, disp), NULL);

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation)
{
    if (panel_handle == NULL || io_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel or IO handle is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t madctl = 0x00;
    int x_gap = 0;
    int y_gap = 0;

    switch (rotation)
    {
    case BSP_DISPLAY_ROTATE_0:
        madctl = 0x00;
        // Panel active area starts at native column 6; no row offset.
        x_gap = 6; y_gap = 0;
        break;
    case BSP_DISPLAY_ROTATE_90:
        // 90 deg is handled by the BSP panel wrapper. Keep the real panel in
        // its native active window to avoid exposing guard columns.
        madctl = 0x00;
        x_gap = 6; y_gap = 0;
        break;
    case BSP_DISPLAY_ROTATE_180:
        madctl = 0xC0;
        // Hardware 180 deg is clean on this panel with the measured column gap.
        x_gap = 8; y_gap = 0;
        break;
    case BSP_DISPLAY_ROTATE_270:
        // 270 deg is handled by the BSP panel wrapper.
        madctl = 0x00;
        x_gap = 6; y_gap = 0;
        break;
    default:
        ESP_LOGE(TAG, "Invalid rotation value: %d", rotation);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = bsp_display_lock(250);
    if (ret != ESP_OK) return ret;

    // Re-apply the draw offset expected by the current panel orientation.
    esp_lcd_panel_set_gap(panel_handle, x_gap, y_gap);

    uint32_t lcd_cmd = 0x36;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;

    ESP_LOGI(TAG, "Set display rotation: %d (MADCTL=0x%02X gap=%d,%d)",
             rotation, madctl, x_gap, y_gap);

    ret = esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &madctl, 1);
    if (ret == ESP_OK) rotation_panel.rotation = rotation;
    bsp_display_unlock();
    return ret;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}
