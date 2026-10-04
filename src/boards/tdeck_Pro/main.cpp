#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"

static DisplayEink g_display;

enum ScreenId : uint8_t {
    SCR_HOME = 0,
    SCR_MESSAGES,
    SCR_SETTINGS,
    SCR_COUNT
};

static ScreenId g_screen = SCR_HOME;
static lv_obj_t* g_root = nullptr;

// ---------------------------------------------------------------------------
// Bold / fat text for e-ink (1-pixel offset double-draw)
// ---------------------------------------------------------------------------
static void fat_label(lv_obj_t* parent, const char* txt,
                      lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs,
                      lv_color_t color = lv_color_black())
{
    lv_obj_t* a = lv_label_create(parent);
    lv_label_set_text(a, txt);
    lv_obj_set_style_text_font(a, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(a, color, 0);
    lv_obj_align(a, align, x_ofs, y_ofs);

    lv_obj_t* b = lv_label_create(parent);
    lv_label_set_text(b, txt);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(b, color, 0);
    lv_obj_align(b, align, x_ofs + 1, y_ofs);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void clear_screen() {
    g_display.fillScreen(false);          // white
    if (g_root) {
        lv_obj_clean(g_root);
    } else {
        g_root = lv_scr_act();
        lv_obj_set_style_bg_color(g_root, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(g_root, 0, 0);
        lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void make_header(const char* title) {
    lv_obj_t* header = lv_obj_create(g_root);
    lv_obj_set_size(header, EPD_WIDTH, 28);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    fat_label(header, title, LV_ALIGN_LEFT_MID, 8, 0, lv_color_white());
}

static void make_footer(const char* text) {
    // Dedicated bottom band so it never collides with content
    fat_label(g_root, text, LV_ALIGN_BOTTOM_LEFT, 10, -8);
}

static void add_row(int& y, const char* left, const char* right) {
    // Left column starts at x=10, right column starts at x=150
    // (safe for 320 px + fat stroke)
    fat_label(g_root, left,  LV_ALIGN_TOP_LEFT, 10,  y);
    fat_label(g_root, right, LV_ALIGN_TOP_LEFT, 150, y);
    y += 24;
}

// ---------------------------------------------------------------------------
// Screens (dummy data) – tighter, no right overflow, clear footer
// ---------------------------------------------------------------------------
static void build_home() {
    clear_screen();
    make_header("Ratspeak");
    int y = 38;
    add_row(y, "Battery", "4.14 V");
    add_row(y, "LoRa",    "online");
    add_row(y, "RSSI",    "-86 dBm");
    add_row(y, "SNR",     "6.2 dB");
    y += 4;
    add_row(y, "Paths",   "1");
    add_row(y, "Links",   "0");
    add_row(y, "LXMF Q",  "0");
    y += 8;
    fat_label(g_root, "LOCAL_DEST", LV_ALIGN_TOP_LEFT, 10, y);
    y += 22;
    fat_label(g_root, "ca53afea5f8f1fbc", LV_ALIGN_TOP_LEFT, 10, y);
    make_footer("1/3 Home | [>] next");
}

static void build_messages() {
    clear_screen();
    make_header("Messages");
    int y = 38;
    add_row(y, "Alice",   "hi from mesh");
    add_row(y, "Bob",     "meeting 14:00");
    add_row(y, "Node-7",  "announce ok");
    y += 10;
    fat_label(g_root, "(dummy list)", LV_ALIGN_TOP_LEFT, 10, y);
    make_footer("2/3 Msgs | [>] next");
}

static void build_settings() {
    clear_screen();
    make_header("Settings");
    int y = 38;
    add_row(y, "Radio",    "Long Fast");
    add_row(y, "Freq",     "915.0 MHz");
    add_row(y, "TX power", "22 dBm");
    add_row(y, "WiFi",     "off");
    add_row(y, "Display",  "e-ink");
    y += 10;
    fat_label(g_root, "(dummy settings)", LV_ALIGN_TOP_LEFT, 10, y);
    make_footer("3/3 Setup | [>] next");
}

static void show_screen(ScreenId id) {
    g_screen = id;
    switch (id) {
        case SCR_HOME:     build_home();     break;
        case SCR_MESSAGES: build_messages(); break;
        case SCR_SETTINGS: build_settings(); break;
        default:           build_home();     break;
    }
    lv_refr_now(nullptr);
    g_display.refreshIfDirty();
    Serial.printf("[UI] screen %u drawn\n", (unsigned)id);
}

static void next_screen() {
    ScreenId n = (ScreenId)((g_screen + 1) % SCR_COUNT);
    show_screen(n);
}

// ---------------------------------------------------------------------------
// Boot / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase D");
    Serial.println(" Multi-screen shell (fat text, fixed layout)");
    Serial.println("========================================");

    if (!g_display.begin()) {
        Serial.println("[BOOT] Display init FAILED");
        return;
    }
    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL init FAILED");
        return;
    }

    show_screen(SCR_HOME);
    Serial.println("[BOOT] Phase D home on panel");
    Serial.println("[BOOT] Serial: n = next, h/m/s = jump");
}

void loop() {
    // Optional auto-cycle (comment out if you only want serial control)
    static uint32_t last_switch = 0;
    if (millis() - last_switch > 3000) {
        last_switch = millis();
        next_screen();
    }

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'n' || c == 'N' || c == '>') {
            next_screen();
            last_switch = millis();
        } else if (c == 'h' || c == 'H' || c == '1') {
            show_screen(SCR_HOME);
            last_switch = millis();
        } else if (c == 'm' || c == 'M' || c == '2') {
            show_screen(SCR_MESSAGES);
            last_switch = millis();
        } else if (c == 's' || c == 'S' || c == '3') {
            show_screen(SCR_SETTINGS);
            last_switch = millis();
        }
    }
    delay(20);
}