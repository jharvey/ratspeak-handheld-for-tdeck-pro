#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"

static DisplayEink g_display;

static void build_demo_ui() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Title bar
    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_set_size(bar, EPD_WIDTH, 32);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, "Ratspeak Pro");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_center(title);

    // Body labels
    lv_obj_t* line1 = lv_label_create(scr);
    lv_label_set_text(line1, "E-ink LVGL bring-up");
    lv_obj_set_style_text_color(line1, lv_color_black(), 0);
    lv_obj_set_style_text_font(line1, &lv_font_montserrat_16, 0);
    lv_obj_align(line1, LV_ALIGN_TOP_LEFT, 12, 48);

    lv_obj_t* line2 = lv_label_create(scr);
    lv_label_set_text(line2, "GxEPD2 + LVGL 8.3");
    lv_obj_set_style_text_color(line2, lv_color_black(), 0);
    lv_obj_align(line2, LV_ALIGN_TOP_LEFT, 12, 72);

    lv_obj_t* line3 = lv_label_create(scr);
    lv_label_set_text(line3, "Phase B: text on panel");
    lv_obj_set_style_text_color(line3, lv_color_black(), 0);
    lv_obj_align(line3, LV_ALIGN_TOP_LEFT, 12, 96);

    lv_obj_t* footer = lv_label_create(scr);
    lv_label_set_text(footer, "320x240 mono");
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -12);
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  E-INK LVGL");
    Serial.println("========================================");

    if (!g_display.begin()) {
        Serial.println("[BOOT] Display init FAILED");
        return;
    }

    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL init FAILED");
        return;
    }

    build_demo_ui();

    // Force first paint (e-ink is slow — one full refresh is fine)
    lv_refr_now(nullptr);
    Serial.println("[BOOT] Demo UI sent to panel");
}

void loop() {
    LvglPort::tick();
    delay(50);
}