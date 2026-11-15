#pragma once
// Private implementation included by the BSP and its DMA lifetime regression.
// The including translation unit supplies the BSP context and panel IO APIs.

extern void printsphere_display_trace(unsigned phase) __attribute__((weak));
static inline void bsp_dma_trace(unsigned phase) {
    if (printsphere_display_trace) printsphere_display_trace(phase);
}

static bool bsp_rotation_panel_ensure_buffer(bsp_rotation_panel_t *ctx, size_t pixels)
{
    if (ctx->rotation_buffer_pixels >= pixels && ctx->rotation_buffer != NULL)
    {
        return true;
    }

    free(ctx->rotation_buffer);
    ctx->rotation_buffer = heap_caps_malloc(pixels * sizeof(uint16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ctx->rotation_buffer_pixels = ctx->rotation_buffer ? pixels : 0;
    return ctx->rotation_buffer != NULL;
}

static esp_err_t bsp_rotation_panel_draw_staged(bsp_rotation_panel_t *ctx, int x_start, int y_start,
                                                int x_end, int y_end, const uint16_t *src)
{
    const int width = x_end - x_start;
    const int height = y_end - y_start;
    if (width <= 0 || height <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // IDF's no-command/no-parameter transaction drains queued SPI color DMA.
    // It does not send a panel command. Drain before reallocating or changing
    // ping-pong geometry, even if an earlier adapter notification was premature.
    bsp_dma_trace(5);
    esp_err_t drain_err = esp_lcd_panel_io_tx_param(io_handle, -1, NULL, 0);
    if (drain_err != ESP_OK) return drain_err;

    const size_t pixels = (size_t)width * height;
    // Use the chunked DMA staging path whenever:
    //   * TE-sync mode is active (need ping-pong slots to overlap CPU/DMA), OR
    //   * The source bitmap lives in PSRAM (cannot be DMA'd directly, and a
    //     full-frame rotated buffer is far too large for internal heap).
    const bool src_in_psram = src != NULL && esp_ptr_external_ram(src);
    const bool use_chunked =
        (ctx->te_sync_mode || src_in_psram) && height > BSP_LCD_DMA_STAGING_ROWS;
    if (use_chunked)
    {
        const int max_rows = BSP_LCD_DMA_STAGING_ROWS;
        const size_t max_pixels = (size_t)width * max_rows;
        if (!bsp_rotation_panel_ensure_buffer(ctx, max_pixels * 2))
        {
            ESP_LOGE(TAG, "Unable to allocate %" PRIu32 " px DMA staging buffer",
                     (uint32_t)(max_pixels * 2));
            return ESP_ERR_NO_MEM;
        }

        for (int y = 0; y < height;)
        {
            const int rows = (height - y) > max_rows ? max_rows : (height - y);
            const size_t chunk_pixels = (size_t)width * rows;
            uint16_t *staging = ctx->rotation_buffer + ((size_t)ctx->staging_slot * max_pixels);
            ctx->staging_slot ^= 1;
            memcpy(staging, src + ((size_t)y * width),
                   chunk_pixels * sizeof(uint16_t));
            bsp_dma_trace(6);
            esp_err_t err = esp_lcd_panel_draw_bitmap(ctx->target,
                                                      x_start,
                                                      y_start + y,
                                                      x_end,
                                                      y_start + y + rows,
                                                      staging);
            if (err != ESP_OK)
            {
                return err;
            }
            y += rows;
        }

        // Every staged draw emits a callback. The adapter's first callback is
        // not proof that the final chunk has completed. Keep all staging memory
        // alive until the SPI driver has returned the final queued transaction.
        bsp_dma_trace(5);
        return esp_lcd_panel_io_tx_param(io_handle, -1, NULL, 0);
    }

    if (!bsp_rotation_panel_ensure_buffer(ctx, pixels))
    {
        ESP_LOGE(TAG, "Unable to allocate %" PRIu32 " px DMA staging buffer",
                 (uint32_t)pixels);
        return ESP_ERR_NO_MEM;
    }

    memcpy(ctx->rotation_buffer, src, pixels * sizeof(uint16_t));
    bsp_dma_trace(6);
    esp_err_t err = esp_lcd_panel_draw_bitmap(ctx->target, x_start, y_start, x_end, y_end,
                                              ctx->rotation_buffer);
    if (err != ESP_OK) return err;
    bsp_dma_trace(5);
    return esp_lcd_panel_io_tx_param(io_handle, -1, NULL, 0);
}
