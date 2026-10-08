#include "LvglPort.h"
#include <lvgl.h>
#include <esp_heap_caps.h>

DisplayEink* LvglPort::display_ = nullptr;
lv_disp_draw_buf_t LvglPort::draw_buf_;
lv_color_t* LvglPort::buf1_ = nullptr;
lv_disp_drv_t LvglPort::disp_drv_;

bool LvglPort::begin(DisplayEink& display) {
    display_ = &display;

    lv_init();

    // 20 lines: 240 * 20 * sizeof(lv_color_t) ≈ 9600 bytes
    // (was 40 → ~19 KB; tight after 200 KB protocol node)
    const size_t lines = 20;
    const size_t pixels = (size_t)EPD_WIDTH * lines;
    buf1_ = (lv_color_t*)ps_malloc(pixels * sizeof(lv_color_t));
    if (!buf1_) buf1_ = (lv_color_t*)malloc(pixels * sizeof(lv_color_t));
    if (!buf1_) {
        Serial.printf("[LVGL] draw buffer alloc failed free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    Serial.printf("[LVGL] draw buf %u lines (%u bytes) free_int=%u\n",
                  (unsigned)lines,
                  (unsigned)(pixels * sizeof(lv_color_t)),
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