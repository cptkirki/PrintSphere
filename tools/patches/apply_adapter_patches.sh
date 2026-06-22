#!/usr/bin/env bash
set -euo pipefail

adapter_root="${1:-managed_components/espressif__esp_lvgl_adapter}"

if [[ ! -d "$adapter_root" ]]; then
  echo "[adapter-patch] adapter not present at $adapter_root -- skipping (run idf.py reconfigure first)."
  exit 0
fi

python3 - "$adapter_root" <<'PY'
from pathlib import Path
import sys

adapter = Path(sys.argv[1])

def patch_file(path: Path, marker: str, find: str, replace: str, label: str) -> None:
    if not path.is_file():
        raise SystemExit(f"[adapter-patch] missing file: {path} -- adapter version mismatch?")
    text = path.read_text(encoding="utf-8")
    text_lf = text.replace("\r\n", "\n")
    find_lf = find.replace("\r\n", "\n")
    replace_lf = replace.replace("\r\n", "\n")
    if marker in text_lf:
        print(f"[adapter-patch] {label} : already applied.")
        return
    if find_lf not in text_lf:
        raise SystemExit(
            f"[adapter-patch] {label} : upstream source does not match expected pattern. "
            "Adapter version may have changed; review the patch script."
        )
    path.write_text(text_lf.replace(find_lf, replace_lf), encoding="utf-8")
    print(f"[adapter-patch] {label} : applied.")

patch_file(
    adapter / "src/display/display_te_sync.c",
    "PrintSphere local patch: bounded TE-vsync wait",
    """    while (true) {
        if (xSemaphoreTake(ctx->te_vsync_sem, portMAX_DELAY) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
""",
    """    /* PrintSphere local patch: bounded TE-vsync wait.
     * Upstream uses portMAX_DELAY which deadlocks the LVGL worker if the
     * TE signal is missed (panel quirk, brightness=0, ESD, scheduling jitter).
     * Bound the wait to 100 ms (~6 frames @ 60 Hz) so the flush path can
     * recover by signalling flush_ready and returning the LVGL lock. */
    const TickType_t kTeWaitTicks = pdMS_TO_TICKS(100);
    while (true) {
        if (xSemaphoreTake(ctx->te_vsync_sem, kTeWaitTicks) != pdTRUE) {
            ESP_LOGW(TAG, "TE vsync wait timed out (%ums) -- letting flush proceed without TE sync",
                     (unsigned)pdTICKS_TO_MS(kTeWaitTicks));
            portENTER_CRITICAL(&ctx->lock);
            ctx->frame_request_ticks = 0;
            ctx->window_defer_count = 0;
            portEXIT_CRITICAL(&ctx->lock);
            return ESP_ERR_TIMEOUT;
        }
""",
    "display_te_sync.c (bounded vsync wait)",
)

patch_file(
    adapter / "src/display/bridge/v9/lvgl_bridge_v9.c",
    "PrintSphere local patch: bound the wait to 200 ms",
    """    /* Wait for transmission to complete */
    ulTaskNotifyValueClear(NULL, ULONG_MAX);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    display_manager_flush_ready(disp);
}

/**
 * @brief Double buffering with full-screen refresh
 */
""",
    """    /* Wait for transmission to complete.
     * PrintSphere local patch: bound the wait to 200 ms instead of
     * portMAX_DELAY. If the panel/SPI driver fails to notify (TE quirk,
     * DMA stall under load), the LVGL worker would otherwise block
     * indefinitely and starve every esp_lv_adapter_lock() caller. */
    ulTaskNotifyValueClear(NULL, ULONG_MAX);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)) == 0) {
        ESP_LOGW(TAG, "TE flush TX-done notify timed out (200ms) -- releasing LVGL lock");
        if (impl->cfg.te_ctx) {
            esp_lv_adapter_te_sync_record_tx_done(impl->cfg.te_ctx);
        }
    }

    display_manager_flush_ready(disp);
}

/**
 * @brief Double buffering with full-screen refresh
 */
""",
    "lvgl_bridge_v9.c (bounded TX-done wait)",
)

patch_file(
    adapter / "src/adapter/esp_lv_adapter.c",
    "PrintSphere local patch: upstream gates esp_timer_stop_blocking",
    """#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ret = esp_timer_stop_blocking(timer, portMAX_DELAY);
    if (ret == ESP_ERR_NOT_FINISHED) {
        ret = ESP_OK;
    }
#else
    ret = esp_timer_stop(timer);
#endif
""",
    """    /* PrintSphere local patch: upstream gates esp_timer_stop_blocking() on
     * ESP_IDF_VERSION >= 6.0.0, but the symbol is not exported in IDF v6.0.1.
     * Tracked upstream as espressif/esp-iot-solution#704 (fix in PR #706).
     * Use the non-blocking variant unconditionally; the timer is one-shot/
     * periodic LVGL tick, so the (rare) race with an in-flight callback is
     * harmless. */
    ret = esp_timer_stop(timer);
""",
    "esp_lv_adapter.c (esp_timer_stop_blocking shim)",
)

print("[adapter-patch] done.")
PY
