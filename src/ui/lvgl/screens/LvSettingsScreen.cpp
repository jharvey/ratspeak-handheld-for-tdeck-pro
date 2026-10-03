// .cpp
#include "LvSettingsScreen.h"
void LvSettingsScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    static const char* items[] = {
        "Radio Preset   Long Fast",
        "Frequency      915.0 MHz",
        "Announce       30 min",
        "Identity QR",
        "Clear Data",
        "About  V0.2-NodeUI"
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t* l = lv_label_create(parent);
        lv_label_set_text(l, items[i]);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 12, 40 + i * 26);
    }
}