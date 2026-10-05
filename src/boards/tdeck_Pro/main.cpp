#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include "ratspeak_protocol.h"
#include "radio/SX1262.h"
#include "storage/FlashStore.h"
#include "storage/MessageStore.h"
#include "reticulum/IdentityManager.h"
#include "hal/Keyboard.h"

// ---------------------------------------------------------------------------
// Phase E chunk 2b — no auto-rotate; next via Enter / serial / touch [>]
// Live: battery, dest, LoRa/RSSI, store counts (1 unread is real data)
// ---------------------------------------------------------------------------

static DisplayEink g_display;
static SX1262* g_radio = nullptr;
static bool g_radioOk = false;
static Keyboard g_kb;
static bool g_kbOk = false;

static FlashStore g_flash;
static IdentityManager g_idMgr;
static MessageStore g_msgStore;
static bool g_storeReady = false;

enum ScreenId : uint8_t {
    SCR_HOME = 0,
    SCR_MESSAGES,
    SCR_SETTINGS,
    SCR_COUNT
};

static ScreenId g_screen = SCR_HOME;
static lv_obj_t* g_root = nullptr;

static char g_batStr[20]   = "n/a";
static char g_pctStr[8]    = "--%";
static char g_destHex[17]  = "................";
static char g_loraStr[12]  = "off";
static char g_rssiStr[16]  = "-";
static char g_snrStr[12]   = "-";
static char g_pathsStr[8]  = "0";
static char g_linksStr[8]  = "0";
static char g_lxmfqStr[8]  = "0";
static char g_convStr[24]  = "0 conv";
static char g_unreadStr[16] = "0 unread";
static bool g_rnsReady     = false;

// Touch hit target: bottom-right ~72x40 px
static constexpr int NEXT_X0 = EPD_WIDTH - 80;
static constexpr int NEXT_Y0 = EPD_HEIGHT - 40;
static constexpr int NEXT_X1 = EPD_WIDTH - 4;
static constexpr int NEXT_Y1 = EPD_HEIGHT - 4;

// ---------- BQ27220 ----------
static bool readBattery(float& volts, int& pct) {
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x08);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    uint16_t mv = Wire.read() | (Wire.read() << 8);
    volts = mv / 1000.0f;

    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x2C);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    pct = Wire.read() | (Wire.read() << 8);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return true;
}

// ---------- Minimal RNS LOCAL_DEST ----------
static bool initLocalDest() {
    rs_handheld_rns_t* ctx = nullptr;
    if (rs_handheld_rns_init(&ctx) != RS_HANDHELD_OK || !ctx) {
        Serial.println("[RNS] init failed");
        return false;
    }

    Preferences prefs;
    uint8_t priv[64] = {0};
    bool haveKey = false;
    if (prefs.begin(NVS_NS_IDENTITY, true)) {
        size_t n = prefs.getBytes("privkey", priv, 64);
        prefs.end();
        if (n == 64) haveKey = true;
    }

    if (!haveKey) {
        uint8_t entropy[64];
        for (int i = 0; i < 64; i += 4) {
            uint32_t r = esp_random();
            memcpy(entropy + i, &r, 4);
        }
        uint8_t idHash[16];
        if (rs_handheld_rns_create_identity(entropy, priv, idHash) != RS_HANDHELD_OK) {
            Serial.println("[RNS] create_identity failed");
            rs_handheld_rns_shutdown(ctx);
            return false;
        }
        if (prefs.begin(NVS_NS_IDENTITY, false)) {
            prefs.putBytes("privkey", priv, 64);
            prefs.end();
            Serial.println("[RNS] new identity created + saved to NVS");
        }
    }

    if (rs_handheld_rns_load_identity(ctx, priv) != RS_HANDHELD_OK) {
        Serial.println("[RNS] load_identity failed");
        rs_handheld_rns_shutdown(ctx);
        return false;
    }

    uint8_t dest[16];
    if (rs_handheld_rns_destination_hash(ctx, dest) != RS_HANDHELD_OK) {
        Serial.println("[RNS] destination_hash failed");
        rs_handheld_rns_shutdown(ctx);
        return false;
    }

    for (int i = 0; i < 16; i++) {
        sprintf(g_destHex + i * 2, "%02x", dest[i]);
    }
    g_destHex[16] = '\0';

    Serial.printf("[RNS] FFI %s\n", rs_handheld_rns_version());
    Serial.printf("[RNS] LOCAL_DEST %s\n", g_destHex);
    rs_handheld_rns_shutdown(ctx);
    return true;
}

// ---------- SX1262 ----------
static bool initRadio() {
    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif

    g_radio = new SX1262(&SPI, LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
                         LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
                         LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);
    if (!g_radio) return false;

    if (!g_radio->begin(LORA_DEFAULT_FREQ)) {
        delete g_radio;
        g_radio = nullptr;
        return false;
    }

    g_radio->setSpreadingFactor(LORA_DEFAULT_SF);
    g_radio->setSignalBandwidth(LORA_DEFAULT_BW);
    g_radio->setCodingRate4(LORA_DEFAULT_CR);
    g_radio->setTxPower(LORA_DEFAULT_TX_POWER);
    g_radio->setPreambleLength(LORA_DEFAULT_PREAMBLE);
    g_radio->receive();

    Serial.printf("[LORA] online freq=%lu SF=%d\n",
                  (unsigned long)LORA_DEFAULT_FREQ, LORA_DEFAULT_SF);
    return true;
}

// ---------- Storage ----------
static bool initStorage() {
    if (!g_flash.begin()) {
        Serial.println("[STORE] FlashStore begin failed");
        return false;
    }
    if (!g_idMgr.begin(&g_flash, nullptr)) {
        Serial.println("[STORE] IdentityManager begin failed");
    }
    if (!g_msgStore.begin(&g_flash, nullptr, false)) {
        Serial.println("[STORE] MessageStore begin failed");
        return false;
    }
    Serial.printf("[STORE] ready conv=%u unread=%d\n",
                  (unsigned)g_msgStore.totalConversations(),
                  g_msgStore.totalUnreadCount());
    return true;
}

// ---------- Minimal CST328 touch (I2C 0x1A) ----------
// Reads first touch point; returns true if finger down with coords.
static bool touchRead(int16_t& x, int16_t& y) {
    // CST3xx-style: reg 0x00.. finger count at 0x02, points follow
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)TOUCH_I2C_ADDR, 7) < 7) return false;

    (void)Wire.read(); // 0x00
    (void)Wire.read(); // 0x01
    uint8_t fingers = Wire.read() & 0x0F;
    if (fingers == 0) return false;

    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();

    x = ((xh & 0x0F) << 8) | xl;
    y = ((yh & 0x0F) << 8) | yl;

    // Clamp to panel
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= EPD_WIDTH) x = EPD_WIDTH - 1;
    if (y >= EPD_HEIGHT) y = EPD_HEIGHT - 1;
    return true;
}

static bool touchHitNext() {
    int16_t x = 0, y = 0;
    if (!touchRead(x, y)) return false;
    return (x >= NEXT_X0 && x <= NEXT_X1 && y >= NEXT_Y0 && y <= NEXT_Y1);
}

static void refreshLiveData() {
    float v = 0;
    int pct = -1;
    if (readBattery(v, pct)) {
        snprintf(g_batStr, sizeof(g_batStr), "%.2f V", v);
        snprintf(g_pctStr, sizeof(g_pctStr), "%d%%", pct);
    } else {
        strcpy(g_batStr, "n/a");
        strcpy(g_pctStr, "--%");
    }

    if (!g_rnsReady) g_rnsReady = initLocalDest();

    if (g_radio && g_radio->isRadioOnline()) {
        strcpy(g_loraStr, "online");
        snprintf(g_rssiStr, sizeof(g_rssiStr), "%d dBm", g_radio->currentRssi());
        strcpy(g_snrStr, "-");
    } else {
        strcpy(g_loraStr, "off");
        strcpy(g_rssiStr, "-");
        strcpy(g_snrStr, "-");
    }

    strcpy(g_pathsStr, "0");
    strcpy(g_linksStr, "0");
    strcpy(g_lxmfqStr, "0");

    if (g_storeReady) {
        g_msgStore.poll();
        snprintf(g_convStr, sizeof(g_convStr), "%u conv",
                 (unsigned)g_msgStore.totalConversations());
        snprintf(g_unreadStr, sizeof(g_unreadStr), "%d unread",
                 g_msgStore.totalUnreadCount());
    } else {
        strcpy(g_convStr, "no store");
        strcpy(g_unreadStr, "-");
    }
}

// ---------- UI ----------
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

static void make_next_button() {
    // Visible [>] in the bottom-right touch zone
    fat_label(g_root, "[>]", NEXT_X0 + 8, NEXT_Y0 + 8);
}

static void add_row(int& y, const char* left, const char* right) {
    fat_label(g_root, left,  8,  y);
    fat_label(g_root, right, 110, y);
    y += 28;
}

static void build_home() {
    clear_screen();
    make_header("Ratspeak");
    int y = 40;
    char batLine[28];
    snprintf(batLine, sizeof(batLine), "%s %s", g_batStr, g_pctStr);
    add_row(y, "Battery", batLine);
    add_row(y, "LoRa",   g_loraStr);
    add_row(y, "RSSI",   g_rssiStr);
    add_row(y, "SNR",    g_snrStr);
    y += 6;
    add_row(y, "Paths",  g_pathsStr);
    add_row(y, "Links",  g_linksStr);
    add_row(y, "LXMF Q", g_lxmfqStr);
    y += 10;
    fat_label(g_root, "LOCAL_DEST", 8, y);
    y += 26;
    fat_label(g_root, g_rnsReady ? g_destHex : "................", 8, y);
    make_footer("1/3 Home  Enter/[>] next");
    make_next_button();
}

static void build_messages() {
    clear_screen();
    make_header("Messages");
    int y = 40;
    add_row(y, "Convs",  g_convStr);
    add_row(y, "Unread", g_unreadStr);
    y += 14;
    if (g_storeReady && g_msgStore.totalConversations() == 0) {
        fat_label(g_root, "(empty)", 8, y);
    } else if (!g_storeReady) {
        fat_label(g_root, "(store not ready)", 8, y);
    } else {
        fat_label(g_root, "(list rows next)", 8, y);
    }
    make_footer("2/3 Msgs  Enter/[>] next");
    make_next_button();
}

static void build_settings() {
    clear_screen();
    make_header("Settings");
    int y = 40;
    char freq[20];
    snprintf(freq, sizeof(freq), "%.1f MHz", LORA_DEFAULT_FREQ / 1e6f);
    char txp[12];
    snprintf(txp, sizeof(txp), "%d dBm", LORA_DEFAULT_TX_POWER);
    add_row(y, "Radio",    "Long Fast");
    add_row(y, "Freq",     freq);
    add_row(y, "TX power", txp);
    add_row(y, "WiFi",     "off");
    add_row(y, "Display",  "e-ink");
    y += 14;
    fat_label(g_root, "(BoardConfig defaults)", 8, y);
    make_footer("3/3 Setup Enter/[>] next");
    make_next_button();
}

static void show_screen(ScreenId id) {
    g_screen = id;
    refreshLiveData();
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

// ---------- Boot / loop ----------
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase E");
    Serial.println(" no auto-rotate; Enter / touch [>]");
    Serial.println("========================================");

    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(30);

    // Touch reset
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(50);
    pinMode(TOUCH_INT, INPUT);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);

    if (!g_display.begin()) {
        Serial.println("[BOOT] Display FAILED");
        return;
    }
    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL FAILED");
        return;
    }

    g_kbOk = g_kb.begin();
    if (!g_kbOk) Serial.println("[BOOT] Keyboard failed (serial still works)");

    g_radioOk = initRadio();
    if (!g_radioOk) Serial.println("[BOOT] Radio failed (UI continues)");

    g_storeReady = initStorage();
    if (!g_storeReady) Serial.println("[BOOT] Store failed (UI continues)");

    show_screen(SCR_HOME);
    Serial.println("[BOOT] ready - Enter / [>] / serial n to next");
}

void loop() {
    if (g_radio && g_radioOk) {
        (void)g_radio->parsePacket();
    }
    if (g_storeReady) {
        g_msgStore.poll();
    }

    // Keyboard Enter -> next
    if (g_kbOk) {
        g_kb.update();
        if (g_kb.hasEvent()) {
            const KeyEvent& e = g_kb.getEvent();
            if (e.enter) {
                next_screen();
            }
        }
    }

    // Touch bottom-right [>]
    static bool wasDown = false;
    static uint32_t lastTouchMs = 0;
    if (millis() - lastTouchMs > 40) {
        lastTouchMs = millis();
        int16_t tx = 0, ty = 0;
        bool down = touchRead(tx, ty);
        if (down && !wasDown) {
            if (tx >= NEXT_X0 && tx <= NEXT_X1 && ty >= NEXT_Y0 && ty <= NEXT_Y1) {
                Serial.printf("[TOUCH] next hit x=%d y=%d\n", tx, ty);
                next_screen();
            } else {
                Serial.printf("[TOUCH] x=%d y=%d (outside [>])\n", tx, ty);
            }
        }
        wasDown = down;
    }

    // Serial still works
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'n' || c == 'N' || c == '>') {
            next_screen();
        } else if (c == 'h' || c == 'H' || c == '1') {
            show_screen(SCR_HOME);
        } else if (c == 'm' || c == 'M' || c == '2') {
            show_screen(SCR_MESSAGES);
        } else if (c == 's' || c == 'S' || c == '3') {
            show_screen(SCR_SETTINGS);
        }
    }
    delay(15);
}