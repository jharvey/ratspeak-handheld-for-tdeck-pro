#pragma once

#include <lvgl.h>
#include "DisplayEink.h"

class LvglPort {
public:
    static bool begin(DisplayEink& display);
    static void tick();
    static DisplayEink* display();

private:
    static DisplayEink* disp_;
    static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map);
    static void touch_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);
};