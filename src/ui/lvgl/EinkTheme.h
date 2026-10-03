#pragma once
#include <lvgl.h>

namespace EinkTheme {
    inline void apply() {
        lv_theme_t* th = lv_theme_default_init(NULL,
            lv_palette_main(LV_PALETTE_GREY),
            lv_palette_main(LV_PALETTE_GREY),
            true,   // dark = false for e-ink (we invert conceptually)
            LV_FONT_DEFAULT);
        lv_disp_set_theme(NULL, th);

        // Force high-contrast black/white
        static lv_style_t style_scr;
        lv_style_init(&style_scr);
        lv_style_set_bg_color(&style_scr, lv_color_white());
        lv_style_set_bg_opa(&style_scr, LV_OPA_COVER);
        lv_obj_add_style(lv_scr_act(), &style_scr, 0);
    }

    inline lv_color_t black() { return lv_color_black(); }
    inline lv_color_t white() { return lv_color_white(); }
}