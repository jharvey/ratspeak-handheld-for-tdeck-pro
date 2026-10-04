#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <lvgl.h>
#include <esp_system.h>
#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include "ratspeak_protocol.h"

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
// Minimal Rust FFI identity (Phase E step 2)
// Init context + load/create identity + expose lxmf.delivery dest hash.
// Does NOT open transport / LoRa / engines — that is the next larger step.
// ---------------------------------------------------------------------------
static rs_handheld_rns_t* g_rns = nullptr;
static bool g_identity_ok = false;

struct LiveStatus {
    float    batt_v;
    int      batt_pct;
    bool     batt_ok;

    bool     lora_online;
    int      rssi_dbm;
    float    snr_db;
    int      paths;
    int      links;
    int      lxmf_q;

    char     local_dest[17];   // 16 hex + NUL
    bool     dest_ok;

    char     msg_lines[4][40];
    int      msg_count;
};

static LiveStatus g_status;

static void bytes_to_hex16(const uint8_t in[16], char out[17]) {
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[16] = '\0';
}

static void fill_entropy64(uint8_t out[64]) {
    for (int i = 0; i < 64; i += 4) {
        uint32_t r = esp_random();
        out[i]     = (uint8_t)(r);
        out[i + 1] = (uint8_t)(r >> 8);
        out[i + 2] = (uint8_t)(r >> 16);
        out[i + 3] = (uint8_t)(r >> 24);
    }
}

// NVS key layout matches cooperative: namespace NVS_NS_IDENTITY, key "rawkey" (64 bytes)
static bool nvs_load_identity(uint8_t key[64]) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS_IDENTITY, true)) return false;
    size_t n = prefs.getBytesLength("rawkey");
    if (n != 64) {
        prefs.end();
        return false;
    }
    size_t got = prefs.getBytes("rawkey", key, 64);
    prefs.end();
    return got == 64;
}

static bool nvs_save_identity(const uint8_t key[64]) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS_IDENTITY, false)) return false;
    size_t w = prefs.putBytes("rawkey", key, 64);
    prefs.end();
    return w == 64;
}

static bool init_identity() {
    rs_handheld_status_t st = rs_handheld_rns_init(&g_rns);
    if (st != RS_HANDHELD_OK || !g_rns) {
        Serial.printf("[RNS] init failed (%d)\n", (int)st);
        g_rns = nullptr;
        return false;
    }
    Serial.printf("[RNS] FFI %s\n", rs_handheld_rns_version());

    uint8_t key[64];
    uint8_t id_hash[16];
    bool have = false;

    if (nvs_load_identity(key) &&
        rs_handheld_rns_validate_identity(key, id_hash, nullptr) == RS_HANDHELD_OK) {
        st = rs_handheld_rns_load_identity(g_rns, key);
        if (st == RS_HANDHELD_OK) {
            have = true;
            Serial.println("[RNS] identity loaded from NVS");
        }
    }

    if (!have) {
        uint8_t entropy[64];
        fill_entropy64(entropy);
        st = rs_handheld_rns_create_identity(entropy, key, id_hash);
        // wipe entropy
        for (int i = 0; i < 64; i++) entropy[i] = 0;
        if (st != RS_HANDHELD_OK) {
            Serial.printf("[RNS] create_identity failed (%d)\n", (int)st);
            return false;
        }
        st = rs_handheld_rns_load_identity(g_rns, key);
        if (st != RS_HANDHELD_OK) {
            Serial.printf("[RNS] load_identity failed (%d)\n", (int)st);
            return false;
        }
        if (nvs_save_identity(key)) {
            Serial.println("[RNS] new identity created + saved to NVS");
        } else {
            Serial.println("[RNS] new identity created (NVS save failed — ephemeral)");
        }
    }

    // wipe key buffer
    for (int i = 0; i < 64; i++) key[i] = 0;

    uint8_t dest[16];
    st = rs_handheld_rns_destination_hash(g_rns, dest);
    if (st != RS_HANDHELD_OK) {
        Serial.printf("[RNS] destination_hash failed (%d)\n", (int)st);
        return false;
    }
    bytes_to_hex16(dest, g_status.local_dest);
    g_status.dest_ok = true;
    Serial.printf("[RNS] LOCAL_DEST %s\n", g_status.local_dest);
    g_identity_ok = true;
    return true;
}

// BQ27220 Voltage() 0x08, LE mV
static bool read_bq27220_voltage(float& v_out, int& pct_out) {
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x08);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)BQ27220_I2C_ADDR, (uint8_t)2) != 2) return false;
    uint16_t mv = Wire.read() | (Wire.read() << 8);
    if (mv < 2500 || mv > 4500) return false;
    v_out = mv / 1000.0f;
    float pct = (v_out - 3.30f) / (4.20f - 3.30f) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pct_out = (int)(pct + 0.5f);
    return true;
}

static void refresh_status() {
    g_status.batt_ok = read_bq27220_voltage(g_status.batt_v, g_status.batt_pct);
    if (!g_status.batt_ok) {
        g_status.batt_v   = 0.0f;
        g_status.batt_pct = -1;
    }

    // Radio / paths still fallback until ProtocolRuntime + LoRa pump
    g_status.lora_online = false;
    g_status.rssi_dbm    = 0;
    g_status.snr_db      = 0.0f;
    g_status.paths       = 0;
    g_status.links       = 0;
    g_status.lxmf_q      = 0;

    if (!g_status.dest_ok) {
        strncpy(g_status.local_dest, "--------", sizeof(g_status.local_dest));
        g_status.local_dest[16] = '\0';
    }

    g_status.msg_count = 0;  // no message store yet
}

// ---------------------------------------------------------------------------
// Fat-text UI (unchanged shell)
// ---------------------------------------------------------------------------
static void fat_label(lv_obj_t* parent, const char* txt,
                      lv_coord_t x, lv_coord_t y,
                      lv_color_t color = lv_color_black())
{
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            lv_obj_t* o = lv_label_create(parent);
            lv_label_set_text(o, txt);
            lv_obj_set_style_text_font(o, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(o, color, 0);
            lv_obj_set_pos(o, x + dx, y + dy);
        }
    }
}

static void clear_screen() {
    g_display.fillScreen(false);
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
    fat_label(g_root, title, 8, 6);
    lv_obj_t* bar = lv_obj_create(g_root);
    lv_obj_set_size(bar, EPD_WIDTH, 3);
    lv_obj_set_pos(bar, 0, 30);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
}

static void make_footer(const char* text) {
    fat_label(g_root, text, 8, EPD_HEIGHT - 22);
}

static void add_row(int& y, const char* left, const char* right) {
    fat_label(g_root, left,  8,  y);
    fat_label(g_root, right, 110, y);
    y += 28;
}

static void build_home() {
    clear_screen();
    make_header("Ratspeak");
    char buf[32];
    int y = 40;

    if (g_status.batt_ok) {
        snprintf(buf, sizeof(buf), "%.2f V %d%%", g_status.batt_v, g_status.batt_pct);
    } else {
        snprintf(buf, sizeof(buf), "n/a");
    }
    add_row(y, "Battery", buf);

    add_row(y, "LoRa", g_status.lora_online ? "online" : "offline");

    if (g_status.lora_online) {
        snprintf(buf, sizeof(buf), "%d dBm", g_status.rssi_dbm);
        add_row(y, "RSSI", buf);
        snprintf(buf, sizeof(buf), "%.1f dB", g_status.snr_db);
        add_row(y, "SNR", buf);
    } else {
        add_row(y, "RSSI", "--");
        add_row(y, "SNR",  "--");
    }

    y += 6;
    snprintf(buf, sizeof(buf), "%d", g_status.paths);
    add_row(y, "Paths", buf);
    snprintf(buf, sizeof(buf), "%d", g_status.links);
    add_row(y, "Links", buf);
    snprintf(buf, sizeof(buf), "%d", g_status.lxmf_q);
    add_row(y, "LXMF Q", buf);

    y += 10;
    fat_label(g_root, "LOCAL_DEST", 8, y);
    y += 26;
    fat_label(g_root, g_status.local_dest, 8, y);

    make_footer("1/3 Home | [>] next");
}

static void build_messages() {
    clear_screen();
    make_header("Messages");
    int y = 40;
    if (g_status.msg_count == 0) {
        fat_label(g_root, "(no messages yet)", 8, y);
    } else {
        for (int i = 0; i < g_status.msg_count && i < 4; i++) {
            fat_label(g_root, g_status.msg_lines[i], 8, y);
            y += 28;
        }
    }
    make_footer("2/3 Msgs | [>] next");
}

static void build_settings() {
    clear_screen();
    make_header("Settings");
    int y = 40;
    add_row(y, "Radio",    "Long Fast");
    add_row(y, "Freq",     "915.0 MHz");
    add_row(y, "TX power", "22 dBm");
    add_row(y, "WiFi",     "off");
    add_row(y, "Display",  "e-ink");
    y += 14;
    fat_label(g_root, g_identity_ok ? "(identity live)" : "(identity n/a)", 8, y);
    make_footer("3/3 Setup | [>] next");
}

static void show_screen(ScreenId id) {
    g_screen = id;
    refresh_status();
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
    show_screen((ScreenId)((g_screen + 1) % SCR_COUNT));
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase E.2");
    Serial.println(" identity + LOCAL_DEST");
    Serial.println("========================================");

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);

    g_status.dest_ok = false;
    g_status.local_dest[0] = '\0';

    if (!g_display.begin()) {
        Serial.println("[BOOT] Display init FAILED");
        return;
    }
    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL init FAILED");
        return;
    }

    if (!init_identity()) {
        Serial.println("[BOOT] identity init failed — LOCAL_DEST will show --------");
    }

    refresh_status();
    if (g_status.batt_ok) {
        Serial.printf("[BAT] %.2f V  %d%%\n", g_status.batt_v, g_status.batt_pct);
    } else {
        Serial.println("[BAT] BQ27220 not responding");
    }

    show_screen(SCR_HOME);
    Serial.println("[BOOT] ready – serial: n / h / m / s");
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last > 3500) {
        last = millis();
        next_screen();
    }

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'n' || c == 'N' || c == '>') {
            next_screen(); last = millis();
        } else if (c == 'h' || c == 'H' || c == '1') {
            show_screen(SCR_HOME); last = millis();
        } else if (c == 'm' || c == 'M' || c == '2') {
            show_screen(SCR_MESSAGES); last = millis();
        } else if (c == 's' || c == 'S' || c == '3') {
            show_screen(SCR_SETTINGS); last = millis();
        }
    }
    delay(20);
}