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

// ---------------------------------------------------------------------------
// Phase E — live battery, LOCAL_DEST, LoRa online + RSSI
// Paths/Links/LXMF Q and Messages list need ProtocolRuntime / MessageStore
// (next slice). Shell + single fullRefresh unchanged. Cooperative green.
// ---------------------------------------------------------------------------

static DisplayEink g_display;
static SX1262* g_radio = nullptr;
static bool g_radioOk = false;

enum ScreenId : uint8_t {
    SCR_HOME = 0,
    SCR_MESSAGES,
    SCR_SETTINGS,
    SCR_COUNT
};

static ScreenId g_screen = SCR_HOME;
static lv_obj_t* g_root = nullptr;

// Live cache
static char g_batStr[20]   = "n/a";
static char g_pctStr[8]    = "--%";
static char g_destHex[17]  = "................";
static char g_loraStr[12]  = "off";
static char g_rssiStr[16]  = "-";
static char g_snrStr[12]   = "-";
static char g_pathsStr[8]  = "0";
static char g_linksStr[8]  = "0";
static char g_lxmfqStr[8]  = "0";
static bool g_rnsReady     = false;

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

// ---------- Minimal RNS identity -> LOCAL_DEST ----------
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

// ---------- SX1262 (shared SPI; hold EPD_CS high while talking) ----------
static bool initRadio() {
    // Isolate e-ink while we talk to LoRa
    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif

    // SPI already started by DisplayEink::begin(); reuse the bus
    g_radio = new SX1262(&SPI, LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
                         LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
                         LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);
    if (!g_radio) {
        Serial.println("[LORA] alloc failed");
        return false;
    }

    if (!g_radio->begin(LORA_DEFAULT_FREQ)) {
        Serial.println("[LORA] begin failed");
        delete g_radio;
        g_radio = nullptr;
        return false;
    }

    g_radio->setSpreadingFactor(LORA_DEFAULT_SF);
    g_radio->setSignalBandwidth(LORA_DEFAULT_BW);
    g_radio->setCodingRate4(LORA_DEFAULT_CR);
    g_radio->setTxPower(LORA_DEFAULT_TX_POWER);
    g_radio->setPreambleLength(LORA_DEFAULT_PREAMBLE);
    g_radio->receive();   // listen so currentRssi is meaningful

    Serial.printf("[LORA] online  freq=%lu SF=%d BW=%lu TX=%d\n",
                  (unsigned long)LORA_DEFAULT_FREQ,
                  LORA_DEFAULT_SF,
                  (unsigned long)LORA_DEFAULT_BW,
                  LORA_DEFAULT_TX_POWER);
    return true;
}

static void refreshLiveData() {
    // Battery
    float v = 0;
    int pct = -1;
    if (readBattery(v, pct)) {
        snprintf(g_batStr, sizeof(g_batStr), "%.2f V", v);
        snprintf(g_pctStr, sizeof(g_pctStr), "%d%%", pct);
        Serial.printf("[BAT] %.2f V  %d%%\n", v, pct);
    } else {
        strcpy(g_batStr, "n/a");
        strcpy(g_pctStr, "--%");
    }

    // LOCAL_DEST (once)
    if (!g_rnsReady) {
        g_rnsReady = initLocalDest();
    }

    // LoRa status + RSSI (live when radio is up)
    if (g_radio && g_radio->isRadioOnline()) {
        strcpy(g_loraStr, "online");
        int rssi = g_radio->currentRssi();
        snprintf(g_rssiStr, sizeof(g_rssiStr), "%d dBm", rssi);
        // packetSnr only valid after a received packet; show dash until then
        strcpy(g_snrStr, "-");
    } else {
        strcpy(g_loraStr, "off");
        strcpy(g_rssiStr, "-");
        strcpy(g_snrStr, "-");
    }

    // ProtocolRuntime counters — next slice
    strcpy(g_pathsStr, "0");
    strcpy(g_linksStr, "0");
    strcpy(g_lxmfqStr, "0");
}

// ---------- UI helpers ----------
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

// ---------- Screens ----------
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

    make_footer("1/3 Home | [>] next");
}

static void build_messages() {
    clear_screen();
    make_header("Messages");
    int y = 40;
    add_row(y, "(none)", "no store yet");
    y += 14;
    fat_label(g_root, "(MessageStore next)", 8, y);
    make_footer("2/3 Msgs | [>] next");
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
    make_footer("3/3 Setup | [>] next");
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
    Serial.println(" live battery + dest + LoRa");
    Serial.println("========================================");

    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(30);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);

    // Display first (owns SPI begin + power gates)
    if (!g_display.begin()) {
        Serial.println("[BOOT] Display init FAILED");
        return;
    }
    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL init FAILED");
        return;
    }

    // Radio after display so SPI is up; CS isolation inside initRadio()
    g_radioOk = initRadio();
    if (!g_radioOk) {
        Serial.println("[BOOT] Radio init failed (UI continues)");
    }

    show_screen(SCR_HOME);
    Serial.println("[BOOT] ready - serial: n / h / m / s");
}

void loop() {
    // Keep radio in RX so currentRssi updates
    if (g_radio && g_radioOk) {
        // parsePacket drains any pending; ignore payload for now
        (void)g_radio->parsePacket();
    }

    static uint32_t last = 0;
    if (millis() - last > 4000) {
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