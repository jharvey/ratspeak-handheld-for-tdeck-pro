// .cpp
#include "LvContactsScreen.h"
void LvContactsScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "Contacts");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    static const char* names[] = {
        "Alice   ca53...b4cc  *",
        "Bob     659b...cdc3  *",
        "Charlie a7c0...86d5",
        "Dana    4f89...64a2",
        "Eve     2fe6...3d7a"
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t* l = lv_label_create(parent);
        lv_label_set_text(l, names[i]);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 12, 40 + i * 28);
    }
}