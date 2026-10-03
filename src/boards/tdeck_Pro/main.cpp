#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"

static DisplayEink g_display;

// High-contrast mono home layout (dummy data)
static void build_home_ui() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // ---- Header strip (thin black bar, white title) ----
    lv_obj_t* header = lv_obj_create(scr);
    lv_obj_set_size(header, EPD_WIDTH, 28);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Ratspeak");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t* mode = lv_label_create(header);
    lv_label_set_text(mode, "Pro");
    lv_obj_set_style_text_color(mode, lv_color_white(), 0);
    lv_obj_set_style_text_font(mode, &lv_font_montserrat_16, 0);
    lv_obj_align(mode, LV_ALIGN_RIGHT_MID, -8, 0);

    // ---- Body: black text on white ----
    const lv_font_t* font = &lv_font_montserrat_16;
    int y = 40;

    auto add_row = [&](const char* left, const char* right) {
        lv_obj_t* l = lv_label_create(scr);
        lv_label_set_text(l, left);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 12, y);

        lv_obj_t* r = lv_label_create(scr);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_color(r, lv_color_black(), 0);
        lv_obj_set_style_text_font(r, font, 0);
        lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -12, y);
        y += 22;
    };

    add_row("Battery", "4.14 V  69%");
    add_row("LoRa",    "online");
    add_row("RSSI",    "-86 dBm");
    add_row("SNR",     "6.2 dB");
    y += 6;
    add_row("Paths",   "1");
    add_row("Links",   "0");
    add_row("LXMF Q",  "0");

    y += 10;
    lv_obj_t* dest_lbl = lv_label_create(scr);
    lv_label_set_text(dest_lbl, "LOCAL_DEST");
    lv_obj_set_style_text_color(dest_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(dest_lbl, font, 0);
    lv_obj_align(dest_lbl, LV_ALIGN_TOP_LEFT, 12, y);
    y += 20;

    lv_obj_t* dest_val = lv_label_create(scr);
    lv_label_set_text(dest_val, "ca53afea5f8f1fbc");
    lv_obj_set_style_text_color(dest_val, lv_color_black(), 0);
    lv_obj_set_style_text_font(dest_val, font, 0);
    lv_obj_align(dest_val, LV_ALIGN_TOP_LEFT, 12, y);

    // ---- Footer ----
    lv_obj_t* foot = lv_label_create(scr);
    lv_label_set_text(foot, "Phase C  |  dummy home");
    lv_obj_set_style_text_color(foot, lv_color_black(), 0);
    lv_obj_set_style_text_font(foot, font, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase C");
    Serial.println("========================================");

    if (!g_display.begin()) {
        Serial.println("[BOOT] Display init FAILED");
        return;
    }
    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL init FAILED");
        return;
    }

    build_home_ui();

    // Paint into FB (many flush strips, no panel I/O yet)
    lv_refr_now(nullptr);
    // One full refresh to the e-ink
    g_display.refreshIfDirty();

    Serial.println("[BOOT] Home UI on panel (single refresh)");
}

void loop() {
    // Idle: no continuous refresh (e-ink should stay static)
    delay(500);
}