#include "LvHomeScreen.h"
#include "../theme/EinkTheme.h"

void LvHomeScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);

    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "Ratspeak  Node");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* name = lv_label_create(parent);
    lv_label_set_text(name, "Name: FieldNode-A7");
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 12, 40);

    lv_obj_t* dest = lv_label_create(parent);
    lv_label_set_text(dest, "ca53afea5f8f1fbc...");
    lv_obj_set_style_text_font(dest, &lv_font_montserrat_12, 0);
    lv_obj_align(dest, LV_ALIGN_TOP_LEFT, 12, 62);

    lv_obj_t* bat = lv_label_create(parent);
    lv_label_set_text(bat, "Battery  4.14 V   69%");
    lv_obj_align(bat, LV_ALIGN_TOP_LEFT, 12, 90);

    lv_obj_t* lora = lv_label_create(parent);
    lv_label_set_text(lora, "LoRa  online   RSSI -14  SNR 6.5");
    lv_obj_align(lora, LV_ALIGN_TOP_LEFT, 12, 112);

    lv_obj_t* paths = lv_label_create(parent);
    lv_label_set_text(paths, "Paths 1   Links 0   Q 0");
    lv_obj_align(paths, LV_ALIGN_TOP_LEFT, 12, 134);

    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Announce");
    lv_obj_center(lbl);
}