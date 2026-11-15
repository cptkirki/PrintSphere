# Production UI regression against LVGL

Run `python tools/test_lvgl_host.py` after resolving the project's pinned managed components. A native GCC/G++ toolchain, CMake and Ninja are required; the script accepts `--cxx`, `--cmake` and `--ninja`. Windows can use the portable toolchain in ignored `build-host/w64devkit`.

This builds real LVGL and the production UI/fonts for both layout macros. A mocked framebuffer replaces the panel; tests explicitly drive LVGL ticks/events on one owner thread while producer threads publish application data. They assert no widget mutation before owner consumption, coalescing during a scroll, no redraw for an unchanged status, stable card objects, correct raw-image handoff/lifetime, concurrent publishing and diagnostic summaries.

The tiny fake PNG codec is deliberate: it tests the UI's decoded-buffer handoff without linking an unrelated host PNG library. It is not a codec correctness test. The host configuration uses a single synchronous software draw unit and no display/touch hardware; physical FreeRTOS drawing, TE, DMA, color output and frame-rate validation remain separate.
