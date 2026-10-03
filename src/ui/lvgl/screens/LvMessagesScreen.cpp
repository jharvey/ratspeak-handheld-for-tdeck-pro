#include "LvMessagesScreen.h"

void LvMessagesScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);

    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "Messages");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    static const char* rows[] = {
        "Alice   Hi from the ridge",
        "Bob     Meeting at 14:00?",
        "Charlie Packet test OK",
        "Dana    Weather looks clear",
        "Eve     New path discovered"
    };

    for (int i = 0; i < 5; i++) {
        lv_obj_t* item = lv_label_create(parent);
        lv_label_set_text(item, rows[i]);
        lv_obj_set_style_text_font(item, &lv_font_montserrat_14, 0);
        lv_obj_align(item, LV_ALIGN_TOP_LEFT, 12, 40 + i * 28);
    }
}