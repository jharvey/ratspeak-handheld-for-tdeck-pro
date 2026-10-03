// .cpp
#include "LvNodesScreen.h"
void LvNodesScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "Nodes / Paths");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    static const char* nodes[] = {
        "ca53afea...  RSSI -14  2m",
        "659bd853...  RSSI -86  12s",
        "a7c0701e...  RSSI -72  45s",
        "4f893685...  (stale)"
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t* l = lv_label_create(parent);
        lv_label_set_text(l, nodes[i]);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 12, 40 + i * 28);
    }
}