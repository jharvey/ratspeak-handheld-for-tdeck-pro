#include "LvglPort.h"
#include <lvgl.h>
#include <esp_heap_caps.h>

DisplayEink* LvglPort::display_ = nullptr;
lv_disp_draw_buf_t LvglPort::draw_buf_;
lv_color_t* LvglPort::buf1_ = nullptr;
lv_disp_drv_t LvglPort::disp_drv_;

// Static fallback: reserved at link time so runtime heap fragmentation
// after the 200KB protocol node cannot starve the draw buffer.
// 10 lines * 240 * 2 = 4800 bytes — enough for fat-text shell.
static lv_color_t s_static_draw[EPD_WIDTH * 10];

bool LvglPort::begin(DisplayEink& display) {
    display_ = &display;

    lv_init();

    // Prefer a small internal buffer. After proto node (~200KB) free_int is
    // ~35KB and often fragmented; 20-line (9600B) malloc can fail.
    const size_t try_lines[] = { 20, 10, 5 };
    size_t lines = 0;
    size_t pixels = 0;
    buf1_ = nullptr;

    for (size_t i = 0; i < sizeof(try_lines) / sizeof(try_lines[0]); i++) {
        lines = try_lines[i];
        pixels = (size_t)EPD_WIDTH * lines;
        const size_t bytes = pixels * sizeof(lv_color_t);

        buf1_ = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf1_) buf1_ = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
        if (!buf1_) buf1_ = (lv_color_t*)malloc(bytes);
        if (buf1_) break;
        Serial.printf("[LVGL] draw buf %u lines (%u B) alloc failed\n",
                      (unsigned)lines, (unsigned)bytes);
    }

    if (!buf1_) {
        // Last resort: static BSS (always present, no heap)
        lines = 10;
        pixels = (size_t)EPD_WIDTH * lines;
        buf1_ = s_static_draw;
        Serial.println("[LVGL] using static draw buffer");
    }

    Serial.printf("[LVGL] draw buf %u bytes (lines=%u) free_int=%u\n",
                  (unsigned)(pixels * sizeof(lv_color_t)), (unsigned)lines,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    lv_disp_draw_buf_init(&draw_buf_, buf1_, nullptr, pixels);

    lv_disp_drv_init(&disp_drv_);
    disp_drv_.hor_res = EPD_WIDTH;
    disp_drv_.ver_res = EPD_HEIGHT;
    disp_drv_.flush_cb = flush_cb;
    disp_drv_.draw_buf = &draw_buf_;
    lv_disp_drv_register(&disp_drv_);

    Serial.println("[LVGL] port ready (batched refresh)");
    return true;
}

void LvglPort::flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    if (display_) display_->flush(area, color_map);
    lv_disp_flush_ready(drv);
}

void LvglPort::tick() {
    lv_timer_handler();
}

void LvglPort::updateAndRefresh() {
    lv_timer_handler();
    if (display_) display_->refreshIfDirty();
}