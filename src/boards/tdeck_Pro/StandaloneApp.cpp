#include "StandaloneApp.h"

#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <LittleFS.h>

#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include "Eink/RatSpeakLogo.h"
#include "audio/AudioNotify.h"
#include "hal/Keyboard.h"
#include "radio/SX1262.h"
#include "storage/FlashStore.h"
#include "storage/MessageStore.h"
#include "storage/ConversationView.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"
#include "transport/LoRaInterface.h"
#include "protocol/ProtocolRuntime.h"
#include "protocol/OutgoingContract.h"
#include "runtime/TaskOwner.h"
#include "ratspeak_protocol.h"
#include "util/Bytes.h"

// From Keyboard.cpp — force number layer for numeric settings fields
extern void keyboardSetNumLayer(bool on);

namespace standalone {
namespace {

DisplayEink g_display;
Keyboard g_kb;
bool g_kbOk = false;
bool g_touchOk = false;  // E22: CST328 present
AudioNotify g_audio;
bool g_featAudio = true;   // beep+vibe on inbound when true
bool g_featWifi  = false;
bool g_featTcp   = false;
bool g_featGps   = false;
bool g_featLora  = true;   // display/radio intent (radio stays up for mesh)
bool g_darkTheme = false;  // false=white paper, true=black-ink select



struct LoraPreset { const char* name; uint8_t sf; float bw; uint8_t cr; };
static const LoraPreset kLoraPresets[] = {
    {"Long Fast",  11, 250000.f, 5},
    {"Long Slow",  12, 125000.f, 8},
    {"Medium",     10, 250000.f, 5},
    {"Fast Short",  7, 500000.f, 5},
};
static constexpr int kLoraPresetCount = 4;
int g_loraPreset = 0;

static inline lv_color_t theme_fg() {
    return g_darkTheme ? lv_color_white() : lv_color_black();
}
static inline lv_color_t theme_bg() {
    return g_darkTheme ? lv_color_black() : lv_color_white();
}

// Settings hierarchy (Plus-like). SET_ROOT = top list; others = sub-pages.
enum SettingsPage : uint8_t {
    SET_ROOT = 0,
    SET_IDENTITY,
    SET_SCREEN,
    SET_BATTERY,
    SET_LORA,
    SET_NETWORK,
    SET_TIME,
    SET_AUDIO,
    SET_DIAG,
    SET_STORAGE,
    SET_TZ,
    SET_IDLE_MODE,
    SET_MANUAL_TIME,
    SET_MANUAL_DATE,
    SET_IDLE_SECS,
    SET_MANUAL_LOC,   // default lat/lon when no GPS
    SET_RTC_FIELD,    // type digits for one RTC field
};
SettingsPage g_settingsPage = SET_ROOT;
int g_settingsSel = 0;  // highlighted row on current page
int g_settingsRootSel = 0;  // remembered root row when entering a sub-page
int g_tzIndex = 0;  // 0=EST default; see kTzNames
static constexpr int kTzCount = 18;

static void loadUiPrefs() {
    Preferences prefs;
    if (!prefs.begin("rs_ui", true)) return;
    g_darkTheme = prefs.getBool("dark", false);
    g_tzIndex = prefs.getInt("tz", 0);
    if (g_tzIndex < 0 || g_tzIndex >= 18) g_tzIndex = 0;
    // Restore last wall clock (survives soft/hard reset when NVS kept)
    int64_t epoch = prefs.getLong64("epoch", 0);
    prefs.end();
    if (epoch > 100000) {
        struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        Serial.printf("[RTC] restored epoch=%lld\n", (long long)epoch);
    }
    Serial.printf("[UI] theme=%s tz=%d\n", g_darkTheme ? "dark" : "light", g_tzIndex);
}
static void saveUiPrefs() {
    Preferences prefs;
    if (!prefs.begin("rs_ui", false)) return;
    prefs.putBool("dark", g_darkTheme);
    prefs.putInt("tz", g_tzIndex);
    time_t now = time(nullptr);
    if (now > 100000)
        prefs.putLong64("epoch", (int64_t)now);
    prefs.end();
}
static void maybeSaveRtcEpoch() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (last && (now - last) < 60000UL) return;  // once/minute
    last = now ? now : 1;
    time_t t = time(nullptr);
    if (t < 100000) return;
    Preferences prefs;
    if (!prefs.begin("rs_ui", false)) return;
    prefs.putLong64("epoch", (int64_t)t);
    prefs.end();
}
int g_manualHour = 12;
int g_manualMin  = 0;
bool g_timeEditMode = false;  // type HHMM for RTC
bool g_autoGpsSync = true;    // sync RTC from GPS after boot / daily
bool g_autoGpsSyncedBoot = false;
uint32_t g_lastGpsSyncMs = 0;
uint8_t  g_idleMode = 1;  // 0=off 1=no-pass default 2=password TBD
bool     g_idleActive = false;
int      g_idleLastUnread = -1;
int      g_idleLastPct = -1;
uint32_t g_lastInputMs = 0;
uint32_t g_idleTimeoutSec = 30;  // 10..99999, default 30s
int      g_manualMon = 1;
int      g_manualDay = 1;
int      g_manualYear = 2027;
bool     g_dateEditMode = false;
bool     g_idleSecsEditMode = false;
char     g_idleRangeMsg[24] = {0};  // "Out of range" feedback
char     g_numsOnlyMsg[24] = {0};   // "nums only" flash


// Which RTC field is being typed: 0=hour 1=min 2=mon 3=day 4=year
int      g_rtcField = 0;
float    g_manualLat = 0.0f;
float    g_manualLon = 0.0f;
bool     g_hasManualPos = false;
uint32_t g_lastPeerAutoMs = 0;
size_t   g_lastPeerCount  = 0;
bool     g_peerDirty      = false;
bool     g_composeMode    = false;  // true while typing a message

FlashStore g_flash;
IdentityManager g_idMgr;
MessageStore g_msgStore;
AnnounceManager g_announceMgr;
ProtocolRuntime g_proto;
bool g_storeReady = false;
bool g_protoReady = false;

SX1262* g_radio = nullptr;
LoRaInterface* g_loraIf = nullptr;

enum ScreenId : uint8_t {
    SCR_HOME = 0,
    SCR_CHATS,      // Plus: Chats (conversations + compose + history)
    SCR_CONTACTS,   // Plus: Contacts (messaged / named peers)
    SCR_PEERS,      // Plus: Peers (announced nodes)
    SCR_SETTINGS,
    SCR_COUNT
};
ScreenId g_screen = SCR_HOME;
bool g_menuOpen = false;  // G1.80: top MENU overlay open
lv_obj_t* g_root = nullptr;

char g_batStr[20]    = "n/a";
char g_pctStr[8]     = "--%";
bool g_batCharging   = false;
bool g_usbPower      = false;  // external 5V present (USB)
int16_t g_avgMa      = 0;      // last AverageCurrent mA (+in charge / -out load)
uint16_t g_batFlags   = 0;      // BQ27220 Flags 0x0A

char g_destHex[17]   = "................";
char g_loraStr[12]   = "off";
char g_rssiStr[16]   = "-";
char g_snrStr[12]    = "-";
char g_pathsStr[8]   = "0";
char g_linksStr[8]   = "0";
char g_lxmfqStr[8]   = "0";
char g_convStr[24]   = "0 conv";
char g_unreadStr[16] = "0 unread";
char g_announceStr[20] = "never";
char g_txStr[24]     = "-";
char g_gpsTimeStr[20] = "--:--";
char g_gpsDateStr[16] = "--/--/--";
bool g_gpsHasFix = false;
bool g_gpsEnabled = true;
uint8_t g_gpsSats = 0;
char g_gpsStatus[20] = "off";
HardwareSerial* g_gpsSerial = nullptr;

constexpr int MSG_ROWS = 6;
struct MsgRow {
    char line1[28];
    char line2[28];
    uint8_t peer[16];
    uint32_t unread;
    bool used;
    bool saved;  // true = trusted contact
};
bool g_nameEditMode = false;  // Contacts: type name then Enter to save
MsgRow g_msgRows[MSG_ROWS];
int g_msgRowCount = 0;
int g_selPeer = 0;  // E20: selected MsgRow for TX / History / mark-read
char g_msgNote[40] = "";
bool g_msgRowsLoaded = false;
uint32_t g_msgCacheRevision = 0;

// Local display-name overrides: show "1> RADIO1" even if announce name is "1".
struct LocalName {
    uint8_t peer[16];
    char name[24];
    bool used;
};
LocalName g_localNames[8] = {};
void setLocalName(const uint8_t peer[16], const char* name) {
    if (!peer || !name || !name[0]) return;
    for (int i = 0; i < 8; i++) {
        if (g_localNames[i].used && memcmp(g_localNames[i].peer, peer, 16) == 0) {
            snprintf(g_localNames[i].name, sizeof(g_localNames[i].name), "%.23s", name);
            return;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (!g_localNames[i].used) {
            g_localNames[i].used = true;
            memcpy(g_localNames[i].peer, peer, 16);
            snprintf(g_localNames[i].name, sizeof(g_localNames[i].name), "%.23s", name);
            return;
        }
    }
}
const char* getLocalName(const uint8_t peer[16]) {
    if (!peer) return nullptr;
    for (int i = 0; i < 8; i++) {
        if (g_localNames[i].used && memcmp(g_localNames[i].peer, peer, 16) == 0)
            return g_localNames[i].name;
    }
    return nullptr;
}

char g_selfName[24] = "";
bool g_selfNameLoaded = false;

bool isLocalPeer(const uint8_t peer[16]) {
    if (!peer || !g_protoReady) return false;
    const uint8_t* loc = g_proto.localDestHash();
    return loc && memcmp(peer, loc, 16) == 0;
}

void loadSelfName() {
    if (g_selfNameLoaded) return;
    g_selfNameLoaded = true;
    Preferences prefs;
    if (prefs.begin("rs_self", true)) {
        String s = prefs.getString("name", "");
        if (s.length() > 0)
            snprintf(g_selfName, sizeof(g_selfName), "%.23s", s.c_str());
        prefs.end();
    }
}

void saveSelfName(const char* name) {
    if (!name) name = "";
    snprintf(g_selfName, sizeof(g_selfName), "%.23s", name);
    Preferences prefs;
    if (prefs.begin("rs_self", false)) {
        prefs.putString("name", g_selfName);
        prefs.end();
    }
    if (g_protoReady) {
        const uint8_t* loc = g_proto.localDestHash();
        if (loc) setLocalName(loc, g_selfName);
    }
    Serial.printf("[NAME] self -> \"%s\"\n", g_selfName);
}

constexpr int HIST_ROWS = 6;
struct HistRow {
    char line[36];   // "> HELLO" or "< hi"
    char st[12];     // out: sent/queued/delivered; in: ""
    bool incoming;
    bool used;
};
HistRow g_histRows[HIST_ROWS];
int g_histRowCount = 0;
bool g_showHistory = false;
char g_histNote[40] = "";

// E19: keyboard compose buffer (no per-key e-ink refresh).
constexpr size_t COMPOSE_CAP = 120;
char g_compose[COMPOSE_CAP + 1] = "";
size_t g_composeLen = 0;
bool g_composeDirty = false;
uint32_t g_lastComposeMs = 0;
constexpr uint32_t COMPOSE_IDLE_MS = 120;   // G1.49: fast partial compose strip
constexpr uint32_t COMPOSE_PARTIAL_MIN_MS = 80;

bool g_bootAnnDone = false;
uint32_t g_bootAnnAt = 0;

uint32_t g_lastUiRevision = 0;
uint32_t g_lastMsgRedrawMs = 0;
constexpr uint32_t MSG_REDRAW_MIN_MS = 2500;

void peerToHex32(const uint8_t peer[16], char out[33]);
void show_screen(ScreenId id);
void next_screen();
const char* tabLabel(ScreenId id);
void draw_nav();
void draw_nav_box(int x, int y, int w, int h, const char* label, bool active, char hotkey = 0);
void draw_compose_or_hint(const char* idleHint);
void build_chats();
void build_contacts();
void build_peers();
void doAnnounce();
void doSendTest(const char* body);
void loadMessageRows(bool force);
void appendAnnouncedPeers();  // E23: fill empty slots from AnnounceManager
void loadHistoryRows();
void composeClear();
void composeAppend(char c);
bool isNumEntryMode();
void syncNumLayer();
void composeBackspace();
void composeLog();
void clampSelPeer();
bool cycleSelPeer();
bool cst328Read(int16_t& x, int16_t& y);  // E22
bool handleTouchAt(int16_t x, int16_t y);  // true if already drew
void settingsGoBack();
void settingsActivate();
bool inMessageUi();
void goBackFromMessage();
void noteUserInput();
void draw_idle_screen();
void maybeEnterIdle();
void maybeBatteryPowerRedraw();
void spiBusIdle();
static const char* tzName();
static void formatLocalTime(char* out, size_t n);
static void formatLocalDate(char* out, size_t n);
void draw_battery_gauge(int x, int y, int w, int h, int pct);
void draw_battery_nav_icon(int x, int y, int w, int h, int pct);

void spiBusIdle() {
    pinMode(EPD_CS, OUTPUT); digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
#endif
}



// LiPo voltage → rough SoC (fallback / sanity check for gauge)
static int lipoPctFromVolts(float v) {
    // Piecewise linear for single-cell LiPo under light load
    static const float vp[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.15f, 4.20f};
    static const int   pp[] = {0, 5, 15, 30, 50, 65, 80, 90, 96, 100};
    if (v <= vp[0]) return 0;
    if (v >= vp[9]) return 100;
    for (int i = 0; i < 9; i++) {
        if (v <= vp[i + 1]) {
            float t = (v - vp[i]) / (vp[i + 1] - vp[i]);
            return pp[i] + (int)(t * (pp[i + 1] - pp[i]) + 0.5f);
        }
    }
    return 50;
}

static bool bqReadU16(uint8_t reg, uint16_t& out) {
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    out = (uint16_t)(Wire.read() | (Wire.read() << 8));
    return true;
}

bool readBattery(float& volts, int& pct) {
    // BQ27220 fuel gauge (I2C 0x55): use chip StateOfCharge as primary.
    // Standard cmds: Voltage 0x08, Flags 0x0A, AvgCurrent 0x14, StateOfCharge 0x2C.
    uint32_t mvSum = 0, socSum = 0;
    int nOk = 0, nSoc = 0;
    int32_t curSum = 0;
    int nCur = 0;
    uint16_t flags = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t mv = 0, soc = 0, curRaw = 0, fl = 0;
        if (!bqReadU16(0x08, mv)) continue;
        if (mv < 2500 || mv > 4500) continue;
        mvSum += mv;
        nOk++;
        if (bqReadU16(0x2C, soc) && soc <= 100) {
            socSum += soc;
            nSoc++;
        }
        if (bqReadU16(0x14, curRaw)) {
            curSum += (int16_t)curRaw;
            nCur++;
        }
        if (bqReadU16(0x0A, fl)) flags = fl;
        delay(2);
    }
    if (nOk == 0) return false;
    volts = (mvSum / (float)nOk) / 1000.0f;
    int16_t avgMa = (nCur > 0) ? (int16_t)(curSum / nCur) : 0;
    g_avgMa = avgMa;
    g_batFlags = flags;
    // BQ27220 Flags bit0 = DSG (1=discharging, 0=charge or relax)
    const bool dsg = (flags & 0x0001) != 0;
    // Charging: current into pack, or gauge says not-DSG with positive current.
    // Clear quickly on any clear discharge so unplug is not sticky.
    static bool s_chg = false;
    if (avgMa > 10 || (!dsg && avgMa > 3))
        s_chg = true;
    if (avgMa < -5 || dsg)
        s_chg = false;
    g_batCharging = s_chg;
    // USB: charging, or mild positive/float current. Unplug (~-15..-25 mA) clears.
    static bool s_usb = false;
    if (s_chg || avgMa > 8)
        s_usb = true;
    if (avgMa < -10 || (dsg && avgMa < 0))
        s_usb = false;
    g_usbPower = s_usb;
    static uint32_t s_lastLog = 0;
    if (millis() - s_lastLog > 5000) {
        s_lastLog = millis();
        Serial.printf("[BAT] V=%.3f pct=%d avgMa=%d chg=%d usb=%d dsg=%d flags=0x%04x\n",
                      volts, pct, (int)avgMa, (int)g_batCharging, (int)g_usbPower,
                      (int)dsg, (unsigned)flags);
    }

    // Prefer BQ27220 StateOfCharge (chip). Do NOT force 100% just because USB
    // is plugged in — that made the UI jump to 100% on charge.
    int gaugePct = (nSoc > 0) ? (int)(socSum / nSoc) : -1;
    int curvePct = lipoPctFromVolts(volts);
    if (gaugePct >= 0) {
        // Only blend if gauge is wildly off vs open-circuit voltage curve
        // (unlearned gauge). Never inflate to 100% on plug alone.
        if (abs(gaugePct - curvePct) > 45)
            pct = (gaugePct * 2 + curvePct) / 3;  // trust chip more
        else
            pct = gaugePct;
    } else {
        pct = curvePct;
        Serial.println("[BAT] SOC reg fail — voltage curve");
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return true;
}

bool fillDestFromNvs() {
    Preferences prefs;
    uint8_t priv[64] = {0};
    if (!prefs.begin(NVS_NS_IDENTITY, true)) return false;
    size_t n = prefs.getBytes("privkey", priv, 64);
    prefs.end();
    if (n != 64) return false;
    rs_handheld_rns_t* ctx = nullptr;
    if (rs_handheld_rns_init(&ctx) != RS_HANDHELD_OK || !ctx) return false;
    if (rs_handheld_rns_load_identity(ctx, priv) != RS_HANDHELD_OK) {
        rs_handheld_rns_shutdown(ctx);
        return false;
    }
    uint8_t dest[16];
    if (rs_handheld_rns_destination_hash(ctx, dest) != RS_HANDHELD_OK) {
        rs_handheld_rns_shutdown(ctx);
        return false;
    }
    for (int i = 0; i < 16; i++) sprintf(g_destHex + i * 2, "%02x", dest[i]);
    g_destHex[16] = '\0';
    rs_handheld_rns_shutdown(ctx);
    Serial.printf("[DEST] NVS %s\n", g_destHex);
    return true;
}

bool initRadio() {
    spiBusIdle();
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    delay(10);

    g_radio = new SX1262(&SPI, LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
                         LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
                         LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);
    if (!g_radio || !g_radio->begin(LORA_DEFAULT_FREQ)) {
        if (g_radio) { delete g_radio; g_radio = nullptr; }
        return false;
    }
    g_radio->setSpreadingFactor(LORA_DEFAULT_SF);
    g_radio->setSignalBandwidth(LORA_DEFAULT_BW);
    g_radio->setCodingRate4(LORA_DEFAULT_CR);
    g_radio->setTxPower(LORA_DEFAULT_TX_POWER);
    g_radio->setPreambleLength(LORA_DEFAULT_PREAMBLE);
    g_radio->receive();
    g_loraIf = new LoRaInterface(g_radio, "LoRa");
    if (!g_loraIf || !g_loraIf->start()) return false;
    Serial.printf("[LORA] online freq=%lu SF=%d\n",
                  (unsigned long)LORA_DEFAULT_FREQ, LORA_DEFAULT_SF);
    return true;
}

void logPsram() {
    // found=1 size=0 means the chip ID was seen but OPI PSRAM was not mapped.
    // Board must use memory_type=qio_opi + BOARD_HAS_PSRAM (see lilygo_t_deck_pro.json).
    bool found = psramFound();
    size_t sz = ESP.getPsramSize();
    size_t fr = ESP.getFreePsram();
    size_t capSp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t capIn = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[PSRAM] found=%d size=%u free=%u  caps_spiram=%u caps_int=%u\n",
                  (int)found, (unsigned)sz, (unsigned)fr,
                  (unsigned)capSp, (unsigned)capIn);
    if (found && sz == 0) {
        Serial.println("[PSRAM] WARN chip seen but size=0 - OPI not mapped");
        Serial.println("[PSRAM] Fix: board=lilygo_t_deck_pro memory_type=qio_qspi");
        Serial.println("[PSRAM]      (Pro uses QSPI PSRAM, not OPI qio_opi)");
    } else if (found && sz > 0) {
        Serial.printf("[PSRAM] OK mapped %u KB - protocol can use SPIRAM\n",
                      (unsigned)(sz / 1024));
    } else {
        Serial.println("[PSRAM] none - stuck on INTERNAL SMALL node");
    }
}

bool tryProto(uint32_t caps, const char* label) {
    Serial.printf("[PROTO] try %s free_int=%u\n", label,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return g_proto.begin(&g_flash, nullptr, &g_idMgr, &g_msgStore,
                         &g_announceMgr, RS_HANDHELD_PROFILE_SMALL, caps);
}

// When FlashStore::begin fails on corrupt media, format() is a no-op because
// it requires _ready==true. Bypass: erase SPIFFS/littlefs partition, format, remount.
bool recoverCorruptLittleFS() {
    Serial.println("[FLASH] recoverCorruptLittleFS...");
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "littlefs");
    if (!part)
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!part)
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) {
        Serial.println("[FLASH] no data/spiffs partition found");
        return false;
    }
    Serial.printf("[FLASH] erase label=%s size=%u offset=0x%x\n",
                  part->label, (unsigned)part->size, (unsigned)part->address);
    LittleFS.end();
    esp_err_t e = esp_partition_erase_range(part, 0, part->size);
    if (e != ESP_OK) {
        Serial.printf("[FLASH] erase failed err=0x%x\n", (unsigned)e);
        return false;
    }
    // format=true mounts a fresh FS on the blank partition
    if (!LittleFS.begin(true, "/littlefs", 10, part->label)) {
        Serial.println("[FLASH] LittleFS.begin(format) failed after erase");
        return false;
    }
    LittleFS.end();
    Serial.println("[FLASH] partition erased + formatted - calling FlashStore::begin");
    return g_flash.begin();
}

bool initStorageAndProto() {
    // Corrupted LittleFS → mount fails, FlashStore::format refuses (!_ready).
    if (!g_flash.begin()) {
        Serial.println("[FLASH] begin failed - partition erase recovery");
        if (!recoverCorruptLittleFS()) {
            Serial.println("[FLASH] recovery failed - no identity/announce");
            return false;
        }
        Serial.println("[FLASH] recovery OK - fresh FS");
    }
    g_idMgr.begin(&g_flash, nullptr);
    if (!g_msgStore.begin(&g_flash, nullptr, false)) return false;
    g_storeReady = true;
    g_announceMgr.setStorage(nullptr, &g_flash);
    g_announceMgr.loadNameCache();

    logPsram();

    const size_t psSize = ESP.getPsramSize();
    const size_t psFree = ESP.getFreePsram();
    if (psramFound() && psSize > 0 && psFree > 250000) {
        if (tryProto(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "SPIRAM|8BIT") ||
            tryProto(MALLOC_CAP_DEFAULT, "DEFAULT"))
            g_protoReady = true;
    }
    if (!g_protoReady) {
        if (tryProto(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "INTERNAL|8BIT") ||
            tryProto(MALLOC_CAP_INTERNAL, "INTERNAL") ||
            tryProto(MALLOC_CAP_8BIT, "8BIT"))
            g_protoReady = true;
    }

    if (g_protoReady) {
        if (g_loraIf) {
            g_proto.pump().attachLoRa(g_loraIf);
            g_announceMgr.setLoRaInterface(g_loraIf);
        }
        const uint8_t* dest = g_proto.localDestHash();
        if (dest) {
            for (int i = 0; i < 16; i++) sprintf(g_destHex + i * 2, "%02x", dest[i]);
            g_destHex[16] = '\0';
            g_announceMgr.setLocalDestHash(rs::Bytes(dest, 16));
        }
        Serial.printf("[PROTO] up dest=%s free_int=%u\n", g_destHex,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        Serial.println("[PROTO] off - store-only");
        fillDestFromNvs();
    }
    return true;
}

void doAnnounce() {
    if (!g_protoReady) {
        strcpy(g_announceStr, "proto off");
        Serial.println("[ANN] proto off");
        return;
    }
    static unsigned long lastMs = 0;
    if (lastMs != 0 && (millis() - lastMs) < 5000UL) {
        strcpy(g_announceStr, "wait 5s");
        Serial.println("[ANN] rate limited (5s)");
        return;
    }
    auto r = g_proto.announce(nullptr, 0);
    lastMs = millis();
    switch (r) {
        case ProtocolBackend::AnnounceResult::Sent:
            strcpy(g_announceStr, "sent");
            Serial.printf("[ANN] sent dest=%s\n", g_destHex);
            break;
        case ProtocolBackend::AnnounceResult::Deferred:
            strcpy(g_announceStr, "deferred");
            Serial.println("[ANN] deferred");
            break;
        default:
            strcpy(g_announceStr, "failed");
            Serial.println("[ANN] failed");
            break;
    }
}

void applyLoraPreset(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kLoraPresetCount) idx = kLoraPresetCount - 1;
    g_loraPreset = idx;
    const LoraPreset& pr = kLoraPresets[g_loraPreset];
    if (g_radio) {
        g_radio->setSpreadingFactor(pr.sf);
        g_radio->setSignalBandwidth(pr.bw);
        g_radio->setCodingRate4(pr.cr);
        Serial.printf("[LORA] preset=%s SF=%u BW=%.0f CR=%u\n",
                      pr.name, (unsigned)pr.sf, pr.bw, (unsigned)pr.cr);
        if (g_loraPreset != 0)
            Serial.println("[LORA] WARN non-default preset - peer may not decode (use Long Fast)");
    } else {
        Serial.printf("[LORA] preset=%s (radio null)\n", pr.name);
    }
}

void doSendTest(const char* body) {
    // Only from open Msg Box thread
    if (g_screen != SCR_CHATS || !g_showHistory) {
        Serial.println("[TX] send only from Msg Box thread");
        return;
    }
    if (g_screen == SCR_SETTINGS || g_screen == SCR_HOME) {
        Serial.println("[TX] ignore - not on Msg Box/Peers/Contacts");
        return;
    }
    if (!g_protoReady) {
        strcpy(g_txStr, "proto off");
        Serial.println("[TX] proto off");
        return;
    }
    clampSelPeer();
    if (g_msgRowCount == 0 || !g_msgRows[g_selPeer].used) {
        strcpy(g_txStr, "no peer");
        Serial.println("[TX] no peer - open Peers/Msg Box, press 0-9 or tap a row first");
        return;
    }
    if (isLocalPeer(g_msgRows[g_selPeer].peer)) {
        strcpy(g_txStr, "pick remote");
        Serial.println("[TX] cannot message THIS radio - select 1-9 remote");
        return;
    }
    static unsigned long lastTx = 0;
    if (lastTx && millis() - lastTx < 3000UL) {
        strcpy(g_txStr, "wait 3s");
        Serial.println("[TX] rate limited");
        return;
    }

    if (!body || !body[0]) body = "ping from tdeck-pro";
    char local[161];
    size_t n = strlen(body);
    if (n >= sizeof(local)) n = sizeof(local) - 1;
    memcpy(local, body, n);
    local[n] = '\0';
    while (n > 0 && (local[n - 1] == '\r' || local[n - 1] == '\n' || local[n - 1] == ' '))
        local[--n] = '\0';
    if (n == 0) {
        strcpy(local, "ping from tdeck-pro");
        n = strlen(local);
    }

    const uint8_t* dest = g_msgRows[g_selPeer].peer;
    {
        char hx[33];
        peerToHex32(dest, hx);
        Serial.printf("[TX] peerIdx=%d name=\"%s\" dest=%.16s\n",
                      g_selPeer, g_msgRows[g_selPeer].line1, hx);
            }
    char hex[33];
    peerToHex32(dest, hex);
    size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[TX] to %s body=\"%s\" free_int=%u\n", hex, local,
                  (unsigned)freeInt);

    // OutgoingContract::Rejection:
    // 0 None 1 Busy 2 Invalid 3 TooLarge 4 NoMemory 5 Unavailable
    // 6 Fenced 7 Exhausted 8 Recovering 9 Stopped
    auto rejectionName = [](unsigned r) -> const char* {
        switch (r) {
            case 0: return "None";
            case 1: return "Busy";
            case 2: return "Invalid";
            case 3: return "TooLarge";
            case 4: return "NoMemory";
            case 5: return "Unavailable";
            case 6: return "Fenced";
            case 7: return "Exhausted";
            case 8: return "Recovering";
            case 9: return "Stopped";
            default: return "?";
        }
    };
    // storage::Outcome: 0 Committed 1 Failed 2 Cancelled
    // storage::Error: 0 None 1 Unavailable 2 Allocation 3 InvalidRecord ...
    auto errorName = [](unsigned e) -> const char* {
        switch (e) {
            case 0: return "None";
            case 1: return "Unavailable";
            case 2: return "Allocation";
            case 3: return "InvalidRecord";
            case 4: return "CounterExhausted";
            case 5: return "RevisionExhausted";
            case 6: return "Read";
            case 7: return "Write";
            case 8: return "Verify";
            case 9: return "Rename";
            case 10: return "Stale";
            case 11: return "Cancelled";
            case 12: return "Internal";
            default: return "?";
        }
    };

    // Reclaim LVGL widgets so MessageStore CreateOutgoing can allocate
    if (g_root) {
        lv_obj_clean(g_root);
        Serial.printf("[TX] lv_obj_clean free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    delay(30);

    // Short recovery tick (boot drain should have finished Recovering)
    for (int i = 0; i < 30; i++) {
        g_proto.loop();
        if (g_storeReady) g_msgStore.poll();
        delay(10);
    }
    Serial.printf("[TX] pre-submit free_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    auto sub = g_proto.lxmfSubmit(dest, nullptr, 0,
                                  (const uint8_t*)local, n, false);
    if (!sub.accepted()) {
        unsigned r = (unsigned)sub.rejection;
        snprintf(g_txStr, sizeof(g_txStr), "rej %u", r);
        Serial.printf("[TX] rejected %u (%s) free_int=%u\n",
                      r, rejectionName(r),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (r == 8 || r == 1) {
            for (int attempt = 0; attempt < 12; attempt++) {
                for (int i = 0; i < 60; i++) {
                    g_proto.loop();
                    if (g_storeReady) g_msgStore.poll();
                    delay(20);
                }
                sub = g_proto.lxmfSubmit(dest, nullptr, 0,
                                         (const uint8_t*)local, n, false);
                if (sub.accepted()) {
                    Serial.printf("[TX] accepted after wait attempt %d\n", attempt + 1);
                    break;
                }
                Serial.printf("[TX] still %u (%s) attempt %d\n",
                              (unsigned)sub.rejection,
                              rejectionName((unsigned)sub.rejection),
                              attempt + 1);
            }
            if (!sub.accepted()) {
                Serial.println("[TX] give up - still Recovering/Busy");
                return;
            }
        } else {
            return;
        }
    }
    lastTx = millis();
    strcpy(g_txStr, "pending");

    for (int i = 0; i < 800; i++) {
        g_proto.loop();
        if (g_storeReady) g_msgStore.poll();
        handheld::outgoing::InitialResult ir{};
        auto p = g_proto.lxmfPoll(sub.ticket, ir);
        if (p == handheld::outgoing::Poll::Pending) {
            delay(10);
            continue;
        }
        if (p == handheld::outgoing::Poll::Ready) {
            unsigned oc = (unsigned)ir.outcome;
            unsigned er = (unsigned)ir.error;
            // Outcome: 0 Committed 1 Failed 2 Cancelled
            const char* ocName = oc == 0 ? "Committed" : oc == 1 ? "Failed" : oc == 2 ? "Cancelled" : "?";
            Serial.printf("[TX] outcome=%u (%s) err=%u (%s) suppressed=%d free_int=%u\n",
                          oc, ocName, er,
                          er == 0 ? "None" : er == 1 ? "Unavailable" : er == 2 ? "Allocation"
                          : er == 3 ? "InvalidRecord" : er == 7 ? "Write" : "?",
                          (int)ir.txSuppressed,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            g_proto.lxmfAcknowledge(sub.ticket);
            if (ir.outcome == handheld::storage::Outcome::Committed) {
                strcpy(g_txStr, ir.txSuppressed ? "saved" : "sent");
                g_msgRowsLoaded = false;
                if (g_featAudio) g_audio.playMessage();
                // N1/N2: refresh thread so new outbound appears with status
                if (g_showHistory) loadHistoryRows();
            } else if (ir.error == handheld::storage::Error::Allocation) {
                // Retry once after another clean - Allocation at ~12K is common
                Serial.println("[TX] Allocation fail - clean+retry once");
                if (g_root) lv_obj_clean(g_root);
                delay(50);
                for (int i = 0; i < 20; i++) {
                    g_proto.loop();
                    if (g_storeReady) g_msgStore.poll();
                    delay(10);
                }
                auto sub2 = g_proto.lxmfSubmit(dest, nullptr, 0,
                                               (const uint8_t*)local, n, false);
                if (!sub2.accepted()) {
                    snprintf(g_txStr, sizeof(g_txStr), "alloc rej");
                    Serial.printf("[TX] retry rejected %u free_int=%u\n",
                                  (unsigned)sub2.rejection,
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                    return;
                }
                for (int j = 0; j < 800; j++) {
                    g_proto.loop();
                    if (g_storeReady) g_msgStore.poll();
                    handheld::outgoing::InitialResult ir2{};
                    auto p2 = g_proto.lxmfPoll(sub2.ticket, ir2);
                    if (p2 == handheld::outgoing::Poll::Pending) { delay(10); continue; }
                    if (p2 == handheld::outgoing::Poll::Ready) {
                        g_proto.lxmfAcknowledge(sub2.ticket);
                        Serial.printf("[TX] retry outcome=%u err=%u free_int=%u\n",
                                      (unsigned)ir2.outcome, (unsigned)ir2.error,
                                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                        if (ir2.outcome == handheld::storage::Outcome::Committed) {
                            strcpy(g_txStr, ir2.txSuppressed ? "saved" : "sent");
                            g_msgRowsLoaded = false;
                            if (g_featAudio) g_audio.playMessage();
                            if (g_showHistory) loadHistoryRows();
                        } else {
                            snprintf(g_txStr, sizeof(g_txStr), "fail %u", (unsigned)ir2.error);
                        }
                        return;
                    }
                    break;
                }
                strcpy(g_txStr, "alloc timeout");
            } else {
                snprintf(g_txStr, sizeof(g_txStr), "fail %u", er);
            }
            return;
        }
        strcpy(g_txStr, "invalid");
        Serial.println("[TX] poll invalid");
        return;
    }
    strcpy(g_txStr, "timeout");
    Serial.println("[TX] timeout");
}

bool waitResult(MessageStore::Ticket ticket, MessageStore::Result& res, int maxSpin = 600) {
    for (int spin = 0; spin < maxSpin; spin++) {
        g_msgStore.poll();
        if (g_msgStore.peekResult(ticket, res)) return true;
        delay(5);
    }
    return false;
}

void peerToHex32(const uint8_t peer[16], char out[33]) {
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", peer[i]);
    out[32] = '\0';
}

std::string resolvePeerName(const uint8_t peer[16]) {
    // Priority: user override > name cache > announce node.name
    // Skip tiny placeholder announce names ("1") so list shows short id until renamed.
    auto usable = [](const std::string& n) -> bool {
        if (n.empty()) return false;
        if (n.size() <= 2) {
            // single digit / two-char placeholders from default display names
            bool allDigit = true;
            for (char c : n) if (c < '0' || c > '9') { allDigit = false; break; }
            if (allDigit) return false;
        }
        return true;
    };
    if (const char* loc = getLocalName(peer)) {
        if (loc[0]) return std::string(loc);
    }
    char hex[33];
    peerToHex32(peer, hex);
    std::string name = g_announceMgr.lookupName(hex);
    if (usable(name)) return name;
    char hexUp[33];
    for (int i = 0; i < 32; i++) {
        char c = hex[i];
        hexUp[i] = (c >= 'a' && c <= 'f') ? (char)(c - 32) : c;
    }
    hexUp[32] = 0;
    name = g_announceMgr.lookupName(hexUp);
    if (usable(name)) return name;
    const DiscoveredNode* n = g_announceMgr.findNode(rs::Bytes(peer, 16));
    if (n && usable(n->name)) return n->name;
    n = g_announceMgr.findNodeByHex(hex);
    if (n && usable(n->name)) return n->name;
    for (const auto& node : g_announceMgr.nodes()) {
        if (node.hash.size() == 16 && memcmp(node.hash.data(), peer, 16) == 0 &&
            usable(node.name))
            return node.name;
    }
    return {};
}

void formatPeerLine(char* out, size_t outLen, const uint8_t peer[16], uint32_t unread) {
    // Title: name only ("RADIO1"); list prefix draws ">1 "
    (void)unread;
    std::string name = resolvePeerName(peer);
    if (!name.empty()) {
        snprintf(out, outLen, "%.20s", name.c_str());
        return;
    }
    snprintf(out, outLen, "%02x%02x%02x%02x%02x%02x",
             peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
}

void formatPeerSub(char* out, size_t outLen, const uint8_t peer[16], const char* preview) {
    // Full address only on line 2
    (void)preview;
    char hex[33];
    peerToHex32(peer, hex);
    snprintf(out, outLen, "%.28s", hex);
}

void notifyInbound() {
    if (!g_featAudio) return;
    g_audio.requestMessage();
#if defined(BOARD_MOTOR_PIN) && (BOARD_MOTOR_PIN >= 0)
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    digitalWrite(BOARD_MOTOR_PIN, HIGH);
    delay(40);
    digitalWrite(BOARD_MOTOR_PIN, LOW);
#endif
}

void composeClear() {
    g_compose[0] = '\0';
    g_composeLen = 0;
    g_composeDirty = false;
    g_lastComposeMs = 0;
}

void composeLog() {
    Serial.printf("[COMPOSE] len=%u \"%s\"\n",
                  (unsigned)g_composeLen, g_compose);
}

void composeAppend(char c) {
    if (c < 32 || c > 126) return;
    // Text only on Msg Box / Contacts / Peers, or settings type-in pages
    const bool msgScreen = (g_screen == SCR_CHATS || g_screen == SCR_CONTACTS ||
                            g_screen == SCR_PEERS);
    // Only when actively typing into a field box (not merely viewing a menu page)
    const bool settingsType = (g_screen == SCR_SETTINGS &&
        (g_timeEditMode || g_dateEditMode || g_idleSecsEditMode));
    if (!msgScreen && !settingsType) {
        Serial.println("[COMPOSE] blocked on this screen");
        return;
    }
    if (g_idleActive) return;
    if (isNumEntryMode()) {
        if (c < '0' || c > '9') {
            snprintf(g_numsOnlyMsg, sizeof(g_numsOnlyMsg), "nums only");
            Serial.println("[COMPOSE] nums only");
            // brief redraw to show message in footer
            if (g_screen == SCR_SETTINGS) show_screen(SCR_SETTINGS);
            return;
        }
        g_numsOnlyMsg[0] = 0;
    }
    g_composeMode = true;
    if (g_composeLen >= COMPOSE_CAP) {
        Serial.println("[COMPOSE] full");
        return;
    }
    g_compose[g_composeLen++] = c;
    g_compose[g_composeLen] = '\0';
    g_composeDirty = true;
    g_lastComposeMs = millis();
    composeLog();
}

void composeBackspace() {
    if (g_composeLen == 0) return;
    g_compose[--g_composeLen] = '\0';
    g_composeDirty = true;
    g_lastComposeMs = millis();
    composeLog();
}

bool inMessageUi() {
    return g_showHistory || g_composeLen > 0 || g_composeMode || g_nameEditMode;
}

void goBackFromMessage() {
    bool changed = false;
    if (g_timeEditMode) {
        g_timeEditMode = false;
        composeClear();
        g_composeMode = false;
        show_screen(SCR_SETTINGS);
        return;
    }
    if (g_nameEditMode) {
        g_nameEditMode = false;
        composeClear();
        g_composeMode = false;
        changed = true;
        Serial.println("[UI] Back - cancel name edit");
    }
    if (g_composeLen > 0 || g_composeMode) {
        composeClear();
        g_composeMode = false;
        changed = true;
        Serial.println("[UI] Back - cleared compose");
    }
    if (g_showHistory) {
        g_showHistory = false;
        changed = true;
        Serial.println("[UI] Back - left thread");
    }
    if (changed) show_screen(g_screen);
}

// Unified Back: message UI, Settings sub-page, or stay

static void menuClose() {
    if (!g_menuOpen) return;
    g_menuOpen = false;
    Serial.println("[MENU] close");
}

static void menuOpen() {
    g_menuOpen = true;
    Serial.println("[MENU] open");
}

static void menuToggle() {
    if (g_menuOpen) menuClose();
    else menuOpen();
}

static ScreenId menuRowScreen(int row) {
    switch (row) {
        case 0: return SCR_HOME;
        case 1: return SCR_SETTINGS;
        case 2: return SCR_CONTACTS;
        case 3: return SCR_PEERS;
        default: return SCR_CHATS;  // Msg Box
    }
}

static void menuGo(ScreenId id) {
    menuClose();
    g_showHistory = false;
    g_nameEditMode = false;
    composeClear();
    g_composeMode = false;
    keyboardSetNumLayer(false);
    if (id == SCR_SETTINGS) {
        g_settingsPage = SET_ROOT;
        g_settingsSel = 0;
    }
    show_screen(id);
}

bool doBackAction() {
    if (g_menuOpen) {
        menuClose();
        show_screen(g_screen);
        return true;
    }
    if (inMessageUi()) {
        goBackFromMessage();
        return true;
    }
    if (g_screen == SCR_SETTINGS && g_settingsPage != SET_ROOT) {
        settingsGoBack();
        return true;
    }
    return false;
}

void clampSelPeer() {
    if (g_msgRowCount <= 0) {
        g_selPeer = 0;
        return;
    }
    if (g_selPeer < 0) g_selPeer = 0;
    if (g_selPeer >= g_msgRowCount) g_selPeer = g_msgRowCount - 1;
}

bool cycleSelPeer() {
    if (g_msgRowCount <= 0) {
        Serial.println("[PEER] no peers");
        return false;
    }
    g_selPeer = (g_selPeer + 1) % g_msgRowCount;
    clampSelPeer();
    char hex[33];
    peerToHex32(g_msgRows[g_selPeer].peer, hex);
    Serial.printf("[PEER] sel=%d/%d %s\n", g_selPeer, g_msgRowCount, hex);
    return true;
}

// Minimal CST328 (addr 0x1A) single-finger read. No SensorLib dependency.
// Register window starts at 0xD000; finger-1 state at first 5 bytes.
bool cst328Read(int16_t& x, int16_t& y) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write((uint8_t)0xD0);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)TOUCH_I2C_ADDR, 7) < 5) return false;
    uint8_t d0 = Wire.read();
    uint8_t d1 = Wire.read();
    uint8_t d2 = Wire.read();
    uint8_t d3 = Wire.read();
    (void)Wire.read();  // pressure
    while (Wire.available()) (void)Wire.read();
    if ((d0 & 0x0F) != 0x06) return false;  // pressed state
    x = (int16_t)((d1 << 4) | ((d3 >> 4) & 0x0F));
    y = (int16_t)((d2 << 4) | (d3 & 0x0F));
    return true;
}

// ---------------------------------------------------------------------------
// Layout (240x320 portrait) - two-row nav, boxed tabs, no page title
//
//   TOP_NAV  y=0 .. TOP_H-1     [ Home ] [ Settings ]   (2 boxes)
//   Content  y=CONTENT_TOP .. CONTENT_BOTTOM
//   Compose  just above BOT_NAV when non-empty
//   BOT_NAV  y=BOT_TOP .. EPD_HEIGHT-1
//            [ Chats ] [ Cont ] [ Peers ]               (3 boxes)
//
// Active tab: black filled box + white label (e-ink invert highlight).
// Touch hit-test must stay in sync with draw_nav().
// ---------------------------------------------------------------------------
constexpr int TOP_H          = 36;
constexpr int TOP_REFRESH_W  = 28;  // refresh icon
constexpr int TOP_BAT_W      = 28;  // vertical battery between refresh and Settings
constexpr int BOT_H          = 40;
constexpr int BOT_TOP        = EPD_HEIGHT - BOT_H;          // 280
constexpr int CONTENT_TOP    = TOP_H;                      // 36 - no gap under nav
constexpr int CONTENT_BOTTOM = BOT_TOP;                    // flush to bot nav
// Reserved band above bottom nav for compose / Enter=send (must not overlap BACK)
// 3-line compose band: 2 lines draft preview + 1 line [Enter] Enter=send
constexpr int COMPOSE_LINE_H  = 18;
constexpr int COMPOSE_STRIP_H = COMPOSE_LINE_H * 3;        // 54
constexpr int COMPOSE_STRIP_Y = BOT_TOP - COMPOSE_STRIP_H; // 226 when BOT_TOP=280
constexpr int THREAD_Y_MAX    = COMPOSE_STRIP_Y - 2;       // bubbles stop above strip
constexpr int TOP_TAB_W     = (EPD_WIDTH - TOP_REFRESH_W - TOP_BAT_W) / 2;  // clock | MENU
// Menu overlay column under the MENU tab (1 wide x 5 tall)
constexpr int MENU_ROW_H     = 36;
constexpr int MENU_ROWS      = 5;
constexpr int MENU_BOX_W     = TOP_TAB_W;
constexpr int MENU_BOX_X     = TOP_REFRESH_W + TOP_BAT_W + TOP_TAB_W;
constexpr int MENU_BOX_Y     = TOP_H;
// Bottom: Contact | Peers | Msg Box
// Bottom order: Contact | Peers | Msg Box
constexpr int BOT_CONTACT_W  = 80;
constexpr int BOT_MSGBOX_W   = 96;
constexpr int BOT_PEERS_W    = 64;   // 80+96+64=240
constexpr int BOT_CHATS_W    = BOT_MSGBOX_W;  // SCR_CHATS = Msg Box
constexpr int STATS_H        = 32;  // CONV / NEW / UNRD strip
constexpr int PEERS_TOP      = CONTENT_TOP + STATS_H + 4;
constexpr int ROW_H          = 44;
constexpr int SET_ROW_H      = 44;
constexpr int SETTINGS_HINT_H  = 12;
constexpr int SETTINGS_LIST_PAD = 4;
int g_settingsScroll = 0;
// Computed each draw; touch uses these so hits match boxes
int g_setListTop = 0;
int g_setListBot = 0;
int g_setMaxVis  = 4;

// Home grid - zero outer margin, 2px tile gap
constexpr int TILE_GAP       = 2;
constexpr int TILE_W         = (EPD_WIDTH - TILE_GAP * 2) / 3;  // 78
constexpr int TILE_H         = 42;
constexpr int HOME_ID_H      = 56;   // identity card height
constexpr int HOME_GRID_Y    = CONTENT_TOP + HOME_ID_H;    // 92
constexpr int HOME_ANN_H     = 34;

const char* tabLabel(ScreenId id) {
    switch (id) {
        case SCR_HOME:     return "Home";
        case SCR_CHATS:    return "Msg Box";
        case SCR_CONTACTS: return "Contact";
        case SCR_PEERS:    return "Peers";
        case SCR_SETTINGS: return "Settings";
        default:           return "?";
    }
}

bool isTopTab(ScreenId id) {
    return id == SCR_HOME || id == SCR_SETTINGS;
}

// Map top-row index 0..1 -> screen
ScreenId topTabScreen(int i) {
    // Left = Settings, Right = Home (right-thumb Home)
    // x already relative or absolute — refresh strip is not a tab
    return (i == 0) ? SCR_SETTINGS : SCR_HOME;
}

// Map bottom-row index 0..2 -> screen
ScreenId botTabScreen(int i) {
    // Contact | Peers | Msg Box
    if (i <= 0) return SCR_CONTACTS;
    if (i == 1) return SCR_PEERS;
    return SCR_CHATS;
}

bool handleTouchAt(int16_t x, int16_t y) {
    // Idle: ignore touch — keyboard only to wake
    if (g_idleActive) {
        Serial.println("[IDLE] touch ignored (press a key)");
        return true;
    }
    noteUserInput();
    Serial.printf("[TOUCH] xy=%d,%d screen=%u\n", (int)x, (int)y, (unsigned)g_screen);

    // Thread / name-edit: Back is bottom bar only (thumb)
    if (inMessageUi()) {
        if (y >= BOT_TOP) {
            Serial.println("[TAB] Back (message mode)");
            goBackFromMessage();
            return true;
        }
        // Top: refresh | clock | MENU (MENU opens overlay; does NOT jump Home)
        if (y < TOP_H) {
            if (x < TOP_REFRESH_W) {
                g_display.fullRefresh();
                return true;
            }
            int adj = x - TOP_REFRESH_W - TOP_BAT_W;
            if (adj >= TOP_TAB_W) {
                // Leave thread/rename first so MENU lists over the parent screen
                g_showHistory = false;
                g_nameEditMode = false;
                composeClear();
                g_composeMode = false;
                menuOpen();
                show_screen(g_screen);
                return true;
            }
            return true;  // clock: display only
        }
        return false;
    }

    // Top nav: Refresh | bat | clock | MENU
    if (y < TOP_H) {
        if (x < TOP_REFRESH_W) {
            Serial.println("[UI] touch fullRefresh");
            g_display.fullRefresh();
            return true;
        }
        int adj = x - TOP_REFRESH_W - TOP_BAT_W;
        if (adj >= TOP_TAB_W) {
            Serial.println("[TAB] top -> MENU");
            menuToggle();
            show_screen(g_screen);  // redraw with/without overlay
            return true;
        }
        return true;  // clock area: display only
    }

    // MENU overlay: column under MENU tab (Home / Settings / Contacts / Peers / Msg Box)
    if (g_menuOpen) {
        const int mx0 = MENU_BOX_X;
        const int mx1 = MENU_BOX_X + MENU_BOX_W;
        const int my0 = MENU_BOX_Y;
        const int my1 = MENU_BOX_Y + MENU_ROWS * MENU_ROW_H;
        if (x >= mx0 && x < mx1 && y >= my0 && y < my1) {
            int row = (y - my0) / MENU_ROW_H;
            if (row < 0) row = 0;
            if (row >= MENU_ROWS) row = MENU_ROWS - 1;
            ScreenId want = menuRowScreen(row);
            Serial.printf("[MENU] touch row=%d -> %s\n", row, tabLabel(want));
            menuGo(want);
            return true;
        }
        // Tap outside overlay closes it
        menuClose();
        show_screen(g_screen);
        return true;
    }

    // Bottom nav: Contact | Msg Box | Peers
    if (y >= BOT_TOP) {
        ScreenId want = SCR_CONTACTS;
        // Contact | Peers | Msg Box
        if (x < BOT_CONTACT_W)
            want = SCR_CONTACTS;
        else if (x < BOT_CONTACT_W + BOT_PEERS_W)
            want = SCR_PEERS;
        else
            want = SCR_CHATS;
        Serial.printf("[TAB] bot -> %s\n", tabLabel(want));
        if (want == g_screen) return true;
        g_showHistory = false;
        show_screen(want);
        return true;
    }

    // Settings list touch — use same geometry as build_settings()
    if (g_screen == SCR_SETTINGS) {
        int listTop = g_setListTop > 0 ? g_setListTop : (CONTENT_TOP + 30);
        int listBot = g_setListBot > 0 ? g_setListBot : (CONTENT_BOTTOM - 30);
        int maxVis  = g_setMaxVis > 0 ? g_setMaxVis : 4;
        if (y >= listTop && y < listBot) {
            int row = (y - listTop) / SET_ROW_H;
            if (row < 0) row = 0;
            if (row >= maxVis) row = maxVis - 1;
            int idx = g_settingsScroll + row;
            int maxIdx = 8;
            if (g_settingsPage == SET_TZ) maxIdx = kTzCount - 1;
            else if (g_settingsPage == SET_TIME) maxIdx = 4;
            else if (g_settingsPage == SET_IDLE_MODE) maxIdx = 3;
            else if (g_settingsPage == SET_SCREEN) maxIdx = 3;
            else if (g_settingsPage == SET_NETWORK) maxIdx = 4;
            else if (g_settingsPage == SET_MANUAL_TIME) maxIdx = 5;
            else if (g_settingsPage == SET_MANUAL_LOC) maxIdx = 2;
            else if (g_settingsPage == SET_IDENTITY) maxIdx = 2;
            else if (g_settingsPage == SET_IDLE_SECS) maxIdx = 1;
            if (idx > maxIdx) idx = maxIdx;
            g_settingsSel = idx;
            Serial.printf("[SET] touch sel=%d y=%d top=%d\n", g_settingsSel, (int)y, listTop);
            settingsActivate();
            return true;
        }
        return false;
    }

    // List row select — y origin matches each screen's drawn list
    if ((g_screen == SCR_CHATS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS) &&
        g_msgRowCount > 0 && y >= CONTENT_TOP + 2 && y < CONTENT_BOTTOM) {
        int listTop = CONTENT_TOP + 2;
        if (g_screen == SCR_CHATS && !g_showHistory)
            listTop = PEERS_TOP;           // Msg Box list under stats strip
        else if (g_screen == SCR_CONTACTS)
            listTop = CONTENT_TOP + 2 + 20; // under hint line
        else if (g_screen == SCR_PEERS)
            listTop = CONTENT_TOP + 24;     // under RECENT PEERS header
        int row = (y - listTop) / ROW_H;
        if (row < 0) row = 0;
        if (row >= g_msgRowCount) row = g_msgRowCount - 1;
        const int was = g_selPeer;
        g_selPeer = row;
        clampSelPeer();
        char hex[33];
        peerToHex32(g_msgRows[g_selPeer].peer, hex);
        Serial.printf("[PEER] touch sel=%d/%d (was %d) %s\n",
                      g_selPeer, g_msgRowCount, was, hex);
        // N1: on Msg Box, open thread for this peer
        if (g_screen == SCR_CHATS) {
            g_showHistory = true;
            loadHistoryRows();
            show_screen(SCR_CHATS);
            Serial.println("[PEER] Msg Box → thread");
            return true;
        }
        if (g_screen == SCR_CONTACTS) {
            // Open rename sub-screen for this contact (no type-on-list)
            g_nameEditMode = true;
            g_composeMode = true;
            composeClear();
            const char* cur = g_msgRows[g_selPeer].line1;
            if (cur && cur[0] && strcmp(cur, "THIS RADIO") != 0) {
                for (size_t i = 0; cur[i] && g_composeLen < COMPOSE_CAP; i++) {
                    char c = cur[i];
                    if (c >= 32 && c <= 126) {
                        g_compose[g_composeLen++] = c;
                        g_compose[g_composeLen] = '\0';
                    }
                }
            }
            show_screen(SCR_CONTACTS);
            Serial.println("[PEER] Contacts → rename");
            return true;
        }
        if (g_selPeer != was) {
            show_screen(g_screen);
            return true;
        }
        Serial.println("[PEER] selected - type message then Enter");
        return true;
    }

    // Home: Announce button
    // Compose Back (top-right above bottom nav)
    if ((g_composeLen > 0 || g_composeMode) &&
        y >= BOT_TOP - 36 && y < BOT_TOP) {
        composeClear();
        g_composeMode = false;
        Serial.println("[COMPOSE] back - leave message mode");
        show_screen(g_screen);
        return true;
    }

    // Home: status tiles toggle (3x2 grid) then Announce
    if (g_screen == SCR_HOME) {
        const int gy = HOME_GRID_Y;
        const int tw = TILE_W;
        const int th = TILE_H;
        const int gap = TILE_GAP;
        if (y >= gy && y < gy + 2 * (th + gap)) {
            int col = x / (tw + gap);
            if (col > 2) col = 2;
            int row = (y - gy) / (th + gap);
            if (row > 1) row = 1;
            int idx = row * 3 + col;
            // 0 LORA 1 TCP 2 WIFI 3 PEERS 4 AUDIO 5 GPS
            if (idx == 0) { g_featLora = !g_featLora; Serial.printf("[FEAT] LORA %s\n", g_featLora?"ON":"OFF"); }
            else if (idx == 1 || idx == 2) {
                Serial.printf("[FEAT] PATH/LINK status-only paths=%s links=%s\n",
                              g_pathsStr, g_linksStr);
            }
            else if (idx == 4) {
                g_featAudio = !g_featAudio;
                g_audio.setEnabled(g_featAudio);
                Serial.printf("[FEAT] AUDIO %s\n", g_featAudio?"ON":"OFF");
                if (g_featAudio) notifyInbound();
            }
            else if (idx == 5) {
                Serial.printf("[FEAT] LXMFQ=%s (status)\n", g_lxmfqStr);
            }
            // PEERS tile (3) status only
            show_screen(SCR_HOME);
            return true;
        }
        if (y >= gy + 2 * (th + gap) && y < BOT_TOP - 2) {
            Serial.println("[HOME] touch Announce");
            doAnnounce();
            if (g_featAudio) g_audio.playAnnounce();
            show_screen(SCR_HOME);
            return true;
        }
    }

    Serial.println("[TOUCH] content ignore");
    return true;
}

void applyView(const handheld::storage::ConversationView& v) {
    if (g_msgRowCount >= MSG_ROWS) return;
    MsgRow& r = g_msgRows[g_msgRowCount];
    r.used = true;
    r.saved = false;
    memcpy(r.peer, v.peer, 16);
    r.unread = v.unreadCount;
    formatPeerLine(r.line1, sizeof(r.line1), r.peer, r.unread);
    {
        char prev[28] = "";
        size_t pl = v.previewLength;
        if (pl >= sizeof(prev)) pl = sizeof(prev) - 1;
        if (pl > 0) {
            memcpy(prev, v.preview, pl);
            prev[pl] = '\0';
            for (size_t k = 0; k < pl; k++) {
                if ((unsigned char)prev[k] < 32 || (unsigned char)prev[k] > 126)
                    prev[k] = '?';
            }
        }
        formatPeerSub(r.line2, sizeof(r.line2), r.peer, prev[0] ? prev : nullptr);
    }
    char hex[33];
    peerToHex32(r.peer, hex);
    Serial.printf("[MSG] row peer=%s name=\"%s\" unread=%u\n",
                  hex, resolvePeerName(r.peer).c_str(), (unsigned)r.unread);
    g_msgRowCount++;
}

void applySelectorOnly(const handheld::storage::ConversationSelector& s) {
    if (g_msgRowCount >= MSG_ROWS) return;
    MsgRow& r = g_msgRows[g_msgRowCount];
    r.used = true;
    r.saved = false;
    memcpy(r.peer, s.cursor.peer, 16);
    r.unread = 0;
    formatPeerLine(r.line1, sizeof(r.line1), r.peer, 0);
    r.line2[0] = '\0';
    g_msgRowCount++;
}

bool tryConversationPage(uint8_t limit) {
    using namespace handheld::storage;
    auto sub = g_msgStore.requestConversationPage(
        {}, false, ConversationOrder::Recent, ConversationDirection::After, limit);
    for (int a = 0; a < 8 && !sub.accepted(); a++) {
        unsigned r = (unsigned)sub.rejection;
        if (r != 1 && r != 8) break;
        for (int i = 0; i < 50; i++) {
            if (g_protoReady) g_proto.loop();
            if (g_storeReady) g_msgStore.poll();
            delay(12);
        }
        sub = g_msgStore.requestConversationPage(
            {}, false, ConversationOrder::Recent, ConversationDirection::After, limit);
    }
    if (!sub.accepted()) {
        Serial.printf("[MSG] page rejected rejection=%u limit=%u\n",
                      (unsigned)sub.rejection, (unsigned)limit);
        return false;
    }
    Result res;
    if (!waitResult(sub.ticket, res)) {
        Serial.println("[MSG] page timeout");
        g_msgStore.releaseResult(sub.ticket);
        return false;
    }
    Serial.printf("[MSG] page outcome=%u err=%u len=%u total=%u limit=%u\n",
                  (unsigned)res.outcome, (unsigned)res.error,
                  (unsigned)res.length, (unsigned)res.total, (unsigned)limit);

    ConversationSelector selectors[MSG_ROWS];
    size_t nSel = 0;
    if (res.outcome == Outcome::Committed && res.error == Error::None &&
        res.length >= sizeof(ConversationSelector)) {
        nSel = res.length / sizeof(ConversationSelector);
        if (nSel > (size_t)MSG_ROWS) nSel = (size_t)MSG_ROWS;
        if (!g_msgStore.readPayload(sub.ticket, selectors,
                                    nSel * sizeof(ConversationSelector)))
            nSel = 0;
    }
    g_msgStore.releaseResult(sub.ticket);

    for (size_t i = 0; i < nSel && g_msgRowCount < MSG_ROWS; i++) {
        const ConversationSelector& sel = selectors[i];
        if (!sel.counter || sel.error != Error::None) {
            applySelectorOnly(sel);
            continue;
        }
        auto dsub = g_msgStore.requestConversation(sel);
        if (!dsub.accepted()) {
            applySelectorOnly(sel);
            continue;
        }
        Result dres;
        if (!waitResult(dsub.ticket, dres)) {
            g_msgStore.releaseResult(dsub.ticket);
            applySelectorOnly(sel);
            continue;
        }
        ConversationView view;
        bool got = (dres.outcome == Outcome::Committed &&
                    dres.error == Error::None &&
                    dres.length >= sizeof(ConversationView) &&
                    g_msgStore.readPayload(dsub.ticket, &view, sizeof(view)));
        g_msgStore.releaseResult(dsub.ticket);
        if (got) applyView(view);
        else applySelectorOnly(sel);
    }
    return g_msgRowCount > 0;
}

void loadHistoryRows() {
    g_histRowCount = 0;
    g_histNote[0] = '\0';
    for (int i = 0; i < HIST_ROWS; i++) {
        g_histRows[i].used = false;
        g_histRows[i].incoming = false;
        g_histRows[i].line[0] = '\0';
        g_histRows[i].st[0] = '\0';
    }

    clampSelPeer();
    if (!g_storeReady || g_msgRowCount == 0 || !g_msgRows[g_selPeer].used) {
        snprintf(g_histNote, sizeof(g_histNote), "(no peer)");
        return;
    }

    char hex[33];
    peerToHex32(g_msgRows[g_selPeer].peer, hex);
    std::string peerHex(hex);
    Serial.printf("[HIST] load peer=%s free_int=%u\n", hex,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Mark conversation read (async ticket path — sync API is deferred-only).
    {
        auto mr = g_msgStore.requestMarkRead(peerHex);
        if (mr.accepted()) {
            MessageStore::Result mres;
            if (waitResult(mr.ticket, mres, 400)) {
                Serial.printf("[HIST] markRead outcome=%u err=%u\n",
                              (unsigned)mres.outcome, (unsigned)mres.error);
            }
            g_msgStore.releaseResult(mr.ticket);
            g_msgStore.poll();
        } else {
            Serial.printf("[HIST] markRead rejected %u\n", (unsigned)mr.rejection);
        }
        g_msgRows[g_selPeer].unread = 0;  // UI immediately
    }

    using namespace handheld::storage;
    auto sub = g_msgStore.requestHistoryPage(
        peerHex, HistoryEntry{}, (uint8_t)HIST_ROWS, HistoryDirection::Before);
    for (int a = 0; a < 8 && !sub.accepted(); a++) {
        unsigned r = (unsigned)sub.rejection;
        if (r != 1 && r != 8) break;
        for (int i = 0; i < 50; i++) {
            if (g_protoReady) g_proto.loop();
            if (g_storeReady) g_msgStore.poll();
            delay(12);
        }
        sub = g_msgStore.requestHistoryPage(
            peerHex, HistoryEntry{}, (uint8_t)HIST_ROWS, HistoryDirection::Before);
    }
    if (!sub.accepted()) {
        Serial.printf("[HIST] page rejected %u\n", (unsigned)sub.rejection);
        snprintf(g_histNote, sizeof(g_histNote), "(hist busy)");
        return;
    }

    Result res;
    if (!waitResult(sub.ticket, res)) {
        g_msgStore.releaseResult(sub.ticket);
        snprintf(g_histNote, sizeof(g_histNote), "(hist timeout)");
        return;
    }
    Serial.printf("[HIST] page outcome=%u err=%u len=%u total=%u\n",
                  (unsigned)res.outcome, (unsigned)res.error,
                  (unsigned)res.length, (unsigned)res.total);

    HistoryEntry entries[HIST_ROWS];
    size_t nEnt = 0;
    if (res.outcome == Outcome::Committed && res.error == Error::None &&
        res.length >= sizeof(HistoryEntry)) {
        nEnt = res.length / sizeof(HistoryEntry);
        if (nEnt > (size_t)HIST_ROWS) nEnt = (size_t)HIST_ROWS;
        if (!g_msgStore.readPayload(sub.ticket, entries, nEnt * sizeof(HistoryEntry)))
            nEnt = 0;
    }
    g_msgStore.releaseResult(sub.ticket);

    for (size_t i = 0; i < nEnt && g_histRowCount < HIST_ROWS; i++) {
        RecordKey key{};
        memcpy(key.peer, g_msgRows[g_selPeer].peer, 16);
        key.counter = entries[i].counter;
        key.incoming = entries[i].incoming;

        // Header alone is ~104B; need room for a short title+body snippet.
        constexpr uint16_t kRecCap = 256;
        auto rsub = g_msgStore.requestRecord(key, 0, kRecCap);
        if (!rsub.accepted()) {
            Serial.printf("[HIST] record rej ctr=%u in=%u rej=%u\n",
                          (unsigned)entries[i].counter, (unsigned)entries[i].incoming,
                          (unsigned)rsub.rejection);
            HistRow& h = g_histRows[g_histRowCount++];
            h.used = true;
            snprintf(h.line, sizeof(h.line), "%s#%u",
                     entries[i].incoming ? "<" : ">", (unsigned)entries[i].counter);
            continue;
        }
        Result rres;
        if (!waitResult(rsub.ticket, rres) ||
            rres.outcome != Outcome::Committed || rres.error != Error::None) {
            Serial.printf("[HIST] record fail ctr=%u out=%u err=%u\n",
                          (unsigned)entries[i].counter,
                          (unsigned)rres.outcome, (unsigned)rres.error);
            g_msgStore.releaseResult(rsub.ticket);
            HistRow& h = g_histRows[g_histRowCount++];
            h.used = true;
            snprintf(h.line, sizeof(h.line), "%s#%u",
                     entries[i].incoming ? "<" : ">", (unsigned)entries[i].counter);
            continue;
        }

        uint8_t buf[256];
        size_t want = rres.length < sizeof(buf) ? rres.length : sizeof(buf);
        bool ok = g_msgStore.readPayload(rsub.ticket, buf, want);
        g_msgStore.releaseResult(rsub.ticket);

        HistRow& h = g_histRows[g_histRowCount++];
        h.used = true;
        const char* arrow = entries[i].incoming ? "<" : ">";
        if (!ok || want < sizeof(StoredRecordHeader)) {
            Serial.printf("[HIST] short payload len=%u need=%u\n",
                          (unsigned)want, (unsigned)sizeof(StoredRecordHeader));
            snprintf(h.line, sizeof(h.line), "%s#%u", arrow, (unsigned)entries[i].counter);
            continue;
        }
        StoredRecordHeader hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        size_t off = sizeof(StoredRecordHeader);
        if (off + hdr.titleLength <= want) off += hdr.titleLength;
        else off = want;
        size_t clen = hdr.contentLength;
        if (off + clen > want) clen = want > off ? want - off : 0;
        char snip[22];
        size_t copy = clen < sizeof(snip) - 1 ? clen : sizeof(snip) - 1;
        if (copy) memcpy(snip, buf + off, copy);
        snip[copy] = '\0';
        for (size_t k = 0; k < copy; k++) {
            if ((unsigned char)snip[k] < 32 || (unsigned char)snip[k] > 126)
                snip[k] = '?';
        }
        h.incoming = entries[i].incoming;
        if (copy)
            snprintf(h.line, sizeof(h.line), "%s %s", arrow, snip);
        else
            snprintf(h.line, sizeof(h.line), "%s#%u", arrow, (unsigned)entries[i].counter);
        // N2: coarse status from stored LXMF status byte (outbound only)
        h.st[0] = '\0';
        if (!entries[i].incoming) {
            // LXMFStatus-ish: 0 queued 1 sending 2 sent 3 delivered 4 failed ...
            switch (hdr.status) {
                case 0: strcpy(h.st, "queued"); break;
                case 1: strcpy(h.st, "sending"); break;
                case 2: strcpy(h.st, "sent"); break;
                case 3: strcpy(h.st, "delivered"); break;
                case 4: strcpy(h.st, "sent"); break;  // air-OK; treat as sent
                default: snprintf(h.st, sizeof(h.st), "s%u", (unsigned)hdr.status); break;
            }
        }
        Serial.printf("[HIST] %s#%u \"%s\" st=%s tlen=%u clen=%u\n",
                      arrow, (unsigned)entries[i].counter, snip, h.st,
                      (unsigned)hdr.titleLength, (unsigned)hdr.contentLength);
    }

    if (g_histRowCount == 0)
        snprintf(g_histNote, sizeof(g_histNote), "(no history)");
    else
        Serial.printf("[HIST] loaded %d rows\n", g_histRowCount);
}

// E23: Top up Messages rows from recently announced peers so the user can
// select one and send a keyboard reply even with no prior conversation.
void appendAnnouncedPeers() {
    if (g_msgRowCount >= MSG_ROWS) return;

    const uint8_t* local = nullptr;
    if (g_protoReady) local = g_proto.localDestHash();

    const auto& nodes = g_announceMgr.nodes();
    int added = 0;
    for (const auto& node : nodes) {
        if (g_msgRowCount >= MSG_ROWS) break;
        if (node.hash.size() != 16) continue;
        if (local && memcmp(node.hash.data(), local, 16) == 0) continue;

        bool already = false;
        for (int i = 0; i < g_msgRowCount; i++) {
            if (g_msgRows[i].used &&
                memcmp(g_msgRows[i].peer, node.hash.data(), 16) == 0) {
                already = true;
                break;
            }
        }
        if (already) continue;

        MsgRow& r = g_msgRows[g_msgRowCount];
        r.used = true;
        r.saved = false;
        memcpy(r.peer, node.hash.data(), 16);
        r.unread = 0;
        formatPeerLine(r.line1, sizeof(r.line1), r.peer, 0);
        {
            char hex[33];
            peerToHex32(r.peer, hex);
            if (const char* loc = getLocalName(r.peer)) {
                snprintf(r.line1, sizeof(r.line1), "%.20s", loc);
                r.saved = true;
            } else {
                std::string cached = g_announceMgr.lookupName(hex);
                if (!cached.empty())
                    snprintf(r.line1, sizeof(r.line1), "%.20s", cached.c_str());
            }
            if (node.rssi != 0)
                snprintf(r.line2, sizeof(r.line2), "ID:%.12s %ddBm", hex, node.rssi);
            else
                snprintf(r.line2, sizeof(r.line2), "ID: %.16s", hex);
        }
        g_msgRowCount++;
        added++;
        char hex[33];
        peerToHex32(r.peer, hex);
        Serial.printf("[MSG] ann peer=%s name=\"%s\" rssi=%d\n",
                      hex, node.name.empty() ? "-" : node.name.c_str(), node.rssi);
    }
    if (added)
        Serial.printf("[MSG] appended %d announced peer(s) total=%d\n",
                      added, g_msgRowCount);
}

void loadMessageRows(bool force) {
    if (g_storeReady) g_msgStore.poll();
    const uint32_t rev = g_storeReady ? g_msgStore.revision() : 0;

    if (g_msgRowsLoaded && !force && g_msgRowCount > 0 && rev == g_msgCacheRevision) {
        Serial.printf("[MSG] using cached %d rows rev=%u\n", g_msgRowCount, (unsigned)rev);
        // E23: still top up empty slots from newly announced peers
        const int before = g_msgRowCount;
        appendAnnouncedPeers();
        if (g_msgRowCount > before) clampSelPeer();
        return;
    }
    if (g_msgRowsLoaded && rev != g_msgCacheRevision)
        Serial.printf("[MSG] cache stale rev %u -> %u\n",
                      (unsigned)g_msgCacheRevision, (unsigned)rev);

    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) { g_msgRows[i].used = false; g_msgRows[i].saved = false; }

    Serial.printf("[MSG] load free_int=%u rev=%u store=%d\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)rev, (int)g_storeReady);

    if (g_storeReady) {
        if (!tryConversationPage(MSG_ROWS)) tryConversationPage(1);
    }

    // E23: fill remaining rows from AnnounceManager nodes (keyboard TX targets)
    appendAnnouncedPeers();

    if (g_msgRowCount == 0 && g_storeReady) {
        auto ids = g_msgStore.startupRecentMessageIds(MSG_ROWS);
        for (size_t i = 0; i < ids.size() && g_msgRowCount < MSG_ROWS; i++) {
            MsgRow& r = g_msgRows[g_msgRowCount];
            r.used = true;
            r.saved = false;
            memset(r.peer, 0, 16);
            r.unread = 0;
            snprintf(r.line1, sizeof(r.line1), "msg %.12s", ids[i].c_str());
            r.line2[0] = '\0';
            g_msgRowCount++;
        }
    }

    if (g_msgRowCount == 0) {
        int nAnn = g_announceMgr.nodeCount();
        if (g_storeReady) {
            uint32_t c = g_msgStore.totalConversations();
            int u = g_msgStore.totalUnreadCount();
            if (c > 0)
                snprintf(g_msgNote, sizeof(g_msgNote), "(%u stored, list empty)", (unsigned)c);
            else if (u > 0)
                snprintf(g_msgNote, sizeof(g_msgNote), "(%d unread, list empty)", u);
            else if (nAnn > 0)
                snprintf(g_msgNote, sizeof(g_msgNote), "(%d ann, wait list)", nAnn);
            else
                snprintf(g_msgNote, sizeof(g_msgNote), "(no peers yet)");
        } else if (nAnn > 0) {
            snprintf(g_msgNote, sizeof(g_msgNote), "(%d ann, no store)", nAnn);
        } else {
            snprintf(g_msgNote, sizeof(g_msgNote), "(no peers yet)");
        }
        g_msgRowsLoaded = false;
    } else {
        g_msgRowsLoaded = true;
        g_msgCacheRevision = g_storeReady ? g_msgStore.revision() : 0;
        clampSelPeer();
        Serial.printf("[MSG] loaded %d rows rev=%u sel=%d\n",
                      g_msgRowCount, (unsigned)g_msgCacheRevision, g_selPeer);
    }
}

void refreshLiveData() {
    float v = 0; int pct = -1;
    if (readBattery(v, pct)) {
        snprintf(g_batStr, sizeof(g_batStr), "%.2f V", v);
        snprintf(g_pctStr, sizeof(g_pctStr), "%d%%", pct);
    } else {
        strcpy(g_batStr, "n/a"); strcpy(g_pctStr, "--%");
    }
    if (g_loraIf && g_loraIf->isOnline()) {
        strcpy(g_loraStr, "online");
        int rssi = g_loraIf->lastRxRssi();
        if (rssi == 0 && g_radio) rssi = g_radio->currentRssi();
        snprintf(g_rssiStr, sizeof(g_rssiStr), "%d dBm", rssi);
        float snr = g_loraIf->lastRxSnr();
        if (snr != 0.0f) snprintf(g_snrStr, sizeof(g_snrStr), "%.1f dB", snr);
        else strcpy(g_snrStr, "-");
    } else if (g_radio && g_radio->isRadioOnline()) {
        strcpy(g_loraStr, "online");
        snprintf(g_rssiStr, sizeof(g_rssiStr), "%d dBm", g_radio->currentRssi());
        strcpy(g_snrStr, "-");
    } else {
        strcpy(g_loraStr, "off"); strcpy(g_rssiStr, "-"); strcpy(g_snrStr, "-");
    }
    if (g_protoReady) {
        snprintf(g_pathsStr, sizeof(g_pathsStr), "%u", (unsigned)g_proto.pathCount());
        snprintf(g_linksStr, sizeof(g_linksStr), "%u", (unsigned)g_proto.linkCount());
        snprintf(g_lxmfqStr, sizeof(g_lxmfqStr), "%d", g_proto.lxmfQueuedCount());
    }
    if (g_storeReady) {
        g_msgStore.poll();
        snprintf(g_convStr, sizeof(g_convStr), "%u conv",
                 (unsigned)g_msgStore.totalConversations());
        snprintf(g_unreadStr, sizeof(g_unreadStr), "%d unread",
                 g_msgStore.totalUnreadCount());
    }
}

void fat_label(lv_obj_t* parent, const char* txt, lv_coord_t x, lv_coord_t y) {
    // Dark theme: triple-draw (+0,+1x,+1y) so white glyphs stay readable on e-ink
    const int passes = g_darkTheme ? 3 : 1;
    static const int8_t ox[] = {0, 1, 0};
    static const int8_t oy[] = {0, 0, 1};
    for (int k = 0; k < passes; k++) {
        lv_obj_t* o = lv_label_create(parent);
        lv_label_set_text(o, txt ? txt : "");
        lv_obj_set_style_text_font(o, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(o, theme_fg(), 0);
        lv_obj_set_pos(o, x + ox[k], y + oy[k]);
    }
}

void clear_screen() {
    g_display.fillScreen(g_darkTheme);
    if (g_root) {
        lv_obj_clean(g_root);
        lv_obj_set_style_bg_color(g_root, theme_bg(), 0);
        lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    } else {
        g_root = lv_scr_act();
        lv_obj_set_style_bg_color(g_root, theme_bg(), 0);
        lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(g_root, 0, 0);
        lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    }
}



// Centered label - use LVGL text width when possible for true center
void draw_label_centered(int x, int y, int w, int h, const char* label, bool bold) {
    const lv_font_t* font = &lv_font_montserrat_16;
    lv_coord_t tw = lv_txt_get_width(label, strlen(label), font, 0, LV_TEXT_FLAG_NONE);
    if (tw <= 0) tw = (lv_coord_t)(strlen(label) * 7);
    int lx = x + (w - (int)tw) / 2;
    if (lx < x) lx = x;
    int ly = y + (h - 16) / 2;
    if (ly < y) ly = y;
    const int passes = bold ? (g_darkTheme ? 3 : 2) : 1;
    static const int8_t ox[] = {0, 1, 0};
    static const int8_t oy[] = {0, 0, 1};
    for (int k = 0; k < passes; k++) {
        lv_obj_t* lab = lv_label_create(g_root);
        lv_label_set_text(lab, label);
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, lx + ox[k], ly + oy[k]);
    }
}

// G1.49: stamp 1-bit PROGMEM logo from RatSpeak_Logo.png into DisplayEink FB.
// PNG is dark-bg + light mark; packed with 1=black. Dark splash = black paper.
static void draw_ratspeak_logo_bitmap(int cx, int cy) {
    const int w = RATSPEAK_LOGO_W;
    const int h = RATSPEAK_LOGO_H;
    const int x0 = cx - w / 2;
    const int y0 = cy - h / 2;
    const int rowBytes = (w + 7) / 8;

    // Invert: PNG is dark-bg + light mark. On black splash paper we want the
    // mark white (panelBlack=false) and keep paper black. Source 1=dark.
    // Treat source black as "paper" (black) and source white as mark (white).
    // Additionally force any source-white pixel to white for visibility.
    for (int y = 0; y < h; y++) {
        for (int xb = 0; xb < rowBytes; xb++) {
            uint8_t byte = pgm_read_byte(&RATSPEAK_LOGO_BITS[y * rowBytes + xb]);
            for (int bit = 0; bit < 8; bit++) {
                int x = xb * 8 + bit;
                if (x >= w) break;
                bool srcBlack = (byte & (0x80 >> bit)) != 0;
                // Dark theme: keep PNG polarity. Light theme: invert so mark is black on white.
                bool panelBlack = g_darkTheme ? srcBlack : !srcBlack;
                int px = x0 + x;
                int py = y0 + y;
                if (px < 0 || py < 0 || px >= (int)EPD_WIDTH || py >= (int)EPD_HEIGHT)
                    continue;
                g_display.setPixel((uint16_t)px, (uint16_t)py, panelBlack);
            }
        }
    }
    g_display.markDirty();
}


bool isNumEntryMode() {
    if (g_screen != SCR_SETTINGS) return false;
    if (g_settingsPage == SET_IDLE_SECS && g_idleSecsEditMode) return true;
    if (g_settingsPage == SET_MANUAL_TIME && g_timeEditMode) return true;
    if (g_settingsPage == SET_MANUAL_DATE && g_dateEditMode) return true;
    return false;
}

void syncNumLayer() {
    keyboardSetNumLayer(isNumEntryMode());
}

void noteUserInput() {
    g_lastInputMs = millis() ? millis() : 1;
    if (g_idleActive) {
        g_idleActive = false;
        // Drop any stale compose; keep g_screen and thread state as they were
        composeClear();
        g_composeMode = false;
        Serial.printf("[IDLE] wake -> screen %u\n", (unsigned)g_screen);
        show_screen(g_screen);  // same page as before idle
    }
}

// paintIdleContent: shared by enter (full) and update (partial)
static void paintIdleContent() {
    // Always full clear — prevents UP/% ghosts from partials
    clear_screen();

    // Title
    draw_label_centered(0, 78, EPD_WIDTH, 22, "RatSpeak", true);

    // Date + time on separate wider lines (easier to read)
    char lt[12], ld[12];
    formatLocalTime(lt, sizeof(lt));
    formatLocalDate(ld, sizeof(ld));
    char dateLine[32], timeLine[32];
    {
        size_t j = 0;
        for (size_t i = 0; ld[i] && j + 2 < sizeof(dateLine); i++) {
            dateLine[j++] = ld[i];
            if (ld[i+1]) dateLine[j++] = ' ';
        }
        dateLine[j] = 0;
    }
    {
        char tmp[20];
        snprintf(tmp, sizeof(tmp), "%s %s", lt, tzName());
        size_t j = 0;
        for (size_t i = 0; tmp[i] && j + 2 < sizeof(timeLine); i++) {
            timeLine[j++] = tmp[i];
            if (tmp[i+1]) timeLine[j++] = ' ';
        }
        timeLine[j] = 0;
    }
    draw_label_centered(0, 102, EPD_WIDTH, 22, dateLine, true);
    draw_label_centered(0, 124, EPD_WIDTH, 22, timeLine, true);

    int pct = -1;
    if (g_pctStr[0] && g_pctStr[0] != '-')
        pct = atoi(g_pctStr);
    // gauge draws the single % — do not draw another
    draw_battery_gauge(EPD_WIDTH / 2 - 40, 152, 36, 16, pct);

    unsigned unread = 0;
    if (g_storeReady)
        unread = (unsigned)g_msgStore.totalUnreadCount();
    char msgLine[40];
    if (unread > 0)
        snprintf(msgLine, sizeof(msgLine), "NEW messages: %u unread", unread);
    else
        snprintf(msgLine, sizeof(msgLine), "No new messages");
    draw_label_centered(0, 188, EPD_WIDTH, 18, msgLine, unread > 0);

    draw_label_centered(0, 220, EPD_WIDTH, 20, "press a KEY to wake", false);

    uint32_t sec = millis() / 1000UL;
    unsigned days = (unsigned)(sec / 86400UL);
    unsigned hrs  = (unsigned)((sec / 3600UL) % 24UL);
    unsigned mins = (unsigned)((sec / 60UL) % 60UL);
    // Two centered lines:
    //   uptime DDDDD:HH:MM   (format label)
    //   #####:##:##          (actual value, 5-digit days)
    char upVal[20], upValSp[40];
    snprintf(upVal, sizeof(upVal), "%05u:%02u:%02u", days, hrs, mins);
    {
        size_t j = 0;
        for (size_t i = 0; upVal[i] && j + 2 < sizeof(upValSp); i++) {
            upValSp[j++] = upVal[i];
            if (upVal[i + 1]) upValSp[j++] = ' ';
        }
        upValSp[j] = 0;
    }
    draw_label_centered(0, 248, EPD_WIDTH, 18, "uptime DDDDD:HH:MM", true);
    draw_label_centered(0, 268, EPD_WIDTH, 18, upValSp, true);

    lv_refr_now(nullptr);
    // Stamp logo AFTER LVGL so it is not wiped by the canvas clear
    draw_ratspeak_logo_bitmap(EPD_WIDTH / 2, 36);
    g_idleLastUnread = (int)unread;
    g_idleLastPct = pct;
}

void draw_idle_screen() {
    paintIdleContent();
    // Full refresh only on enter — clears ghosting from previous UI
    g_display.fullRefresh();
    Serial.println("[IDLE] fullRefresh done");
}

// While idle: partial redraw if unread or battery % changed
void maybeUpdateIdle() {
    if (!g_idleActive) return;
    refreshLiveData();
    int pct = -1;
    if (g_pctStr[0] && g_pctStr[0] != '-')
        pct = atoi(g_pctStr);
    int unread = g_storeReady ? (int)g_msgStore.totalUnreadCount() : 0;
    // Minute tick — update clock + uptime
    static uint32_t s_lastMin = 0;
    uint32_t minNow = millis() / 60000UL;
    bool minTick = (minNow != s_lastMin);
    if (!minTick && pct == g_idleLastPct && unread == g_idleLastUnread)
        return;
    s_lastMin = minNow;
    paintIdleContent();
    g_display.refreshIfDirty();  // partial / localized only
    Serial.printf("[IDLE] update unread=%d pct=%d minTick=%d\n", unread, pct, (int)minTick);
}

void maybeHomeMinuteRedraw() {
    if (g_idleActive) return;
    if (g_screen != SCR_HOME) return;
    if (g_composeLen > 0 || g_composeMode) return;
    static uint32_t s_homeMin = 0;
    uint32_t minNow = millis() / 60000UL;
    if (minNow == s_homeMin) return;
    s_homeMin = minNow;
    refreshLiveData();
    show_screen(SCR_HOME);
    Serial.println("[HOME] minute redraw");
}

void maybeBatteryPowerRedraw() {
    // Poll gauge every ~2s; full redraw when charging or USB presence flips
    static uint32_t s_last = 0;
    static bool s_init = false;
    static bool s_prevChg = false;
    static bool s_prevUsb = false;
    uint32_t now = millis();
    if (now - s_last < 2000) return;
    s_last = now;

    bool prevChg = g_batCharging;
    bool prevUsb = g_usbPower;
    float v = 0;
    int pct = -1;
    if (!readBattery(v, pct)) return;
    snprintf(g_batStr, sizeof(g_batStr), "%.2f V", v);
    if (pct >= 0) snprintf(g_pctStr, sizeof(g_pctStr), "%d%%", pct);
    else strcpy(g_pctStr, "--%");

    if (!s_init) {
        s_init = true;
        s_prevChg = g_batCharging;
        s_prevUsb = g_usbPower;
        return;
    }
    if (g_batCharging == s_prevChg && g_usbPower == s_prevUsb)
        return;
    Serial.printf("[BAT] power change chg %d->%d usb %d->%d — redraw\n",
                  (int)s_prevChg, (int)g_batCharging,
                  (int)s_prevUsb, (int)g_usbPower);
    s_prevChg = g_batCharging;
    s_prevUsb = g_usbPower;

    // Do not clobber active typing
    if (g_composeLen > 0 || g_composeMode || g_nameEditMode ||
        g_timeEditMode || g_dateEditMode || g_idleSecsEditMode)
        return;

    if (g_idleActive) {
        paintIdleContent();
        g_display.partialRefresh(0, 0, EPD_WIDTH, EPD_HEIGHT);
        return;
    }
    show_screen(g_screen);
}

void maybeEnterIdle() {
    if (g_idleMode == 0) return;           // disabled
    if (g_idleActive) return;
    if (g_composeLen > 0 || g_composeMode || g_timeEditMode || g_nameEditMode ||
        g_dateEditMode || g_idleSecsEditMode)
        return;
    if (g_lastInputMs == 0) {
        g_lastInputMs = millis() ? millis() : 1;
        return;
    }
    if (g_idleTimeoutSec < 10) g_idleTimeoutSec = 10;  // min 10s
    uint32_t idleMs = g_idleTimeoutSec * 1000UL;
    uint32_t elapsed = millis() - g_lastInputMs;
    if (elapsed < idleMs) return;
    g_idleActive = true;
    composeClear();
    g_composeMode = false;
    Serial.printf("[IDLE] enter splash after %u s (limit %u s) screen=%u\n",
                  (unsigned)(elapsed / 1000UL), (unsigned)g_idleTimeoutSec,
                  (unsigned)g_screen);
    draw_idle_screen();
}

void draw_splash() {
    // Match current theme (white paper default; dark if already selected/persisted)
    clear_screen();
    // Ensure full panel bg (no leftover white strip at top)
    lv_obj_t* bg = lv_obj_create(g_root);
    lv_obj_set_size(bg, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, theme_bg(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    // Wordmark first (LVGL) — logo stamped into FB after flush so it is not wiped
    draw_label_centered(0, 130, EPD_WIDTH, 22, "RatSpeak", true);
    fat_label(g_root, "T-Deck Pro  V1.1", 36, 160);
    fat_label(g_root, "RatSpeak     V0.1", 36, 188);
    fat_label(g_root, "BOOTING...", 70, 240);

    lv_refr_now(nullptr);  // LVGL -> FB (black paper + white labels)

    // Stamp PNG logo last into the 1-bit FB (white mark on black)
    const int logoCx = EPD_WIDTH / 2;
    const int logoCy = 64;
    draw_ratspeak_logo_bitmap(logoCx, logoCy);

    g_display.refreshIfDirty();  // partial only; mic = full when needed
    Serial.println("[BOOT] splash drawn (PNG RatSpeak logo 96x96)");
    // keep theme as-is after splash
}


void draw_tile(int x, int y, int w, int h, const char* title, const char* value) {
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    draw_label_centered(x, y + 1, w, 16, title, false);
    draw_label_centered(x, y + h / 2, w, 18, value, true);
}

void draw_nav_box(int x, int y, int w, int h, const char* label, bool active, char hotkey) {
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, active ? 3 : 1, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (active) {
        lv_obj_t* under = lv_obj_create(g_root);
        lv_obj_set_size(under, w - 8, 3);
        lv_obj_set_pos(under, x + 4, y + h - 5);
        lv_obj_set_style_radius(under, 0, 0);
        lv_obj_set_style_border_width(under, 0, 0);
        lv_obj_set_style_bg_color(under, theme_fg(), 0);
        lv_obj_set_style_bg_opa(under, LV_OPA_COVER, 0);
        lv_obj_clear_flag(under, LV_OBJ_FLAG_SCROLLABLE);
    }

    const lv_font_t* font = &lv_font_montserrat_16;
    lv_coord_t full = lv_txt_get_width(label, strlen(label), font, 0, LV_TEXT_FLAG_NONE);
    if (full <= 0) full = (lv_coord_t)(strlen(label) * 7);
    int lx = x + (w - (int)full) / 2;
    if (lx < x + 2) lx = x + 2;
    int ly = y + (h - (active ? 4 : 0) - 16) / 2;
    if (ly < y) ly = y;

    int idx = -1;
    if (hotkey) {
        char hk = (char)toupper((unsigned char)hotkey);
        for (int i = 0; label[i]; i++) {
            if (toupper((unsigned char)label[i]) == hk) { idx = i; break; }
        }
    }
    if (idx < 0) {
        draw_label_centered(x, y, w, h - (active ? 4 : 0), label, active);
        return;
    }

    char before[24] = {0};
    if (idx > 0) {
        memcpy(before, label, (size_t)idx);
        before[idx] = '\0';
    }
    lv_coord_t bw = before[0]
        ? lv_txt_get_width(before, strlen(before), font, 0, LV_TEXT_FLAG_NONE) : 0;
    char one[2] = { label[idx], 0 };
    lv_coord_t cw = lv_txt_get_width(one, 1, font, 0, LV_TEXT_FLAG_NONE);
    if (cw <= 0) cw = 8;
    const char* after = label + idx + 1;

    if (before[0]) {
        lv_obj_t* lab = lv_label_create(g_root);
        lv_label_set_text(lab, before);
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, lx, ly);
    }
    for (int k = 0; k < 2; k++) {
        lv_obj_t* lab = lv_label_create(g_root);
        lv_label_set_text(lab, one);
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, lx + (int)bw + k, ly);
    }
    if (after && after[0]) {
        for (int k = 0; k < (active ? 2 : 1); k++) {
            lv_obj_t* lab = lv_label_create(g_root);
            lv_label_set_text(lab, after);
            lv_obj_set_style_text_font(lab, font, 0);
            lv_obj_set_style_text_color(lab, theme_fg(), 0);
            lv_obj_set_pos(lab, lx + (int)bw + (int)cw + k, ly);
        }
    }
    // Underline tight under hotkey glyph
    lv_obj_t* bar = lv_obj_create(g_root);
    lv_obj_set_size(bar, (int)cw + 2, 2);
    lv_obj_set_pos(bar, lx + (int)bw, ly + 16);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, theme_fg(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

}


static void draw_refresh_btn(int x, int y, int w, int h) {
    // Box — 1px border like non-selected nav tabs
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    // Two curved arrows in a circle (simple 1-bit glyph via lines/arcs as rects)
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    // Outer ring (approx with border square inset)
    lv_obj_t* ring = lv_obj_create(g_root);
    lv_obj_set_size(ring, 16, 16);
    lv_obj_set_pos(ring, cx - 8, cy - 8);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, theme_fg(), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    // Arrow tips (small filled triangles as rects)
    lv_obj_t* a1 = lv_obj_create(g_root);
    lv_obj_set_size(a1, 5, 3);
    lv_obj_set_pos(a1, cx + 3, cy - 7);
    lv_obj_set_style_radius(a1, 0, 0);
    lv_obj_set_style_border_width(a1, 0, 0);
    lv_obj_set_style_bg_color(a1, theme_fg(), 0);
    lv_obj_set_style_bg_opa(a1, LV_OPA_COVER, 0);
    lv_obj_clear_flag(a1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* a2 = lv_obj_create(g_root);
    lv_obj_set_size(a2, 5, 3);
    lv_obj_set_pos(a2, cx - 8, cy + 4);
    lv_obj_set_style_radius(a2, 0, 0);
    lv_obj_set_style_border_width(a2, 0, 0);
    lv_obj_set_style_bg_color(a2, theme_fg(), 0);
    lv_obj_set_style_bg_opa(a2, LV_OPA_COVER, 0);
    lv_obj_clear_flag(a2, LV_OBJ_FLAG_SCROLLABLE);
}

void draw_menu_overlay() {
    if (!g_menuOpen) return;
    // 1-wide x 5-tall column under MENU; top bar stays visible
    static const char* kLab[5] = {"Home", "Settings", "Contacts", "Peers", "Msg Box"};
    static const char kKey[5]  = {'H', 'S', 'C', 'P', 'M'};
    for (int i = 0; i < MENU_ROWS; i++) {
        const int y = MENU_BOX_Y + i * MENU_ROW_H;
        const bool active =
            (i == 0 && g_screen == SCR_HOME) ||
            (i == 1 && g_screen == SCR_SETTINGS) ||
            (i == 2 && g_screen == SCR_CONTACTS) ||
            (i == 3 && g_screen == SCR_PEERS) ||
            (i == 4 && g_screen == SCR_CHATS);
        draw_nav_box(MENU_BOX_X, y, MENU_BOX_W, MENU_ROW_H, kLab[i], active, kKey[i]);
    }
}

void draw_nav() {
    int pct = -1;
    if (g_pctStr[0] && g_pctStr[0] != '-')
        pct = atoi(g_pctStr);
    // Top always: refresh | bat | clock | MENU (visible even with overlay)
    draw_refresh_btn(0, 0, TOP_REFRESH_W, TOP_H);
    // Battery cell: same border box as neighbors (bottom line included)
    {
        lv_obj_t* bbox = lv_obj_create(g_root);
        lv_obj_set_size(bbox, TOP_BAT_W, TOP_H);
        lv_obj_set_pos(bbox, TOP_REFRESH_W, 0);
        lv_obj_set_style_radius(bbox, 0, 0);
        lv_obj_set_style_border_width(bbox, 1, 0);
        lv_obj_set_style_border_color(bbox, theme_fg(), 0);
        lv_obj_set_style_bg_color(bbox, theme_bg(), 0);
        lv_obj_set_style_bg_opa(bbox, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(bbox, 0, 0);
        lv_obj_clear_flag(bbox, LV_OBJ_FLAG_SCROLLABLE);
        draw_battery_nav_icon(TOP_REFRESH_W + 5, 3, TOP_BAT_W - 10, TOP_H - 6, pct);
    }
    int sx = TOP_REFRESH_W + TOP_BAT_W;
    // Clock cell: date on top row, spaced time on bottom (idle-style spacing)
    {
        lv_obj_t* cbox = lv_obj_create(g_root);
        lv_obj_set_size(cbox, TOP_TAB_W, TOP_H);
        lv_obj_set_pos(cbox, sx, 0);
        lv_obj_set_style_radius(cbox, 0, 0);
        lv_obj_set_style_border_width(cbox, 1, 0);
        lv_obj_set_style_border_color(cbox, theme_fg(), 0);
        lv_obj_set_style_bg_color(cbox, theme_bg(), 0);
        lv_obj_set_style_bg_opa(cbox, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(cbox, 0, 0);
        lv_obj_clear_flag(cbox, LV_OBJ_FLAG_SCROLLABLE);

        char lt[12], ld[12];
        formatLocalTime(lt, sizeof(lt));
        formatLocalDate(ld, sizeof(ld));
        // Compact MM/DD only so spaced digits fit in ~92px cell
        char md[8];
        if (ld[0] && ld[2] == '/' && ld[5] == '/')
            snprintf(md, sizeof(md), "%c%c/%c%c", ld[0], ld[1], ld[3], ld[4]);
        else
            snprintf(md, sizeof(md), "%.5s", ld);
        char dateSp[24] = {0}, timeSp[24] = {0};
        {
            size_t j = 0;
            for (size_t i = 0; md[i] && j + 2 < sizeof(dateSp); i++) {
                dateSp[j++] = md[i];
                if (md[i + 1]) dateSp[j++] = ' ';
            }
            dateSp[j] = 0;
        }
        {
            size_t j = 0;
            for (size_t i = 0; lt[i] && j + 2 < sizeof(timeSp); i++) {
                timeSp[j++] = lt[i];
                if (lt[i + 1]) timeSp[j++] = ' ';
            }
            timeSp[j] = 0;
        }
        draw_label_centered(sx, 1, TOP_TAB_W, 16, dateSp[0] ? dateSp : "--", false);
        draw_label_centered(sx, 17, TOP_TAB_W, 16, timeSp[0] ? timeSp : "- - : - -", false);
    }
    draw_nav_box(sx + TOP_TAB_W, 0, TOP_TAB_W, TOP_H, "MENU", g_menuOpen, 'M');

    if (g_showHistory || g_nameEditMode) {
        const char* back = g_nameEditMode ? "^ BACK UP TO CONTACTS" : "^ BACK UP TO MSG BOX";
        draw_nav_box(0, BOT_TOP, EPD_WIDTH, BOT_H, back, true, '^');
        draw_menu_overlay();
        return;
    }
    // Bottom: Contact | Peers | Msg Box on every page
    int x0 = 0;
    draw_nav_box(x0, BOT_TOP, BOT_CONTACT_W, BOT_H, "Contact", g_screen == SCR_CONTACTS, 'C');
    x0 += BOT_CONTACT_W;
    draw_nav_box(x0, BOT_TOP, BOT_PEERS_W, BOT_H, "Peers", g_screen == SCR_PEERS, 'P');
    x0 += BOT_PEERS_W;
    draw_nav_box(x0, BOT_TOP, BOT_MSGBOX_W, BOT_H, "Msg Box", g_screen == SCR_CHATS, 'M');
    draw_menu_overlay();
}



void draw_action_btn(int x, int y, int w, int h, const char* label) {
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    draw_label_centered(x, y, w, h, label, true);
}

// Plus-style settings/list row: title + subtitle + trailing ">"
void draw_menu_row(int y, int h, const char* title, const char* sub, bool selected) {
    // Selection = thick border only (same in both themes - no solid fill bar)
    lv_obj_t* row = lv_obj_create(g_root);
    lv_obj_set_size(row, EPD_WIDTH - 4, h);
    lv_obj_set_pos(row, 2, y);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(row, theme_fg(), 0);
    lv_obj_set_style_bg_color(row, theme_bg(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t* font = &lv_font_montserrat_16;
    const bool hasSub = sub && sub[0];
    const int passes = g_darkTheme ? 2 : 1;
    for (int k = 0; k < passes; k++) {
        lv_obj_t* lab = lv_label_create(row);
        lv_label_set_text(lab, title ? title : "");
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, 6 + k, hasSub ? 4 : (h - 16) / 2);
    }
    // Underline leading 0-9 so keyboard digit nav is obvious
    if (title && title[0] >= '0' && title[0] <= '9') {
        lv_obj_t* bar = lv_obj_create(row);
        lv_obj_set_size(bar, 10, 2);
        lv_obj_set_pos(bar, 6, hasSub ? 20 : (h - 16) / 2 + 16);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, theme_fg(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }
    if (hasSub) {
        for (int k = 0; k < passes; k++) {
            lv_obj_t* s = lv_label_create(row);
            lv_label_set_text(s, sub);
            lv_obj_set_style_text_font(s, font, 0);
            lv_obj_set_style_text_color(s, theme_fg(), 0);
            lv_obj_set_pos(s, 6 + k, 22);
        }
    }
}


void draw_stat_boxes(int y, unsigned conv, unsigned neu, unsigned unread) {
    (void)conv; (void)neu;
    char c[20];
    if (unread > 99)
        snprintf(c, sizeof(c), "Unread +99");
    else
        snprintf(c, sizeof(c), "Unread %02u", unread);
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, 124, 18);  // +space so "00" not clipped
    lv_obj_set_pos(box, 0, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    fat_label(g_root, c, 4, y + 1);
}

// Settings / list footer: short key legend just above bottom nav (underlined keys)

// Settings path under refresh icon, e.g. 7  7>1  7>3>2>4
static void drawSettingsPath() {
    if (g_screen != SCR_SETTINGS) return;
    char path[24] = "7";
    switch (g_settingsPage) {
        case SET_ROOT:      strcpy(path, "S"); break;
        case SET_TIME:      strcpy(path, "S.1"); break;
        case SET_MANUAL_TIME: strcpy(path, "S.1.1"); break;
        case SET_TZ:        strcpy(path, "S>1>2"); break;
        case SET_MANUAL_LOC: strcpy(path, "S>1>4"); break;
        case SET_NETWORK:   strcpy(path, "S.2"); break;
        case SET_SCREEN:    strcpy(path, "S.3"); break;
        case SET_IDLE_MODE: strcpy(path, "S.3.2"); break;
        case SET_IDLE_SECS: strcpy(path, "S.3.2.4"); break;
        case SET_BATTERY:   strcpy(path, "S.4"); break;
        case SET_IDENTITY:  strcpy(path, "S.5"); break;
        case SET_LORA:      strcpy(path, "S.6"); break;
        case SET_AUDIO:     strcpy(path, "S.7"); break;
        case SET_DIAG:      strcpy(path, "S.8"); break;
        case SET_STORAGE:   strcpy(path, "S.9"); break;
        default:            strcpy(path, "S"); break;
    }
    // Just under refresh button (left)
    fat_label(g_root, path, 2, TOP_H + 1);
}

// Small up-chevron for Back (Shift) — drawn near path on settings sub-pages
static void drawUpArrowBack(int x, int y) {
    // 10x8 chevron pointing up
    lv_obj_t* stem = lv_obj_create(g_root);
    lv_obj_set_size(stem, 2, 6);
    lv_obj_set_pos(stem, x + 4, y + 3);
    lv_obj_set_style_border_width(stem, 0, 0);
    lv_obj_set_style_bg_color(stem, theme_fg(), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    // Left arm
    for (int i = 0; i < 4; i++) {
        lv_obj_t* p = lv_obj_create(g_root);
        lv_obj_set_size(p, 2, 2);
        lv_obj_set_pos(p, x + 4 - i, y + i);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_bg_color(p, theme_fg(), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_t* p = lv_obj_create(g_root);
        lv_obj_set_size(p, 2, 2);
        lv_obj_set_pos(p, x + 4 + i, y + i);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_bg_color(p, theme_fg(), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    }
}

// Shared list footer used by Settings / Contacts / Peers / Msg Box
static bool g_footerMoreDn = false;
static void draw_list_footer(const char* extra) {
    const int fh = 20;
    const int y = BOT_TOP - fh;
    auto boxlab = [&](int x, int w, const char* t) {
        lv_obj_t* b = lv_obj_create(g_root);
        lv_obj_set_size(b, w, fh);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, theme_fg(), 0);
        lv_obj_set_style_bg_color(b, theme_bg(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        // center text in box
        if (t && t[0])
            draw_label_centered(x, y - 1, w, fh, t, false);
    };
    // Align vertical dividers with bottom nav: Contact(80) | Peers(64) | Msg Box(96)
    // Flip order: V=Down under Contact, G=up under Peers, v more under Msg Box
    boxlab(0, BOT_CONTACT_W, "V=Down");
    boxlab(BOT_CONTACT_W, BOT_PEERS_W, "G=up");
    const char* ex = (extra && extra[0]) ? extra : "";
    if (g_numsOnlyMsg[0]) ex = g_numsOnlyMsg;
    const bool showEx = ex && ex[0]
        && strcmp(ex, "Enter open") != 0 && strcmp(ex, "Enter opens item") != 0
        && strcmp(ex, "Enter name") != 0 && strcmp(ex, "Enter send") != 0;
    if (showEx)
        boxlab(BOT_CONTACT_W + BOT_PEERS_W, BOT_MSGBOX_W, ex);
    else
        boxlab(BOT_CONTACT_W + BOT_PEERS_W, BOT_MSGBOX_W,
               g_footerMoreDn ? "v more" : "");
    draw_nav();
}
static void draw_settings_footer(const char* extra) {
    draw_list_footer(extra);
}


void draw_compose_or_hint(const char* idleHint) {
    // Thread / name-edit: 3-line compose box with Enter=send/save
    // Settings / other: short footer only (no Enter=send overlay)
    const int y0 = COMPOSE_STRIP_Y;
    const bool typing = (g_composeLen > 0 || g_composeMode);
    const bool settingsType =
        (g_screen == SCR_SETTINGS &&
         (g_settingsPage == SET_IDLE_SECS || g_settingsPage == SET_MANUAL_TIME ||
          g_settingsPage == SET_MANUAL_DATE || g_settingsPage == SET_MANUAL_LOC ||
          g_timeEditMode || g_dateEditMode || g_idleSecsEditMode));
    const bool threadSend = idleHint && (strstr(idleHint, "Enter=send") || strstr(idleHint, "Enter = send"));
    const bool showComposeBox =
        !settingsType && (typing || threadSend || g_nameEditMode ||
                          (idleHint && idleHint[0] == '\0' && g_showHistory));

    if (showComposeBox) {
        const int boxBottom = BOT_TOP;
        lv_obj_t* box = lv_obj_create(g_root);
        lv_obj_set_size(box, EPD_WIDTH - 4, boxBottom - y0);
        lv_obj_set_pos(box, 2, y0);
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, theme_fg(), 0);
        lv_obj_set_style_bg_color(box, theme_bg(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        char line1[28] = {0};
        char line2[28] = {0};
        if (typing) {
            if (g_nameEditMode) {
                snprintf(line1, sizeof(line1), "Name>%.20s", g_compose);
            } else {
                size_t n = g_composeLen;
                size_t take1 = n > 26 ? 26 : n;
                memcpy(line1, g_compose, take1);
                line1[take1] = 0;
                if (n > 26) {
                    size_t take2 = n - 26;
                    if (take2 > 26) take2 = 26;
                    memcpy(line2, g_compose + 26, take2);
                    line2[take2] = 0;
                }
            }
        } else if (g_nameEditMode) {
            strcpy(line1, "Name: _");
        }
        if (line1[0]) fat_label(g_root, line1, 6, y0 + 1);
        if (line2[0]) fat_label(g_root, line2, 6, y0 + COMPOSE_LINE_H);
        const int y3 = y0 + 2 * COMPOSE_LINE_H;
        fat_label(g_root, g_nameEditMode ? "[Ent] save" : "[Ent] send", 6, y3);
        draw_nav();
        return;
    }

    // Settings: compact footer with underlined G/V
    if (g_screen == SCR_SETTINGS) {
        const char* extra = "Enter open";
        if (settingsType) {
            if (g_settingsPage == SET_IDLE_SECS)
                extra = g_composeLen ? "Enter save" : "type secs";
            else if (g_timeEditMode || g_dateEditMode)
                extra = "Enter set";
            else
                extra = "type+Enter";
        } else if (idleHint && strstr(idleHint, "Enter=select"))
            extra = "Enter open";
        else if (idleHint && strstr(idleHint, "pick"))
            extra = "Enter pick";
        draw_settings_footer(extra);
        return;
    }

    // List screens: short footer (fit 240px)
    const int yf = BOT_TOP - 18;
    if (g_screen == SCR_CHATS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS) {
        g_footerMoreDn = false;  // list screens: no scroll strip yet
        const char* shortH = "";
        if (g_screen == SCR_CONTACTS) shortH = "";  // no Enter hint on contacts list
        else if (g_screen == SCR_PEERS) shortH = "";
        else if (g_showHistory) shortH = "Enter send";
        // Msg Box list: no default Enter open
        draw_list_footer(shortH);
        return;
    }
    if (idleHint && idleHint[0]) {
        // Truncate to ~22 chars so it fits 240px
        char buf[24];
        snprintf(buf, sizeof(buf), "%.22s", idleHint);
        fat_label(g_root, buf, 4, yf);
    }
    draw_nav();
}

void add_row(int& y, const char* left, const char* right) {
    fat_label(g_root, left, 8, y);
    fat_label(g_root, right, 118, y);
    y += 22;
}
// Draw a solid rect in theme color (fg or bg)
static void bat_rect(int px, int py, int pw, int ph, bool useFg) {
    if (pw < 1 || ph < 1) return;
    lv_obj_t* z = lv_obj_create(g_root);
    lv_obj_set_size(z, pw, ph);
    lv_obj_set_pos(z, px, py);
    lv_obj_set_style_radius(z, 0, 0);
    lv_obj_set_style_border_width(z, 0, 0);
    // Over filled (black) region → white bolt; over empty (white) → black bolt
    lv_obj_set_style_bg_color(z, useFg ? theme_fg() : theme_bg(), 0);
    lv_obj_set_style_bg_opa(z, LV_OPA_COVER, 0);
    lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
}

// Horizontal fill grows left→right. Bolt inverts vs local fill.
// fillRightX = exclusive right edge of filled region (body interior).
static void bat_bolt_h(int cx, int cy, int fillRightX) {
    auto seg = [&](int px, int py, int pw, int ph) {
        int x1 = px, x2 = px + pw;
        if (x2 <= fillRightX) {
            bat_rect(px, py, pw, ph, false);  // white on fill
        } else if (x1 >= fillRightX) {
            bat_rect(px, py, pw, ph, true);   // black on empty
        } else {
            int wL = fillRightX - x1;
            bat_rect(x1, py, wL, ph, false);
            bat_rect(fillRightX, py, x2 - fillRightX, ph, true);
        }
    };
    // Thicker zig-zag so e-ink shows the bolt clearly
    seg(cx + 1, cy - 6, 6, 3);
    seg(cx - 2, cy - 3, 5, 3);
    seg(cx - 4, cy - 0, 9, 3);
    seg(cx - 1, cy + 3, 5, 3);
    seg(cx - 5, cy + 6, 6, 3);
}

// Vertical fill grows bottom→top. fillTopY = top edge of filled region.
static void bat_bolt_v(int cx, int cy, int fillTopY) {
    auto seg = [&](int px, int py, int pw, int ph) {
        int y1 = py, y2 = py + ph;
        if (y1 >= fillTopY) {
            bat_rect(px, py, pw, ph, false);  // white on fill
        } else if (y2 <= fillTopY) {
            bat_rect(px, py, pw, ph, true);   // black on empty
        } else {
            int hTop = fillTopY - y1;
            bat_rect(px, y1, pw, hTop, true);
            bat_rect(px, fillTopY, pw, y2 - fillTopY, false);
        }
    };
    // Thicker for small nav icon
    seg(cx + 1, cy - 7, 5, 3);
    seg(cx - 2, cy - 4, 4, 3);
    seg(cx - 4, cy - 1, 8, 3);
    seg(cx - 1, cy + 2, 4, 3);
    seg(cx - 5, cy + 5, 5, 3);
}

// Plug icon (USB powered, not actively charging / float-full).
// Same invert-vs-fill rules as the bolt.
static void bat_plug_h(int cx, int cy, int fillRightX) {
    auto seg = [&](int px, int py, int pw, int ph) {
        int x1 = px, x2 = px + pw;
        if (x2 <= fillRightX)
            bat_rect(px, py, pw, ph, false);
        else if (x1 >= fillRightX)
            bat_rect(px, py, pw, ph, true);
        else {
            int wL = fillRightX - x1;
            bat_rect(x1, py, wL, ph, false);
            bat_rect(fillRightX, py, x2 - fillRightX, ph, true);
        }
    };
    // Two prongs left, body right — reads as a plug inside the cell
    seg(cx - 6, cy - 4, 3, 2);
    seg(cx - 6, cy + 2, 3, 2);
    seg(cx - 3, cy - 5, 2, 10);
    seg(cx - 1, cy - 3, 6, 6);
}

static void bat_plug_v(int cx, int cy, int fillTopY) {
    auto seg = [&](int px, int py, int pw, int ph) {
        int y1 = py, y2 = py + ph;
        if (y1 >= fillTopY)
            bat_rect(px, py, pw, ph, false);
        else if (y2 <= fillTopY)
            bat_rect(px, py, pw, ph, true);
        else {
            int hTop = fillTopY - y1;
            bat_rect(px, y1, pw, hTop, true);
            bat_rect(px, fillTopY, pw, y2 - fillTopY, false);
        }
    };
    // Prongs on top, body below
    seg(cx - 3, cy - 7, 2, 4);
    seg(cx + 1, cy - 7, 2, 4);
    seg(cx - 4, cy - 3, 8, 2);
    seg(cx - 3, cy - 1, 6, 7);
}

// true when we should show lightning (active charge into pack)
static bool batShowBolt() { return g_batCharging; }
// true when plugged in but not charging (float / full on USB)
static bool batShowPlug() { return g_usbPower && !g_batCharging; }

void draw_battery_gauge(int x, int y, int w, int h, int pct) {
    lv_obj_t* body = lv_obj_create(g_root);
    lv_obj_set_size(body, w, h);
    lv_obj_set_pos(body, x, y);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, theme_fg(), 0);
    lv_obj_set_style_bg_color(body, theme_bg(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* tip = lv_obj_create(g_root);
    lv_obj_set_size(tip, 3, h / 2);
    lv_obj_set_pos(tip, x + w, y + h / 4);
    lv_obj_set_style_radius(tip, 0, 0);
    lv_obj_set_style_border_width(tip, 0, 0);
    lv_obj_set_style_bg_color(tip, theme_fg(), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    int fw = 0;
    if (pct > 0) {
        fw = ((w - 4) * pct) / 100;
        if (fw < 1) fw = 1;
        if (fw > w - 4) fw = w - 4;
        lv_obj_t* fill = lv_obj_create(g_root);
        lv_obj_set_size(fill, fw, h - 4);
        lv_obj_set_pos(fill, x + 2, y + 2);
        lv_obj_set_style_radius(fill, 0, 0);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_set_style_bg_color(fill, theme_fg(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    }
    {
        int fillRight = x + 2 + fw;
        int cx = x + w / 2, cy = y + h / 2;
        if (batShowBolt())
            bat_bolt_h(cx, cy, fillRight);
        else if (batShowPlug())
            bat_plug_h(cx, cy, fillRight);
    }
    char ps[8];
    if (pct >= 0) snprintf(ps, sizeof(ps), "%d%%", pct);
    else strcpy(ps, "--%");
    fat_label(g_root, ps, x + w + 6, y);
}

void draw_battery_nav_icon(int x, int y, int w, int h, int pct) {
    lv_obj_t* tip = lv_obj_create(g_root);
    lv_obj_set_size(tip, w / 3, 3);
    lv_obj_set_pos(tip, x + w / 3, y);
    lv_obj_set_style_radius(tip, 0, 0);
    lv_obj_set_style_border_width(tip, 0, 0);
    lv_obj_set_style_bg_color(tip, theme_fg(), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    int by = y + 3;
    int bh = h - 5;
    lv_obj_t* body = lv_obj_create(g_root);
    lv_obj_set_size(body, w, bh);
    lv_obj_set_pos(body, x, by);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, theme_fg(), 0);
    lv_obj_set_style_bg_color(body, theme_bg(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    int fh = 0;
    if (pct > 0) {
        fh = ((bh - 4) * pct) / 100;
        if (fh < 1) fh = 1;
        if (fh > bh - 4) fh = bh - 4;
        lv_obj_t* fill = lv_obj_create(g_root);
        lv_obj_set_size(fill, w - 4, fh);
        lv_obj_set_pos(fill, x + 2, by + bh - 2 - fh);
        lv_obj_set_style_radius(fill, 0, 0);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_set_style_bg_color(fill, theme_fg(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    }
    {
        int fillTopY = by + bh - 2 - fh;
        int cx = x + w / 2, cy = by + bh / 2;
        if (batShowBolt())
            bat_bolt_v(cx, cy, fillTopY);
        else if (batShowPlug())
            bat_plug_v(cx, cy, fillTopY);
    }
}




// Zone list; index 0 = EST (default) — kTzCount declared above
static const char* const kTzNames[] = {
    "EST", "EDT", "CST", "CDT", "MST", "MDT", "PST", "PDT",
    "AKST", "HST", "UTC", "UTC+1", "UTC+2", "UTC+5:30", "UTC+8", "UTC+9",
    "UTC-3", "UTC-10"
};
static const char* const kTzEnv[] = {
    "EST5", "EST4", "CST6", "CST5", "MST7", "MST6", "PST8", "PST7",
    "AKST9", "HST10", "UTC0", "UTC-1", "UTC-2", "UTC-5:30", "UTC-8", "UTC-9",
    "UTC3", "UTC10"
};
static const int8_t kTzHours[] = {
    -5, -4, -6, -5, -7, -6, -8, -7,
    -9, -10, 0, 1, 2, 5, 8, 9,
    -3, -10
};

static const char* tzName() {
    int i = g_tzIndex;
    if (i < 0 || i >= kTzCount) i = 0;
    return kTzNames[i];
}
static const char* tzEnvString() {
    int i = g_tzIndex;
    if (i < 0 || i >= kTzCount) i = 0;
    return kTzEnv[i];
}
static int tzOffsetHours() {
    int i = g_tzIndex;
    if (i < 0 || i >= kTzCount) i = 0;
    return (int)kTzHours[i];
}
static void applyTimezoneEnv() {
    setenv("TZ", tzEnvString(), 1);
    tzset();
}
static void ensureDefaultRtc() {
    // If wall clock never set, seed Jan 1 2027 12:00 local so UI is not all dashes
    time_t now = time(nullptr);
    if (now >= 100000) return;
    applyTimezoneEnv();
    struct tm t = {};
    t.tm_year = 2027 - 1900;
    t.tm_mon = 0;   // January
    t.tm_mday = 1;
    t.tm_hour = 12;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    if (epoch <= 0) return;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    g_manualYear = 2027; g_manualMon = 1; g_manualDay = 1;
    g_manualHour = 12; g_manualMin = 0;
    Serial.println("[RTC] defaulted to 2027-01-01 12:00 local");
}
static void formatLocalTime(char* out, size_t n) {
    ensureDefaultRtc();
    time_t now = time(nullptr);
    if (now < 100000) {
        snprintf(out, n, "12:00");
        return;
    }
    applyTimezoneEnv();
    struct tm t;
    localtime_r(&now, &t);
    snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
}
static bool syncGpsToRtc() {
    if (!g_gpsHasFix || strlen(g_gpsTimeStr) < 8 || strlen(g_gpsDateStr) < 8)
        return false;
    // g_gpsTimeStr "HH:MM:SS", g_gpsDateStr "DD/MM/20YY"
    int hh=0, mm=0, ss=0, dd=0, mo=0, yy=0;
    if (sscanf(g_gpsTimeStr, "%d:%d:%d", &hh, &mm, &ss) != 3) return false;
    if (sscanf(g_gpsDateStr, "%d/%d/%d", &dd, &mo, &yy) != 3) return false;
    if (yy >= 100) yy %= 100;
    struct tm tm = {};
    tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = ss;
    tm.tm_mday = dd; tm.tm_mon = mo - 1;
    tm.tm_year = (yy >= 70 ? 1900 + yy : 2000 + yy) - 1900;
    setenv("TZ", "UTC0", 1); tzset();
    time_t epoch = mktime(&tm);
    if (epoch <= 0) return false;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    applyTimezoneEnv();
    Serial.printf("[RTC] synced from GPS %s %s (%s)\n",
                  g_gpsDateStr, g_gpsTimeStr, tzName());
    return true;
}

static void maybeAutoGpsSync() {
    if (!g_autoGpsSync || !g_gpsHasFix) return;
    uint32_t now = millis();
    const uint32_t dayMs = 24UL * 60UL * 60UL * 1000UL;
    bool need = !g_autoGpsSyncedBoot;
    if (g_lastGpsSyncMs != 0 && (now - g_lastGpsSyncMs) >= dayMs)
        need = true;
    if (!need) return;
    if (syncGpsToRtc()) {
        g_autoGpsSyncedBoot = true;
        g_lastGpsSyncMs = now ? now : 1;
        Serial.println("[RTC] auto GPS sync done");
    }
}


static void applyManualLocalTime() {
    // Interpret hour/min as local wall clock under current TZ
    applyTimezoneEnv();
    time_t now = time(nullptr);
    struct tm t;
    if (now > 100000) {
        localtime_r(&now, &t);
    } else {
        memset(&t, 0, sizeof(t));
        t.tm_mday = 1; t.tm_mon = 9; t.tm_year = 2026 - 1900;  // Oct 2026 fallback
    }
    t.tm_hour = g_manualHour;
    t.tm_min  = g_manualMin;
    t.tm_sec  = 0;
    time_t epoch = mktime(&t);  // TZ already set → converts local→UTC
    if (epoch <= 0) return;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    Serial.printf("[RTC] manual set %02d:%02d %s (UTC%+d)\n",
                  g_manualHour, g_manualMin, tzName(), tzOffsetHours());
}

static bool parseComposeToManualTime() {
    // Accept HHMM, H:MM, HH:MM
    char buf[16];
    size_t n = 0;
    for (size_t i = 0; i < g_composeLen && n + 1 < sizeof(buf); i++) {
        char c = g_compose[i];
        if (c >= '0' && c <= '9') buf[n++] = c;
    }
    buf[n] = 0;
    if (n == 3) {
        // HMM → 0H MM
        int h = buf[0] - '0';
        int m = (buf[1] - '0') * 10 + (buf[2] - '0');
        if (h > 23 || m > 59) return false;
        g_manualHour = h; g_manualMin = m;
        return true;
    }
    if (n == 4) {
        int h = (buf[0] - '0') * 10 + (buf[1] - '0');
        int m = (buf[2] - '0') * 10 + (buf[3] - '0');
        if (h > 23 || m > 59) return false;
        g_manualHour = h; g_manualMin = m;
        return true;
    }
    return false;
}

static void loadManualDateFromClock() {
    time_t now = time(nullptr);
    if (now < 100000) return;
    applyTimezoneEnv();
    struct tm t;
    localtime_r(&now, &t);
    g_manualMon = t.tm_mon + 1;
    g_manualDay = t.tm_mday;
    g_manualYear = t.tm_year + 1900;
}

static void applyManualLocalDate() {
    applyTimezoneEnv();
    time_t now = time(nullptr);
    struct tm t;
    if (now > 100000)
        localtime_r(&now, &t);
    else
        memset(&t, 0, sizeof(t));
    t.tm_mon = g_manualMon - 1;
    t.tm_mday = g_manualDay;
    t.tm_year = g_manualYear - 1900;
    if (t.tm_hour < 0 || t.tm_hour > 23) t.tm_hour = 12;
    t.tm_sec = 0;
    time_t epoch = mktime(&t);
    if (epoch <= 0) return;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    Serial.printf("[RTC] manual date %04d-%02d-%02d\n", g_manualYear, g_manualMon, g_manualDay);
}

static bool parseComposeToManualDate() {
    // Accept MMDDYY, MMDDYYYY, YYYYMMDD, or MM/DD/YY
    char buf[16];
    size_t n = 0;
    for (size_t i = 0; i < g_composeLen && n + 1 < sizeof(buf); i++) {
        char c = g_compose[i];
        if (c >= '0' && c <= '9') buf[n++] = c;
    }
    buf[n] = 0;
    int mon = 0, day = 0, year = 0;
    if (n == 6) {
        mon = (buf[0]-'0')*10 + (buf[1]-'0');
        day = (buf[2]-'0')*10 + (buf[3]-'0');
        year = 2000 + (buf[4]-'0')*10 + (buf[5]-'0');
    } else if (n == 8) {
        // YYYYMMDD if first pair looks like year
        int a = (buf[0]-'0')*10 + (buf[1]-'0');
        if (a >= 20) {
            year = (buf[0]-'0')*1000 + (buf[1]-'0')*100 + (buf[2]-'0')*10 + (buf[3]-'0');
            mon = (buf[4]-'0')*10 + (buf[5]-'0');
            day = (buf[6]-'0')*10 + (buf[7]-'0');
        } else {
            mon = (buf[0]-'0')*10 + (buf[1]-'0');
            day = (buf[2]-'0')*10 + (buf[3]-'0');
            year = (buf[4]-'0')*1000 + (buf[5]-'0')*100 + (buf[6]-'0')*10 + (buf[7]-'0');
        }
    } else {
        return false;
    }
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || year < 2000 || year > 2099)
        return false;
    g_manualMon = mon; g_manualDay = day; g_manualYear = year;
    return true;
}

static void loadManualFromClock() {
    time_t now = time(nullptr);
    if (now < 100000) return;
    applyTimezoneEnv();
    struct tm t;
    localtime_r(&now, &t);
    g_manualHour = t.tm_hour;
    g_manualMin  = t.tm_min;
}

static void formatLocalDate(char* out, size_t n) {
    ensureDefaultRtc();
    time_t now = time(nullptr);
    if (now < 100000) {
        snprintf(out, n, "01/01/27");
        return;
    }
    applyTimezoneEnv();
    struct tm t;
    localtime_r(&now, &t);
    snprintf(out, n, "%02d/%02d/%02d", t.tm_mon + 1, t.tm_mday, (t.tm_year + 1900) % 100);
}

void gpsBegin() {
#if defined(HAS_GPS) && HAS_GPS
    pinMode(GPS_EN, OUTPUT);
    if (!g_gpsEnabled) {
        digitalWrite(GPS_EN, LOW);
        g_gpsSerial = nullptr;
        snprintf(g_gpsStatus, sizeof(g_gpsStatus), "disabled");
        Serial.println("[GPS] disabled");
        return;
    }
    digitalWrite(GPS_EN, HIGH);
    delay(30);
    static HardwareSerial gpsPort(1);
    gpsPort.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
    g_gpsSerial = &gpsPort;
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "acquiring");
    Serial.printf("[GPS] UART1 RX=%d TX=%d baud=%d EN=%d\n",
                  GPS_RX, GPS_TX, GPS_BAUD, GPS_EN);
#else
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "n/a");
#endif
}

void gpsSetEnabled(bool on) {
    g_gpsEnabled = on;
    g_gpsHasFix = false;
    g_gpsSats = 0;
    if (!on) {
#if defined(HAS_GPS) && HAS_GPS
        digitalWrite(GPS_EN, LOW);
#endif
        g_gpsSerial = nullptr;
        snprintf(g_gpsStatus, sizeof(g_gpsStatus), "disabled");
        snprintf(g_gpsTimeStr, sizeof(g_gpsTimeStr), "--:--");
        snprintf(g_gpsDateStr, sizeof(g_gpsDateStr), "--/--/--");
    } else {
        applyTimezoneEnv();
    g_lastInputMs = millis() ? millis() : 1;
    gpsBegin();
    }
}


// Minimal RMC parser: $GPRMC,hhmmss.sss,A,....,ddmmyy,...
void gpsPoll() {
    if (!g_gpsSerial) return;
    static char line[96];
    static size_t len = 0;
    while (g_gpsSerial->available()) {
        char c = (char)g_gpsSerial->read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = 0;
            if (len > 12 && line[0] == '$') {
                char* p = line;
                char* fld[16] = {};
                int n = 0;
                fld[n++] = p;
                for (; *p && n < 16; p++) {
                    if (*p == ',') { *p = 0; fld[n++] = p + 1; }
                }
                // RMC: time + date + fix A/V
                if (n > 9 && (strncmp(fld[0], "$GPRMC", 6) == 0 || strncmp(fld[0], "$GNRMC", 6) == 0)) {
                    if (fld[1] && strlen(fld[1]) >= 6) {
                        const char* t = fld[1];
                        snprintf(g_gpsTimeStr, sizeof(g_gpsTimeStr), "%c%c:%c%c:%c%c",
                                 t[0], t[1], t[2], t[3], t[4], t[5]);
                    }
                    if (fld[9] && strlen(fld[9]) >= 6) {
                        const char* d = fld[9];
                        snprintf(g_gpsDateStr, sizeof(g_gpsDateStr), "%c%c/%c%c/20%c%c",
                                 d[0], d[1], d[2], d[3], d[4], d[5]);
                    }
                    if (fld[2] && fld[2][0] == 'A') {
                        g_gpsHasFix = true;
                        snprintf(g_gpsStatus, sizeof(g_gpsStatus), "fix");
                        maybeAutoGpsSync();
                        // Optional: also stamp system clock on every RMC (auto path)
                        if (fld[1] && strlen(fld[1]) >= 6 && fld[9] && strlen(fld[9]) >= 6) {
                            const char* t = fld[1];
                            const char* d = fld[9];
                            struct tm tm = {};
                            tm.tm_hour = (t[0]-'0')*10 + (t[1]-'0');
                            tm.tm_min  = (t[2]-'0')*10 + (t[3]-'0');
                            tm.tm_sec  = (t[4]-'0')*10 + (t[5]-'0');
                            tm.tm_mday = (d[0]-'0')*10 + (d[1]-'0');
                            tm.tm_mon  = (d[2]-'0')*10 + (d[3]-'0') - 1;
                            int yy = (d[4]-'0')*10 + (d[5]-'0');
                            tm.tm_year = (yy >= 70 ? 1900 + yy : 2000 + yy) - 1900;
                            setenv("TZ", "UTC0", 1); tzset();
                            time_t epoch = mktime(&tm);
                            if (epoch > 0) {
                                struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
                                settimeofday(&tv, nullptr);
                                applyTimezoneEnv();
                            }
                        }
                    } else {
                        g_gpsHasFix = false;
                        if (g_gpsEnabled)
                            snprintf(g_gpsStatus, sizeof(g_gpsStatus), "acquiring");
                    }
                }
                // GGA: satellites in use (field 7)
                if (n > 7 && (strncmp(fld[0], "$GPGGA", 6) == 0 || strncmp(fld[0], "$GNGGA", 6) == 0)) {
                    if (fld[7] && fld[7][0])
                        g_gpsSats = (uint8_t)atoi(fld[7]);
                    // quality field 6: 0=invalid 1=GPS 2=DGPS
                    if (fld[6] && fld[6][0] == '0') {
                        g_gpsHasFix = false;
                        if (g_gpsEnabled)
                            snprintf(g_gpsStatus, sizeof(g_gpsStatus), "acquiring");
                    } else if (fld[6] && fld[6][0] > '0') {
                        g_gpsHasFix = true;
                        snprintf(g_gpsStatus, sizeof(g_gpsStatus), "fix");
                    }
                }
            }
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = c;
        } else {
            len = 0;
        }
    }
}

void build_home() {
    clear_screen();
    const int y0 = CONTENT_TOP - 1;  // flush under top nav

    // Identity - full width, no side gap, flush under Settings|Home
    lv_obj_t* idBox = lv_obj_create(g_root);
    lv_obj_set_size(idBox, EPD_WIDTH, HOME_ID_H);
    lv_obj_set_pos(idBox, 0, y0);
    lv_obj_set_style_radius(idBox, 0, 0);
    lv_obj_set_style_border_width(idBox, 2, 0);
    lv_obj_set_style_border_color(idBox, theme_fg(), 0);
    lv_obj_set_style_bg_color(idBox, theme_bg(), 0);
    lv_obj_set_style_bg_opa(idBox, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(idBox, 0, 0);
    lv_obj_clear_flag(idBox, LV_OBJ_FLAG_SCROLLABLE);

    loadSelfName();
    fat_label(g_root, g_selfName[0] ? g_selfName : "IDENTITY", 6, y0 + 2);
    const char* on = (g_loraIf && g_loraIf->isOnline())
        ? (g_batCharging ? "CHG" : (g_usbPower ? "USB PWR" : "ONLINE"))
        : "OFFLINE";
    // ~30px left of prior 118; gauge ~15px left of prior far-right
    fat_label(g_root, on, 88, y0 + 2);
    int pct = 0;
    if (g_pctStr[0] && g_pctStr[0] != '-')
        pct = atoi(g_pctStr);
    draw_battery_gauge(EPD_WIDTH - 77, y0 + 3, 32, 14, pct);
    char shortId[14];
    snprintf(shortId, sizeof(shortId), "%.10s", g_destHex);
    fat_label(g_root, shortId, 6, y0 + 20);
    char lxmfLine[28];
    snprintf(lxmfLine, sizeof(lxmfLine), "LXMF %.10s", g_destHex);
    fat_label(g_root, lxmfLine, 6, y0 + 38);

    // 3x2 tiles - flush, 2px gaps only
    const int gx = 0;
    const int gy = HOME_GRID_Y;
    const int tw = TILE_W;
    const int th = TILE_H;
    const int gap = TILE_GAP;
    // N3: live path/link/RSSI (refreshLiveData already called from show_screen)
    char loraV[14], peerV[14], pathV[14];
    if (g_featLora && g_loraIf && g_loraIf->isOnline()) {
        // RSSI is negative dBm - still a valid reading (was wrongly treated as missing)
        if (g_rssiStr[0] && !(g_rssiStr[0] == '-' && g_rssiStr[1] == '\0'))
            snprintf(loraV, sizeof(loraV), "%s", g_rssiStr);
        else
            strcpy(loraV, "ON");
    } else {
        strcpy(loraV, "OFF");
    }
    // PEERS: announced nodes + stored conversations
    {
        int ann = g_announceMgr.nodeCount();
        unsigned conv = g_storeReady ? g_msgStore.totalConversations() : 0;
        snprintf(peerV, sizeof(peerV), "%d/%u", ann, conv);
    }
    // Reuse TCP tile for paths, WIFI for links when those features off (live mesh stats)
    snprintf(pathV, sizeof(pathV), "p%s", g_pathsStr);
    char linkV[14];
    snprintf(linkV, sizeof(linkV), "l%s", g_linksStr);
    draw_tile(gx,                gy,            tw, th, "LORA",  loraV);
    draw_tile(gx + tw + gap,     gy,            tw, th, "PATH",  pathV);
    draw_tile(gx + 2*(tw + gap), gy,            tw, th, "LINK",  linkV);
    draw_tile(gx,                gy + th + gap, tw, th, "PEERS", peerV);
    draw_tile(gx + tw + gap,     gy + th + gap, tw, th, "AUDIO", g_featAudio ? "ON" : "OFF");
    draw_tile(gx + 2*(tw + gap), gy + th + gap, tw, th, "LXMFQ", g_lxmfqStr);

    // Status + full-width Announce - packed under tiles
    int sy = gy + 2 * (th + gap) + 2;
    fat_label(g_root, "Listening for peers", 4, sy);
    char annAgo[28];
    snprintf(annAgo, sizeof(annAgo), "Announced: %s", g_announceStr);
    fat_label(g_root, annAgo, 4, sy + 16);
    draw_action_btn(0, sy + 34, EPD_WIDTH, HOME_ANN_H, "Announce");

    // Home: nav only — never compose/send strip (clock is in top header)
    draw_nav();
}

void build_list_body(const char* /*section*/, const char* emptyNote,
                     bool withStats, unsigned conv, unsigned neu, unsigned unread) {
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used) continue;
        formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                       g_msgRows[i].peer, g_msgRows[i].unread);
        if (!g_msgRows[i].line2[0])
            formatPeerSub(g_msgRows[i].line2, sizeof(g_msgRows[i].line2),
                          g_msgRows[i].peer, nullptr);
    }
    clear_screen();
    int y = CONTENT_TOP;
    if (withStats) {
        draw_stat_boxes(y, conv, neu, unread);
        y += STATS_H + 2;
    }
    if (g_msgRowCount == 0) {
        fat_label(g_root, emptyNote && emptyNote[0] ? emptyNote : "(empty)", 8, y + 4);
    } else {
        for (int i = 0; i < g_msgRowCount; i++) {
            char title[28], sub[28];
            if (i == g_selPeer)
                snprintf(title, sizeof(title), ">%d : %s", i + 1, g_msgRows[i].line1);
            else
                snprintf(title, sizeof(title), " %d : %s", i + 1, g_msgRows[i].line1);
            if (g_msgRows[i].line2[0])
                snprintf(sub, sizeof(sub), "%.26s", g_msgRows[i].line2);
            else
                sub[0] = 0;
            draw_menu_row(y, ROW_H, title, sub, i == g_selPeer);
            y += ROW_H;
            if (y > CONTENT_BOTTOM - 4) break;
        }
    }
}

// Plus-style outline bubbles: left=in, right=out; BOTH white bg / black text
static void draw_chat_bubble(int y, const char* text, bool incoming, const char* status) {
    const int margin = 4;
    const int maxW = EPD_WIDTH - 28;
    const char* bodyTxt = text ? text : "";
    lv_coord_t tw = lv_txt_get_width(bodyTxt, strlen(bodyTxt),
                                     &lv_font_montserrat_16, 0, LV_TEXT_FLAG_NONE) + 16;
    if (tw > maxW) tw = maxW;
    if (tw < 40) tw = 40;
    const int h = 24;
    // User/Plus-style preference: sent on LEFT, received on RIGHT
    int x = incoming ? (EPD_WIDTH - margin - tw) : margin;

    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, tw, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    // Always match page bg so text (theme_fg) stays readable
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    const int passes = g_darkTheme ? 3 : 1;
    for (int k = 0; k < passes; k++) {
        lv_obj_t* lab = lv_label_create(box);
        lv_label_set_text(lab, text ? text : "");
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, 6 + k, 4);
    }

    if (status && status[0]) {
        int sx = x;
        if (sx + 40 > EPD_WIDTH - 4) sx = EPD_WIDTH - 44;
        fat_label(g_root, status, sx, y + h + 1);
    }
}

void build_chats() {
    // Plus-like thread: left = from peer, right = from us
    if (g_showHistory) {
        clear_screen();
        int y = CONTENT_TOP + 2;
        char hdr[40];
        if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used) {
            char hex[33];
            peerToHex32(g_msgRows[g_selPeer].peer, hex);
            snprintf(hdr, sizeof(hdr), "%d: >>> %s",
                     g_selPeer + 1, g_msgRows[g_selPeer].line1);
        } else
            strcpy(hdr, "Chat");
        fat_label(g_root, hdr, 4, y);
        y += 20;
        if (g_txStr[0] && g_txStr[0] != '-') {
            char stline[36];
            snprintf(stline, sizeof(stline), "last: %s", g_txStr);
            fat_label(g_root, stline, 4, y);
            y += 18;
        }
        if (g_histRowCount == 0) {
            fat_label(g_root, g_histNote[0] ? g_histNote : "(no messages)", 4, y);
        } else {
            // Prefer NEWEST messages: older ones drop off the top when space is tight
            // (previous loop drew oldest-first and clipped the latest inbound).
            auto rowH = [](int i) -> int {
                if (!g_histRows[i].used) return 0;
                const bool inc = g_histRows[i].incoming ||
                                 (g_histRows[i].line[0] == '<');
                return (inc || !g_histRows[i].st[0]) ? 28 : 40;
            };
            const int yMax = (g_composeLen > 0 || g_composeMode)
                ? (THREAD_Y_MAX - 4)   // extra room while typing
                : THREAD_Y_MAX;        // leave compose strip clear
            int start = 0;
            {
                int need = 0;
                for (int i = g_histRowCount - 1; i >= 0; i--) {
                    int h = rowH(i);
                    if (h <= 0) continue;
                    if (y + need + h > yMax) break;
                    need += h;
                    start = i;
                }
            }
            for (int i = start; i < g_histRowCount; i++) {
                if (!g_histRows[i].used) continue;
                const char* raw = g_histRows[i].line;
                const char* body = raw;
                if ((raw[0] == '<' || raw[0] == '>') && raw[1] == ' ')
                    body = raw + 2;
                else if (raw[0] == '<' || raw[0] == '>')
                    body = raw + 1;
                bool incoming = g_histRows[i].incoming || raw[0] == '<';
                if (y + 28 > yMax) break;
                draw_chat_bubble(y, body, incoming, incoming ? nullptr : g_histRows[i].st);
                y += rowH(i);
            }
        }
        draw_compose_or_hint("Enter=send");  // always 3-line box in thread
        return;
    }
    loadMessageRows(true);  // refresh unread per row after markRead
    unsigned conv = 0, neu = 0, unread = 0;
    if (g_storeReady) {
        conv = (unsigned)g_msgStore.totalConversations();
        unread = (unsigned)g_msgStore.totalUnreadCount();
    }
    // Prefer live row counts so UI matches what the user just read
    unsigned rowUnread = 0, rowNew = 0;
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used) continue;
        if (g_msgRows[i].unread > 0) {
            rowNew++;
            rowUnread += g_msgRows[i].unread;
        }
    }
    // NEW = conversations with unread; UNRD = message unread total
    neu = rowNew;
    // If store total lags behind rows (or vice versa), show the more conservative
    // non-inflated value so we don't claim unread after the user opened a chat.
    if (rowUnread > 0 || unread == 0)
        unread = rowUnread;
    else if (rowUnread == 0 && g_msgRowCount > 0)
        unread = 0;  // rows say all read
    if (conv == 0 && g_msgRowCount > 0)
        conv = (unsigned)g_msgRowCount;
    build_list_body(nullptr, g_msgNote[0] ? g_msgNote : "(no chats)",
                    true, conv, neu, unread);
    draw_compose_or_hint("Enter=open");
}

void loadContactsRows() {
    // Plus-like: TRUSTED (saved) then RECENT (announced, not saved)
    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) {
        g_msgRows[i].used = false;
        g_msgRows[i].saved = false;
    }
    const uint8_t* local = g_protoReady ? g_proto.localDestHash() : nullptr;
    const auto& nodes = g_announceMgr.nodes();

    auto addNode = [&](const DiscoveredNode& node, bool asSaved) {
        if (g_msgRowCount >= MSG_ROWS) return;
        if (node.hash.size() != 16) return;
        if (local && memcmp(node.hash.data(), local, 16) == 0) return;
        // dedupe
        for (int i = 0; i < g_msgRowCount; i++) {
            if (g_msgRows[i].used && memcmp(g_msgRows[i].peer, node.hash.data(), 16) == 0)
                return;
        }
        MsgRow& r = g_msgRows[g_msgRowCount];
        r.used = true;
        r.saved = asSaved;
        r.unread = 0;
        memcpy(r.peer, node.hash.data(), 16);
        char hex[33];
        peerToHex32(r.peer, hex);
        std::string nm;
        if (const char* loc = getLocalName(r.peer)) nm = loc;
        if (nm.empty()) nm = g_announceMgr.lookupName(hex);
        if (nm.empty() && !node.name.empty()) nm = node.name;
        // Only cache real names (not announce placeholders like "1")
        if (!nm.empty() && !(nm.size() <= 2 && nm.find_first_not_of("0123456789") == std::string::npos))
            setLocalName(r.peer, nm.c_str());
        formatPeerLine(r.line1, sizeof(r.line1), r.peer, 0);
        formatPeerSub(r.line2, sizeof(r.line2), r.peer, nullptr);
        // second line: full-ish id + age
        unsigned long ageMs = (node.lastSeen > 0 && millis() >= node.lastSeen)
                                  ? (millis() - node.lastSeen) : 0;
        unsigned ageM = (unsigned)(ageMs / 60000UL);
        if (asSaved)
            snprintf(r.line2, sizeof(r.line2), "ID: %.14s", hex);
        else if (ageM > 0)
            snprintf(r.line2, sizeof(r.line2), "ID: %.10s  %um", hex, ageM);
        else
            snprintf(r.line2, sizeof(r.line2), "ID: %.14s", hex);
        g_msgRowCount++;
    };

    // Pass 1: trusted / saved
    for (const auto& node : nodes) {
        if (node.saved) addNode(node, true);
    }
    // Also: name-cache entries that might not be in nodes yet
    // (skip - nodes carry saved flag after saveNode)

    // Pass 2: recent peers not saved
    for (const auto& node : nodes) {
        if (!node.saved) addNode(node, false);
    }
    clampSelPeer();
}

void build_contacts() {
    // Flat numbered list: 0 = THIS radio, 1+ = remotes (touch/keys match rows)
    // Preserve selection by peer hash — reload+inject shifts indices and was
    // wiping sel=2 back to 1 every redraw (touch/key looked broken).
    uint8_t keepPeer[16];
    bool keep = (g_msgRowCount > 0 && g_selPeer >= 0 && g_selPeer < g_msgRowCount &&
                 g_msgRows[g_selPeer].used);
    if (keep) memcpy(keepPeer, g_msgRows[g_selPeer].peer, 16);

    g_showHistory = false;
    g_msgRowsLoaded = false;
    loadSelfName();
    loadMessageRows(true);
    appendAnnouncedPeers();

    const uint8_t* loc = g_protoReady ? g_proto.localDestHash() : nullptr;
    if (loc) {
        // Drop accidental self from store rows
        for (int i = 0; i < g_msgRowCount; ) {
            if (g_msgRows[i].used && memcmp(g_msgRows[i].peer, loc, 16) == 0) {
                for (int j = i; j < g_msgRowCount - 1; j++)
                    g_msgRows[j] = g_msgRows[j + 1];
                g_msgRowCount--;
                continue;
            }
            i++;
        }
        if (g_msgRowCount < MSG_ROWS) {
            for (int i = g_msgRowCount; i > 0; i--)
                g_msgRows[i] = g_msgRows[i - 1];
            g_msgRowCount++;
            MsgRow& r = g_msgRows[0];
            r.used = true;
            r.saved = true;
            r.unread = 0;
            memcpy(r.peer, loc, 16);
            if (g_selfName[0])
                snprintf(r.line1, sizeof(r.line1), "%.20s", g_selfName);
            else
                strcpy(r.line1, "THIS RADIO");
            char hex[33];
            peerToHex32(loc, hex);
            snprintf(r.line2, sizeof(r.line2), "local %.16s", hex);
        }
    }
    for (int i = 1; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used) continue;
        formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                       g_msgRows[i].peer, 0);
        formatPeerSub(g_msgRows[i].line2, sizeof(g_msgRows[i].line2),
                      g_msgRows[i].peer, nullptr);
        if (getLocalName(g_msgRows[i].peer))
            g_msgRows[i].saved = true;
    }
    // Re-find selected peer after inject/shift
    if (keep) {
        g_selPeer = 0;
        for (int i = 0; i < g_msgRowCount; i++) {
            if (g_msgRows[i].used && memcmp(g_msgRows[i].peer, keepPeer, 16) == 0) {
                g_selPeer = i;
                break;
            }
        }
    }
    clampSelPeer();

    clear_screen();
    int y = CONTENT_TOP + 2;

    if (g_nameEditMode) {
        const bool self = (g_msgRowCount > 0 && g_msgRows[g_selPeer].used &&
                           isLocalPeer(g_msgRows[g_selPeer].peer));
        y = CONTENT_TOP + 18;
        {
            char cn[32];
            snprintf(cn, sizeof(cn), "%d : Contact", g_selPeer);
            fat_label(g_root, cn, 6, y);
            y += 22;
        }
        fat_label(g_root, self ? "Set THIS radio name" : "Set contact name", 6, y);
        y += 22;
        if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used) {
            char hex[33];
            peerToHex32(g_msgRows[g_selPeer].peer, hex);
            char line[36];
            snprintf(line, sizeof(line), "Was : %s", g_msgRows[g_selPeer].line1);
            fat_label(g_root, line, 6, y);
            y += 20;
            char sub[40];
            snprintf(sub, sizeof(sub), "ID  : %.16s", hex);
            fat_label(g_root, sub, 6, y);
            y += 24;
        }
        fat_label(g_root, "Type name, Enter = save", 6, y);
        y += 22;
        if (g_composeLen > 0) {
            char shown[40];
            snprintf(shown, sizeof(shown), "Name: %s", g_compose);
            fat_label(g_root, shown, 6, y);
        } else {
            fat_label(g_root, "Name: _", 6, y);
        }
        draw_compose_or_hint("Enter=save  ^=BACK");
        return;
    }

    // Shift list down so "^ more" can sit under top nav (same as Settings)
    y = CONTENT_TOP + 18;
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used) continue;
        char title[32];
        // Match Peers format: " N : Name" / ">N : Name"
        if (i == g_selPeer)
            snprintf(title, sizeof(title), ">%d : %s", i, g_msgRows[i].line1);
        else
            snprintf(title, sizeof(title), " %d : %s", i, g_msgRows[i].line1);
        draw_menu_row(y, ROW_H, title, g_msgRows[i].line2, i == g_selPeer);
        y += ROW_H;
        if (y > CONTENT_BOTTOM - 8) break;
    }
    if (g_msgRowCount == 0)
        fat_label(g_root, "(no peers - Announce)", 8, y);
    draw_compose_or_hint("");
}

void build_peers() {
    g_lastPeerCount = g_announceMgr.nodes().size();
    g_peerDirty = false;
    // Plus Peers: RECENT PEERS section
    g_showHistory = false;
    loadMessageRows(true);
    appendAnnouncedPeers();
    if (g_msgRowCount == 0)
        snprintf(g_msgNote, sizeof(g_msgNote), "(none - Announce on Home)");
    clampSelPeer();

    clear_screen();
    // Shift list down so "^ more" can sit under top nav (same as Settings/Contacts)
    int y = CONTENT_TOP + 18;
    // no RECENT PEERS title — more room for rows
    if (g_msgRowCount == 0) {
        fat_label(g_root, g_msgNote, 8, y);
    } else {
        for (int i = 0; i < g_msgRowCount; i++) {
            formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                           g_msgRows[i].peer, 0);
            formatPeerSub(g_msgRows[i].line2, sizeof(g_msgRows[i].line2),
                          g_msgRows[i].peer, nullptr);
            char title[28], sub[36];
            // Peers list is display-only (no selection chrome)
            snprintf(title, sizeof(title), " %d : %s", i + 1, g_msgRows[i].line1);
            if (g_msgRows[i].line2[0])
                snprintf(sub, sizeof(sub), "%.28s", g_msgRows[i].line2);
            else {
                char hex[33];
                peerToHex32(g_msgRows[i].peer, hex);
                snprintf(sub, sizeof(sub), "%.12s...", hex);
            }
            draw_menu_row(y, ROW_H, title, sub, false);
            y += ROW_H;
            if (y > CONTENT_BOTTOM - 8) break;
        }
    }
    draw_compose_or_hint("");
}

void settingsGoBack() {
    if (g_settingsPage != SET_ROOT) {
        g_timeEditMode = false;
        g_dateEditMode = false;
        g_idleSecsEditMode = false;
        composeClear();
        g_composeMode = false;
        keyboardSetNumLayer(false);
        if (g_settingsPage == SET_TZ) {
            g_settingsPage = SET_TIME;
            g_settingsSel = 1;
            g_settingsScroll = 0;
            show_screen(SCR_SETTINGS);
            return;
        }
        if (g_settingsPage == SET_IDLE_MODE) {
            g_settingsPage = SET_SCREEN;
            g_settingsSel = 2;
            g_settingsScroll = 0;
            show_screen(SCR_SETTINGS);
            return;
        }
        if (g_settingsPage == SET_MANUAL_TIME || g_settingsPage == SET_MANUAL_DATE ||
            g_settingsPage == SET_MANUAL_LOC) {
            g_timeEditMode = false;
            g_dateEditMode = false;
            composeClear();
            g_composeMode = false;
            g_settingsPage = SET_TIME;
            g_settingsSel = 0;
            show_screen(SCR_SETTINGS);
            return;
        }
        if (g_settingsPage == SET_IDLE_SECS) {
            g_idleSecsEditMode = false;
            composeClear();
            g_composeMode = false;
            g_settingsPage = SET_IDLE_MODE;
            g_settingsSel = 3;
            show_screen(SCR_SETTINGS);
            return;
        }
        g_settingsPage = SET_ROOT;
        g_settingsSel = g_settingsRootSel;
        // Keep scroll so the remembered row stays visible
        const int listTop = CONTENT_TOP + 2;
        const int maxVis = (CONTENT_BOTTOM - listTop - 24) / SET_ROW_H;
        if (maxVis > 0) {
            if (g_settingsSel < g_settingsScroll)
                g_settingsScroll = g_settingsSel;
            if (g_settingsSel >= g_settingsScroll + maxVis)
                g_settingsScroll = g_settingsSel - maxVis + 1;
            if (g_settingsScroll < 0) g_settingsScroll = 0;
        }
        show_screen(SCR_SETTINGS);
    }
}

void settingsActivate() {
    // Activate highlighted row on current settings page
    if (g_settingsPage == SET_ROOT) {
        static const SettingsPage kMap[] = {
            SET_TIME, SET_NETWORK, SET_SCREEN, SET_BATTERY, SET_IDENTITY,
            SET_LORA, SET_AUDIO, SET_DIAG, SET_STORAGE
        };
        if (g_settingsSel >= 0 && g_settingsSel < 9) {
            g_settingsRootSel = g_settingsSel;  // remember for Back
            g_settingsPage = kMap[g_settingsSel];
            g_settingsSel = 0;
            g_settingsScroll = 0;
            if (g_settingsPage == SET_TIME)
                loadManualFromClock();
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    // Sub-page actions
    if (g_settingsPage == SET_NETWORK) {
        if (g_settingsSel == 0) {
            g_featWifi = !g_featWifi;
            Serial.printf("[WIFI] %s (radio bring-up TBD)\n", g_featWifi ? "ON" : "OFF");
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_AUDIO) {
        if (g_settingsSel == 0) {
            g_featAudio = !g_featAudio;
            g_audio.setEnabled(g_featAudio);
            Serial.printf("[FEAT] AUDIO %s\n", g_featAudio ? "ON" : "OFF");
            if (g_featAudio) notifyInbound();
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_SCREEN) {
        if (g_settingsSel == 0) {
            g_darkTheme = !g_darkTheme;
            saveUiPrefs();
            Serial.printf("[THEME] %s\n", g_darkTheme ? "black-ink" : "white-paper");
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 1) {
            g_settingsPage = SET_IDLE_MODE;
            g_settingsSel = (int)g_idleMode;
            g_settingsScroll = 0;
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 2) {
            if (g_kb.backlightIsLit()) g_kb.backlightOff();
            else g_kb.backlightOn();
            Serial.printf("[KB] backlight %s\n", g_kb.backlightIsLit() ? "ON" : "OFF");
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_TZ) {
        if (g_settingsSel >= 0 && g_settingsSel < kTzCount) {
            g_tzIndex = g_settingsSel;
            applyTimezoneEnv();
            saveUiPrefs();
            Serial.printf("[TZ] %s %s\n", tzName(), tzEnvString());
        }
        g_settingsPage = SET_TIME;
        g_settingsSel = 0;
        g_settingsScroll = 0;
        show_screen(SCR_SETTINGS);
        return;
    }
    if (g_settingsPage == SET_IDLE_MODE) {
        if (g_settingsSel == 0) {
            g_idleMode = 0;
            Serial.println("[IDLE] mode=off");
            g_settingsPage = SET_SCREEN;
            g_settingsSel = 2;
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 1) {
            g_idleMode = 1;
            Serial.println("[IDLE] mode=no-password");
            g_settingsPage = SET_SCREEN;
            g_settingsSel = 2;
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 2) {
            Serial.println("[IDLE] password TBD");
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 3) {
            g_settingsPage = SET_IDLE_SECS;
            g_settingsSel = 0;
            g_idleSecsEditMode = true;
            g_composeMode = true;
            g_idleRangeMsg[0] = 0;
            composeClear();
            char tmp[12];
            snprintf(tmp, sizeof(tmp), "%u", (unsigned)g_idleTimeoutSec);
            for (size_t i = 0; tmp[i] && g_composeLen < COMPOSE_CAP; i++) {
                g_compose[g_composeLen++] = tmp[i];
                g_compose[g_composeLen] = 0;
            }
            Serial.printf("[IDLE] edit timeout was %u s\n", (unsigned)g_idleTimeoutSec);
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_LORA) {
        if (g_settingsSel == 0) {
            g_featLora = !g_featLora;
            Serial.printf("[FEAT] LORA %s\n", g_featLora ? "ON" : "OFF");
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 2) {
            applyLoraPreset((g_loraPreset + 1) % kLoraPresetCount);
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_TIME) {
        if (g_settingsSel == 0) {
            // Set RTC: unified field list
            loadManualFromClock();
            loadManualDateFromClock();
            g_settingsPage = SET_MANUAL_TIME;
            g_settingsSel = 0;
            g_settingsScroll = 0;
            g_timeEditMode = false;
            g_dateEditMode = false;
            composeClear();
            g_composeMode = false;
            keyboardSetNumLayer(false);
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 1) {
            g_settingsPage = SET_TZ;
            g_settingsSel = g_tzIndex;
            g_settingsScroll = 0;
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 2) {
            gpsSetEnabled(!g_gpsEnabled);
            if (g_gpsEnabled) g_autoGpsSync = true;  // sync when fix good
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 3) {
            g_settingsPage = SET_MANUAL_LOC;
            g_settingsSel = 0;
            g_settingsScroll = 0;
            g_composeMode = true;
            composeClear();
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 4) {
            bool ok = syncGpsToRtc();
            if (ok) { g_autoGpsSyncedBoot = true; g_lastGpsSyncMs = millis() ? millis() : 1; }
            Serial.printf("[RTC] GPS sync %s\n", ok ? "OK" : "FAIL");
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_MANUAL_TIME) {
        // 0-4: enter type mode for field; 5: apply all to RTC
        if (g_settingsSel >= 0 && g_settingsSel <= 4) {
            g_rtcField = g_settingsSel;
            g_timeEditMode = true;
            g_composeMode = true;
            composeClear();
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 5) {
            applyManualLocalTime();
            applyManualLocalDate();
            g_timeEditMode = false;
            composeClear();
            g_composeMode = false;
            g_settingsPage = SET_TIME;
            g_settingsSel = 0;
            show_screen(SCR_SETTINGS);
            Serial.println("[RTC] applied all fields");
        }
        return;
    }
    if (g_settingsPage == SET_MANUAL_LOC) {
        if (g_settingsSel == 0) {
            g_composeMode = true;
            composeClear();
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 1) {
            float la = 0, lo = 0;
            if (g_composeLen > 0 && sscanf(g_compose, "%f,%f", &la, &lo) == 2) {
                g_manualLat = la;
                g_manualLon = lo;
                g_hasManualPos = true;
                Serial.printf("[LOC] manual %.5f,%.5f\n", la, lo);
            }
            composeClear();
            g_composeMode = false;
            g_settingsPage = SET_TIME;
            g_settingsSel = 3;
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 2) {
            g_hasManualPos = false;
            g_manualLat = g_manualLon = 0;
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_STORAGE) {
        if (g_settingsSel == 3) {
            doAnnounce();
            if (g_featAudio) g_audio.playAnnounce();
            show_screen(SCR_SETTINGS);
        }
        return;
    }
}



void build_settings() {
    clear_screen();
    char a[40], b[36];
    int y = CONTENT_TOP + 2;

    drawSettingsPath();  // e.g. S>1>1 under refresh
    // List starts below path line
    y = TOP_H + 14;
    y += SETTINGS_LIST_PAD;
    g_setListTop = y + SETTINGS_HINT_H;
    g_setListBot = CONTENT_BOTTOM - 22 - SETTINGS_HINT_H;
    g_setMaxVis = (g_setListBot - g_setListTop) / SET_ROW_H;
    if (g_setMaxVis < 1) g_setMaxVis = 1;
    int totalRowsHint = 9;

    auto row = [&](const char* title, const char* sub, int idx) {
        if (idx + 1 > totalRowsHint) totalRowsHint = idx + 1;
        if (idx < g_settingsScroll) return;
        if (idx >= g_settingsScroll + g_setMaxVis) return;
        int yy = g_setListTop + (idx - g_settingsScroll) * SET_ROW_H;
        if (yy + SET_ROW_H > g_setListBot) return;
        draw_menu_row(yy, SET_ROW_H, title, sub, idx == g_settingsSel);
    };
    auto drawScrollHints = [&]() {
        bool moreUp = g_settingsScroll > 0;
        bool moreDn = (g_settingsScroll + g_setMaxVis) < totalRowsHint;
        g_footerMoreDn = moreDn;
        const int pathH = 18;
        if (moreUp) {
            lv_obj_t* b = lv_obj_create(g_root);
            lv_obj_set_size(b, 68, pathH);
            lv_obj_set_pos(b, EPD_WIDTH - 68, TOP_H);
            lv_obj_set_style_radius(b, 0, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, theme_fg(), 0);
            lv_obj_set_style_bg_color(b, theme_bg(), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            draw_label_centered(EPD_WIDTH - 68, TOP_H - 1, 68, pathH, "^ more", false);
        }
    };

    if (g_settingsPage == SET_ROOT) {
        char lt[12];
        formatLocalTime(lt, sizeof(lt));
        snprintf(a, sizeof(a), "%s %s", lt, tzName());
        row("1 : Menu : Time & Location", a, 0);
        row("2 : Menu : WiFi", g_featWifi ? "ON" : "OFF", 1);
        row("3 : Menu : Screen & I/O", "e-ink / keys", 2);
        snprintf(a, sizeof(a), "%s %s", g_batStr, g_pctStr);
        row("4 : Menu : Battery", a, 3);
        snprintf(a, sizeof(a), "%.14s", g_destHex);
        row("5 : Menu : Identity", a, 4);
        snprintf(a, sizeof(a), "LF %.0fMHz %s", LORA_DEFAULT_FREQ / 1e6f,
                 g_featLora ? "ON" : "OFF");
        row("6 : Menu : LoRa", a, 5);
        snprintf(a, sizeof(a), "Vol %s", g_featAudio ? "ON" : "OFF");
        row("7 : Menu : Alerts", a, 6);
        snprintf(a, sizeof(a), "P%s L%s", g_pathsStr, g_linksStr);
        row("8 : Menu : Diagnostics", a, 7);
        row("9 : Menu : Storage", "heap/flash", 8);
        totalRowsHint = 9;
        drawScrollHints();
        draw_compose_or_hint("");
        return;
    }

    if (g_settingsPage == SET_IDENTITY) {
        snprintf(a, sizeof(a), "%.16s", g_destHex);
        row("1 Destination", a, 0);
        loadSelfName();
        snprintf(a, sizeof(a), "%.16s", g_selfName[0] ? g_selfName : "(none)");
        row("2 Display name", a, 1);
        row("3 LXMF address", "same as dest", 2);
        totalRowsHint = 3;
        drawScrollHints();
        draw_compose_or_hint("^=back");
        return;
    }

    if (g_settingsPage == SET_SCREEN) {
        row("1 : Toggle : Theme", g_darkTheme ? "Black ink" : "White paper", 0);
        const char* idleLab = (g_idleMode == 0) ? "Off" :
                              (g_idleMode == 1) ? "No password" : "Password TBD";
        row("2 : Menu : Idle splash", idleLab, 1);
        row("3 : Toggle : Key backlight", g_kb.backlightIsLit() ? "ON" : "OFF", 2);
        totalRowsHint = 3;
        drawScrollHints();
        draw_compose_or_hint("Enter=select");
        return;
    }

    if (g_settingsPage == SET_IDLE_MODE) {
        row("1 No idle screen", g_idleMode == 0 ? "*" : "", 0);
        row("2 Idle no password", g_idleMode == 1 ? "*" : "", 1);
        row("3 Idle + password TBD", "", 2);
        char is[16];
        snprintf(is, sizeof(is), "%u s", (unsigned)g_idleTimeoutSec);
        row("4 Idle timeout >", is, 3);
        totalRowsHint = 4;
        drawScrollHints();
        draw_compose_or_hint("Enter=select  ^=back");
        return;
    }

    if (g_settingsPage == SET_IDLE_SECS) {
        char st[32];
        {
            char left[16];
            snprintf(left, sizeof(left), ">%s", g_composeLen ? g_compose : "30");
            snprintf(st, sizeof(st), "%-12s (sym)", left);
        }
        row("1 Seconds (10-99999)", st, 0);
        if (g_idleRangeMsg[0])
            row("2 Status", g_idleRangeMsg, 1);
        else if (g_numsOnlyMsg[0])
            row("2 Status", g_numsOnlyMsg, 1);
        else
            row("2 Status", "Enter=save", 1);
        totalRowsHint = 2;
        drawScrollHints();
        draw_compose_or_hint("Enter save");
        return;
    }

    if (g_settingsPage == SET_MANUAL_TIME) {
        char st[36];
        auto fieldSub = [&](int field, int val, int width) {
            if (g_timeEditMode && g_rtcField == field) {
                // digits left, (sym) right
                char left[20];
                snprintf(left, sizeof(left), ">%s_", g_composeLen ? g_compose : "");
                snprintf(st, sizeof(st), "%-12s (sym)", left);
            } else if (width == 4)
                snprintf(st, sizeof(st), "%04d Enter=type", val);
            else
                snprintf(st, sizeof(st), "%02d Enter=type", val);
        };
        fieldSub(0, g_manualHour, 2); row("1 : Entry : Hour (0-23)", st, 0);
        fieldSub(1, g_manualMin, 2);  row("2 : Entry : Minute (0-59)", st, 1);
        fieldSub(2, g_manualMon, 2);  row("3 : Entry : Month (1-12)", st, 2);
        fieldSub(3, g_manualDay, 2);  row("4 : Entry : Day (1-31)", st, 3);
        fieldSub(4, g_manualYear, 4); row("5 : Entry : Year", st, 4);
        row("6 : Toggle : Apply to RTC", "Enter=save all", 5);
        totalRowsHint = 6;
        drawScrollHints();
        draw_compose_or_hint(g_timeEditMode ? "Enter set" : "Enter type");
        return;
    }

    if (g_settingsPage == SET_MANUAL_LOC) {
        char st[36];
        if (g_composeLen)
            snprintf(st, sizeof(st), ">%s", g_compose);
        else if (g_hasManualPos)
            snprintf(st, sizeof(st), "%.5f,%.5f", g_manualLat, g_manualLon);
        else
            strcpy(st, "lat,lon");
        row("1 Type lat,lon", st, 0);
        row("2 Apply position", "Enter=save", 1);
        row("3 Clear position", g_hasManualPos ? "Enter=clear" : "-", 2);
        totalRowsHint = 3;
        drawScrollHints();
        draw_compose_or_hint("e.g. 40.71,-74.00");
        return;
    }

    if (g_settingsPage == SET_TZ) {
        for (int i = 0; i < kTzCount; i++) {
            char sub[20];
            snprintf(sub, sizeof(sub), "UTC%+d", (int)kTzHours[i]);
            char title[28];
            snprintf(title, sizeof(title), "%s%s", kTzNames[i], (i == g_tzIndex) ? " *" : "");
            row(title, sub, i);
        }
        totalRowsHint = kTzCount;
        drawScrollHints();
        draw_compose_or_hint("G/V scroll  Enter=pick");
        return;
    }

    if (g_settingsPage == SET_BATTERY) {
        // Info menu — | columns aligned between title and value lines
        char a[44];
        // Row 1: "1 : Info : Voltage | SOC %" — | aligned on both lines
        {
            char volt[12];
            if (g_batStr[0] && g_batStr[0] != 'n')
                snprintf(volt, sizeof(volt), "%s", g_batStr);
            else
                strcpy(volt, "--");
            int soc = -1;
            if (g_pctStr[0] && g_pctStr[0] != '-')
                soc = atoi(g_pctStr);
            // Title: "1 : Info : Voltage | SOC %"
            // Prefix before | = "1 : Info : Voltage " = 19 chars
            char left[24];
            snprintf(left, sizeof(left), "%-19s", volt);
            if (soc >= 0)
                snprintf(a, sizeof(a), "%s| %d %%", left, soc);
            else
                snprintf(a, sizeof(a), "%s| -- %%", left);
        }
        row("1 : Info : Voltage | SOC %", a, 0);

        // Row 2: keep dedicated SOC for clarity (chip reading)
        snprintf(a, sizeof(a), "%s", g_pctStr[0] ? g_pctStr : "--");
        row("2 : Info : State of charge", a, 1);

        // Row 3: current
        if (g_avgMa > 5)
            snprintf(a, sizeof(a), "+%d mA charging", (int)g_avgMa);
        else if (g_avgMa < -5)
            snprintf(a, sizeof(a), "%d mA consuming", (int)g_avgMa);
        else
            snprintf(a, sizeof(a), "%d mA idle", (int)g_avgMa);
        row("3 : Info : Average current", a, 2);

        // Row 4: "4 : Info : Status | Cord stat" — | aligned on both lines
        {
            const char* st = "Not charging";
            const char* cord = "unplugged";
            if (g_batCharging && g_usbPower) {
                st = "charging";
                cord = "plugged in";
            } else if (g_batCharging) {
                st = "charging";
                cord = "unplugged";
            } else if (g_usbPower) {
                st = "Bat full/idle";
                cord = "plugged in";
            }
            // Title is "4 : Info : Status | Cord stat"
            // Prefix before | is 18 chars: "4 : Info : Status "
            // Value uses same 18-char left field so pipes stack
            char left[20];
            snprintf(left, sizeof(left), "%-18s", st);
            snprintf(a, sizeof(a), "%s| %s", left, cord);
        }
        row("4 : Info : Status | Cord stat", a, 3);

        snprintf(a, sizeof(a), "%s", g_usbPower ? "YES (5V / USB)" : "no (on battery)");
        row("5 : Info : USB power", a, 4);
        snprintf(a, sizeof(a), "0x%04X", (unsigned)g_batFlags);
        row("6 : Info : Gauge flags", a, 5);
        if (g_batCharging)
            snprintf(a, sizeof(a), "USB charging");
        else if (g_usbPower)
            snprintf(a, sizeof(a), "USB powered (not charging)");
        else
            snprintf(a, sizeof(a), "battery only");
        row("7 : Info : Power source", a, 6);
        totalRowsHint = 7;
        drawScrollHints();
        draw_compose_or_hint("info only");
        return;
    }

    if (g_settingsPage == SET_LORA) {
        row("1 LoRa Radio", g_featLora ? "ON" : "OFF", 0);
        snprintf(a, sizeof(a), "Americas (%.0f)", LORA_DEFAULT_FREQ / 1e6f);
        row("2 Region Preset", a, 1);
        row("3 Link Preset", kLoraPresets[g_loraPreset].name, 2);
        snprintf(a, sizeof(a), "%s", g_rssiStr);
        row("4 Last RSSI", a, 3);
        totalRowsHint = 4;
        drawScrollHints();
        draw_compose_or_hint("0=radio 2=preset  ^=back");
        return;
    }

    if (g_settingsPage == SET_NETWORK) {
        row("1 : Toggle : Enable WiFi", g_featWifi ? "ON" : "OFF", 0);
        row("2 Status", g_featWifi ? "enabled (no radio yet)" : "disabled", 1);
        row("3 IP address", g_featWifi ? "0.0.0.0" : "n/a", 2);
        row("4 MAC", "see ESP32", 3);
        row("5 Scan networks", "[future]", 4);
        totalRowsHint = 5;
        drawScrollHints();
        draw_compose_or_hint("");
        return;
    }

    if (g_settingsPage == SET_TIME) {
        char st[36], loc[16], dt[16];
        formatLocalTime(loc, sizeof(loc));
        formatLocalDate(dt, sizeof(dt));
        snprintf(st, sizeof(st), "%s %s", loc, dt);
        row("1 Set RTC time >", st, 0);
        snprintf(st, sizeof(st), "%s UTC%+d", tzName(), tzOffsetHours());
        row("2 : Menu : Time zone", st, 1);
        snprintf(st, sizeof(st), "%s %s %us",
                 g_gpsEnabled ? "ON" : "OFF", g_gpsStatus, (unsigned)g_gpsSats);
        row("3 : Toggle : Enable GPS", st, 2);
        if (g_hasManualPos)
            snprintf(st, sizeof(st), "%.2f,%.2f", g_manualLat, g_manualLon);
        else
            strcpy(st, "not set");
        row("4 : Menu : Manual location", st, 3);
        row("5 : Toggle : Sync GPS now", g_gpsHasFix ? "Enter=sync" : "need fix", 4);
        totalRowsHint = 5;
        drawScrollHints();
        draw_compose_or_hint("");
        return;
    }

    if (g_settingsPage == SET_AUDIO) {
        row("1 Audio Alerts", g_featAudio ? "ON" : "OFF", 0);
        row("2 Volume", "default", 1);
        row("3 Vibrate on msg", g_featAudio ? "ON" : "OFF", 2);
        totalRowsHint = 3;
        drawScrollHints();
        draw_compose_or_hint("Enter toggles audio");
        return;
    }

    if (g_settingsPage == SET_DIAG) {
        snprintf(a, sizeof(a), "P%s L%s", g_pathsStr, g_linksStr);
        row("1 Paths/Links", a, 0);
        snprintf(a, sizeof(a), "%s", g_rssiStr);
        row("2 Last RSSI", a, 1);
        snprintf(a, sizeof(a), "%s", g_lxmfqStr);
        row("3 LXMF queue", a, 2);
        totalRowsHint = 3;
        drawScrollHints();
        draw_compose_or_hint("^=back");
        return;
    }

    if (g_settingsPage == SET_STORAGE) {
        snprintf(a, sizeof(a), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
        row("1 Free heap", a, 0);
        snprintf(a, sizeof(a), "%u KB", (unsigned)(ESP.getFreePsram() / 1024));
        row("2 Free PSRAM", a, 1);
        row("3 Flash", "LittleFS", 2);
        snprintf(a, sizeof(a), "Ann %s", g_announceStr);
        row("4 Send Announce", a, 3);
        totalRowsHint = 4;
        drawScrollHints();
        draw_compose_or_hint("Enter on Announce to send");
        return;
    }

    draw_compose_or_hint("^=back");
}

void show_screen(ScreenId id) {
    if (id == SCR_HOME || id == SCR_SETTINGS) {
        if (!g_timeEditMode && !g_dateEditMode && !g_idleSecsEditMode && !g_nameEditMode) {
            composeClear();
            g_composeMode = false;
        }
    }

    if (id == SCR_SETTINGS && g_screen != SCR_SETTINGS) {
        // Entering Settings from another tab → root list
        g_settingsPage = SET_ROOT;
        g_settingsSel = 0;
    }
    if (id != SCR_SETTINGS) {
        // Leaving settings clears sub-page so next visit starts clean
        // (keep if only redrawing same settings)
    }
    // Close MENU when navigating to a different screen
    if (id != g_screen)
        g_menuOpen = false;
    g_screen = id;
    // SYM/force-num only while typing digits
    if (!isNumEntryMode())
        keyboardSetNumLayer(false);
    else
        syncNumLayer();
    refreshLiveData();
    switch (id) {
        case SCR_HOME:     build_home(); break;
        case SCR_CHATS:    build_chats(); break;
        case SCR_CONTACTS: build_contacts(); break;
        case SCR_PEERS:    build_peers(); break;
        case SCR_SETTINGS: build_settings(); break;
        default:           build_home(); break;
    }
    lv_refr_now(nullptr);
    // Prefer localized/fast partial (no flash). DisplayEink forces full every N partials.
    g_display.refreshIfDirty();
    if (g_storeReady) g_lastUiRevision = g_msgStore.revision();
    g_composeDirty = false;
    Serial.printf("[UI] screen %u (%s) drawn\n", (unsigned)id, tabLabel(id));
    syncNumLayer();
}

void next_screen() {
    show_screen((ScreenId)((g_screen + 1) % SCR_COUNT));
}

void maybeAutoRedrawMessages() {
    if (g_screen != SCR_CHATS || !g_storeReady) return;
    if (g_composeLen > 0) return;  // don't clobber while typing
    g_msgStore.poll();
    const uint32_t rev = g_msgStore.revision();
    if (rev == g_lastUiRevision) return;
    if (millis() - g_lastMsgRedrawMs < MSG_REDRAW_MIN_MS) return;
    g_lastMsgRedrawMs = millis();
    Serial.printf("[UI] auto Messages redraw rev %u -> %u hist=%d\n",
                  (unsigned)g_lastUiRevision, (unsigned)rev, (int)g_showHistory);
    // Must refresh data, not only paint: thread uses cached g_histRows
    if (g_showHistory) {
        loadHistoryRows();
    } else {
        g_msgRowsLoaded = false;
        loadMessageRows(true);
    }
    show_screen(SCR_CHATS);
}


// E21: after typing pauses, one fullRefresh so the footer shows the compose line.
// Never refresh per keypress - only when idle and dirty.
// Peers screen: refresh when announce set grows; throttle full refresh to 30s
void maybeAutoRedrawPeers() {
    if (g_screen != SCR_PEERS || g_composeLen > 0) return;
    size_t n = g_announceMgr.nodes().size();
    if (n == g_lastPeerCount && !g_peerDirty) return;
    uint32_t now = millis();
    bool first = (g_lastPeerAutoMs == 0);
    if (!first && (now - g_lastPeerAutoMs) < 30000u) {
        g_peerDirty = true;  // pending until throttle allows
        return;
    }
    g_lastPeerCount = n;
    g_lastPeerAutoMs = now;
    g_peerDirty = false;
    Serial.printf("[PEER] auto-refresh count=%u\n", (unsigned)n);
    show_screen(SCR_PEERS);
}

void maybeComposeRedraw() {
    if (!g_composeDirty) return;
    if (g_lastComposeMs == 0) return;
    if ((millis() - g_lastComposeMs) < COMPOSE_IDLE_MS) return;

    // Always rebuild the screen so the 3-line compose band (2 draft lines +
    // Enter icon/label) stays intact. A strip-only partial was painting a
    // single ">text" label that covered/cut the Enter line.
    if (millis() - g_lastMsgRedrawMs < COMPOSE_PARTIAL_MIN_MS) return;
    size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freeInt < 12000) {
        static uint32_t s_lastSkipLog = 0;
        uint32_t now = millis();
        if (s_lastSkipLog == 0 || (now - s_lastSkipLog) > 30000u) {
            s_lastSkipLog = now;
            Serial.printf("[UI] skip compose redraw low heap %u\n", (unsigned)freeInt);
        }
        g_lastComposeMs = now;
        return;
    }
    g_lastMsgRedrawMs = millis();
    g_composeDirty = false;
    Serial.printf("[UI] compose redraw len=%u\n", (unsigned)g_composeLen);
    show_screen(g_screen);
}


// Handle one keyboard event. Returns true if a full screen redraw is needed.
bool handleKeyEvent(const KeyEvent& ev) {
    // Microphone: ignore (no full refresh — use header refresh icon)
    if (ev.fn) {
        return false;
    }
    // Idle: any key wakes back to the same screen
    if (g_idleActive) {
        Serial.printf("[IDLE] wake key screen=%u\n", (unsigned)g_screen);
        noteUserInput();
        return true;
    }
    // Any key/touch activity resets idle timer (touch does this in handleTouchAt)
    noteUserInput();

    // G1.80: never leave force-num/SYM layer on outside true digit fields.
    // Sticky force was mapping C/M/H/S/P to symbols so nav letters failed.
    if (!isNumEntryMode())
        keyboardSetNumLayer(false);
    else
        syncNumLayer();

    // MENU overlay open: H/S/C/P/M pick destination; M again or Esc closes
    if (g_menuOpen && ev.character && !ev.enter && !ev.del) {
        const char ch = ev.character;
        if (ch == 'h' || ch == 'H') { menuGo(SCR_HOME); return true; }
        if (ch == 's' || ch == 'S') { menuGo(SCR_SETTINGS); return true; }
        if (ch == 'c' || ch == 'C') { menuGo(SCR_CONTACTS); return true; }
        if (ch == 'p' || ch == 'P') { menuGo(SCR_PEERS); return true; }
        if (ch == 'm' || ch == 'M') { menuGo(SCR_CHATS); return true; }
        if (ch == 0x1b) { menuClose(); show_screen(g_screen); return true; }
        // other keys close overlay
        menuClose();
        show_screen(g_screen);
        return true;
    }

    // G/V navigate lists (before other shortcuts). S is NOT down (Settings tab).
    if (ev.character && !ev.enter && !ev.del && !ev.space) {
        const char ch = ev.character;
        const bool upKey = (ch == 'g' || ch == 'G' || ch == 'w' || ch == 'W');
        const bool dnKey = (ch == 'v' || ch == 'V');
        if ((upKey || dnKey) && g_composeLen == 0 &&
            !g_timeEditMode && !g_dateEditMode && !g_idleSecsEditMode) {
            if (g_screen == SCR_SETTINGS) {
                int maxNav = 8;
                if (g_settingsPage == SET_ROOT) maxNav = 8;
                else if (g_settingsPage == SET_MANUAL_TIME) maxNav = 5;
                else if (g_settingsPage == SET_MANUAL_LOC) maxNav = 2;
                else if (g_settingsPage == SET_IDLE_MODE) maxNav = 3;
                else if (g_settingsPage == SET_IDLE_SECS) maxNav = 1;
                else if (g_settingsPage == SET_TIME) maxNav = 4;
                else if (g_settingsPage == SET_SCREEN) maxNav = 2;
                else if (g_settingsPage == SET_NETWORK) maxNav = 4;
                else if (g_settingsPage == SET_BATTERY) maxNav = 6;
                else if (g_settingsPage == SET_TZ) maxNav = kTzCount - 1;
                else if (g_settingsPage == SET_IDENTITY) maxNav = 2;
                else if (g_settingsPage == SET_LORA) maxNav = 3;
                else if (g_settingsPage == SET_AUDIO) maxNav = 2;
                else if (g_settingsPage == SET_DIAG) maxNav = 2;
                else if (g_settingsPage == SET_STORAGE) maxNav = 3;
                else if (g_settingsPage == SET_NETWORK) maxNav = 2;
                if (maxNav >= 0) {
                    if (upKey && g_settingsSel > 0) g_settingsSel--;
                    if (dnKey && g_settingsSel < maxNav) g_settingsSel++;
                    if (g_settingsSel < g_settingsScroll) g_settingsScroll = g_settingsSel;
                    if (g_setMaxVis > 0 && g_settingsSel >= g_settingsScroll + g_setMaxVis)
                        g_settingsScroll = g_settingsSel - g_setMaxVis + 1;
                    if (g_settingsScroll < 0) g_settingsScroll = 0;
                    Serial.printf("[SET] G/V sel=%d\n", g_settingsSel);
                    show_screen(SCR_SETTINGS);
                    return true;
                }
            }
            if ((g_screen == SCR_CHATS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS) &&
                !g_showHistory && !g_nameEditMode && g_msgRowCount > 0) {
                if (upKey && g_selPeer > 0) g_selPeer--;
                if (dnKey && g_selPeer < g_msgRowCount - 1) g_selPeer++;
                show_screen(g_screen);
                return true;
            }
        }
    }

    // Keyboard Up / lone Shift: in Settings list move selection up; else Back
    if (ev.up) {
        if (g_screen == SCR_SETTINGS && g_settingsPage == SET_ROOT && g_settingsSel > 0) {
            g_settingsSel--;
            if (g_settingsSel < g_settingsScroll) g_settingsScroll = g_settingsSel;
            Serial.printf("[SET] shift-up sel=%d\n", g_settingsSel);
            show_screen(SCR_SETTINGS);
            return true;
        }
        Serial.println("[KEY] Up/Back");
        if (doBackAction()) return true;
        return false;
    }

    // Enter: send compose if non-empty, else next screen.
    if (ev.enter) {
        // RTC type-time / date / idle-secs
        if (g_screen == SCR_SETTINGS && g_settingsPage == SET_MANUAL_TIME) {
            if (g_timeEditMode || g_composeLen > 0) {
                // Commit digits to active field only
                int v = 0;
                for (size_t i = 0; i < g_composeLen; i++) {
                    if (g_compose[i] >= '0' && g_compose[i] <= '9')
                        v = v * 10 + (g_compose[i] - '0');
                }
                if (g_rtcField == 0) { if (v > 23) v = 23; g_manualHour = v; }
                else if (g_rtcField == 1) { if (v > 59) v = 59; g_manualMin = v; }
                else if (g_rtcField == 2) { if (v < 1) v = 1; if (v > 12) v = 12; g_manualMon = v; }
                else if (g_rtcField == 3) { if (v < 1) v = 1; if (v > 31) v = 31; g_manualDay = v; }
                else if (g_rtcField == 4) {
                    if (v < 100) v += 2000;
                    if (v < 2000) v = 2000;
                    if (v > 2099) v = 2099;
                    g_manualYear = v;
                }
                // Push field to wall clock immediately so Home/Idle reflect change
                applyManualLocalTime();
                Serial.printf("[RTC] field %d = %d\n", g_rtcField, v);
                g_timeEditMode = false;
                g_numsOnlyMsg[0] = 0;
                composeClear();
                g_composeMode = false;
                keyboardSetNumLayer(false);
                show_screen(SCR_SETTINGS);
                return true;
            }
            settingsActivate();
            return true;
        }

        if (g_screen == SCR_SETTINGS && g_settingsPage == SET_IDLE_SECS) {
            settingsActivate();  // applies seconds
            return true;
        }
        if (g_timeEditMode && g_screen == SCR_SETTINGS) {
            if (g_composeLen > 0 && parseComposeToManualTime()) {
                applyManualLocalTime();
            }
            g_timeEditMode = false;
            composeClear();
            g_composeMode = false;
            show_screen(SCR_SETTINGS);
            return true;
        }
        // Contacts list: Enter opens rename sub-screen (do not save yet)
        if (g_screen == SCR_CONTACTS && !g_nameEditMode) {
            if (g_msgRowCount <= 0 || !g_msgRows[g_selPeer].used) {
                Serial.println("[NAME] no contact selected");
                return false;
            }
            g_nameEditMode = true;
            g_composeMode = true;
            composeClear();
            // Prefill with current display name when not THIS RADIO / hex-only
            const char* cur = g_msgRows[g_selPeer].line1;
            if (cur && cur[0] && strcmp(cur, "THIS RADIO") != 0) {
                for (size_t i = 0; cur[i] && g_composeLen < COMPOSE_CAP; i++) {
                    char c = cur[i];
                    if (c >= 32 && c <= 126) {
                        g_compose[g_composeLen++] = c;
                        g_compose[g_composeLen] = '\0';
                    }
                }
            }
            Serial.printf("[NAME] open rename sel=%d\n", g_selPeer);
            show_screen(SCR_CONTACTS);
            return false;  // already drew
        }
        // Contacts name-edit: save typed name as trusted contact
        if (g_nameEditMode) {
            if (g_composeLen == 0) {
                Serial.println("[NAME] empty - type a name or ^=cancel");
                return false;
            }
            if (g_msgRowCount <= 0 || !g_msgRows[g_selPeer].used) {
                Serial.println("[NAME] no peer selected");
                g_nameEditMode = false;
                composeClear();
                return true;
            }
            char body[COMPOSE_CAP + 1];
            memcpy(body, g_compose, g_composeLen + 1);
            char hex[33];
            peerToHex32(g_msgRows[g_selPeer].peer, hex);
            bool ok = true;
            if (isLocalPeer(g_msgRows[g_selPeer].peer)) {
                saveSelfName(body);
            } else {
                ok = g_announceMgr.setContactName(std::string(hex), std::string(body));
                g_announceMgr.saveNode(std::string(hex));
                g_announceMgr.saveNameCache();
                setLocalName(g_msgRows[g_selPeer].peer, body);
            }
            snprintf(g_msgRows[g_selPeer].line1, sizeof(g_msgRows[g_selPeer].line1),
                     "%.20s", body);
            g_msgRows[g_selPeer].saved = true;
            if (isLocalPeer(g_msgRows[g_selPeer].peer)) {
                snprintf(g_msgRows[g_selPeer].line2, sizeof(g_msgRows[g_selPeer].line2),
                         "local %.16s", hex);
            } else {
                formatPeerSub(g_msgRows[g_selPeer].line2, sizeof(g_msgRows[g_selPeer].line2),
                              g_msgRows[g_selPeer].peer, nullptr);
            }
            Serial.printf("[NAME] save %s -> \"%s\" ok=%d line1=\"%s\"\n",
                          hex, body, (int)ok, g_msgRows[g_selPeer].line1);
            g_nameEditMode = false;
            composeClear();
            g_composeMode = false;
            g_msgRowsLoaded = false;
            show_screen(SCR_CONTACTS);
            return true;
        }
        // Contacts: non-empty compose = contact name (never LXMF send)
        if (g_composeLen > 0 && (g_screen == SCR_CONTACTS || g_nameEditMode)) {
            if (g_msgRowCount <= 0 || !g_msgRows[g_selPeer].used) {
                Serial.println("[NAME] select a peer first");
                composeClear();
                g_nameEditMode = false;
                g_composeMode = false;
                return true;
            }
            char body[COMPOSE_CAP + 1];
            memcpy(body, g_compose, g_composeLen + 1);
            char hex[33];
            peerToHex32(g_msgRows[g_selPeer].peer, hex);
            bool ok = true;
            if (isLocalPeer(g_msgRows[g_selPeer].peer)) {
                saveSelfName(body);
            } else {
                ok = g_announceMgr.setContactName(std::string(hex), std::string(body));
                g_announceMgr.saveNode(std::string(hex));
                g_announceMgr.saveNameCache();
                setLocalName(g_msgRows[g_selPeer].peer, body);
            }
            snprintf(g_msgRows[g_selPeer].line1, sizeof(g_msgRows[g_selPeer].line1),
                     "%.20s", body);
            g_msgRows[g_selPeer].saved = true;
            formatPeerSub(g_msgRows[g_selPeer].line2, sizeof(g_msgRows[g_selPeer].line2),
                          g_msgRows[g_selPeer].peer, nullptr);
            Serial.printf("[NAME] save %s -> \"%s\" ok=%d\n", hex, body, (int)ok);
            g_nameEditMode = false;
            composeClear();
            g_composeMode = false;
            g_msgRowsLoaded = false;
            show_screen(SCR_CONTACTS);
            return true;
        }
        if (g_composeLen > 0) {
            // Send only from open thread, never from Msg Box/Peers list
            if (!g_showHistory) {
                Serial.println("[TX] open a chat first (tap peer) to send");
                composeClear();
                g_composeMode = false;
                return true;
            }
            char body[COMPOSE_CAP + 1];
            memcpy(body, g_compose, g_composeLen + 1);
            Serial.printf("[TX] Enter compose len=%u body=\"%s\"\n",
                          (unsigned)g_composeLen, body);
            composeClear();
            g_composeMode = false;
            doSendTest(body);
            return true;
        }
        // Empty Enter on Contacts: start name-edit for selected peer
        if (g_screen == SCR_CONTACTS) {
            if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used) {
                g_nameEditMode = true;
                composeClear();
                g_composeMode = true;
                Serial.println("[NAME] edit mode - type name, Enter=save");
                show_screen(SCR_CONTACTS);
                return true;
            }
            Serial.println("[NAME] select a peer first");
            return false;
        }
        // Empty Enter on Msg Box list: open selected chat
        if (g_screen == SCR_CHATS && !g_showHistory) {
            if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used) {
                g_showHistory = true;
                loadHistoryRows();
                show_screen(SCR_CHATS);
                Serial.println("[KEY] Enter → open thread");
                return true;
            }
            Serial.println("[KEY] select a peer first");
            return false;
        }
        if (g_screen == SCR_PEERS) {
            Serial.println("[KEY] Enter empty - use Msg Box to chat");
            return false;
        }
        // Settings: Enter activates highlighted row (same as touch)
        if (g_screen == SCR_SETTINGS) {
            Serial.printf("[SET] Enter activate sel=%d page=%d\n",
                          g_settingsSel, (int)g_settingsPage);
            settingsActivate();
            return true;
        }
        // Home / other: stay put (do not cycle screens)
        return false;
    }

    if (ev.del) {
        composeBackspace();
        return false;
    }

    if (ev.space) {
        // Space inserts only while composing on Msg Box / Contacts / Peers
        if (g_screen == SCR_CHATS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS ||
            g_nameEditMode) {
            composeAppend(' ');
        }
        return false;
    }

    // Printable character
    if (ev.character >= 32 && ev.character <= 126) {
        // Shortcuts only when compose is empty
        // A announce  S settings  H home  I inbox  M msg box  P peers
        // V view thread  N next peer  T test ping
        if (g_composeLen == 0) {
            const bool onMsgScreens =
                (g_screen == SCR_CHATS || g_screen == SCR_CONTACTS ||
                 g_screen == SCR_PEERS);

            // Settings type-in pages: digits always go to compose (even if empty)
            if (g_screen == SCR_SETTINGS &&
                (g_settingsPage == SET_IDLE_SECS || g_settingsPage == SET_MANUAL_TIME ||
                 g_settingsPage == SET_MANUAL_DATE || g_settingsPage == SET_MANUAL_LOC ||
                 g_idleSecsEditMode || g_timeEditMode || g_dateEditMode)) {
                if (ev.character >= '0' && ev.character <= '9') {
                    composeAppend(ev.character);
                    show_screen(SCR_SETTINGS);
                    return true;
                }
                // allow comma/minus/dot for lat,lon
                if (g_settingsPage == SET_MANUAL_LOC &&
                    (ev.character == ',' || ev.character == '-' || ev.character == '.')) {
                    composeAppend(ev.character);
                    show_screen(SCR_SETTINGS);
                    return true;
                }
            }
            // 1-9: select peer (never type digit while compose empty on msg screens)
            if (g_screen == SCR_SETTINGS && g_composeLen == 0 &&
                g_settingsPage != SET_IDLE_SECS &&
                g_settingsPage != SET_MANUAL_TIME &&
                g_settingsPage != SET_MANUAL_DATE) {
                // W/S move highlight (scroll into view). Lone Shift = Back.
                // G = up (above V), W also up; V/S = down
                if (ev.character == 'g' || ev.character == 'G' ||
                    ev.character == 'w' || ev.character == 'W') {
                    if (g_settingsSel > 0) g_settingsSel--;
                    if (g_settingsSel < g_settingsScroll) g_settingsScroll = g_settingsSel;
                    Serial.printf("[SET] up sel=%d scroll=%d\n", g_settingsSel, g_settingsScroll);
                    show_screen(SCR_SETTINGS);
                    return true;
                }
                if (ev.character == 'v' || ev.character == 'V') {
                    int maxNav = 8;
                    if (g_settingsPage == SET_ROOT) maxNav = 8;
                    else if (g_settingsPage == SET_MANUAL_TIME) maxNav = 5;
                    else if (g_settingsPage == SET_MANUAL_LOC) maxNav = 2;
                    else if (g_settingsPage == SET_IDLE_MODE) maxNav = 3;
                    else if (g_settingsPage == SET_IDLE_SECS) maxNav = 1;
                    else if (g_settingsPage == SET_TIME) maxNav = 4;
                    else if (g_settingsPage == SET_SCREEN) maxNav = 2;
                else if (g_settingsPage == SET_NETWORK) maxNav = 4;
                    else if (g_settingsPage == SET_TZ) maxNav = kTzCount - 1;
                    else if (g_settingsPage == SET_IDENTITY) maxNav = 2;
                    // stop at last row — no wrap
                    if (g_settingsSel < maxNav) g_settingsSel++;
                    // Match build_settings visible window (~4 rows)
                    const int listTop = CONTENT_TOP + 2;
                    const int maxVis = (CONTENT_BOTTOM - listTop - 24) / SET_ROW_H;
                    if (maxVis > 0 && g_settingsSel >= g_settingsScroll + maxVis)
                        g_settingsScroll = g_settingsSel - maxVis + 1;
                    if (g_settingsScroll < 0) g_settingsScroll = 0;
                    Serial.printf("[SET] dn sel=%d scroll=%d\n", g_settingsSel, g_settingsScroll);
                    show_screen(SCR_SETTINGS);
                    return true;
                }
                if (ev.character >= '1' && ev.character <= '9') {
                    g_settingsSel = (int)(ev.character - '1');
                    if (g_settingsSel < g_settingsScroll) g_settingsScroll = g_settingsSel;
                    Serial.printf("[SET] key sel=%d\n", g_settingsSel);
                    if (g_settingsPage == SET_ROOT)
                        settingsActivate();
                    else
                        show_screen(SCR_SETTINGS);
                    return true;
                }
            }
            if (onMsgScreens && ev.character >= '0' && ev.character <= '9') {
                if (g_msgRowCount == 0) {
                    if (g_screen == SCR_PEERS) {
                        appendAnnouncedPeers();
                    } else if (g_screen == SCR_CONTACTS) {
                        show_screen(SCR_CONTACTS);
                    } else {
                        loadMessageRows(true);
                    }
                }
                int idx = ev.character - '0';
                if (idx < g_msgRowCount && g_msgRows[idx].used) {
                    const int was = g_selPeer;
                    g_selPeer = idx;
                    char hex[33];
                    peerToHex32(g_msgRows[idx].peer, hex);
                    Serial.printf("[PEER] key %c -> sel=%d/%d %s\n",
                                  ev.character, g_selPeer, g_msgRowCount, hex);
                    if (g_screen == SCR_CONTACTS) {
                        Serial.println("[PEER] selected - Enter to name, or type name");
                        return true;  // redraw highlight for 0/1/2/...
                    }
                    if (g_selPeer != was)
                        Serial.println("[PEER] selected - type message then Enter");
                    return (g_selPeer != was);
                }
                Serial.printf("[PEER] key %c ignored (rows=%d)\n",
                              ev.character, g_msgRowCount);
                return false;  // do not append digit into compose
            }

            // Msg Box list / Peers list: navigation only (no compose/send).
            // Type+send only inside thread (g_showHistory). Contacts = name edit.
            if (onMsgScreens) {
                if (ev.character == 'b' || ev.character == 'B') {
                    if (g_showHistory || g_nameEditMode) {
                        goBackFromMessage();
                        return true;
                    }
                }
                if (ev.character == 'v' || ev.character == 'V') {
                    if (g_screen == SCR_CHATS) {
                        g_showHistory = !g_showHistory;
                        if (g_showHistory) loadHistoryRows();
                        show_screen(SCR_CHATS);
                        return true;
                    }
                }
                if (ev.character == 'n' || ev.character == 'N') {
                    if (cycleSelPeer()) {
                        if (g_showHistory) loadHistoryRows();
                        return true;
                    }
                    return false;
                }
                // Compose only in thread, or Contacts name-edit
                if (g_showHistory) {
                    // fall through to composeAppend
                } else if (g_screen == SCR_CONTACTS) {
                    // fall through — name edit starts below
                } else {
                    // Msg Box list / Peers: nav letters
                    if (ev.character == 'a' || ev.character == 'A') {
                        doAnnounce();
                        return true;
                    }
                    if (ev.character == 's' || ev.character == 'S') {
                        show_screen(SCR_SETTINGS);
                        return true;
                    }
                    if (ev.character == 'h' || ev.character == 'H') {
                        show_screen(SCR_HOME);
                        return true;
                    }
                    if (ev.character == 'm' || ev.character == 'M') {
                        menuToggle();
                        show_screen(g_screen);
                        return true;
                    }
                    if (ev.character == 'c' || ev.character == 'C') {
                        show_screen(SCR_CONTACTS);
                        return true;
                    }
                    if (ev.character == 'p' || ev.character == 'P') {
                        show_screen(SCR_PEERS);
                        return true;
                    }
                    Serial.println("[KEY] open a chat (tap row) to type a message");
                    return false;
                }
            } else {
                // Home / Settings: full nav shortcuts
                if (ev.character == 'a' || ev.character == 'A') {
                    doAnnounce();
                    if (g_featAudio) g_audio.playAnnounce();
                    return true;
                }
                if (ev.character == 's' || ev.character == 'S') {
                    g_showHistory = false;
                    show_screen(SCR_SETTINGS);
                    return false;
                }
                if (ev.character == 'h' || ev.character == 'H') {
                    g_showHistory = false;
                    show_screen(SCR_HOME);
                    return false;
                }
                if (ev.character == 'm' || ev.character == 'M') {
                    g_showHistory = false;
                    menuToggle();
                    show_screen(g_screen);
                    return true;
                }
                if (ev.character == 'c' || ev.character == 'C') {
                    g_showHistory = false;
                    show_screen(SCR_CONTACTS);
                    return true;
                }
                if (ev.character == 'p' || ev.character == 'P') {
                    g_showHistory = false;
                    show_screen(SCR_PEERS);
                    return true;
                }
                if (ev.character == 't' || ev.character == 'T') {
                    doSendTest(nullptr);
                    return true;
                }
            }
        }
        // Contacts list: never type-to-rename (nav letters collided).
        // Rename only inside name-edit sub-screen (Enter on a row opens it).
        if (g_screen == SCR_CONTACTS && !g_nameEditMode) {
            Serial.println("[NAME] select row, Enter to rename");
            return false;
        }
        // Name-edit sub-screen or message thread: append to compose
        composeAppend(ev.character);
        if (g_screen == SCR_SETTINGS &&
            (g_settingsPage == SET_IDLE_SECS || g_settingsPage == SET_MANUAL_TIME ||
             g_settingsPage == SET_MANUAL_DATE)) {
            show_screen(SCR_SETTINGS);
            return true;
        }
        return false;
    }

    return false;
}

}  // namespace (anonymous)

bool begin() {
    Serial.println();
    loadUiPrefs();   // theme + tz + last epoch (before default seed)
    applyTimezoneEnv();
    ensureDefaultRtc();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase G1.88");
    Serial.println(" Time first; type HHMM; home RTC only");
    Serial.println("========================================");

    handheld::bindDeviceOwner();

    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(30);

    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW); delay(10);
    digitalWrite(TOUCH_RST, HIGH); delay(50);
    pinMode(TOUCH_INT, INPUT_PULLUP);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);

    spiBusIdle();
    logPsram();

    if (!initRadio()) Serial.println("[BOOT] Radio failed");
    initStorageAndProto();
    if (g_destHex[0] == '.') fillDestFromNvs();
    logPsram();

    if (g_storeReady) {
        Serial.printf("[MSG] preload free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        loadMessageRows(true);
    }

    // CRITICAL: finish LXMF outbound recovery while heap is still ~40K+.
    // After LVGL/e-ink, free_int drops to ~12K and requestPending can stall,
    // leaving Rejection::Recovering forever (submit always rejected 8).
    if (g_protoReady) {
        Serial.printf("[BOOT] LXMF recovery drain free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        for (int i = 0; i < 300; i++) {
            g_proto.loop();
            if (g_storeReady) g_msgStore.poll();
            delay(10);
        }
        // Probe: tiny submit attempt to self is invalid; instead try preferred
        // no-op by watching path - just log heap after drain.
        Serial.printf("[BOOT] recovery drain done free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    spiBusIdle();
    Serial.printf("[BOOT] before display free_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (!g_display.begin()) {
        Serial.println("[BOOT] Display FAILED");
        return false;
    }
    Serial.println("[BOOT] display OK");
    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL FAILED");
        return false;
    }
    Serial.println("[BOOT] LVGL OK");
    draw_splash();

    g_kbOk = g_kb.begin();
    applyTimezoneEnv();
    g_lastInputMs = millis() ? millis() : 1;
    gpsBegin();
    if (g_kbOk) Serial.println("[KEYBOARD] TCA8418 keyboard ready");

    g_audio.begin();
    g_audio.setEnabled(g_featAudio);
#if defined(BOARD_MOTOR_PIN) && (BOARD_MOTOR_PIN >= 0)
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    digitalWrite(BOARD_MOTOR_PIN, LOW);
#endif
    Serial.printf("[AUDIO] notify %s motor=%d\n",
                  g_audio.isReady() ? "ready" : "no-i2s",
#if defined(BOARD_MOTOR_PIN)
                  BOARD_MOTOR_PIN
#else
                  -1
#endif
    );

    // E22: probe CST328 (already reset earlier). Soft-fail keeps INT-only next.
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    g_touchOk = (Wire.endTransmission() == 0);
    Serial.printf("[TOUCH] CST328 %s (0x%02X)\n",
                  g_touchOk ? "ok" : "absent", (unsigned)TOUCH_I2C_ADDR);

    show_screen(SCR_HOME);
    Serial.println("[BOOT] ready");
    g_bootAnnAt = millis() + 3000;
    Serial.println("[HINT] type+Enter=send | p=peer | V=thread | a=ann | F=formatFS");
    return true;
}

void loop() {
    if (g_protoReady) g_proto.loop();
    if (g_storeReady) g_msgStore.poll();
    g_announceMgr.loop();
    g_audio.loop();

    // Beep+vibe on new inbound (revision bump while not composing)
    static uint32_t s_prevRev = 0;
    if (g_storeReady) {
        uint32_t rev = g_msgStore.revision();
        if (s_prevRev != 0 && rev > s_prevRev && g_composeLen == 0) {
            if (g_msgStore.totalUnreadCount() > 0)
                notifyInbound();
        }
        s_prevRev = rev;
    }

    if (!g_bootAnnDone && g_bootAnnAt != 0 && millis() >= g_bootAnnAt) {
        g_bootAnnDone = true;
        Serial.println("[ANN] boot announce");
        doAnnounce();
    }

    maybeAutoRedrawMessages();
    maybeAutoRedrawPeers();
    maybeComposeRedraw();
    maybeUpdateIdle();
    maybeHomeMinuteRedraw();
    maybeBatteryPowerRedraw();
    maybeSaveRtcEpoch();
    maybeEnterIdle();

    if (g_kbOk) {
        g_kb.update();
        if (g_kb.hasEvent()) {
            const KeyEvent& ev = g_kb.getEvent();
            bool needRedraw = handleKeyEvent(ev);
            g_kb.discardPending();
            if (needRedraw)
                show_screen(g_screen);
        }
    }

    static int lastInt = HIGH;
    static uint32_t lastTouchMs = 0;
    int tInt = digitalRead(TOUCH_INT);
    if (lastInt == HIGH && tInt == LOW && (millis() - lastTouchMs > 400)) {
        lastTouchMs = millis();
        int16_t tx = 0, ty = 0;
        if (g_touchOk) {
            // CST328 present: only act on successful coordinate reads.
            // Failed reads used to call next_screen() → jump to Settings after peer tap.
            if (cst328Read(tx, ty))
                handleTouchAt(tx, ty);
            else
                Serial.println("[TOUCH] INT no-xy (ignored)");
        } else {
            // Soft-fail path when no touch IC: INT alone cycles screens
            Serial.println("[TOUCH] INT next (no CST328)");
            next_screen();
        }
    }
    lastInt = tInt;

    static char line[180];
    static size_t lineLen = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (lineLen == 0) continue;
            line[lineLen] = '\0';
            lineLen = 0;
            char* p = line;
            while (*p == ' ') p++;
            if (p[0] == 't' || p[0] == 'T') {
                const char* body = nullptr;
                if (p[1] == ':' || p[1] == ' ') {
                    body = p + 2;
                    while (*body == ' ') body++;
                    if (!*body) body = nullptr;
                }
                doSendTest(body);
                if (g_screen == SCR_SETTINGS || g_screen == SCR_CHATS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS)
                    show_screen(g_screen);
            } else if ((p[0] == 'n' || p[0] == 'N') && p[1] == ':') {
                // n:DisplayName - name the selected peer (Msg Box/Peers/Contact)
                const char* nm = p + 2;
                while (*nm == ' ') nm++;
                if (!*nm) {
                    Serial.println("[NAME] usage: n:Alice");
                } else if (g_msgRowCount <= 0 || !g_msgRows[g_selPeer].used) {
                    Serial.println("[NAME] select a peer first (Peers/Msg Box)");
                } else {
                    char hex[33];
                    peerToHex32(g_msgRows[g_selPeer].peer, hex);
                    if (g_announceMgr.setContactName(std::string(hex), std::string(nm))) {
                        g_announceMgr.saveNameCache();
                        g_announceMgr.saveNode(std::string(hex));
                        setLocalName(g_msgRows[g_selPeer].peer, nm);
                        snprintf(g_msgRows[g_selPeer].line1, sizeof(g_msgRows[g_selPeer].line1),
                                 "%.20s", nm);
                        g_msgRows[g_selPeer].saved = true;
                        formatPeerSub(g_msgRows[g_selPeer].line2, sizeof(g_msgRows[g_selPeer].line2),
                                      g_msgRows[g_selPeer].peer, nullptr);
                        Serial.printf("[NAME] %s -> \"%s\" line1=\"%s\"\n",
                                      hex, nm, g_msgRows[g_selPeer].line1);
                        g_msgRowsLoaded = false;
                        show_screen(g_screen);
                    } else {
                        Serial.println("[NAME] setContactName failed");
                    }
                }
            } else if (p[0] == 'L' && p[1] == 0) {
                static bool s_lit = false;
                s_lit = !s_lit;
                if (s_lit) g_kb.backlightOn();
                else g_kb.backlightOff();
                Serial.printf("[KB] backlight %s\n", s_lit ? "ON" : "OFF");
            } else if (p[0] == 'F') {
                // Serial F - erase+format LittleFS (works even when !_ready)
                Serial.println("[FLASH] serial F recovery");
                if (recoverCorruptLittleFS()) {
                    Serial.println("[FLASH] OK - reboot now for clean identity");
                    strcpy(g_announceStr, "fmt ok");
                } else {
                    Serial.println("[FLASH] recovery failed");
                    strcpy(g_announceStr, "fmt fail");
                }
                show_screen(g_screen);
            } else if (p[0] == 'a' || p[0] == 'A') {
                doAnnounce();
                if (g_screen == SCR_SETTINGS) show_screen(SCR_SETTINGS);
            } else if (p[0] == 'r' || p[0] == 'R') {
                loadMessageRows(true);
                show_screen(SCR_CHATS);
            } else if (p[0] == 'H') {
                g_showHistory = !g_showHistory;
                if (g_showHistory) loadHistoryRows();
                show_screen(SCR_CHATS);
            } else if (p[0] == 'n' || p[0] == 'N' || p[0] == '>') {
                g_showHistory = false;
                next_screen();
            } else if (p[0] == 'h' || p[0] == '1') {
                g_showHistory = false;
                show_screen(SCR_HOME);
            } else if (p[0] == 'm' || p[0] == 'M' || p[0] == '2') {
                g_showHistory = false;
                show_screen(SCR_CHATS);
            } else if (p[0] == '3') {
                g_showHistory = false;
                show_screen(SCR_CONTACTS);
            } else if (p[0] == 'e' || p[0] == 'E' || p[0] == '4') {
                g_showHistory = false;
                show_screen(SCR_PEERS);
            } else if (p[0] == 's' || p[0] == 'S' || p[0] == '5') {
                g_showHistory = false;
                show_screen(SCR_SETTINGS);
            } else if (p[0] == 'c' || p[0] == 'C') {
                // serial clear compose
                composeClear();
                Serial.println("[COMPOSE] cleared");
                if (g_screen == SCR_CHATS || g_screen == SCR_SETTINGS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS)
                    show_screen(g_screen);
            } else if (p[0] == 'p' || p[0] == 'P') {
                if (cycleSelPeer()) {
                    if (g_showHistory) loadHistoryRows();
                    if (g_screen == SCR_CHATS || g_screen == SCR_CONTACTS ||
                        g_screen == SCR_PEERS)
                        show_screen(g_screen);
                }
            } else {
                Serial.printf("[CMD] unknown \"%s\" (t/a/r/n/m/h/s/H/c/p)\n", p);
            }
        } else if (c >= 32 && c < 127) {
            if (lineLen + 1 < sizeof(line)) line[lineLen++] = c;
        }
    }
    delay(5);
}

}  // namespace standalone
