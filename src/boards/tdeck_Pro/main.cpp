#include <Arduino.h>
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include <lvgl.h>

DisplayEink display;

// ---------------------------------------------------------------------------
// Fat-text helper (1-pixel offset double-draw = readable on GDEQ031T10)
// ---------------------------------------------------------------------------
static lv_obj_t* make_fat_label(lv_obj_t* parent, const char* txt,
                                lv_coord_t x, lv_coord_t y,
                                const lv_font_t* font = &lv_font_montserrat_16)
{
    // Base
    lv_obj_t* a = lv_label_create(parent);
    lv_label_set_text(a, txt);
    lv_obj_set_style_text_font(a, font, 0);
    lv_obj_set_style_text_color(a, lv_color_black(), 0);
    lv_obj_set_pos(a, x, y);

    // Offset copy → thicker stroke
    lv_obj_t* b = lv_label_create(parent);
    lv_label_set_text(b, txt);
    lv_obj_set_style_text_font(b, font, 0);
    lv_obj_set_style_text_color(b, lv_color_black(), 0);
    lv_obj_set_pos(b, x + 1, y);

    return a;
}

// ---------------------------------------------------------------------------
// Three dummy screens (exactly the content visible in the attached video)
// ---------------------------------------------------------------------------
static int g_screen = 0;   // 0 = Home, 1 = Messages, 2 = Settings

static void build_home(lv_obj_t* scr)
{
    // black header bar
    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, "Ratspeak");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    // status rows (fat)
    make_fat_label(scr, "Battery          +1",  12, 40);
    make_fat_label(scr, "LoRa",                 12, 62);
    make_fat_label(scr, "Root",                 12, 84);
    make_fat_label(scr, "SNR",                  12, 106);
    make_fat_label(scr, "Paths",                12, 128);
    make_fat_label(scr, "Links",                12, 150);
    make_fat_label(scr, "LXMF Q",               12, 172);

    make_fat_label(scr, "LOCAL_DEST  Home | [ ] next", 12, 204);
}

static void build_messages(lv_obj_t* scr)
{
    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, "Messages");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    make_fat_label(scr, "Alice          hi from", 12, 40);
    make_fat_label(scr, "Bob            meet",    12, 62);
    make_fat_label(scr, "Node-7         anna",    12, 84);
    make_fat_label(scr, "(dummy list)",           12, 110);

    make_fat_label(scr, "2/3 Msgs | [ ] next",    12, 204);
}

static void build_settings(lv_obj_t* scr)
{
    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 28);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    make_fat_label(scr, "Radio",      12, 40);
    make_fat_label(scr, "Freq",       12, 62);
    make_fat_label(scr, "TX power",   12, 84);
    make_fat_label(scr, "WiFi",       12, 106);
    make_fat_label(scr, "Display",    12, 128);
    make_fat_label(scr, "(dummy settings)", 12, 154);

    make_fat_label(scr, "3/3 Setup | [ ] next", 12, 204);
}

static void show_screen(int id)
{
    g_screen = id % 3;
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    switch (g_screen) {
        case 0: build_home(scr);     break;
        case 1: build_messages(scr); break;
        case 2: build_settings(scr); break;
    }
    LvglPort::updateAndRefresh();   // single full panel update
}

// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println("RATSPEAK · T-Deck Pro  STANDALONE e-ink");
    Serial.println("Phase D multi-screen (fat text)");
    Serial.println("========================================");

    if (!display.begin()) {
        Serial.println("[FATAL] DisplayEink begin failed");
        while (1) delay(1000);
    }
    if (!LvglPort::begin(display)) {
        Serial.println("[FATAL] LvglPort begin failed");
        while (1) delay(1000);
    }

    show_screen(0);
    Serial.println("[BOOT] UI ready – press any key or wait for auto-cycle");
}

void loop()
{
    LvglPort::tick();

    // Simple auto-cycle for bring-up demo (replace with real key later)
    static uint32_t last = 0;
    if (millis() - last > 4500) {
        last = millis();
        show_screen(g_screen + 1);
    }

    // Optional: if you already have TCA8418 / keyboard reading,
    // call show_screen(g_screen + 1) on the “next” / Enter key here.
    delay(10);
}