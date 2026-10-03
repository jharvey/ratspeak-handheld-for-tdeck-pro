// .cpp
#include "LvMessageView.h"
void LvMessageView::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "Alice");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    // simple bubbles
    lv_obj_t* in1 = lv_label_create(parent);
    lv_label_set_text(in1, "Hi from the ridge");
    lv_obj_set_style_bg_color(in1, lv_color_black(), 0);
    lv_obj_set_style_text_color(in1, lv_color_white(), 0);
    lv_obj_set_style_pad_all(in1, 6, 0);
    lv_obj_align(in1, LV_ALIGN_TOP_LEFT, 8, 36);

    lv_obj_t* out1 = lv_label_create(parent);
    lv_label_set_text(out1, "Copy, heading up");
    lv_obj_set_style_bg_color(out1, lv_color_white(), 0);
    lv_obj_set_style_border_width(out1, 1, 0);
    lv_obj_set_style_pad_all(out1, 6, 0);
    lv_obj_align(out1, LV_ALIGN_TOP_RIGHT, -8, 70);
}