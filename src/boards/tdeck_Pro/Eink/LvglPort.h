#pragma once

#include "DisplayEink.h"

class LvglPort {
public:
    static bool begin(DisplayEink& display);
    static void tick();
    static DisplayEink* display() { return display_; }

private:
    static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map);

    static DisplayEink* display_;
    static lv_disp_draw_buf_t draw_buf_;
    static lv_color_t* buf1_;
    static lv_disp_drv_t disp_drv_;
};