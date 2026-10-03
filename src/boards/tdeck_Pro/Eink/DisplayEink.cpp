#include "LvglPort.h"
#include <Arduino.h>
#include "config/BoardConfig.h"

DisplayEink* LvglPort::disp_ = nullptr;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[EPD_WIDTH * 20];   // small 1-bit buffer

bool LvglPort::begin(DisplayEink& display) {
    disp_ = &display;
    if (!disp_->begin()) return false;

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, EPD_WIDTH * 20);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EPD_WIDTH;
    disp_drv.ver_res = EPD_HEIGHT;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.antialiasing = 0;
    lv_disp_drv_register(&disp_drv);

    // Touch stub (wire CST328 later)
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    Serial.println("[LVGL] port ready");
    return true;
}

void LvglPort::flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    if (disp_) disp_->flush(area, color_map);
    lv_disp_flush_ready(drv);
}

void LvglPort::touch_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    data->state = LV_INDEV_STATE_RELEASED;
}

void LvglPort::tick() {
    lv_tick_inc(5);
    lv_timer_handler();
}

DisplayEink* LvglPort::display() {
    return disp_;
}