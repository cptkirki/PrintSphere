"""Apply the pinned LVGL safety patches, fail closed on source drift."""
from pathlib import Path
import argparse

helperFind = """\
    return row_bytes;
}

/**
 * @brief Default flush (single buffer, no tear protection)
 */"""

helperReplace = """\
    return row_bytes;
}

static bool display_bridge_v9_wait_tx_done(esp_lv_adapter_display_bridge_v9_t *impl,
                                           const char *context)
{
    const TickType_t timeout = pdMS_TO_TICKS(200);
    if (ulTaskNotifyTake(pdTRUE, timeout) != 0) {
        return true;
    }

    ESP_LOGW(TAG, "PrintSphere local patch: %s TX-done notify timed out (%ums) -- releasing LVGL flush",
             context ? context : "display", (unsigned)pdTICKS_TO_MS(timeout));
    if (impl && impl->cfg.te_ctx) {
        esp_lv_adapter_te_sync_record_tx_done(impl->cfg.te_ctx);
    }
    return false;
}

/**
 * @brief Default flush (single buffer, no tear protection)
 */"""

gpioFind = """\
    /* Wait for transmission to complete */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (impl->cfg.te_ctx) {
        esp_lv_adapter_te_sync_record_tx_done(impl->cfg.te_ctx);
    }

    display_manager_flush_ready(disp);"""

gpioReplace = """\
    /* Wait for transmission to complete, but do not deadlock the LVGL worker if
     * the panel IO completion notify is lost under TE/DMA load. */
    const bool tx_done = display_bridge_v9_wait_tx_done(impl, "GPIO TE flush");
    if (tx_done && impl->cfg.te_ctx) {
        esp_lv_adapter_te_sync_record_tx_done(impl->cfg.te_ctx);
    }

    display_manager_flush_ready(disp);"""

# The adapter's wakeups use index 0. SPI completion must have its own channel,
# independent of LVGL draw synchronization and invalidation wakeups.
notifyFind = """\
        TaskHandle_t notify_task = display_bridge_v9_get_notify_task(impl);
        if (notify_task) {
            vTaskNotifyGiveFromISR(notify_task, &need_yield);
        }
        return (need_yield == pdTRUE);
    }
    bool vsync = display_bridge_v9_handle_vsync(impl);"""
notifyReplace = notifyFind.replace(
    "vTaskNotifyGiveFromISR(notify_task, &need_yield)",
    "vTaskNotifyGiveIndexedFromISR(notify_task, 1, &need_yield)")

clearFind = """\
    /* Clear stale completion notifications before kicking off a new transfer. */
    ulTaskNotifyValueClear(NULL, ULONG_MAX);"""
clearReplace = clearFind.replace("ulTaskNotifyValueClear(NULL, ULONG_MAX)",
                                "ulTaskNotifyValueClearIndexed(NULL, 1, ULONG_MAX)")

traceDeclaration = """\
/* Optional application diagnostics: never logs or acquires the LVGL mutex. */
extern void printsphere_display_trace(unsigned phase) __attribute__((weak));
#define PRINTSPHERE_DISPLAY_TRACE(phase) do { \\
    if (printsphere_display_trace) printsphere_display_trace(phase); \\
} while (0)
"""

def replace_once(text, before, after):
    if after in text:
        return text
    if text.count(before) != 1:
        raise RuntimeError("LVGL adapter source mismatch: " + before[:90])
    return text.replace(before, after, 1)

def apply(root):
    path = Path(root) / "src/display/bridge/v9/lvgl_bridge_v9.c"
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    patches = [
        ("PrintSphere local patch: %s TX-done notify timed out", helperFind, helperReplace),
        ('display_bridge_v9_wait_tx_done(impl, "GPIO TE flush")', gpioFind, gpioReplace),
    ]
    original = text
    for marker, before, after in patches:
        if marker in text:
            continue
        if text.count(before) != 1:
            raise RuntimeError(f"LVGL adapter source mismatch: {marker}")
        text = text.replace(before, after, 1)
    text = replace_once(text, notifyFind, notifyReplace)
    text = replace_once(text, clearFind, clearReplace)
    text = replace_once(text, "if (ulTaskNotifyTake(pdTRUE, timeout) != 0)",
                        "if (ulTaskNotifyTakeIndexed(1, pdTRUE, timeout) != 0)")
    text = replace_once(text, '#include "display_te_sync.h"',
                        '#include "display_te_sync.h"\n' + traceDeclaration)
    # Instrument only GPIO-TE flush, using stable boundaries instead of matching
    # unrelated RGB/MIPI flushes. Phases: prepare, TE, submit, completion, idle.
    start = text.index("static void display_bridge_v9_flush_gpio_te(", text.index("static bool display_bridge_v9_wait_tx_done("))
    end = text.index("\n/**", start)
    segment = text[start:end]
    for before, after in [
        ("    compact_stride_to_packed(", "    PRINTSPHERE_DISPLAY_TRACE(1);\n    compact_stride_to_packed("),
        ("    /* Wait for TE signal", "    PRINTSPHERE_DISPLAY_TRACE(2);\n    /* Wait for TE signal"),
        ("    esp_err_t ret = esp_lcd_panel_draw_bitmap(", "    PRINTSPHERE_DISPLAY_TRACE(3);\n    esp_err_t ret = esp_lcd_panel_draw_bitmap("),
        ("    const bool tx_done =", "    PRINTSPHERE_DISPLAY_TRACE(4);\n    const bool tx_done ="),
        ("    display_manager_flush_ready(disp);", "    PRINTSPHERE_DISPLAY_TRACE(0);\n    display_manager_flush_ready(disp);"),
    ]:
        if after not in segment:
            if before not in segment:
                raise RuntimeError("LVGL GPIO-TE diagnostics source mismatch")
            segment = segment.replace(before, after)
    text = text[:start] + segment + text[end:]
    if text != original:
        path.write_text(text, encoding="utf-8", newline="\n")
    print("LVGL adapter safety patch verified")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default="managed_components/espressif__esp_lvgl_adapter")
    apply(parser.parse_args().root)
