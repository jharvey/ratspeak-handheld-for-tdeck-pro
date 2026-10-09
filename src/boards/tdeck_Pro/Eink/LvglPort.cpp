#include "LvglPort.h"
#include <lvgl.h>
#include <esp_heap_caps.h>

DisplayEink* LvglPort::display_ = nullptr;
lv_disp_draw_buf_t LvglPort::draw_buf_;
lv_color_t* LvglPort::buf1_ = nullptr;
lv_disp_drv_t LvglPort::disp_drv_;

static lv_color_t* allocDrawBuf(size_t pixels, size_t* outBytes) {
    const size_t bytes = pixels * sizeof(lv_color_t);
    *outBytes = bytes;

    // PSRAM reports found=1 size=0 on this board — do not use ps_malloc.
    lv_color_t* p = nullptr;
    if (psramFound() && ESP.getPsramSize() > 0) {
        p = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            Serial.printf("[LVGL] draw buf SPIRAM %u bytes\n", (unsigned)bytes);
            return p;
        }
    }

    p = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) {
        Serial.printf("[LVGL] draw buf INTERNAL %u bytes\n", (unsigned)bytes);
        return p;
    }

    // Last resort: plain malloc (may still fail if fragmented)
    p = (lv_color_t*)malloc(bytes);
    if (p) Serial.printf("[LVGL] draw buf malloc %u bytes\n", (unsigned)bytes);
    return p;
}

bool LvglPort::begin(DisplayEink& display) {
    display_ = &display;

    lv_init();

    // Prefer 20 lines (~9600 B). On fragmented internal heap after the 200 KB
    // protocol node, fall back to 10 then 5 so bring-up still works.
    static const size_t kLineTries[] = {20, 10, 5};
    size_t lines = 0;
    size_t bytes = 0;
    buf1_ = nullptr;

    for (size_t t = 0; t < sizeof(kLineTries) / sizeof(kLineTries[0]); t++) {
        lines = kLineTries[t];
        const size_t pixels = (size_t)EPD_WIDTH * lines;
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        Serial.printf("[LVGL] try %u lines need=%u largest_int=%u free_int=%u\n",
                      (unsigned)lines,
                      (unsigned)(pixels * sizeof(lv_color_t)),
                      (unsigned)largest,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        buf1_ = allocDrawBuf(pixels, &bytes);
        if (buf1_) break;
    }

    if (!buf1_) {
        Serial.printf("[LVGL] draw buffer alloc failed free_int=%u largest=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }

    const size_t pixels = (size_t)EPD_WIDTH * lines;
    Serial.printf("[LVGL] draw buf %u lines (%u bytes) free_int=%u\n",
                  (unsigned)lines, (unsigned)bytes,
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