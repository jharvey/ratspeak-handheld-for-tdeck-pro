// =============================================================================
// RatSpeak — LilyGo T-Deck Pro standalone shell (clean rewrite)
//
// Spec: RatSpeak_TDeck-Pro_Description.md (keyboard first, LoRa/LXMF first).
//
// Structure
//   1. Services & persisted config
//   2. Platform helpers (battery, SD, GPS, clock, radio, storage/proto)
//   3. Peer rows, history, send
//   4. UI state model  (screen / menu overlay / entry mode / settings stack)
//   5. Drawing primitives + touch hit registry
//   6. Screens (home, lists, thread, rename, settings, idle)
//   7. Input (keys, touch, serial) — handlers mutate state, never draw
//   8. begin() / loop() — the only place that renders
//
// Rules that keep navigation consistent:
//   * Settings rows are built by ONE function per page. Row count, numbering,
//     digit jumps, G/V limits and touch hits are all derived from that list.
//   * Settings sub-pages are a stack: Back (^) returns to the exact row.
//   * Touch hit-boxes are recorded while drawing, so taps always match pixels.
//   * Open thread / rename target are tracked by peer hash, not row index.
//   * Input handlers request a redraw; loop() renders once per pass.
// =============================================================================

#include "StandaloneApp.h"

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <lvgl.h>
#include <Preferences.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <sys/time.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#if !defined(SIMULATOR)
#include <soc/gpio_periph.h>
#include <driver/gpio.h>
#endif
#include <LittleFS.h>

#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include "Eink/RatSpeakLogo.h"
#include "MapView.h"
#include "Chess.h"
#include "audio/AudioNotify.h"
#include "hal/Keyboard.h"
#include "hal/UsbDrive.h"
#include "hal/SysImage.h"
#include "radio/SX1262.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "storage/MessageStore.h"
#include "storage/MessageTransactions.h"
#include "storage/ConversationView.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"
#include "transport/LoRaInterface.h"
#include "transport/WiFiInterface.h"
#include "config/Config.h"
#if !defined(SIMULATOR)
#include "transport/TcpClientSet.h"     // TCP hub client (device only: FreeRTOS tasks + lwIP)
#include "config/UserConfig.h"
#include "ff.h"                          // sdformat: FatFs f_mkfs / f_getfree on the card
#endif
#include "hal/SharedSPIBus.h"
#include "security/SecretVault.h"
#include "remote/ScreenMirror.h"
#include "power/PowerModes.h"
#include "power/LightSleep.h"
#include "power/RtcLog.h"
#include "power/EnergyMonitor.h"
#include "hal/PeerClock.h"
#include "hal/ClockConfidence.h"
#include <WiFi.h>
#include "protocol/ProtocolRuntime.h"
#include "protocol/OutgoingContract.h"
#include "runtime/TaskOwner.h"
#include "ratspeak_protocol.h"
#include <vector>
#include "util/Bytes.h"
#include "util/EventTrace.h"

// hal/Keyboard.cpp — app-driven number layer for numeric fields
extern void keyboardSetNumLayer(bool on);

#if defined(SIMULATOR)
namespace sim { void setPowerInfo(const char* line); }   // side panel (sim/shim)
namespace sim { size_t processBytes(); void wearDump(); }   // sim/shim: process memory, flash wear counters
#endif

namespace standalone {
namespace {

// =============================================================================
// 1. Services & persisted config
// =============================================================================

DisplayEink     g_display;
Keyboard        g_kb;
AudioNotify     g_audio;
FlashStore      g_flash;
IdentityManager g_idMgr;
MessageStore    g_msgStore;
SDStore         g_sdStore;      // shared-SPI locked; the message store mirrors to it when enabled
AnnounceManager g_announceMgr;
ProtocolRuntime g_proto;

bool g_kbOk       = false;
bool g_touchOk    = false;
bool g_storeReady = false;
bool g_protoReady = false;

SX1262*        g_radio  = nullptr;
LoRaInterface* g_loraIf = nullptr;
bool rtRxTap(const uint8_t* d, size_t n, int rssi, float snr);

constexpr const char* kDefaultSelfName = "MyRatSpeakPro";

// Idle screens (S.3.2 "Idle screen"); add new ones at the end.
enum IdleScreen : uint8_t { IDLE_SPLASH, IDLE_MAP, IDLE_LIBRARY, IDLE_FOUND, IDLE_COUNT };
const char* const kIdleScreenName[IDLE_COUNT] = {"Splash", "Map", "Library page", "If found"};

struct Config {
    bool     dark        = false;   // false = white paper, true = black ink
    int      tz          = 0;       // index into kTz
    uint8_t  idleMode    = 1;       // 0 off, 1 on (no password), 2 on + password lock
    uint32_t idleSecs    = 30;      // 10..99999
    bool     beep        = true;
    bool     vibe        = true;
    uint8_t  volume      = 80;      // 0..100 %, steps of 20; 80 spares the small speaker
    uint8_t  idleScreen  = 0;       // IdleScreen: splash, map, library page, if-found
    char     foundText[121] = "";   // if-found screen: owner text, ';' = new line (SYM J)
    bool     foundAddr   = true;    // if-found screen shows this radio's LXMF address
    char     libLast[40] = "";      // library file last read (idle screen "Library")
    uint32_t libLastOff  = 0;       // and the page it was on
    bool     libLastWrap = true;
    bool     clock12     = false;   // 12 h clock (1:45 PM) instead of 24 h
    bool     imperial    = false;   // distances in ft / mi instead of m / km
    uint8_t  lowBatPct   = 0;       // low-battery notice at this % (0 = off)
    uint8_t  lowBatTo[16] = {};     // ... sent to this contact
    bool     lowBatSent  = false;   // sent in this discharge (reset when charging)
    bool     simple      = false;   // Simple mode (wizard step 7; the mode itself comes later)
    bool     wizDone     = true;    // setup wizard finished or skipped
    bool     trackOn     = false;   // breadcrumb track recording (S.1.10)
    uint8_t  coordFmt    = 0;       // 0 decimal degrees, 1 deg-min-sec, 2 MGRS (S.1.12)
    bool     kbLight     = false;
    bool     gpsOn       = true;
    bool     gpsAutoSync = true;
    bool     hasPos      = false;
    float    lat         = 0.0f;
    float    lon         = 0.0f;
};
Config g_cfg;

// ---- Energy modes (Settings > Power; power/PowerModes.h) ----------------------
power::Settings g_pw;                 // chosen mode, USB rule, custom knobs (NVS rs_power)
power::Profile  g_pwEff = power::preset(power::MODE_FULL);   // profile in force now
bool     g_pwOnUsb = false;           // effective profile was forced to Full by USB power
bool     g_gpsAllowed = true;         // power scheduler lets the GPS receiver be powered
// Sleep statistics (decayed every 10 min so they describe the recent past).
uint32_t g_pwAwakeMs = 0, g_pwSleepMs = 0, g_pwSleeps = 0, g_pwWakes[6] = {};
uint8_t  g_epdOffInSleep = 1;         // before light sleep: 1 e-ink power stage off, 2 controller deep sleep (console "epdoff")
uint32_t g_pwLoraWakeMs = 0;         // last wake by a LoRa packet (stay awake for the reply)
uint32_t g_pwSleepTotalMs = 0;       // since boot (not decayed), for the RTC log
uint32_t g_usbSleepUntil = 0;       // console "usbsleep": allow light sleep with a USB host
uint32_t g_benchUntil = 0;            // bench: charger input off, run from battery (console)
void powerApply(bool force);
void powerTouchTick();
void energyAccountAwake();
void energyAfterSleep(uint32_t sleptMs);
bool g_energyShowCurrent = false;     // Energy page: this period instead of the last closed one
bool chargerSetHiz(bool on);

char g_selfName[24] = "";
uint8_t g_selfHash[16] = {0};
bool g_haveSelf = false;
char g_destHex[33] = "";

// ---- Time zones ------------------------------------------------------------
struct TzDef { const char* name; const char* posix; const char* label; };
const TzDef kTz[] = {
    {"EST", "EST5", "UTC-5"},   {"EDT", "EST4", "UTC-4"},
    {"CST", "CST6", "UTC-6"},   {"CDT", "CST5", "UTC-5"},
    {"MST", "MST7", "UTC-7"},   {"MDT", "MST6", "UTC-6"},
    {"PST", "PST8", "UTC-8"},   {"PDT", "PST7", "UTC-7"},
    {"AKST", "AKST9", "UTC-9"}, {"HST", "HST10", "UTC-10"},
    {"UTC", "UTC0", "UTC+0"},   {"UTC+1", "UTC-1", "UTC+1"},
    {"UTC+2", "UTC-2", "UTC+2"}, {"UTC+5:30", "UTC-5:30", "UTC+5:30"},
    {"UTC+8", "UTC-8", "UTC+8"}, {"UTC+9", "UTC-9", "UTC+9"},
    {"UTC-3", "UTC3", "UTC-3"},  {"UTC-10", "UTC10", "UTC-10"},
};
constexpr int kTzCount = sizeof(kTz) / sizeof(kTz[0]);

const TzDef& tzCur() {
    int i = (g_cfg.tz >= 0 && g_cfg.tz < kTzCount) ? g_cfg.tz : 0;
    return kTz[i];
}
void applyTz() {
    setenv("TZ", tzCur().posix, 1);
    tzset();
}

// ---- LoRa link presets (session only; reboot restores Long Fast) -------------
struct LoraPreset { const char* name; uint8_t sf; float bw; uint8_t cr; };
const LoraPreset kLoraPresets[] = {
    {"Long Fast", 11, 250000.f, 5},
    {"Long Slow", 12, 125000.f, 8},
    {"Medium",    10, 250000.f, 5},
    {"Fast Short", 7, 500000.f, 5},
};
constexpr int kLoraPresetCount = sizeof(kLoraPresets) / sizeof(kLoraPresets[0]);
int g_loraPreset = 0;

bool g_wizNeeded = false;          // boot: a wizard was started and not finished
bool g_cfgFresh = false;           // no UI settings saved yet
bool g_hadIdentity = false;        // an identity existed before this boot (not a new device)
bool g_wizOpen = false;            // setup wizard on screen
int  g_wizStep = 0, g_wizGot = 0;    // step; contacts added (radio + SD)
bool g_wizListen = false;          // import step: incoming contact cards are saved
void wizStart();
void wizListen(bool on);
void wizFinish(bool announce);
void wizKey(const KeyEvent& ev);
void wizCardIn(const char* text);
void renderWizard();
void shareAllStart();
int contactsExport();
int contactsImport();

void loadConfig() {
    Preferences p;
    // No settings at all (no namespace, or nothing saved in it yet): a fresh device.
    // The wizard runs only if there is no identity from an earlier boot either (decided at
    // the end of boot, see g_hadIdentity): a used unit whose owner never changed a setting
    // does not see it.
    if (!p.begin("rs_ui", true) || !p.isKey("idleMode")) {
        p.end();
        g_cfgFresh = true;
        return;
    }
    g_cfg.dark        = p.getBool("dark", false);
    g_cfg.tz          = p.getInt("tz", 0);
    g_cfg.idleMode    = p.getUChar("idleMode", 1);
    g_cfg.idleSecs    = p.getUInt("idleSecs", 30);
    g_cfg.beep        = p.getBool("beep", true);
    g_cfg.vibe        = p.getBool("vibe", true);
    g_cfg.volume      = p.getUChar("vol", 80);
    // "idleScr" replaces the old idleMap bool (true -> Map).
    if (p.isKey("idleScr")) g_cfg.idleScreen = p.getUChar("idleScr", IDLE_SPLASH);
    else g_cfg.idleScreen = p.getBool("idleMap", false) ? IDLE_MAP : IDLE_SPLASH;
    if (g_cfg.idleScreen >= IDLE_COUNT) g_cfg.idleScreen = IDLE_SPLASH;
    p.getString("foundTxt", g_cfg.foundText, sizeof(g_cfg.foundText));
    g_cfg.foundAddr   = p.getBool("foundAddr", true);
    p.getString("libLast", g_cfg.libLast, sizeof(g_cfg.libLast));
    g_cfg.libLastOff  = p.getUInt("libOff", 0);
    g_cfg.libLastWrap = p.getBool("libWrap", true);
    g_cfg.clock12     = p.getBool("clk12", false);
    g_cfg.imperial    = p.getBool("imperial", false);
    g_cfg.lowBatPct   = p.getUChar("lbPct", 0);
    p.getBytes("lbTo", g_cfg.lowBatTo, 16);
    g_cfg.lowBatSent  = p.getBool("lbSent", false);
    g_cfg.simple      = p.getBool("simple", false);
    // Units that had settings before the wizard existed never see it (flag absent = done).
    g_cfg.wizDone     = p.isKey("wizDone") ? p.getBool("wizDone", true) : true;
    g_wizNeeded       = !g_cfg.wizDone;
    g_cfg.trackOn     = p.getBool("trackOn", false);
    g_cfg.coordFmt    = p.getUChar("coordFmt", 0);
    if (g_cfg.coordFmt > 2) g_cfg.coordFmt = 0;
    g_cfg.kbLight     = p.getBool("kbl", false);
    g_cfg.gpsOn       = p.getBool("gps", true);
    g_cfg.gpsAutoSync = p.getBool("gpsSync", true);
    g_cfg.hasPos      = p.getBool("hasPos", false);
    g_cfg.lat         = p.getFloat("lat", 0.0f);
    g_cfg.lon         = p.getFloat("lon", 0.0f);
    int64_t epoch     = p.getLong64("epoch", 0);
    p.end();

    if (g_cfg.tz < 0 || g_cfg.tz >= kTzCount) g_cfg.tz = 0;
    if (g_cfg.idleMode > 2) g_cfg.idleMode = 1;   // 2 = on + password (needs a vault password)
    if (g_cfg.idleSecs < 10 || g_cfg.idleSecs > 99999) g_cfg.idleSecs = 30;
    if (g_cfg.volume > 100) g_cfg.volume = 80;

    if (epoch > 100000) {
        struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        Serial.printf("[RTC] restored epoch=%lld\n", (long long)epoch);
    }
    Serial.printf("[CFG] theme=%s tz=%s idle=%u/%us\n", g_cfg.dark ? "dark" : "light",
                  tzCur().name, (unsigned)g_cfg.idleMode, (unsigned)g_cfg.idleSecs);
}

void saveConfig() {
    Preferences p;
    if (!p.begin("rs_ui", false)) return;
    p.putBool("dark", g_cfg.dark);
    p.putInt("tz", g_cfg.tz);
    p.putUChar("idleMode", g_cfg.idleMode);
    p.putUInt("idleSecs", g_cfg.idleSecs);
    p.putBool("beep", g_cfg.beep);
    p.putBool("vibe", g_cfg.vibe);
    p.putUChar("vol", g_cfg.volume);
    p.putUChar("idleScr", g_cfg.idleScreen);
    if (p.isKey("idleMap")) p.remove("idleMap");
    p.putString("foundTxt", g_cfg.foundText);
    p.putBool("foundAddr", g_cfg.foundAddr);
    p.putString("libLast", g_cfg.libLast);
    p.putUInt("libOff", g_cfg.libLastOff);
    p.putBool("libWrap", g_cfg.libLastWrap);
    p.putBool("clk12", g_cfg.clock12);
    p.putBool("imperial", g_cfg.imperial);
    p.putUChar("lbPct", g_cfg.lowBatPct);
    p.putBytes("lbTo", g_cfg.lowBatTo, 16);
    p.putBool("lbSent", g_cfg.lowBatSent);
    p.putBool("simple", g_cfg.simple);
    p.putBool("wizDone", g_cfg.wizDone);
    p.putBool("trackOn", g_cfg.trackOn);
    p.putUChar("coordFmt", g_cfg.coordFmt);
    p.putBool("kbl", g_cfg.kbLight);
    p.putBool("gps", g_cfg.gpsOn);
    p.putBool("gpsSync", g_cfg.gpsAutoSync);
    p.putBool("hasPos", g_cfg.hasPos);
    p.putFloat("lat", g_cfg.lat);
    p.putFloat("lon", g_cfg.lon);
    time_t now = time(nullptr);
    if (now > 100000) p.putLong64("epoch", (int64_t)now);
    p.end();
}

void saveEpochThrottled() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (last && now - last < 60000UL) return;
    last = now ? now : 1;
    time_t t = time(nullptr);
    if (t < 100000) return;
    Preferences p;
    if (!p.begin("rs_ui", false)) return;
    p.putLong64("epoch", (int64_t)t);
    p.end();
}

// Default display name: "TDP-" + the last two MAC bytes (e.g. TDP-179C). Older firmware
// gave every unit the same "MyRatSpeakPro"; that stored default is replaced too.
void defaultSelfName(char* out, size_t n) {
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(out, n, "TDP-%02X%02X", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
}

void loadSelfName() {
    Preferences p;
    if (p.begin("rs_self", true)) {
        String s = p.getString("name", "");
        if (s.length()) snprintf(g_selfName, sizeof(g_selfName), "%.23s", s.c_str());
        p.end();
    }
    if (!g_selfName[0] || !strcmp(g_selfName, kDefaultSelfName)) {
        defaultSelfName(g_selfName, sizeof(g_selfName));
        Preferences w;
        if (w.begin("rs_self", false)) { w.putString("name", g_selfName); w.end(); }
        Serial.printf("[NAME] default -> %s\n", g_selfName);
    }
}

void saveSelfName(const char* name) {
    if (name && name[0]) snprintf(g_selfName, sizeof(g_selfName), "%.23s", name);
    else                 defaultSelfName(g_selfName, sizeof(g_selfName));
    Preferences p;
    if (p.begin("rs_self", false)) { p.putString("name", g_selfName); p.end(); }
    Serial.printf("[NAME] self -> \"%s\"\n", g_selfName);
}

// =============================================================================
// 2. Platform helpers
// =============================================================================

void spiBusIdle() {
    pinMode(EPD_CS, OUTPUT);  digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
    pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);
}

void hashToHex(const uint8_t h[16], char out[33]) {
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", h[i]);
    out[32] = '\0';
}

bool isSelf(const uint8_t h[16]) {
    return g_haveSelf && h && memcmp(h, g_selfHash, 16) == 0;
}

void setSelfHash(const uint8_t h[16]) {
    memcpy(g_selfHash, h, 16);
    g_haveSelf = true;
    hashToHex(h, g_destHex);
}

// ---- Battery (BQ27220 @0x55) -------------------------------------------------
struct Battery {
    float    volts    = 0;
    int      pct      = -1;      // displayed (calibrated) percent
    int      rawSoc   = -1;      // BQ27220 StateOfCharge as reported
    int16_t  avgMa    = 0;
    uint16_t flags    = 0;
    uint16_t remMah   = 0;       // RemainingCapacity
    uint16_t fccMah   = 0;       // FullChargeCapacity
    uint16_t tteMin   = 0xFFFF;  // TimeToEmpty (min), 0xFFFF = not discharging
    uint8_t  chrgStat = 0xFF;    // BQ25896 CHRG_STAT, 0xFF = unreadable
    bool     charging = false;
    bool     usb      = false;
    bool     full     = false;   // charger reports charge done
    bool     valid    = false;
};
Battery g_bat;

int lipoPctFromVolts(float v) {
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

bool bqReadU16(uint8_t reg, uint16_t& out) {
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    out = (uint16_t)(Wire.read() | (Wire.read() << 8));
    return true;
}

// ---- BQ25896 charger status (REG0B) ---------------------------------------------
// VBUS_STAT[7:5]  CHRG_STAT[4:3] (0 none, 1 pre, 2 fast, 3 done)  PG_STAT[2]
bool chargerReadStatus(uint8_t& chrgStat, bool& powerGood) {
    Wire.beginTransmission(BQ25896_I2C_ADDR);
    Wire.write((uint8_t)0x0B);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ25896_I2C_ADDR, 1) != 1) return false;
    uint8_t r = Wire.read();
    chrgStat = (r >> 3) & 0x03;
    powerGood = (r & 0x04) != 0;
    return true;
}

// ---- Full-charge calibration --------------------------------------------------------
// The BQ27220 on this board often never sees the charge-taper it needs to report
// 100% (the charger terminates first), so its SOC tops out ~85%. Each device learns
// the raw gauge SOC at a real charge-done and scales by it. Stored per device in NVS.
uint8_t g_fullSoc = 100;          // raw gauge SOC that means "full" (60..100)

void loadBatteryCal() {
    Preferences p;
    if (!p.begin("rs_bat", true)) return;
    uint8_t v = p.getUChar("fullSoc", 100);
    p.end();
    g_fullSoc = (v >= 60 && v <= 100) ? v : 100;
    Serial.printf("[BAT] full-charge cal = %u%%\n", (unsigned)g_fullSoc);
}

void saveBatteryCal(uint8_t v) {
    g_fullSoc = v;
    Preferences p;
    if (p.begin("rs_bat", false)) { p.putUChar("fullSoc", v); p.end(); }
    Serial.printf("[BAT] full-charge cal learned = %u%%\n", (unsigned)v);
}

// Watches for a confirmed full charge and learns g_fullSoc from it.
//   Primary: charger reports CHRG_STAT=done with power good for 30 s.
//   Fallback (charger unreadable): USB, >=4.15 V, 0..60 mA into the cell for 60 s.
void learnFullCharge(bool full, bool fallback, int rawSoc) {
    static uint32_t since = 0;
    static int episodeMax = -1;
    if (!full || rawSoc < 0) { since = 0; episodeMax = -1; return; }
    if (!since) since = millis() ? millis() : 1;
    if (rawSoc > episodeMax) episodeMax = rawSoc;
    const uint32_t need = fallback ? 60000UL : 30000UL;
    if (millis() - since < need) return;
    if (episodeMax < 60 || episodeMax > 100) return;      // implausible: ignore
    if (episodeMax != g_fullSoc) saveBatteryCal((uint8_t)episodeMax);
}

// ---- Learned battery capacity (Future_steps 15) ------------------------------------------
// The mAh this battery really holds, learned from ordinary use. Between two points whose
// charge is known without the gauge's own capacity model - a confirmed full charge (100 %)
// or the cell at rest (draw under 100 mA for 10 min; voltage + draw x 0.15 ohm read on a
// rested-cell table) - the mAh counted on battery (gauge current, as the energy monitor and
// "time left" count it) divided by the % drop is one estimate. Only drops of 40 % or more
// count; each is averaged in by the size of its drop (newer ones keep weight: the sum is
// capped at 300). Per unit in NVS rs_bat (capMah / capW / capN), so a same-unit backup
// carries it. Runtimes (S.4.12), "time left" and the battery page use it once there is one.
constexpr uint16_t kNominalMah = 1450;     // a new battery (published figure)
uint16_t g_capMah = 0;                     // learned, 0 = not yet
uint16_t g_capW = 0;                       // sum of the % drops used (weight), capped
uint8_t  g_capN = 0;                       // discharges used
double   g_capMas = 0;                     // mA x s drawn on battery since the anchor
int      g_capAnchor = -1;                 // % at the anchor, -1 = none
bool     g_capSawFull = false;             // charge-done seen while on USB
uint32_t g_capCalmSince = 0, g_capLastRest = 0;
char     g_capLast[48] = "none yet";

// Rested Li-ion / LiPo cell (no load for a while): volts -> %.
int restedPct(float v) {
    static const float vv[] = {3.27f, 3.61f, 3.69f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f, 3.82f, 3.84f,
                               3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.08f, 4.11f, 4.15f, 4.20f};
    constexpr int n = sizeof(vv) / sizeof(vv[0]);        // 0, 5, 10 ... 100 %
    if (v <= vv[0]) return 0;
    if (v >= vv[n - 1]) return 100;
    for (int i = 0; i < n - 1; i++)
        if (v <= vv[i + 1]) return (int)(5 * i + 5 * (v - vv[i]) / (vv[i + 1] - vv[i]) + 0.5f);
    return 50;
}

void capLoad() {
    Preferences p;
    if (!p.begin("rs_bat", true)) return;
    g_capMah = p.getUShort("capMah", 0);
    g_capW = p.getUShort("capW", 0);
    g_capN = p.getUChar("capN", 0);
    p.end();
    if (g_capMah < 600 || g_capMah > 3000) g_capMah = g_capW = g_capN = 0;
    if (g_capMah) Serial.printf("[BAT] learned capacity %u mAh from %u discharges\n", (unsigned)g_capMah, (unsigned)g_capN);
}

void capSave() {
    Preferences p;
    if (!p.begin("rs_bat", false)) return;
    p.putUShort("capMah", g_capMah);
    p.putUShort("capW", g_capW);
    p.putUChar("capN", g_capN);
    p.end();
}

void capAnchor(int pct, const char* how) {
    g_capAnchor = pct;
    g_capMas = 0;
    Serial.printf("[BAT] capacity: start counting at %d %% (%s)\n", pct, how);
}

// One discharge from fromPct to toPct that used mah.
void capSample(int fromPct, int toPct, float mah) {
    const int span = fromPct - toPct;
    if (span < 40 || mah <= 0) return;
    const float est = mah * 100.0f / span;
    if (est < 600 || est > 3000) {
        snprintf(g_capLast, sizeof(g_capLast), "%d->%d%%: %u mAh, not used", fromPct, toPct, (unsigned)est);
        Serial.printf("[BAT] capacity: %s (outside 600-3000)\n", g_capLast);
        return;
    }
    g_capMah = g_capW ? (uint16_t)((g_capMah * (float)g_capW + est * span) / (g_capW + span) + 0.5f) : (uint16_t)(est + 0.5f);
    g_capW = (uint16_t)(g_capW + span > 300 ? 300 : g_capW + span);
    if (g_capN < 255) g_capN++;
    capSave();
    snprintf(g_capLast, sizeof(g_capLast), "%d->%d%%: %u mAh", fromPct, toPct, (unsigned)est);
    Serial.printf("[BAT] capacity: %s (%.0f mAh used) -> learned %u mAh, %u discharges\n", g_capLast, mah,
                  (unsigned)g_capMah, (unsigned)g_capN);
}

// Capacity for estimates; *learned says which.
uint16_t batCapacityMah(bool* learned) {
    if (learned) *learned = g_capMah != 0;
    if (g_capMah) return g_capMah;
    return (g_bat.valid && g_bat.fccMah >= 500 && g_bat.fccMah <= 6000) ? g_bat.fccMah : kNominalMah;
}

// After each battery reading (main loop).
void capTick() {
    if (!g_bat.valid) return;
    const uint32_t now = millis();
    if (g_bat.usb) {                                  // on USB: counting starts again at unplug
        if (g_bat.full) g_capSawFull = true;
        g_capAnchor = -1;
        g_capMas = 0;
        g_capCalmSince = 0;
        return;
    }
    if (g_capSawFull) { g_capSawFull = false; capAnchor(100, "full charge"); }
    if (g_bat.avgMa > 0 || -g_bat.avgMa >= 100) { g_capCalmSince = 0; return; }
    if (!g_capCalmSince) g_capCalmSince = now ? now : 1;
    if (now - g_capCalmSince < 600000UL || (g_capLastRest && now - g_capLastRest < 1800000UL)) return;
    g_capLastRest = now ? now : 1;
    const float ocv = g_bat.volts + (-g_bat.avgMa) * 0.00015f;     // 0.15 ohm cell + wiring
    const int pct = restedPct(ocv);
    Serial.printf("[BAT] capacity: rest %.3f V (%d mA) = %d %%, anchor %d %%, %.0f mAh since\n", ocv,
                  (int)-g_bat.avgMa, pct, g_capAnchor, g_capMas / 3600.0);
    if (g_capAnchor < 0) capAnchor(pct, "rest");
    else if (g_capAnchor - pct >= 40) { capSample(g_capAnchor, pct, (float)(g_capMas / 3600.0)); capAnchor(pct, "rest"); }
    else if (pct > g_capAnchor + 5) capAnchor(pct, "rest (higher)");   // the first reading was low
}

// Voltage 0x08, Flags 0x0A, RemainingCap 0x10, FullChargeCap 0x12,
// AvgCurrent 0x14, StateOfCharge 0x2C
bool readBattery() {
    uint32_t mvSum = 0, socSum = 0;
    int nOk = 0, nSoc = 0, nCur = 0;
    int32_t curSum = 0;
    uint16_t flags = 0;
    for (int i = 0; i < 2; i++) {     // 2 samples, no sleeps: keeps the input loop snappy
        uint16_t mv = 0, soc = 0, cur = 0, fl = 0;
        if (!bqReadU16(0x08, mv) || mv < 2500 || mv > 4500) continue;
        mvSum += mv; nOk++;
        if (bqReadU16(0x2C, soc) && soc <= 100) { socSum += soc; nSoc++; }
        if (bqReadU16(0x14, cur)) { curSum += (int16_t)cur; nCur++; }
        if (bqReadU16(0x0A, fl)) flags = fl;
    }
    if (nOk == 0) { g_bat.valid = false; return false; }
    uint16_t rem = 0, fcc = 0;
    bqReadU16(0x10, rem);
    bqReadU16(0x12, fcc);
    uint16_t tte = 0xFFFF;
    bqReadU16(0x16, tte);

    const float volts = (mvSum / (float)nOk) / 1000.0f;
    const int16_t avgMa = nCur ? (int16_t)(curSum / nCur) : 0;
    const bool dsg = (flags & 0x0001) != 0;
    const int raw = nSoc ? (int)(socSum / nSoc) : -1;

    // Charging / USB: trust the charger IC; fall back to current heuristics.
    uint8_t chrg = 0;
    bool pg = false;
    const bool chargerOk = chargerReadStatus(chrg, pg);
    bool charging, usb, chargeDone;
    if (chargerOk) {
        charging = (chrg == 1 || chrg == 2);
        chargeDone = pg && chrg == 3;
        usb = pg;
    } else {
        static bool s_chg = false, s_usb = false;
        static uint32_t s_lastChargeMs = 0;
        if (avgMa > 15) s_chg = true;
        else if (avgMa < 0 || (dsg && avgMa <= 5)) s_chg = false;
        if (avgMa > 20) s_lastChargeMs = millis();
        if (s_chg || avgMa > 20) s_usb = true;
        else if (avgMa < 0 || (dsg && avgMa <= 5)) s_usb = false;
        if (!s_chg && avgMa <= 8 && (millis() - s_lastChargeMs) > 2500UL) s_usb = false;
        charging = s_chg;
        usb = s_usb;
        chargeDone = usb && volts >= 4.15f && avgMa >= 0 && avgMa <= 60;
    }
    learnFullCharge(chargeDone, !chargerOk, raw);

    // Scale the raw gauge by this device's learned full point, then blend toward
    // the voltage curve only when the two disagree badly (unlearned gauge).
    int pct = lipoPctFromVolts(volts);
    if (raw >= 0) {
        const int gauge = constrain((raw * 100 + g_fullSoc / 2) / g_fullSoc, 0, 100);
        const int curve = pct;
        const int err = abs(gauge - curve);
        if (err <= 25)                                       pct = gauge;
        else if (charging && volts >= 4.00f)                 pct = (gauge + curve * 2) / 3;
        else if (!charging && volts <= 3.55f && gauge > curve + 20) pct = (gauge + curve * 2) / 3;
        else if (err > 45)                                   pct = (gauge + curve) / 2;
        else                                                 pct = (gauge * 2 + curve) / 3;
    }
    if (chargeDone) pct = 100;    // charger says full: it is full

    g_bat.volts = volts;
    g_bat.pct = constrain(pct, 0, 100);
    g_bat.rawSoc = raw;
    g_bat.avgMa = avgMa;
    g_bat.flags = flags;
    g_bat.remMah = rem;
    g_bat.fccMah = fcc;
    g_bat.tteMin = tte;
    g_bat.chrgStat = chargerOk ? chrg : 0xFF;
    g_bat.charging = charging;
    g_bat.usb = usb;
    g_bat.full = chargeDone;
    g_bat.valid = true;
    return true;
}

// ---- SD card + battery log --------------------------------------------------
bool g_sdMounted = false;
// Boot phase marks, kept in RAM too: the first seconds are lost on the PC while USB comes
// up (console "boottime").
struct BootMark { uint32_t ms; const char* phase; };
BootMark g_bootMarks[16];
int g_bootMarkN = 0;
bool g_usbDrive = false;          // the card is a USB drive on the PC: no file access until restart
void usbDriveStart();
// SD tools (S.10.5 rows 8-10): back up / restore, firmware update. Defined after the USB block.
constexpr int kMaxBackups = 12;
char g_bkNames[kMaxBackups][24];
int g_bkCount = 0;
char g_fwName[40] = "";
uint32_t g_fwSize = 0;
bool g_fwInstalled = false;
void sdToolsScan(bool force);
void backupNow();
void backupsOpen();
void backupPick(int i);
void fwUpdateKey();
void backupPretty(const char* name, char* out, size_t cap);
char g_sdStatus[28] = "not mounted";
constexpr const char* kBatLogPath = "/batlog2.csv";
constexpr const char* kBatLogHeader =
    "ms,mv,avgMa,pct,rawSoc,remMah,fccMah,fullCal,chrgStat,flags,chg,usb";

// SD message archive (NVS rs_sd/msgs, default OFF until timed on hardware): with a card at
// boot the message store writes every new message to the card as well as to flash (a
// mirror / backup). No flash eviction: nothing on this board requests Trim, whose passes
// reload every record of a conversation. Read at boot only (media fixed by begin()).
bool sdArchiveEnabled() {
    Preferences p;
    if (!p.begin("rs_sd", true)) return false;
    const bool on = p.getBool("msgs", false);
    p.end();
    return on;
}
void sdArchiveSet(bool on) {
    Preferences p;
    if (p.begin("rs_sd", false)) { p.putBool("msgs", on); p.end(); }
}
bool g_sdArchiveActive = false;   // the store was started with the card as its archive

void sdStatusRefresh() {
    if (!g_sdStore.isReady()) { snprintf(g_sdStatus, sizeof(g_sdStatus), "not mounted"); return; }
    snprintf(g_sdStatus, sizeof(g_sdStatus), "OK %u/%u MB",
             (unsigned)(g_sdStore.usedBytes() >> 20), (unsigned)(g_sdStore.totalBytes() >> 20));
}

bool g_haveMaps = false;          // the card has a maps folder: the middle tab is Atlas, else Peers

bool sdMount() {
    if (g_usbDrive) { snprintf(g_sdStatus, sizeof(g_sdStatus), "USB drive (on PC)"); return false; }
    if (g_sdMounted && g_sdStore.isReady()) return true;
    spiBusIdle();
    if (!g_sdStore.begin(&SPI, SD_CS)) {
        g_sdMounted = false;
        snprintf(g_sdStatus, sizeof(g_sdStatus), "mount failed");
        Serial.println("[SD] mount failed");
        return false;
    }
    g_sdMounted = true;
    if (!g_sdStore.formatForRsDeck()) Serial.println("[SD] folder setup failed");
    g_haveMaps = g_sdStore.exists(SD_PATH_ROOT "/maps");
    sdStatusRefresh();
    if (!g_sdStore.exists(kBatLogPath)) {
        File f = g_sdStore.openFile(kBatLogPath, FILE_WRITE);
        if (f) { f.println(kBatLogHeader); f.close(); }
    }
    Serial.printf("[SD] mounted %s\n", g_sdStatus);
    return true;
}

struct BatLogSample {
    uint32_t ms; uint16_t mv; int16_t avgMa; int8_t pct, rawSoc;
    uint16_t remMah, fccMah, flags; uint8_t fullCal, chrgStat, chg, usb;
};
constexpr int kBatLogCap = 120;   // 10 min at 5 s
BatLogSample g_batLog[kBatLogCap];
uint16_t g_batLogHead = 0, g_batLogCount = 0;
bool g_batLogInternal = false, g_batLogSd = false;

void mapReadDone();

void sdUnmount() {
    g_batLogSd = false;
    mapReadDone();
    if (g_sdMounted) { g_sdStore.end(); g_sdMounted = false; }
    g_haveMaps = false;
    snprintf(g_sdStatus, sizeof(g_sdStatus), "not mounted");
    Serial.println("[SD] unmounted");
}

void batLogSample() {
    BatLogSample s{};
    s.ms = millis();
    s.mv = (uint16_t)(g_bat.volts * 1000.0f + 0.5f);
    s.avgMa = g_bat.avgMa;
    s.pct = (int8_t)g_bat.pct;
    s.rawSoc = (int8_t)g_bat.rawSoc;
    s.remMah = g_bat.remMah;
    s.fccMah = g_bat.fccMah;
    s.fullCal = g_fullSoc;
    s.chrgStat = g_bat.chrgStat;
    s.flags = g_bat.flags;
    s.chg = g_bat.charging;
    s.usb = g_bat.usb;
    if (g_batLogInternal) {
        g_batLog[g_batLogHead] = s;
        g_batLogHead = (g_batLogHead + 1) % kBatLogCap;
        if (g_batLogCount < kBatLogCap) g_batLogCount++;
    }
    if (g_batLogSd && g_sdMounted) {
        File f = g_sdStore.openFile(kBatLogPath, FILE_APPEND);
        if (f) {
            f.printf("%lu,%u,%d,%d,%d,%u,%u,%u,%u,0x%04X,%u,%u\n", (unsigned long)s.ms, s.mv,
                     s.avgMa, s.pct, s.rawSoc, s.remMah, s.fccMah, s.fullCal, s.chrgStat,
                     s.flags, s.chg, s.usb);
            f.close();
        }
    }
}

void batLogTick() {
    static uint32_t last = 0;
    if (!g_batLogInternal && !g_batLogSd) return;
    if (millis() - last < 5000UL) return;
    last = millis();
    batLogSample();
}

void batLogDump() {
    Serial.println("[BATLOG] ---- begin ----");
    Serial.println(kBatLogHeader);
    uint16_t start = (g_batLogHead + kBatLogCap - g_batLogCount) % kBatLogCap;
    for (uint16_t i = 0; i < g_batLogCount; i++) {
        const BatLogSample& s = g_batLog[(start + i) % kBatLogCap];
        Serial.printf("%lu,%u,%d,%d,%d,%u,%u,%u,%u,0x%04X,%u,%u\n", (unsigned long)s.ms, s.mv,
                      s.avgMa, s.pct, s.rawSoc, s.remMah, s.fccMah, s.fullCal, s.chrgStat,
                      s.flags, s.chg, s.usb);
    }
    Serial.printf("[BATLOG] ---- %u samples ----\n", (unsigned)g_batLogCount);
}

// ---- Clock ---------------------------------------------------------------------
void setEpoch(time_t epoch) {
    if (epoch <= 0) return;
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
}

void ensureClockSeeded() {
    if (time(nullptr) >= 100000) return;
    applyTz();
    struct tm t = {};
    t.tm_year = 2027 - 1900; t.tm_mon = 0; t.tm_mday = 1; t.tm_hour = 12; t.tm_isdst = -1;
    setEpoch(mktime(&t));
    Serial.println("[RTC] seeded 2027-01-01 12:00 local");
}

bool localNow(struct tm& t) {
    ensureClockSeeded();
    time_t now = time(nullptr);
    if (now < 100000) return false;
    applyTz();
    localtime_r(&now, &t);
    return true;
}

void fmtTime(char* out, size_t n) {
    struct tm t;
    if (!localNow(t)) snprintf(out, n, "--:--");
    else if (g_cfg.clock12) snprintf(out, n, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min,
                                     t.tm_hour < 12 ? "AM" : "PM");
    else snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

// A minute of the day as "7:30 am" (12 h clock) or "07:30".
void fmtClockMin(int m, char* out, size_t n) {
    const int h = (m / 60) % 24, mm = m % 60;
    if (g_cfg.clock12) snprintf(out, n, "%d:%02d %s", h % 12 ? h % 12 : 12, mm, h < 12 ? "am" : "pm");
    else snprintf(out, n, "%02d:%02d", h, mm);
}

void fmtDate(char* out, size_t n) {
    struct tm t;
    if (localNow(t)) snprintf(out, n, "%02d/%02d/%04d", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);
    else snprintf(out, n, "--/--/----");
}

// Set UTC clock from broken-down UTC fields.
bool setClockUtc(int y, int mo, int d, int hh, int mm, int ss) {
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
    setenv("TZ", "UTC0", 1); tzset();
    time_t e = mktime(&t);
    applyTz();
    if (e <= 0) return false;
    setEpoch(e);
    return true;
}

// Set clock from local wall-clock fields under the selected time zone.
bool setClockLocal(int y, int mo, int d, int hh, int mm) {
    applyTz();
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = hh; t.tm_min = mm; t.tm_isdst = -1;
    time_t e = mktime(&t);
    if (e <= 0) return false;
    setEpoch(e);
    return true;
}

// ---- GPS (UART1, minimal RMC/GGA parser) ---------------------------------------
HardwareSerial* g_gpsSerial = nullptr;
bool    g_gpsFix = false;
uint8_t g_gpsSats = 0;
float    g_gpsSpeedKn = 0, g_gpsCourse = 0;   // RMC: speed over ground (knots), course (deg true)
bool     g_gpsCourseOk = false;
uint32_t g_gpsCourseAt = 0;
bool    g_gpsPosValid = false;   // last GGA/RMC with a fix carried a position
double  g_gpsLat = 0, g_gpsLon = 0;
uint32_t g_gpsPosAt = 0;
float   g_gpsHdop = 0;               // GGA horizontal dilution (accuracy ~ HDOP x 5 m)

// NMEA ddmm.mmmm / dddmm.mmmm + hemisphere -> signed degrees. false if empty.
bool nmeaDegrees(const char* v, const char* hemi, double& out) {
    if (!v || !v[0] || !hemi || !hemi[0]) return false;
    const double raw = atof(v);
    const int deg = (int)(raw / 100.0);
    out = deg + (raw - deg * 100.0) / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') out = -out;
    return true;
}
void gpsNotePosition(const char* lat, const char* ns, const char* lon, const char* ew) {
    double la, lo;
    if (!nmeaDegrees(lat, ns, la) || !nmeaDegrees(lon, ew, lo)) return;
    g_gpsLat = la; g_gpsLon = lo; g_gpsPosValid = true; g_gpsPosAt = millis();
}
char    g_gpsStatus[16] = "off";
int     g_gpsUtc[6] = {0};       // y mo d hh mm ss of last valid RMC
bool    g_gpsUtcValid = false;
bool    g_gpsSyncedOnce = false;
uint32_t g_gpsLastSyncMs = 0;

// What the receiver hears (from GSV), per constellation talker, plus link health.
struct GpsSky { char talker[3]; uint8_t inView, heard, heardAcc, bestSnr, bestAcc; uint32_t atMs; };
GpsSky   g_gpsSky[6] = {};
uint32_t g_gpsBytes = 0, g_gpsGood = 0, g_gpsBad = 0;
char     g_gpsLastGga[96] = "", g_gpsLastRmc[96] = "";

// Link recovery: a u-blox M10 keeps its settings in battery-backed RAM, so a config saved
// by other firmware (NMEA off, power save, another baud) survives power cycles and the
// receiver says nothing. When no valid sentence has arrived for a while, cycle the baud
// rate and send the vendor's "clear saved config + load defaults" (UBX-CFG-CFG), like
// LilyGo's own GPS_Recovery(). An L76K (9600 baud) simply ignores the UBX frames.
uint32_t g_gpsLastGoodMs = 0, g_gpsStepAt = 0, g_gpsBaud = GPS_BAUD;
uint8_t  g_gpsStep = 0;
// "CANNOT USE BBR -> NO GNSS OPERATION": the receiver will not navigate because its backup
// domain (V_BCKP, fed by the small MS621FE cell J6 through D8 + R15) is not usable. The
// cell recharges only while the device is on, so keep power-cycling the receiver now and
// then until it starts navigating.
bool     g_gpsBbrFault = false;
uint32_t g_gpsBbrFaultCount = 0, g_gpsPowerOffAt = 0, g_gpsNextCycleAt = 0;
uint32_t g_gpsVisitStart = 0, g_gpsNextVisit = 0;   // periodic GPS schedule (powerGpsTick)

GpsSky* gpsSkyFor(const char* talker) {
    GpsSky* freeSlot = nullptr;
    for (auto& k : g_gpsSky) {
        if (k.talker[0] == talker[0] && k.talker[1] == talker[1]) return &k;
        if (!k.talker[0] && !freeSlot) freeSlot = &k;
    }
    if (freeSlot) { freeSlot->talker[0] = talker[0]; freeSlot->talker[1] = talker[1]; }
    return freeSlot;
}

// Satellites in view / heard (SNR > 0) / best SNR over the last 5 s, all constellations.
void gpsSkySummary(int& inView, int& heard, int& best) {
    inView = heard = best = 0;
    for (auto& k : g_gpsSky) {
        if (!k.talker[0] || millis() - k.atMs > 5000) continue;
        inView += k.inView; heard += k.heard;
        if (k.bestSnr > best) best = k.bestSnr;
    }
}

void gpsApplyPower() {
#if defined(HAS_GPS) && HAS_GPS
    static HardwareSerial gpsPort(1);
    static bool portOpen = false;
    pinMode(GPS_EN, OUTPUT);
    if (!g_cfg.gpsOn || !g_gpsAllowed) {
        digitalWrite(GPS_EN, LOW);
        // With the receiver unpowered, an idle-high UART TX line back-powers it through its
        // RX pin: measured +18 mA on the sleeping floor (26 -> 8 mA). Release the UART and
        // hold TX low (RX as plain input) whenever the GPS rail is off.
        if (portOpen) { gpsPort.end(); portOpen = false; }
        pinMode(GPS_TX, OUTPUT);
        digitalWrite(GPS_TX, LOW);
        pinMode(GPS_RX, INPUT);
        g_gpsSerial = nullptr;
        g_gpsFix = false; g_gpsSats = 0; g_gpsUtcValid = false;
        snprintf(g_gpsStatus, sizeof(g_gpsStatus), g_cfg.gpsOn ? "resting" : "off");
        if (g_cfg.gpsOn) Serial.println("[GPS] off (power mode schedule)");
        return;
    }
    digitalWrite(GPS_EN, HIGH);
    delay(30);
    // 38400 baud = ~3.8 KB/s; a full e-ink refresh blocks ~1.6 s, so give the UART room.
    if (!portOpen) gpsPort.setRxBufferSize(4096);
    gpsPort.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
    portOpen = true;
    g_gpsSerial = &gpsPort;
    g_gpsBaud = GPS_BAUD;
    g_gpsStep = 0;
    g_gpsLastGoodMs = g_gpsStepAt = millis();
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "searching");
    Serial.printf("[GPS] on RX=%d TX=%d baud=%d\n", GPS_RX, GPS_TX, GPS_BAUD);
#else
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "n/a");
#endif
}

void ubxSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)len, (uint8_t)(len >> 8)};
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += hdr[i]; b += a; }
    for (uint16_t i = 0; i < len; i++) { a += payload[i]; b += a; }
    g_gpsSerial->write(hdr, 6);
    if (len) g_gpsSerial->write(payload, len);
    const uint8_t ck[2] = {a, b};
    g_gpsSerial->write(ck, 2);
}

// Log the receiver's binary replies: ACK/NAK of our commands and the MON-VER strings.
void gpsHandleUbx(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t len) {
    if (cls == 0x05 && len >= 2) {
        Serial.printf("[GPS] UBX %s for %02X-%02X\n", id == 0x01 ? "ACK" : "NAK", p[0], p[1]);
    } else if (cls == 0x0A && id == 0x04 && len >= 40) {
        char sw[31] = {0}, hw[11] = {0};
        memcpy(sw, p, 30); memcpy(hw, p + 30, 10);
        Serial.printf("[GPS] receiver sw=%s hw=%s\n", sw, hw);
        for (uint16_t o = 40; o + 30 <= len; o += 30) {
            char ext[31] = {0};
            memcpy(ext, p + o, 30);
            Serial.printf("[GPS]   %s\n", ext);
        }
    } else {
        Serial.printf("[GPS] UBX %02X-%02X len %u\n", cls, id, len);
    }
}

void gpsSendDefaults() {
    static const uint8_t wake[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t clearFlash[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x1C, 0xA2};
    static const uint8_t clearBbr[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B, 0xA1};
    static const uint8_t loadDefaults[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0xFF, 0xFF, 0x00, 0x00, 0x03, 0x1D, 0xB3};
    g_gpsSerial->write(wake, sizeof(wake));          // UART activity wakes a sleeping receiver
    delay(20);
    g_gpsSerial->write(clearFlash, sizeof(clearFlash));
    g_gpsSerial->write(clearBbr, sizeof(clearBbr));
    g_gpsSerial->write(loadDefaults, sizeof(loadDefaults));
    delay(50);

    // u-blox M10 configuration (CFG-VALSET, RAM + BBR): NMEA out on UART1 with GGA, RMC
    // and GSV every fix, and full-power operation (no power-save mode).
    struct KV { uint32_t key; uint8_t val; };
    static const KV kv[] = {
        {0x10740002, 1},   // CFG-UART1OUTPROT-NMEA
        {0x209100bb, 1},   // CFG-MSGOUT-NMEA_ID_GGA_UART1
        {0x209100ac, 1},   // CFG-MSGOUT-NMEA_ID_RMC_UART1
        {0x209100c5, 1},   // CFG-MSGOUT-NMEA_ID_GSV_UART1
        {0x20d00001, 0},   // CFG-PM-OPERATEMODE = FULL
    };
    uint8_t pl[4 + sizeof(kv) / sizeof(kv[0]) * 5];
    size_t n = 0;
    pl[n++] = 0; pl[n++] = 0x01; pl[n++] = 0; pl[n++] = 0;   // version 0, layer RAM
    for (const auto& e : kv) {
        for (int i = 0; i < 4; i++) pl[n++] = (uint8_t)(e.key >> (8 * i));
        pl[n++] = e.val;
    }
    ubxSend(0x06, 0x8A, pl, (uint16_t)n);             // UBX-CFG-VALSET
    delay(50);
    ubxSend(0x0A, 0x04, nullptr, 0);                  // UBX-MON-VER poll: which receiver is this?
    delay(150);
    // UBX-CFG-RST: cold start, controlled software reset (receiver restarts and navigates).
    static const uint8_t rst[] = {0xFF, 0xFF, 0x01, 0x00};
    ubxSend(0x06, 0x04, rst, sizeof(rst));
}

// Switch the receiver's supply (GPS_EN -> GPS_VDD = VCC + V_IO) off for 2 s and back on.
void gpsPowerCycle(const char* why) {
    if (!g_gpsSerial || g_gpsPowerOffAt) return;
    Serial.printf("[GPS] power-cycling receiver (%s)\n", why);
    digitalWrite(GPS_EN, LOW);
    g_gpsPowerOffAt = millis() ? millis() : 1;
}

void gpsHardReset() {
    if (!g_gpsSerial) return;
    static const uint8_t rst[] = {0xFF, 0xFF, 0x00, 0x00};   // cold start, hardware (watchdog) reset
    ubxSend(0x06, 0x04, rst, sizeof(rst));
    Serial.println("[GPS] hardware reset sent");
}

void gpsWatchLink() {
    if (!g_gpsSerial) return;
    const uint32_t now = millis();
    if (g_gpsPowerOffAt) {                             // finishing a power cycle
        if (now - g_gpsPowerOffAt >= 2000) {
            digitalWrite(GPS_EN, HIGH);
            g_gpsPowerOffAt = 0;
            g_gpsBbrFault = false;                     // re-detected from the boot banner if still bad
            g_gpsLastGoodMs = g_gpsStepAt = now;
        }
        return;
    }
    if (g_gpsBbrFault) {                               // only a power cycle can help; not too often
        if ((int32_t)(now - g_gpsNextCycleAt) >= 0) {
            g_gpsNextCycleAt = now + (g_gpsBbrFaultCount < 10 ? 60000 : 300000);   // then every 5 min
            gpsPowerCycle("backup supply fault");
        }
        return;
    }
    if (now - g_gpsLastGoodMs < 3000) return;         // navigation sentences are arriving
    const uint32_t wait = g_gpsStep < 6 ? 2500 : 20000;
    if (now - g_gpsStepAt < wait) return;
    g_gpsStepAt = now;
    static const uint32_t bauds[] = {38400, 9600, 115200};
    g_gpsBaud = bauds[g_gpsStep % 3];
    g_gpsSerial->updateBaudRate(g_gpsBaud);
    gpsSendDefaults();
    Serial.printf("[GPS] no NMEA (%lu bytes so far) - reset receiver config, listening at %lu baud\n",
                  (unsigned long)g_gpsBytes, (unsigned long)g_gpsBaud);
    if (g_gpsStep < 255) g_gpsStep++;
}

bool gpsSyncClock() {
    if (!g_gpsFix || !g_gpsUtcValid) return false;
    if (!setClockUtc(g_gpsUtc[0], g_gpsUtc[1], g_gpsUtc[2], g_gpsUtc[3], g_gpsUtc[4], g_gpsUtc[5]))
        return false;
    g_gpsSyncedOnce = true;
    g_gpsLastSyncMs = millis() ? millis() : 1;
    // Real time now: the protocol may use it (announces carry it; check-in peers adopt it).
    handheld::ClockConfidence::markSynchronized(time(nullptr));
    Serial.println("[RTC] synced from GPS");
    return true;
}

int twoDigits(const char* s) { return (s[0] - '0') * 10 + (s[1] - '0'); }

bool nmeaChecksumOk(char* line) {
    char* star = strchr(line, '*');
    if (!star || !isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2])) return false;
    uint8_t x = 0;
    for (char* p = line + 1; p < star; p++) x ^= (uint8_t)*p;
    const bool ok = x == (uint8_t)strtoul(star + 1, nullptr, 16);
    *star = 0;                                   // fields end before the checksum
    return ok;
}

void gpsHandleSentence(char* line) {
    if (!nmeaChecksumOk(line)) { g_gpsBad++; return; }
    g_gpsGood++;
    if (!strncmp(line + 3, "TXT", 3)) {          // receiver notices: antenna status, firmware...
        Serial.printf("[GPS] %s\n", line);
        if (strstr(line, "CANNOT USE BBR")) {
            if (!g_gpsBbrFault) {
                g_gpsBbrFault = true;
                g_gpsBbrFaultCount++;
                g_gpsNextCycleAt = millis() + 60000;
                snprintf(g_gpsStatus, sizeof(g_gpsStatus), "backup low");
                Serial.println("[GPS] receiver refuses to navigate: backup supply (V_BCKP) not usable."
                               " Retrying while the device is on (backup cell charges).");
            }
        }
        return;
    }
    if ((g_gpsStep || g_gpsBbrFault) && millis() - g_gpsLastGoodMs >= 3000) Serial.printf("[GPS] receiving NMEA at %lu baud\n", (unsigned long)g_gpsBaud);
    g_gpsLastGoodMs = millis();
    g_gpsStep = 0;
    g_gpsBbrFault = false;
    if (!strncmp(line + 3, "GGA", 3)) snprintf(g_gpsLastGga, sizeof(g_gpsLastGga), "%s", line);
    if (!strncmp(line + 3, "RMC", 3)) snprintf(g_gpsLastRmc, sizeof(g_gpsLastRmc), "%s", line);
    char* fld[24] = {};
    int n = 0;
    fld[n++] = line;
    for (char* p = line; *p && n < 24; p++)
        if (*p == ',') { *p = 0; fld[n++] = p + 1; }

    if (!strncmp(fld[0] + 3, "GSV", 3) && n >= 4) {
        // $xxGSV,numMsgs,msgNum,inView,(id,elev,az,snr)x4[,signalId]
        GpsSky* k = gpsSkyFor(fld[0] + 1);
        if (!k) return;
        const int msgNum = atoi(fld[2]), numMsgs = atoi(fld[1]);
        if (msgNum == 1) { k->heardAcc = 0; k->bestAcc = 0; }
        for (int i = 7; i < n; i += 4) {
            const int snr = fld[i][0] ? atoi(fld[i]) : 0;
            if (snr > 0) { k->heardAcc++; if (snr > k->bestAcc) k->bestAcc = (uint8_t)snr; }
        }
        if (msgNum >= numMsgs) {
            k->inView = (uint8_t)atoi(fld[3]);
            k->heard = k->heardAcc;
            k->bestSnr = k->bestAcc;
            k->atMs = millis();
        }
        return;
    }

    const bool rmc = !strncmp(fld[0], "$GPRMC", 6) || !strncmp(fld[0], "$GNRMC", 6);
    const bool gga = !strncmp(fld[0], "$GPGGA", 6) || !strncmp(fld[0], "$GNGGA", 6);
    if (rmc && n > 9) {
        const bool valid = fld[2][0] == 'A';
        if (valid && strlen(fld[1]) >= 6 && strlen(fld[9]) >= 6) {
            g_gpsUtc[3] = twoDigits(fld[1]);
            g_gpsUtc[4] = twoDigits(fld[1] + 2);
            g_gpsUtc[5] = twoDigits(fld[1] + 4);
            g_gpsUtc[2] = twoDigits(fld[9]);
            g_gpsUtc[1] = twoDigits(fld[9] + 2);
            g_gpsUtc[0] = 2000 + twoDigits(fld[9] + 4);
            g_gpsUtcValid = true;
        }
        if (valid && n > 6) gpsNotePosition(fld[3], fld[4], fld[5], fld[6]);
        if (valid && n > 8) {                    // speed + course over ground (no compass on board)
            g_gpsSpeedKn = fld[7][0] ? (float)atof(fld[7]) : 0.0f;
            g_gpsCourseOk = fld[8][0] != 0;
            if (g_gpsCourseOk) g_gpsCourse = (float)atof(fld[8]);
            g_gpsCourseAt = millis();
        }
        g_gpsFix = valid;
    } else if (gga && n > 7) {
        if (fld[7][0]) g_gpsSats = (uint8_t)atoi(fld[7]);
        if (n > 8 && fld[8][0]) g_gpsHdop = (float)atof(fld[8]);
        if (fld[6][0]) g_gpsFix = fld[6][0] > '0';
        if (fld[6][0] > '0') gpsNotePosition(fld[2], fld[3], fld[4], fld[5]);
    } else {
        return;
    }
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "%s", g_gpsFix ? "fix" : g_gpsBbrFault ? "backup low" : "searching");

    // Auto sync: once after boot, then daily.
    if (g_cfg.gpsAutoSync && g_gpsFix && g_gpsUtcValid) {
        const uint32_t day = 24UL * 3600UL * 1000UL;
        if (!g_gpsSyncedOnce || millis() - g_gpsLastSyncMs >= day) gpsSyncClock();
    }
}

void gpsDump() {
    int inView, heard, best;
    gpsSkySummary(inView, heard, best);
    Serial.printf("[GPS] %s  power=%s  baud=%lu  fix=%d sats=%u  bytes=%lu ok=%lu badsum=%lu\n", g_gpsStatus,
                  g_cfg.gpsOn ? "on" : "off", (unsigned long)g_gpsBaud, g_gpsFix, g_gpsSats,
                  (unsigned long)g_gpsBytes, (unsigned long)g_gpsGood, (unsigned long)g_gpsBad);
    Serial.printf("[GPS] sky: %d in view, %d heard, best %d dB-Hz\n", inView, heard, best);
    Serial.printf("[GPS] backup supply fault: %s (seen %lu times)\n", g_gpsBbrFault ? "YES" : "no",
                  (unsigned long)g_gpsBbrFaultCount);
    for (auto& k : g_gpsSky)
        if (k.talker[0])
            Serial.printf("[GPS]   %s: %u in view, %u heard, best %u (%lus ago)\n", k.talker, k.inView, k.heard,
                          k.bestSnr, (unsigned long)((millis() - k.atMs) / 1000));
    Serial.printf("[GPS] GGA %s\n[GPS] RMC %s\n", g_gpsLastGga, g_gpsLastRmc);
}

void gpsPoll() {
    if (!g_gpsSerial) return;
    static char line[100];
    static size_t len = 0;
    // UBX frames (B5 62 cls id len16 payload ck_a ck_b) are taken out before NMEA parsing.
    static uint8_t ust = 0, ucls = 0, uid = 0, ubuf[260];
    static uint16_t ulen = 0, upos = 0;
    while (g_gpsSerial->available()) {
        const int raw = g_gpsSerial->read();
        if (raw < 0) break;
        const uint8_t u = (uint8_t)raw;
        g_gpsBytes++;
        if (ust == 0 && u == 0xB5) { ust = 1; continue; }
        if (ust == 1) {
            if (u == 0x62) { ust = 2; continue; }
            ust = 0;
        }
        if (ust >= 2) {
            switch (ust) {
                case 2: ucls = u; ust = 3; break;
                case 3: uid = u; ust = 4; break;
                case 4: ulen = u; ust = 5; break;
                case 5: ulen |= (uint16_t)(u << 8); upos = 0; ust = 6; break;
                default:
                    if (upos < sizeof(ubuf)) ubuf[upos] = u;
                    if (++upos >= ulen + 2) {             // payload + 2 checksum bytes
                        if (ulen + 2 <= (int)sizeof(ubuf)) gpsHandleUbx(ucls, uid, ubuf, ulen);
                        ust = 0;
                    }
                    break;
            }
            continue;
        }
        const char c = (char)u;
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = 0;
            if (len > 12 && line[0] == '$') gpsHandleSentence(line);
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = c;
        } else {
            len = 0;
        }
    }
}

// ---- Radio ---------------------------------------------------------------------
// Transmit power (dBm, -9..22), saved in NVS rs_radio/txp. Bench units on one table can run
// at minimum power, which is also safe for a radio whose antenna is not fitted.
int loraTxPower() {
#if defined(RS_BENCH_TX_DBM)
    const int def = RS_BENCH_TX_DBM;           // bench build: low power unless set otherwise
#else
    const int def = LORA_DEFAULT_TX_POWER;
#endif
    Preferences p;
    int v = def;
    if (p.begin("rs_radio", true)) { v = p.getChar("txp", (int8_t)def); p.end(); }
    return v < -9 ? -9 : (v > 22 ? 22 : v);
}

bool initRadio() {
    spiBusIdle();
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    delay(10);
    g_radio = new SX1262(&SPI, LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
                         LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
                         LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);
    if (!g_radio || !g_radio->begin(LORA_DEFAULT_FREQ)) {
        delete g_radio;
        g_radio = nullptr;
        return false;
    }
    g_radio->setSpreadingFactor(LORA_DEFAULT_SF);
    g_radio->setSignalBandwidth(LORA_DEFAULT_BW);
    g_radio->setCodingRate4(LORA_DEFAULT_CR);
    g_radio->setTxPower(loraTxPower());
    g_radio->setPreambleLength(LORA_DEFAULT_PREAMBLE);
    g_radio->receive();
    g_loraIf = new LoRaInterface(g_radio, "LoRa");
    g_loraIf->setRxTap(rtRxTap);
    if (!g_loraIf || !g_loraIf->start()) return false;
    Serial.printf("[LORA] online %lu Hz SF%d\n", (unsigned long)LORA_DEFAULT_FREQ, LORA_DEFAULT_SF);
    return true;
}

bool loraOnline() {
    if (g_loraIf && g_loraIf->isOnline()) return true;
    return g_radio && g_radio->isRadioOnline();
}

int loraRssi() {
    int r = 0;
    if (g_loraIf) r = g_loraIf->lastRxRssi();
    if (r == 0 && g_radio) r = g_radio->currentRssi();
    return r;
}

float loraSnr() { return g_loraIf ? g_loraIf->lastRxSnr() : 0.0f; }

void applyLoraPreset(int idx) {
    g_loraPreset = ((idx % kLoraPresetCount) + kLoraPresetCount) % kLoraPresetCount;
    const LoraPreset& p = kLoraPresets[g_loraPreset];
    if (!g_radio) return;
    g_radio->setSpreadingFactor(p.sf);
    g_radio->setSignalBandwidth(p.bw);
    g_radio->setCodingRate4(p.cr);
    Serial.printf("[LORA] preset %s SF%u BW%.0f CR%u\n", p.name, p.sf, p.bw, p.cr);
}

// ---- Storage + protocol ---------------------------------------------------------
void logPsram() {
    Serial.printf("[PSRAM] found=%d size=%u free=%u int_free=%u\n", (int)psramFound(),
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// FlashStore::format refuses when not mounted, so erase the partition directly.
bool recoverCorruptLittleFS() {
    Serial.println("[FLASH] erase + format data partition");
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "littlefs");
    if (!part) part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!part) part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) { Serial.println("[FLASH] no data partition"); return false; }
    LittleFS.end();
    if (esp_partition_erase_range(part, 0, part->size) != ESP_OK) return false;
    if (!LittleFS.begin(true, "/littlefs", 10, part->label)) return false;
    LittleFS.end();
    return g_flash.begin();
}

bool tryProto(uint32_t caps, const char* label) {
    Serial.printf("[PROTO] try %s int_free=%u\n", label,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return g_proto.begin(&g_flash, nullptr, &g_idMgr, &g_msgStore, &g_announceMgr,
                         RS_HANDHELD_PROFILE_SMALL, caps);
}

bool fillDestFromNvs() {
    Preferences p;
    uint8_t priv[64] = {0};
    if (!p.begin(NVS_NS_IDENTITY, true)) return false;
    size_t n = p.getBytes("privkey", priv, sizeof(priv));
    p.end();
    if (n != sizeof(priv)) return false;
    rs_handheld_rns_t* ctx = nullptr;
    if (rs_handheld_rns_init(&ctx) != RS_HANDHELD_OK || !ctx) return false;
    uint8_t dest[16];
    bool ok = rs_handheld_rns_load_identity(ctx, priv) == RS_HANDHELD_OK &&
              rs_handheld_rns_destination_hash(ctx, dest) == RS_HANDHELD_OK;
    rs_handheld_rns_shutdown(ctx);
    memset(priv, 0, sizeof(priv));
    if (ok) setSelfHash(dest);
    return ok;
}

void initStorageAndProto() {
    if (!g_flash.begin()) {
        Serial.println("[FLASH] mount failed - recovering");
        if (!recoverCorruptLittleFS()) { Serial.println("[FLASH] recovery failed"); return; }
    }
    g_idMgr.begin(&g_flash, nullptr);
    g_hadIdentity = g_idMgr.count() > 0;     // before the protocol makes one for a new device
    const bool archive = sdArchiveEnabled() && sdMount();
    if (!g_msgStore.begin(&g_flash, archive ? &g_sdStore : nullptr, archive)) {
        Serial.println("[MSG] store failed"); return;
    }
    g_sdArchiveActive = archive;
    Serial.printf("[SD] message archive %s\n", archive ? "on (new messages also go to the card)"
                  : sdArchiveEnabled() ? "off (no card)" : "off (setting)");
    g_storeReady = true;
    g_announceMgr.setStorage(nullptr, &g_flash);
    g_announceMgr.loadNameCache();
    g_announceMgr.loadContacts();   // saved contacts survive reboot

    logPsram();
    if (psramFound() && ESP.getPsramSize() > 0 && ESP.getFreePsram() > 250000) {
        g_protoReady = tryProto(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "SPIRAM") ||
                       tryProto(MALLOC_CAP_DEFAULT, "DEFAULT");
    }
    if (!g_protoReady) {
        g_protoReady = tryProto(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "INTERNAL|8BIT") ||
                       tryProto(MALLOC_CAP_INTERNAL, "INTERNAL") ||
                       tryProto(MALLOC_CAP_8BIT, "8BIT");
    }
    if (!g_protoReady) {
        Serial.println("[PROTO] off - store only");
        fillDestFromNvs();
        return;
    }
    if (g_loraIf) {
        g_proto.pump().attachLoRa(g_loraIf);
        g_announceMgr.setLoRaInterface(g_loraIf);
    }
    if (const uint8_t* d = g_proto.localDestHash()) {
        setSelfHash(d);
        g_announceMgr.setLocalDestHash(rs::Bytes(d, 16));
    }
    Serial.printf("[PROTO] up dest=%s\n", g_destHex);
}

void pumpOnce() {
    if (g_protoReady) g_proto.loop();
    if (g_storeReady) g_msgStore.poll();
}

void pumpFor(int iterations, int delayMs) {
    for (int i = 0; i < iterations; i++) { pumpOnce(); delay(delayMs); }
}

// ---- Announce / alerts --------------------------------------------------------
char g_announceStr[16] = "never";

void doAnnounce() {
    if (!g_protoReady) { snprintf(g_announceStr, sizeof(g_announceStr), "proto off"); return; }
    static uint32_t last = 0;
    if (last && millis() - last < 5000UL) {
        snprintf(g_announceStr, sizeof(g_announceStr), "wait 5s");
        return;
    }
    last = millis();
    switch (g_proto.announce(nullptr, 0)) {
        case ProtocolBackend::AnnounceResult::Sent:     snprintf(g_announceStr, sizeof(g_announceStr), "sent"); break;
        case ProtocolBackend::AnnounceResult::Deferred: snprintf(g_announceStr, sizeof(g_announceStr), "deferred"); break;
        default:                                        snprintf(g_announceStr, sizeof(g_announceStr), "failed"); break;
    }
    Serial.printf("[ANN] %s dest=%s\n", g_announceStr, g_destHex);
}

// Key backlight = user setting AND allowed by the power mode.
void applyKeyLight() {
    if (!g_kbOk) return;
    if (g_cfg.kbLight && g_pwEff.keyLight) g_kb.backlightOn(); else g_kb.backlightOff();
}

void vibrate(uint16_t ms) {
#if defined(BOARD_MOTOR_PIN) && (BOARD_MOTOR_PIN >= 0)
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    digitalWrite(BOARD_MOTOR_PIN, HIGH);
    delay(ms);
    digitalWrite(BOARD_MOTOR_PIN, LOW);
#else
    (void)ms;
#endif
}

void notifyInbound() {
    evtrace::mark("msg-in");
    Serial.printf("[MSG] inbound message at uptime %lu ms\n", (unsigned long)millis());
    if (g_cfg.beep) g_audio.requestMessage();
    if (g_cfg.vibe) vibrate(40);
}

// =============================================================================
// 3. Peer rows, history, send
// =============================================================================

constexpr int MAX_ROWS = 48;      // list rows; shown in pages (Contacts 0..9, Inbox/Peers 1..9)
struct PeerRow {
    uint8_t  peer[16];
    char     name[24];
    char     sub[32];
    uint16_t unread;
    bool     self;
    bool     saved;
};
PeerRow g_rows[MAX_ROWS];
int g_rowCount = 0;

// Names typed on this device this session (persisted via AnnounceManager too).
struct LocalName { uint8_t peer[16]; char name[24]; bool used; };
LocalName g_localNames[12] = {};

void rememberName(const uint8_t peer[16], const char* name) {
    int slot = -1;
    for (int i = 0; i < 12; i++) {
        if (g_localNames[i].used && !memcmp(g_localNames[i].peer, peer, 16)) { slot = i; break; }
        if (!g_localNames[i].used && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;
    g_localNames[slot].used = true;
    memcpy(g_localNames[slot].peer, peer, 16);
    snprintf(g_localNames[slot].name, sizeof(g_localNames[slot].name), "%.23s", name);
}

bool usableName(const std::string& n) {
    if (n.empty()) return false;
    // Skip 1-2 digit announce placeholders ("1", "12").
    if (n.size() <= 2 && n.find_first_not_of("0123456789") == std::string::npos) return false;
    return true;
}

void peerName(const uint8_t peer[16], char* out, size_t n) {
    if (isSelf(peer)) { snprintf(out, n, "%s", g_selfName); return; }
    for (const auto& ln : g_localNames)
        if (ln.used && !memcmp(ln.peer, peer, 16)) { snprintf(out, n, "%s", ln.name); return; }
    char hex[33];
    hashToHex(peer, hex);
    std::string nm = g_announceMgr.lookupName(hex);
    if (usableName(nm)) { snprintf(out, n, "%.23s", nm.c_str()); return; }
    snprintf(out, n, "%.12s", hex);
}

int findRow(const uint8_t peer[16]) {
    for (int i = 0; i < g_rowCount; i++)
        if (!memcmp(g_rows[i].peer, peer, 16)) return i;
    return -1;
}

// List search (key F on Inbox / Contacts / Peers): rows whose name or hash contain the
// text (any case). Cleared when the list screen changes or with ^.
char g_filter[24] = "";

bool containsNoCase(const char* hay, const char* needle) {
    const size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return n == 0;
}

PeerRow* addRow(const uint8_t peer[16]) {
    if (g_rowCount >= MAX_ROWS || findRow(peer) >= 0) return nullptr;
    PeerRow& r = g_rows[g_rowCount];
    memset(&r, 0, sizeof(r));
    memcpy(r.peer, peer, 16);
    r.self = isSelf(peer);
    peerName(peer, r.name, sizeof(r.name));
    char hex[33];
    hashToHex(peer, hex);
    if (g_filter[0] && !containsNoCase(r.name, g_filter) && !containsNoCase(hex, g_filter)) return nullptr;
    g_rowCount++;
    const DiscoveredNode* node = g_announceMgr.findNode(rs::Bytes(peer, 16));
    r.saved = node && node->saved;
    snprintf(r.sub, sizeof(r.sub), "%.24s", hex);
    return &r;
}

bool waitResult(MessageStore::Ticket t, MessageStore::Result& res, int maxSpin = 600) {
    for (int i = 0; i < maxSpin; i++) {
        g_msgStore.poll();
        if (g_msgStore.peekResult(t, res)) return true;
        delay(5);
    }
    return false;
}

// Conversations (most recent first) with unread counts. The store hands out pages of
// up to ConversationPageCapacity (16 on the device); this walks up to `limit` with the
// cursor. The result is cached until the store's revision changes, so typing a search
// or coming back to the Inbox does not query the store again.
struct ConvRow { uint8_t peer[16]; uint16_t unread; };
ConvRow  g_convCache[MAX_ROWS];
int      g_convCount = 0;
uint32_t g_convRev = 0;
bool     g_convValid = false;
// Page size: the device store's (16 with async writes); the same in the simulator so
// it walks the same multi-page path.
constexpr int kConvPage = MessageStore::ConversationPageCapacity < 16 ? (int)MessageStore::ConversationPageCapacity : 16;

int fetchConversationPage(handheld::storage::ConversationSelector* sel, int cap,
                          const handheld::storage::ConversationCursor* after) {
    using namespace handheld::storage;
    const uint8_t lim = (uint8_t)std::min<int>(cap, kConvPage);
    const ConversationCursor cur = after ? *after : ConversationCursor{};
    auto sub = g_msgStore.requestConversationPage(cur, after != nullptr, ConversationOrder::Recent,
                                                  ConversationDirection::After, lim);
    for (int a = 0; a < 8 && !sub.accepted(); a++) {
        unsigned r = (unsigned)sub.rejection;
        if (r != 1 && r != 8) break;            // only Busy / Recovering are worth retrying
        pumpFor(50, 12);
        sub = g_msgStore.requestConversationPage(cur, after != nullptr, ConversationOrder::Recent,
                                                 ConversationDirection::After, lim);
    }
    if (!sub.accepted()) { Serial.printf("[MSG] page rejected %u\n", (unsigned)sub.rejection); return -1; }
    MessageStore::Result res;
    size_t nSel = 0;
    if (waitResult(sub.ticket, res) && res.outcome == Outcome::Committed &&
        res.error == Error::None && res.length >= sizeof(ConversationSelector)) {
        nSel = res.length / sizeof(ConversationSelector);
        if (nSel > (size_t)lim) nSel = lim;
        if (!g_msgStore.readPayload(sub.ticket, sel, nSel * sizeof(ConversationSelector))) nSel = 0;
    }
    g_msgStore.releaseResult(sub.ticket);
    return (int)nSel;
}

void loadConversations(int limit) {
    using namespace handheld::storage;
    const uint32_t t0 = millis();
    g_convCount = 0;
    g_convValid = false;
    if (!g_storeReady) return;
    const uint32_t rev = g_msgStore.revision();
    static ConversationSelector sel[kConvPage];
    ConversationCursor last{};
    bool haveLast = false;
    int pages = 0;
    while (g_convCount < limit && pages < 8) {
        const int n = fetchConversationPage(sel, limit - g_convCount, haveLast ? &last : nullptr);
        if (n < 0) return;                      // keep "not valid": retried next time
        pages++;
        for (int i = 0; i < n && g_convCount < limit; i++) {
            if (isSelf(sel[i].cursor.peer)) continue;
            bool dup = false;
            for (int k = 0; k < g_convCount && !dup; k++) dup = !memcmp(g_convCache[k].peer, sel[i].cursor.peer, 16);
            if (dup) continue;
            uint16_t unread = 0;
            if (sel[i].counter && sel[i].error == Error::None) {
                auto d = g_msgStore.requestConversation(sel[i]);
                if (d.accepted()) {
                    MessageStore::Result dres;
                    ConversationView view;
                    if (waitResult(d.ticket, dres) && dres.outcome == Outcome::Committed &&
                        dres.error == Error::None && dres.length >= sizeof(ConversationView) &&
                        g_msgStore.readPayload(d.ticket, &view, sizeof(view)))
                        unread = (uint16_t)view.unreadCount;
                    g_msgStore.releaseResult(d.ticket);
                }
            }
            memcpy(g_convCache[g_convCount].peer, sel[i].cursor.peer, 16);
            g_convCache[g_convCount].unread = unread;
            g_convCount++;
        }
        if (n < kConvPage || n == 0) break;   // last page
        last = sel[n - 1].cursor;
        haveLast = true;
    }
    g_convRev = rev;
    g_convValid = true;
    Serial.printf("[LIST] %d conversations in %lu ms (%d pages)\n", g_convCount,
                  (unsigned long)(millis() - t0), pages);
}

void appendConversations(int limit) {
    if (!g_storeReady || limit <= 0) return;
    if (!g_convValid || g_convRev != g_msgStore.revision()) loadConversations(MAX_ROWS);
    for (int i = 0; i < g_convCount && i < limit; i++)
        if (PeerRow* r = addRow(g_convCache[i].peer)) r->unread = g_convCache[i].unread;
}

void appendAnnounced(bool savedOnly, bool withSignal) {
    for (const auto& node : g_announceMgr.nodes()) {
        if (node.hash.size() != 16) continue;
        if (savedOnly && !node.saved) continue;
        if (isSelf(node.hash.data())) continue;
        PeerRow* r = addRow(node.hash.data());
        if (!r || !withSignal) continue;
        char hex[33];
        hashToHex(r->peer, hex);
        unsigned ageM = (node.lastSeen && millis() >= node.lastSeen)
                            ? (unsigned)((millis() - node.lastSeen) / 60000UL) : 0;
        if (node.rssi)
            snprintf(r->sub, sizeof(r->sub), "%.10s %ddBm %um", hex, node.rssi, ageM);
        else
            snprintf(r->sub, sizeof(r->sub), "%.12s %um", hex, ageM);
    }
}

// ---- Contact cards (share a contact as an LXMF message) -----------------------------
// Plain text, readable on any LXMF client:   "Contact card: <name>\n<32 hex dest hash>"
// This firmware recognises it in a thread ("[card] <name>") and offers to save it as a
// contact (Enter on an empty draft). Sent from a contact's page: Share -> pick who gets it.
constexpr const char* kCardPrefix = "Contact card: ";
struct InCard { bool valid; uint8_t hash[16]; char name[24]; uint32_t counter; };
InCard  g_threadCard = {};         // newest incoming card in the open thread
bool    g_cardOpen = false;        // contact page (Message / Rename / Share) over Contacts / Peers
uint8_t g_cardPeer[16];
int     g_cardSel = 0;
bool    g_shareActive = false;     // the Contacts list is picking who gets g_sharePeer
bool    g_shareAll = false;        // ... and gets every contact's card (Settings S.6)
bool    g_pickRt = false;          // ... or is the range-test partner (Settings S.7)
bool    g_pickLowbat = false;      // ... or gets the low-battery notice (Settings S.5)
uint8_t g_sharePeer[16];

int hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexToHash(const char* s, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) {
        const int h = hexNib(s[2 * i]), l = hexNib(s[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

bool cardParse(const char* s, size_t len, uint8_t hash[16], char* name, size_t nameCap) {
    const size_t pl = strlen(kCardPrefix);
    if (len < pl + 1 + 1 + 32 || strncmp(s, kCardPrefix, pl) != 0) return false;
    size_t i = pl, n = 0;
    while (i < len && s[i] != '\n' && s[i] != '\r') {
        if (n + 1 < nameCap && (uint8_t)s[i] >= 32 && (uint8_t)s[i] < 127) name[n++] = s[i];
        i++;
    }
    while (n && name[n - 1] == ' ') n--;
    name[n] = 0;
    while (i < len && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) i++;
    if (i + 32 > len) return false;
    for (int k = 0; k < 16; k++) {
        const int h = hexNib(s[i + 2 * k]), l = hexNib(s[i + 2 * k + 1]);
        if (h < 0 || l < 0) return false;
        hash[k] = (uint8_t)((h << 4) | l);
    }
    if (i + 32 < len && hexNib(s[i + 32]) >= 0) return false;   // longer hex: not a dest hash
    return n > 0;
}

int cardFormat(const uint8_t hash[16], const char* name, char* out, size_t n) {
    char hex[33];
    hashToHex(hash, hex);
    return snprintf(out, n, "%s%.23s\n%s", kCardPrefix, name, hex);
}

bool cardIsSaved(const uint8_t hash[16]) {
    if (isSelf(hash)) return true;
    const DiscoveredNode* node = g_announceMgr.findNode(rs::Bytes(hash, 16));
    return node && node->saved;
}

bool cardOffered() { return g_threadCard.valid && !cardIsSaved(g_threadCard.hash); }

extern char g_placeLast[28];                 // place search (map F): last place jumped to
// Waypoints + track data (functions in "Waypoints + breadcrumb track" below).
struct Wpt { float lat, lon; char name[16]; uint32_t at; };
constexpr int kWptMax = 32;
Wpt g_wpt[kWptMax];
int g_wptCount = 0;
int g_wptSel = -1;                           // selected on the map (N), -1 = none
uint32_t g_wptDelAt = 0;                     // Del pressed once (confirm within 3 s)
constexpr int kTrackRam = 1000;
float g_track[kTrackRam * 2];
int g_trackN = 0;
bool g_trackLoaded = false;
bool ownPosition(double& lat, double& lon);
void distText(double la, double lo, double tla, double tlo, char* out, size_t n);
void trackLoad();
void trackClear();
bool g_tbOn = false;                 // track back: guide to earlier track points, newest first
int  g_tbIdx = 0;                    // the track point being walked to
bool tbStart();
void tbStop(const char* why);
int  trackExportGpx();
bool g_navOpen = false;              // bearing + distance screen (G on the Atlas)
int  batHoursLeft10(int* dataMin);
void batLeftText(char* out, size_t n);
bool g_rtOn = false, g_rtOpen = false; // range test running / its screen shown
void rtStart(const uint8_t peer[16], int everySecs);
void rtStop();
// Arrival alerts (functions under "Arrival alerts" below)
constexpr int kGeofMax = 8;
struct Geof { uint8_t dest[16]; float lat, lon; uint16_t radius; uint8_t place, repeat, armed, pad; char wname[16]; };
Geof g_geof[kGeofMax];
int g_geofCount = 0;
const uint16_t kGeofSteps[] = {100, 250, 500, 1000, 2000, 5000};
constexpr int kGeofStepN = sizeof(kGeofSteps) / sizeof(kGeofSteps[0]);
uint8_t g_gnContacts[MAX_ROWS][16];        // the New alert page: saved contacts to pick from
int g_gnContactN = 0, g_gnContact = 0, g_gnPlace = 0, g_gnDist = 2;
bool g_gnRepeat = false;
void geofText(const Geof& g, char* out, size_t n);
bool geofAdd(const uint8_t dest[16], int place, uint16_t radius, bool repeat);
void geofRemove(int i);
void geofNewStart(const uint8_t* dest);
void geofLoad();
void navOpen();
void navClose();
void fmtDist(double m, char* out, size_t n);
float metersBetween(double la1, double lo1, double la2, double lo2);
bool ownPosition(double& lat, double& lon);
extern char g_trackGpx[40];
void wptSelect(int i);
void openMap();
void wptOpenFromSettings(int i);

// Location messages (see "Location sharing" below): newest incoming one in the thread.
struct InLoc { bool valid; double lat, lon; uint32_t counter; };
InLoc g_threadLoc = {};
bool locParse(const char* s, size_t len, double& lat, double& lon, bool* autoUpdate = nullptr);
void cposNote(const uint8_t dest[16], double lat, double lon, uint32_t at);
uint32_t msgEpoch(double ts);
// Enter on an empty draft: the newest of an unsaved card / a location wins.
void gpsPosText(char* out, size_t n);
int ashareActive();
void ashareStopAll();
bool locOffered() { return g_threadLoc.valid && (!cardOffered() || g_threadLoc.counter > g_threadCard.counter); }

// ---- Thread history ------------------------------------------------------------
constexpr int HIST_ROWS = 6;
struct HistRow { char text[36]; char status[12]; bool incoming; bool autoLoc; };
HistRow g_hist[HIST_ROWS];
int  g_histCount = 0;
char g_histNote[24] = "";

void loadHistory(const uint8_t peer[16]) {
    using namespace handheld::storage;
    g_histCount = 0;
    g_histNote[0] = 0;
    g_threadCard.valid = false;
    g_threadLoc.valid = false;
    if (!g_storeReady) { snprintf(g_histNote, sizeof(g_histNote), "(store off)"); return; }

    char hex[33];
    hashToHex(peer, hex);
    const std::string peerHex(hex);

    // Mark read when the thread is opened / refreshed.
    auto mr = g_msgStore.requestMarkRead(peerHex);
    if (mr.accepted()) {
        MessageStore::Result r;
        waitResult(mr.ticket, r, 400);
        g_msgStore.releaseResult(mr.ticket);
    }
    int ri = findRow(peer);
    if (ri >= 0) g_rows[ri].unread = 0;
    g_convValid = false;

    auto sub = g_msgStore.requestHistoryPage(peerHex, HistoryEntry{}, (uint8_t)HIST_ROWS,
                                             HistoryDirection::Before);
    for (int a = 0; a < 8 && !sub.accepted(); a++) {
        unsigned r = (unsigned)sub.rejection;
        if (r != 1 && r != 8) break;
        pumpFor(50, 12);
        sub = g_msgStore.requestHistoryPage(peerHex, HistoryEntry{}, (uint8_t)HIST_ROWS,
                                            HistoryDirection::Before);
    }
    if (!sub.accepted()) { snprintf(g_histNote, sizeof(g_histNote), "(history busy)"); return; }

    MessageStore::Result res;
    HistoryEntry entries[HIST_ROWS];
    size_t nEnt = 0;
    if (waitResult(sub.ticket, res) && res.outcome == Outcome::Committed &&
        res.error == Error::None && res.length >= sizeof(HistoryEntry)) {
        nEnt = res.length / sizeof(HistoryEntry);
        if (nEnt > (size_t)HIST_ROWS) nEnt = HIST_ROWS;
        if (!g_msgStore.readPayload(sub.ticket, entries, nEnt * sizeof(HistoryEntry))) nEnt = 0;
    }
    g_msgStore.releaseResult(sub.ticket);

    for (size_t i = 0; i < nEnt; i++) {
        HistRow& h = g_hist[g_histCount++];
        memset(&h, 0, sizeof(h));
        h.incoming = entries[i].incoming;
        snprintf(h.text, sizeof(h.text), "#%u", (unsigned)entries[i].counter);

        RecordKey key{};
        memcpy(key.peer, peer, 16);
        key.counter = entries[i].counter;
        key.incoming = entries[i].incoming;
        auto rs = g_msgStore.requestRecord(key, 0, 256);
        if (!rs.accepted()) continue;
        MessageStore::Result rr;
        uint8_t buf[256];
        size_t got = 0;
        if (waitResult(rs.ticket, rr) && rr.outcome == Outcome::Committed && rr.error == Error::None) {
            got = rr.length < sizeof(buf) ? rr.length : sizeof(buf);
            if (!g_msgStore.readPayload(rs.ticket, buf, got)) got = 0;
        }
        g_msgStore.releaseResult(rs.ticket);
        if (got < sizeof(StoredRecordHeader)) continue;

        StoredRecordHeader hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        size_t off = sizeof(hdr) + hdr.titleLength;
        if (off > got) off = got;
        size_t clen = hdr.contentLength;
        if (off + clen > got) clen = got - off;
        uint8_t cardHash[16];
        char cardName[24];
        const bool isCard = cardParse((const char*)buf + off, clen, cardHash, cardName, sizeof(cardName));
        if (isCard) {
            snprintf(h.text, sizeof(h.text), "[card] %s", cardName);
            if (h.incoming && (!g_threadCard.valid || entries[i].counter >= g_threadCard.counter)) {
                g_threadCard.valid = true;
                g_threadCard.counter = entries[i].counter;
                memcpy(g_threadCard.hash, cardHash, 16);
                strlcpy(g_threadCard.name, cardName, sizeof(g_threadCard.name));
            }
        }
        double la = 0, lo = 0;
        bool autoLoc = false;
        const bool isLoc = !isCard && locParse((const char*)buf + off, clen, la, lo, &autoLoc);
        if (isLoc) {
            snprintf(h.text, sizeof(h.text), "[loc%s] %.4f,%.4f", autoLoc ? " auto" : "", la, lo);
            h.autoLoc = autoLoc;
            if (h.incoming && (!g_threadLoc.valid || entries[i].counter >= g_threadLoc.counter))
                g_threadLoc = {true, la, lo, entries[i].counter};
            if (h.incoming) cposNote(peer, la, lo, msgEpoch(hdr.timestamp));   // map marker
        }
        if (clen >= sizeof(h.text)) clen = sizeof(h.text) - 1;
        if (clen && !isCard && !isLoc) {
            memcpy(h.text, buf + off, clen);
            h.text[clen] = 0;
            for (size_t k = 0; k < clen; k++)
                if ((unsigned char)h.text[k] < 32 || (unsigned char)h.text[k] > 126) h.text[k] = '?';
        }
        if (!h.incoming) {
            // Stored value = LXMFStatus: 0 draft, 1 queued, 2 sending, 3 sent (on air, no proof
            // yet), 4 delivered (proof received), 5 failed, 6 unconfirmed. (Until 2026-11-12 this
            // was off by one: sent showed "delivered" and delivered showed "sent".)
            switch (hdr.status) {
                case 0:
                case 1:  snprintf(h.status, sizeof(h.status), "queued"); break;
                case 2:  snprintf(h.status, sizeof(h.status), "sending"); break;
                case 3:  snprintf(h.status, sizeof(h.status), "sent"); break;
                case 4:  snprintf(h.status, sizeof(h.status), "delivered"); break;
                case 5:  snprintf(h.status, sizeof(h.status), "failed"); break;
                case 6:  snprintf(h.status, sizeof(h.status), "no proof"); break;
                default: snprintf(h.status, sizeof(h.status), "s%u", (unsigned)hdr.status); break;
            }
        }
    }
    // Auto location updates in a row (same direction): keep only the newest.
    for (int i = g_histCount - 1; i > 0; i--) {
        if (g_hist[i].autoLoc && g_hist[i - 1].autoLoc && g_hist[i].incoming == g_hist[i - 1].incoming) {
            for (int k = i - 1; k < g_histCount - 1; k++) g_hist[k] = g_hist[k + 1];
            g_histCount--;
        }
    }
    if (g_histCount == 0) snprintf(g_histNote, sizeof(g_histNote), "(no messages yet)");
}

// ---- Send ------------------------------------------------------------------------
char g_txStr[16] = "";
bool g_txTransient = false;   // last sendMessage failure will pass by itself (busy, spacing)
lv_obj_t* g_root = nullptr;

enum class TxStep { Done, Rejected, Timeout, Late, Invalid };

// Submissions the engine accepted but had not saved within txOnce's ~8 s wait (a slow flash
// write: 55 s once in a simulator run). The engine still saves and sends them, so they count
// as sent - reporting "failed" made the outbox / the user send them again (a duplicate) and
// left the ticket unacknowledged, holding one of the engine's 20 slots for good. The main loop
// finishes them (txLateTick).
handheld::outgoing::Ticket g_txLate[8];
void toast(const char* fmt, ...);

void txLateTick() {
    for (auto& t : g_txLate) {
        if (!t.valid()) continue;
        handheld::outgoing::InitialResult ir{};
        const auto p = g_proto.lxmfPoll(t, ir);
        if (p == handheld::outgoing::Poll::Pending) continue;
        if (p == handheld::outgoing::Poll::Ready) {
            g_proto.lxmfAcknowledge(t);
            const bool saved = ir.outcome == handheld::storage::Outcome::Committed;
            Serial.printf("[TX] late save: %s (error %u)\n", saved ? "saved" : "NOT saved", (unsigned)ir.error);
            if (!saved) toast("A message was not saved");
        }
        t = {};
    }
}

TxStep txOnce(const uint8_t dest[16], const char* body, size_t n,
              handheld::outgoing::InitialResult& ir, unsigned& rejection) {
    auto sub = g_proto.lxmfSubmit(dest, nullptr, 0, (const uint8_t*)body, n, false);
    if (!sub.accepted()) { rejection = (unsigned)sub.rejection; return TxStep::Rejected; }
    for (int i = 0; i < 800; i++) {
        pumpOnce();
        auto p = g_proto.lxmfPoll(sub.ticket, ir);
        if (p == handheld::outgoing::Poll::Pending) { delay(10); continue; }
        if (p == handheld::outgoing::Poll::Ready) { g_proto.lxmfAcknowledge(sub.ticket); return TxStep::Done; }
        return TxStep::Invalid;
    }
    for (auto& t : g_txLate)
        if (!t.valid()) { t = sub.ticket; return TxStep::Late; }
    return TxStep::Timeout;                      // 8 already late: something is stuck
}

bool sendMessage(const uint8_t dest[16], const char* text) {
    g_txTransient = false;
    if (!g_protoReady) { snprintf(g_txStr, sizeof(g_txStr), "proto off"); return false; }
    if (isSelf(dest))  { snprintf(g_txStr, sizeof(g_txStr), "pick remote"); return false; }

    char body[161];
    size_t n = strlen(text);
    if (n >= sizeof(body)) n = sizeof(body) - 1;
    memcpy(body, text, n);
    body[n] = 0;
    while (n && (body[n - 1] == ' ' || body[n - 1] == '\r' || body[n - 1] == '\n')) body[--n] = 0;
    if (!n) return false;

    static uint32_t lastTx = 0;
    if (lastTx && millis() - lastTx < 3000UL) {
        snprintf(g_txStr, sizeof(g_txStr), "wait 3s");
        g_txTransient = true;
        return false;
    }

    // Free LVGL widgets so the store can allocate the outgoing record.
    if (g_root) lv_obj_clean(g_root);
    pumpFor(30, 10);

    // One attempt only. Busy (engine rows all waiting on proofs) or Recovering used to be
    // retried here up to 12 x ~1.2 s, freezing the screen and keys for up to ~16-24 s when
    // the radio was busy; now the caller queues the message in the outbox (sendOrQueue),
    // which retries in the background.
    bool allocRetried = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        handheld::outgoing::InitialResult ir{};
        unsigned rej = 0;
        TxStep st = txOnce(dest, body, n, ir, rej);
        if (st == TxStep::Rejected) {
            g_txTransient = rej == 1 || rej == 8;   // Busy / Recovering
            snprintf(g_txStr, sizeof(g_txStr), g_txTransient ? "busy" : "rejected %u", rej);
            return false;
        }
        if (st == TxStep::Timeout) { snprintf(g_txStr, sizeof(g_txStr), "timeout"); return false; }
        if (st == TxStep::Late) {                // accepted, still saving: it will go out
            lastTx = millis();
            snprintf(g_txStr, sizeof(g_txStr), "saving");
            Serial.printf("[TX] saving (slow store) \"%s\"\n", body);
            return true;
        }
        if (st == TxStep::Invalid) { snprintf(g_txStr, sizeof(g_txStr), "invalid"); return false; }
        if (ir.outcome == handheld::storage::Outcome::Committed) {
            lastTx = millis();
            snprintf(g_txStr, sizeof(g_txStr), "%s", ir.txSuppressed ? "saved" : "sent");
            Serial.printf("[TX] %s \"%s\"\n", g_txStr, body);
            return true;
        }
        if (ir.error == handheld::storage::Error::Allocation && !allocRetried) {
            allocRetried = true;
            pumpFor(20, 10);
            continue;
        }
        snprintf(g_txStr, sizeof(g_txStr), "fail %u", (unsigned)ir.error);
        return false;
    }
    snprintf(g_txStr, sizeof(g_txStr), "busy");
    return false;
}

// ---- Check-in mode: schedule, outbox, peer clock --------------------------------------
// Where the local clock is in the check-in cycle (power::checkinPhase).
power::CheckinPhase checkinNow() {
    struct tm t;
    uint32_t sod = 0;
    if (localNow(t)) sod = (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
    return power::checkinPhase(g_pw, sod);
}

// Messages written outside a check-in window wait here and go out kCheckinSendS after the
// check-in time, when every device should be listening. Kept in NVS "rs_outbox" so a reset or
// battery swap does not lose them: blob "q2" = per item dest[16], text length (1 byte), the
// text without padding (a short text costs ~3 NVS entries instead of 6; NVS wear, see
// Stress_test_plan). Older firmware wrote blob "q" (fixed 177-byte items); it is read once
// and moved to "q2".
struct OutboxItem { uint8_t dest[16]; char text[161]; };
constexpr int kOutboxCap = 8;
OutboxItem g_outbox[kOutboxCap];
int g_outboxCount = 0;

void outboxSave() {
    Preferences p;
    if (!p.begin("rs_outbox", false)) { Serial.println("[CHECKIN] outbox save failed (nvs)"); return; }
    if (g_outboxCount) {
        static uint8_t buf[kOutboxCap * (16 + 1 + sizeof(OutboxItem::text))];   // main loop only
        size_t n = 0;
        for (int i = 0; i < g_outboxCount; i++) {
            const size_t len = strnlen(g_outbox[i].text, sizeof(OutboxItem::text) - 1);
            memcpy(buf + n, g_outbox[i].dest, 16);
            buf[n + 16] = (uint8_t)len;
            memcpy(buf + n + 17, g_outbox[i].text, len);
            n += 17 + len;
        }
        p.putBytes("q2", buf, n);
    } else {
        p.remove("q2");
    }
    if (p.isKey("q")) p.remove("q");            // old format, already moved
    p.end();
}

void outboxLoad() {
    Preferences p;
    if (!p.begin("rs_outbox", true)) return;
    bool migrate = false;
    size_t n = p.getBytesLength("q2");
    if (n) {
        static uint8_t buf[kOutboxCap * (16 + 1 + sizeof(OutboxItem::text))];   // main loop only
        if (n <= sizeof(buf) && p.getBytes("q2", buf, n) == n) {
            size_t off = 0;
            while (off + 17 <= n && g_outboxCount < kOutboxCap) {
                const size_t len = buf[off + 16];
                if (len >= sizeof(OutboxItem::text) || off + 17 + len > n) break;   // damaged: keep what was read
                OutboxItem& o = g_outbox[g_outboxCount++];
                memset(&o, 0, sizeof(o));
                memcpy(o.dest, buf + off, 16);
                memcpy(o.text, buf + off + 17, len);
                off += 17 + len;
            }
        }
    } else if ((n = p.getBytesLength("q")) && n % sizeof(OutboxItem) == 0 && n <= sizeof(g_outbox)) {
        g_outboxCount = (int)(p.getBytes("q", g_outbox, n) / sizeof(OutboxItem));
        for (int i = 0; i < g_outboxCount; i++) g_outbox[i].text[sizeof(g_outbox[i].text) - 1] = 0;
        migrate = true;
    }
    p.end();
    if (migrate) outboxSave();                  // write "q2", remove "q"
    if (g_outboxCount) Serial.printf("[CHECKIN] outbox restored: %d waiting\n", g_outboxCount);
}

bool outboxAdd(const uint8_t dest[16], const char* text) {
    if (g_outboxCount >= kOutboxCap) return false;
    OutboxItem& o = g_outbox[g_outboxCount++];
    memset(&o, 0, sizeof(o));
    memcpy(o.dest, dest, 16);
    strlcpy(o.text, text, sizeof(o.text));
    outboxSave();
    Serial.printf("[CHECKIN] queued \"%s\" (%d waiting)\n", o.text, g_outboxCount);
    return true;
}

// Send now, or queue in the outbox when the radio is busy / the 3 s spacing applies (the
// outbox retries in the background, see checkinTick). While anything waits in the outbox,
// new messages queue behind it so they keep their order.
enum class SendResult { Sent, Queued, Failed };
SendResult sendOrQueue(const uint8_t dest[16], const char* text) {
    if (g_outboxCount == 0) {
        if (sendMessage(dest, text)) return SendResult::Sent;
        if (!g_txTransient) return SendResult::Failed;
    }
    if (!outboxAdd(dest, text)) { snprintf(g_txStr, sizeof(g_txStr), "outbox full"); return SendResult::Failed; }
    return SendResult::Queued;
}

// Peer clock adoption (check-in mode, clock not GPS/NTP synchronized this boot):
//   announce time  - the sender is synchronized: take it (either direction, within a day)
//   message time   - the sender may not be: only move forward, by at most 15 min (a late
//                    retry can only look older), so unsynced devices converge on the fastest
uint32_t g_clkSeq = 0;
int32_t  g_clkLastOff = 0;
time_t   g_clkLastAt = 0;
uint8_t  g_clkLastSrc = peerclock::NONE;

void peerClockTick() {
    peerclock::Sample s;
    if (!peerclock::take(g_clkSeq, s)) return;
    if (!g_pwEff.checkin || handheld::ClockConfidence::synchronized()) return;
    const time_t now = time(nullptr);
    if (now < 1700000000) return;
    const int64_t est = (int64_t)s.senderSecs + (int64_t)((millis() - s.atMs) / 1000) + 1;   // +airtime
    const int64_t off = est - (int64_t)now;
    const bool ok = s.source == peerclock::ANNOUNCE ? (off >= 2 || off <= -2) && off < 86400 && off > -86400
                                                    : off >= 2 && off <= 900;
    Serial.printf("[CLOCK] peer %s time %+lld s%s\n", s.source == peerclock::ANNOUNCE ? "announce" : "message",
                  (long long)off, ok ? " -> adopted" : "");
    if (!ok) return;
    struct timeval tv = { .tv_sec = (time_t)est, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    g_clkLastOff = (int32_t)off;
    g_clkLastAt = (time_t)est;
    g_clkLastSrc = s.source;
}

void saveContactName(const uint8_t peer[16], const char* name) {
    if (isSelf(peer)) { saveSelfName(name); return; }
    char hex[33];
    hashToHex(peer, hex);
    // addManualContact works whether or not the node is currently announced.
    bool ok = g_announceMgr.addManualContact(std::string(hex), std::string(name));
    g_announceMgr.saveNameCache();
    rememberName(peer, name);
    Serial.printf("[NAME] %s -> \"%s\" ok=%d\n", hex, name, (int)ok);
}

// =============================================================================
// 4. UI state model
// =============================================================================

enum ScreenId : uint8_t { SCR_HOME, SCR_INBOX, SCR_CONTACTS, SCR_PEERS, SCR_SETTINGS, SCR_COUNT };

const char* screenName(ScreenId s) {
    switch (s) {
        case SCR_HOME:     return "Home";
        case SCR_INBOX:    return "Inbox";
        case SCR_CONTACTS: return "Contacts";
        case SCR_PEERS:    return "Peers";
        case SCR_SETTINGS: return "Settings";
        default:           return "?";
    }
}

ScreenId g_screen = SCR_HOME;

// MENU overlay rows (doc §4.3): Home · Settings · Contacts · Peers · Inbox
// Last row "Map" is not a screen: it opens the offline map (same as L); hotkey A.
// "Library" opens the reference texts on the SD card; hotkey R.
constexpr int  kMenuCount = 7, kMenuMap = 5, kMenuLib = 6;
const ScreenId kMenuScreens[kMenuCount] = {SCR_HOME, SCR_SETTINGS, SCR_CONTACTS, SCR_PEERS, SCR_INBOX, SCR_HOME, SCR_HOME};
const char*    kMenuLabels[kMenuCount]  = {"Home", "Settings", "Contacts", "Peers", "Inbox", "Atlas", "Library"};
const char     kMenuKeys[kMenuCount]    = {'H', 'S', 'C', 'P', 'I', 'A', 'R'};
bool g_menuOpen = false;
int  g_menuSel = 0;

// Lists: selection remembered per screen by peer hash.
int     g_listSel = 0, g_listScroll = 0;
uint8_t g_rememberHash[SCR_COUNT][16];
bool    g_rememberValid[SCR_COUNT] = {};

// Thread
bool    g_threadOpen = false;
uint8_t g_threadPeer[16];

// Entry mode: one text buffer, one owner.
enum class Entry : uint8_t { None, Message, Name, RtcField, IdleSecs, Latitude, Longitude, WifiPass,
                             PassOld, PassNew, PassConfirm,     // Pass*: lock password (arg = purpose)
                             Search,                            // list filter (Inbox / Contacts / Peers)
                             TcpHost, TcpPort,
                             MapNum,                            // contact page: map number
                             WptName,                           // map P: waypoint name
                             LibFind,                           // library: find text
                             PlaceFind,                         // map R: place name
                             FoundText,                         // idle "If found": owner text
                             CheckinTime };                     // S.4.8: hhmm
Entry   g_entry = Entry::None;
int     g_entryArg = 0;              // RtcField index
uint8_t g_nameTarget[16];
constexpr size_t BUF_CAP = 120;
char    g_buf[BUF_CAP + 1] = "";
size_t  g_bufLen = 0;

bool entryActive() { return g_entry != Entry::None; }
bool passEntry(Entry e) { return e == Entry::PassOld || e == Entry::PassNew || e == Entry::PassConfirm; }
bool browseMode()  { return !entryActive(); }
bool entryNumeric() {
    return g_entry == Entry::RtcField || g_entry == Entry::IdleSecs || g_entry == Entry::TcpPort ||
           g_entry == Entry::CheckinTime ||
           g_entry == Entry::MapNum ||
           g_entry == Entry::Latitude || g_entry == Entry::Longitude;
}

// Settings
enum Page : uint8_t {
    PG_ROOT, PG_TIME, PG_RTC, PG_TZ, PG_LOC, PG_WIFI, PG_SCREEN, PG_IDLE,
    PG_BATTERY, PG_BATLOG, PG_IDENTITY, PG_LORA, PG_ALERTS, PG_DIAG, PG_STORAGE, PG_SD,
    PG_WIFI_SCAN, PG_WIFI_SAVED, PG_WIFI_NET, PG_WIFI_JOIN, PG_POWER, PG_POWER_CUSTOM, PG_ENERGY, PG_ENERGY_GRAPH,
    PG_CHECKIN, PG_TCP, PG_IDS, PG_ID_ONE, PG_WPTS, PG_TRACK, PG_GEOFS, PG_GEOF_NEW, PG_LOWBAT,
    PG_BACKUPS
};
struct NavFrame { Page page; int sel; int scroll; };
constexpr int NAV_DEPTH = 6;
NavFrame g_navStack[NAV_DEPTH];
int  g_navDepth = 0;
Page g_page = PG_ROOT;
int  g_sel = 0, g_scroll = 0;
int  g_rtc[5] = {12, 0, 1, 1, 2027};   // staged hour, min, month, day, year

// Redraw / toast / idle
bool     g_redraw = true;
uint32_t g_redrawAt = 0;           // debounced redraw deadline (0 = none)
bool     g_fullRefreshReq = false;
char     g_toast[28] = "";
uint32_t g_toastUntil = 0;
bool     g_idleActive = false;
uint32_t g_lastInputMs = 0;

void requestRedraw() { g_redraw = true; }

void toast(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
    va_end(ap);
    g_toastUntil = millis() + 3000;
    Serial.printf("[UI] %s\n", g_toast);
    requestRedraw();
}

void syncNumLayer() { keyboardSetNumLayer(entryNumeric()); }

void bufClear() { g_buf[0] = 0; g_bufLen = 0; }
void bufSet(const char* s) {
    bufClear();
    for (size_t i = 0; s && s[i] && g_bufLen < BUF_CAP; i++)
        if (s[i] >= 32 && s[i] <= 126) g_buf[g_bufLen++] = s[i];
    g_buf[g_bufLen] = 0;
}

bool g_showPass = false;
bool g_entryReplace = false;                 // the default text goes on the first typed key            // MIC in a password field: show / hide what was typed

void startEntry(Entry kind, const char* initial = nullptr, int arg = 0) {
    g_entry = kind;
    g_showPass = false;
    g_entryReplace = false;
    g_entryArg = arg;
    bufSet(initial);
    syncNumLayer();
    requestRedraw();
}

char g_passNew[33] = "";            // lock password typed once, waiting for the repeat

void endEntry() {
    g_entry = Entry::None;
    g_showPass = false;
    memset(g_buf, 0, sizeof(g_buf));     // may have held a password
    memset(g_passNew, 0, sizeof(g_passNew));
    bufClear();
    syncNumLayer();
    requestRedraw();
}

// ---- WiFi (station) ----------------------------------------------------------------
// Saved networks live in NVS "rs_wifi" (n, s<i>, p<i>, a<i>). The radio stays off
// unless the user scans/joins or a saved network has auto-connect on. Auto-connect
// scans, joins the strongest saved network in range, and retries every few minutes.
constexpr int WIFI_MAX_SAVED = 8;
constexpr int WIFI_MAX_SEEN  = 11;               // + "Rescan" row = 12 settings rows
constexpr uint32_t WIFI_JOIN_MS  = 20000;
constexpr uint32_t WIFI_RETRY_MS = 180000;
constexpr size_t WIFI_PASS_MAX = 63;

struct WifiCred { char ssid[33]; char pass[65]; bool autoJoin; bool passBad; };  // passBad: stored password unreadable
struct WifiSeen { char ssid[33]; int16_t rssi; bool secure; };
enum class WifiSt : uint8_t { Off, Connecting, Connected, Failed };

WifiCred g_wifiSaved[WIFI_MAX_SAVED];
int      g_wifiSavedCount = 0;
WifiSeen g_wifiSeen[WIFI_MAX_SEEN];
int      g_wifiSeenCount = 0;
bool     g_wifiScanned = false;
bool     g_wifiScanning = false;
bool     g_wifiScanForAuto = false;
WifiSt   g_wifiSt = WifiSt::Off;
bool     g_wifiUserOff = false;                  // "Disconnect" pauses auto-connect until a manual join
uint32_t g_wifiDeadline = 0;
uint32_t g_wifiNextAuto = 0;
char     g_wifiMsg[28] = "off";
// Current / pending join
char     g_wifiTarget[33] = "";
char     g_wifiTargetPass[65] = "";
bool     g_wifiTargetAuto = true;
bool     g_wifiSaveOnOk = false;
bool     g_wifiJoinAfterScan = false;
// Settings page staging
int      g_wifiNetIdx = 0;                       // PG_WIFI_NET
char     g_joinSsid[33] = "";                    // PG_WIFI_JOIN
char     g_joinPass[65] = "";
bool     g_joinSecure = true;
bool     g_joinAuto = true;

bool isWifiPage(Page p) {
    return p == PG_WIFI || p == PG_WIFI_SCAN || p == PG_WIFI_SAVED ||
           p == PG_WIFI_NET || p == PG_WIFI_JOIN || p == PG_TCP;
}

void wifiUiChanged() {
    if (g_idleActive || g_screen != SCR_SETTINGS) return;
    if (g_page == PG_ROOT || isWifiPage(g_page)) requestRedraw();
}

// Overwrite a String's bytes before it is freed (best effort for heap copies of secrets).
void wipeString(String& s) {
    for (size_t i = 0; i < s.length(); i++) s.setCharAt(i, 0);
}

// Passwords are stored sealed by the SecretVault (AES-256-GCM, AAD = SSID), never
// as plaintext. They are plaintext only in RAM, where WiFi.begin() needs them.
void wifiSaveAll() {
    Preferences p;
    if (!p.begin("rs_wifi", false)) return;
    p.clear();
    p.putUChar("n", (uint8_t)g_wifiSavedCount);
    for (int i = 0; i < g_wifiSavedCount; i++) {
        const WifiCred& c = g_wifiSaved[i];
        char k[4];
        snprintf(k, sizeof(k), "s%d", i); p.putString(k, c.ssid);
        String sealed;
        if (c.pass[0] && !vault::seal(c.pass, c.ssid, sealed)) {
            Serial.printf("[WIFI] can't encrypt \"%s\" (vault %s); password not stored\n",
                          c.ssid, vault::status());
            sealed = "";                         // never fall back to plaintext
        }
        snprintf(k, sizeof(k), "p%d", i); p.putString(k, sealed);
        snprintf(k, sizeof(k), "a%d", i); p.putUChar(k, c.autoJoin ? 1 : 0);
    }
    p.end();
}

void wifiLoad() {
    Preferences p;
    bool legacy = false;
    g_wifiSavedCount = 0;
    if (!p.begin("rs_wifi", true)) return;
    int n = p.getUChar("n", 0);
    if (n > WIFI_MAX_SAVED) n = WIFI_MAX_SAVED;
    for (int i = 0; i < n; i++) {
        char k[4];
        WifiCred& c = g_wifiSaved[g_wifiSavedCount];
        snprintf(k, sizeof(k), "s%d", i);
        String s = p.getString(k, "");
        if (s.isEmpty()) continue;
        strlcpy(c.ssid, s.c_str(), sizeof(c.ssid));
        snprintf(k, sizeof(k), "p%d", i);
        String stored = p.getString(k, "");
        c.passBad = false;
        if (vault::isSealed(stored.c_str())) {
            if (!vault::unseal(stored.c_str(), c.ssid, c.pass, sizeof(c.pass))) {
                c.pass[0] = 0;
                c.passBad = true;                // vault reset / locked: user must re-type it
                Serial.printf("[WIFI] password for \"%s\" unreadable\n", c.ssid);
            }
        } else {
            strlcpy(c.pass, stored.c_str(), sizeof(c.pass));
            if (c.pass[0]) legacy = true;        // pre-vault plaintext: re-saved sealed below
        }
        wipeString(stored);
        snprintf(k, sizeof(k), "a%d", i);
        c.autoJoin = p.getUChar(k, 1) != 0;
        g_wifiSavedCount++;
    }
    p.end();
    Serial.printf("[WIFI] %d saved network(s)%s\n", g_wifiSavedCount,
                  legacy ? ", encrypting old plaintext" : "");
    if (legacy) wifiSaveAll();
}

int wifiFindSaved(const char* ssid) {
    for (int i = 0; i < g_wifiSavedCount; i++)
        if (strcmp(g_wifiSaved[i].ssid, ssid) == 0) return i;
    return -1;
}

// Add or update; the newest network goes last, the oldest is dropped when full.
void wifiRemember(const char* ssid, const char* pass, bool autoJoin) {
    int i = wifiFindSaved(ssid);
    if (i < 0) {
        if (g_wifiSavedCount >= WIFI_MAX_SAVED) {
            memmove(&g_wifiSaved[0], &g_wifiSaved[1], sizeof(WifiCred) * (WIFI_MAX_SAVED - 1));
            g_wifiSavedCount--;
        }
        i = g_wifiSavedCount++;
        strlcpy(g_wifiSaved[i].ssid, ssid, sizeof(g_wifiSaved[i].ssid));
    }
    strlcpy(g_wifiSaved[i].pass, pass, sizeof(g_wifiSaved[i].pass));
    g_wifiSaved[i].passBad = false;
    g_wifiSaved[i].autoJoin = autoJoin;
    wifiSaveAll();
}

void wifiForget(int i) {
    if (i < 0 || i >= g_wifiSavedCount) return;
    memmove(&g_wifiSaved[i], &g_wifiSaved[i + 1], sizeof(WifiCred) * (g_wifiSavedCount - i - 1));
    g_wifiSavedCount--;
    wifiSaveAll();
}

bool wifiHasAutoCreds() {
    for (int i = 0; i < g_wifiSavedCount; i++)
        if (g_wifiSaved[i].autoJoin && !g_wifiSaved[i].passBad) return true;
    return false;
}

bool wifiConnected() { return g_wifiSt == WifiSt::Connected && WiFi.status() == WL_CONNECTED; }

void wifiRadioOn() {
    if (WiFi.getMode() != WIFI_OFF) return;
    WiFi.persistent(false);                      // credentials are ours (rs_wifi), not the SDK's
    WiFi.setHostname("ratspeak-tdeck");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    Serial.printf("[WIFI] radio on (heap %u, int %u)\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void wifiRadioOff() {
    if (WiFi.getMode() == WIFI_OFF) return;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] radio off");
}

void wifiBeginJoin() {
    wifiRadioOn();
    if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(false);
    WiFi.begin(g_wifiTarget, g_wifiTargetPass[0] ? g_wifiTargetPass : nullptr);
    g_wifiSt = WifiSt::Connecting;
    g_wifiDeadline = millis() + WIFI_JOIN_MS;
    snprintf(g_wifiMsg, sizeof(g_wifiMsg), "joining %.18s", g_wifiTarget);
    Serial.printf("[WIFI] joining \"%s\"\n", g_wifiTarget);
    wifiUiChanged();
}

// save: store the credentials once the join succeeds.
void wifiConnect(const char* ssid, const char* pass, bool save, bool autoJoin) {
    if (!g_pwEff.wifi) { toast("WiFi off in %s mode", power::modeName(g_pw.mode)); return; }
    strlcpy(g_wifiTarget, ssid, sizeof(g_wifiTarget));
    strlcpy(g_wifiTargetPass, pass ? pass : "", sizeof(g_wifiTargetPass));
    g_wifiTargetAuto = autoJoin;
    g_wifiSaveOnOk = save;
    g_wifiUserOff = false;
    if (g_wifiScanning) {                        // the radio can't join mid-scan
        g_wifiJoinAfterScan = true;
        snprintf(g_wifiMsg, sizeof(g_wifiMsg), "joining %.18s", g_wifiTarget);
        g_wifiSt = WifiSt::Connecting;
        g_wifiDeadline = millis() + WIFI_JOIN_MS + 15000;
        wifiUiChanged();
        return;
    }
    wifiBeginJoin();
}

void wifiStartScan(bool forAuto) {
    if (g_wifiScanning) return;
    if (!g_pwEff.wifi) {
        if (!forAuto) toast("WiFi off in %s mode", power::modeName(g_pw.mode));
        return;
    }
    wifiRadioOn();
    if (!WiFiInterface::startAsyncScan()) {
        strlcpy(g_wifiMsg, "scan failed", sizeof(g_wifiMsg));
        wifiUiChanged();
        return;
    }
    g_wifiScanning = true;
    g_wifiScanForAuto = forAuto;
    wifiUiChanged();
}

void wifiDisconnect() {
    g_wifiUserOff = true;
    g_wifiJoinAfterScan = false;
    wifiRadioOff();
    g_wifiScanning = false;
    g_wifiSt = WifiSt::Off;
    strlcpy(g_wifiMsg, "off (auto paused)", sizeof(g_wifiMsg));
    wifiUiChanged();
}

void wifiFinishScan() {
    auto res = WiFiInterface::getScanResults(WIFI_MAX_SEEN);
    WiFi.scanDelete();
    g_wifiScanning = false;
    g_wifiScanned = true;
    g_wifiSeenCount = 0;
    for (auto& r : res) {
        if (g_wifiSeenCount >= WIFI_MAX_SEEN) break;
        WifiSeen& s = g_wifiSeen[g_wifiSeenCount++];
        strlcpy(s.ssid, r.ssid.c_str(), sizeof(s.ssid));
        s.rssi = (int16_t)r.rssi;
        s.secure = r.encrypted;
    }
    if (g_wifiJoinAfterScan) {
        g_wifiJoinAfterScan = false;
        wifiBeginJoin();
    } else if (g_wifiScanForAuto && g_wifiSt != WifiSt::Connected) {
        int best = -1;                           // results are strongest first
        for (int i = 0; i < g_wifiSeenCount && best < 0; i++) {
            int k = wifiFindSaved(g_wifiSeen[i].ssid);
            if (k >= 0 && g_wifiSaved[k].autoJoin && !g_wifiSaved[k].passBad) best = k;
        }
        if (best >= 0) {
            wifiConnect(g_wifiSaved[best].ssid, g_wifiSaved[best].pass, false, true);
        } else {
            strlcpy(g_wifiMsg, "no saved net in range", sizeof(g_wifiMsg));
            g_wifiNextAuto = millis() + WIFI_RETRY_MS;
            if (!(g_screen == SCR_SETTINGS && isWifiPage(g_page))) wifiRadioOff();
        }
    }
    g_wifiScanForAuto = false;
    wifiUiChanged();
}

void watchWifi() {
    static uint32_t last = 0;
    if (millis() - last < 250) return;
    last = millis();

    if (g_wifiScanning && WiFiInterface::isScanComplete()) wifiFinishScan();

    const wl_status_t s = WiFi.getMode() == WIFI_OFF ? WL_DISCONNECTED : WiFi.status();
    if (g_wifiSt == WifiSt::Connecting && !g_wifiJoinAfterScan) {
        const uint32_t started = g_wifiDeadline - WIFI_JOIN_MS;
        if (s == WL_CONNECTED) {
            g_wifiSt = WifiSt::Connected;
            snprintf(g_wifiMsg, sizeof(g_wifiMsg), "%.18s", g_wifiTarget);
            Serial.printf("[WIFI] connected \"%s\" ip %s rssi %d\n", g_wifiTarget,
                          WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            if (g_wifiSaveOnOk) wifiRemember(g_wifiTarget, g_wifiTargetPass, g_wifiTargetAuto);
            g_wifiSaveOnOk = false;
            toast("WiFi: %.18s", g_wifiTarget);
            wifiUiChanged();
        } else {
            const bool hardFail = (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) &&
                                  millis() - started > 4000;
            if (hardFail || (int32_t)(millis() - g_wifiDeadline) >= 0) {
                const char* why = s == WL_NO_SSID_AVAIL ? "not found"
                                : s == WL_CONNECT_FAILED ? "wrong password?" : "timed out";
                Serial.printf("[WIFI] join \"%s\" failed: %s (status %d)\n", g_wifiTarget, why, (int)s);
                WiFi.disconnect(false);          // stop the SDK retrying a bad password
                g_wifiSt = WifiSt::Failed;
                g_wifiSaveOnOk = false;
                snprintf(g_wifiMsg, sizeof(g_wifiMsg), "failed: %s", why);
                toast("WiFi %s", g_wifiMsg);
                g_wifiNextAuto = millis() + WIFI_RETRY_MS;
                wifiUiChanged();
            }
        }
    } else if (g_wifiSt == WifiSt::Connected && s != WL_CONNECTED) {
        Serial.println("[WIFI] link lost, reconnecting");
        g_wifiSt = WifiSt::Connecting;           // SDK auto-reconnect gets one join window
        g_wifiDeadline = millis() + WIFI_JOIN_MS;
        g_wifiSaveOnOk = false;
        snprintf(g_wifiMsg, sizeof(g_wifiMsg), "rejoining %.16s", g_wifiTarget);
        wifiUiChanged();
    } else if (g_wifiSt == WifiSt::Connected) {
        static uint32_t lastRssi = 0;            // keep the dBm on the WiFi page fresh
        if (millis() - lastRssi > 10000) { lastRssi = millis(); if (g_page == PG_WIFI) wifiUiChanged(); }
    }

    // Auto-connect: only when idle, not paused by the user, and not while the user
    // is looking at the scan list (a background scan would reshuffle it).
    const bool idle = g_wifiSt == WifiSt::Off || g_wifiSt == WifiSt::Failed;
    const bool onWifiPage = g_screen == SCR_SETTINGS && isWifiPage(g_page);
    if (idle && !g_wifiScanning && !onWifiPage && WiFi.getMode() != WIFI_OFF) wifiRadioOff();
    if (idle && g_pwEff.wifi && !g_wifiUserOff && !g_wifiScanning && wifiHasAutoCreds() &&
        (int32_t)(millis() - g_wifiNextAuto) >= 0 &&
        !(g_screen == SCR_SETTINGS && (g_page == PG_WIFI_SCAN || g_page == PG_WIFI_JOIN))) {
        g_wifiNextAuto = millis() + WIFI_RETRY_MS;
        Serial.println("[WIFI] auto-connect scan");
        wifiStartScan(true);
    }
}

void wifiStatusLine(char* out, size_t n) {
    if (wifiConnected())
        snprintf(out, n, "%.18s %d dBm", g_wifiTarget, (int)WiFi.RSSI());
    else if (g_wifiScanning && g_wifiSt != WifiSt::Connecting)
        snprintf(out, n, "scanning...");
    else
        snprintf(out, n, "%s", g_wifiMsg);
}

// =============================================================================
// 5. Drawing primitives + touch hits
// =============================================================================

// Layout (240 x 320 portrait)
constexpr int W           = EPD_WIDTH;
constexpr int H           = EPD_HEIGHT;
constexpr int TOP_H       = 36;
constexpr int REF_W       = 28;                      // refresh icon
constexpr int BAT_W       = 28;                      // battery column
constexpr int TAB_W       = (W - REF_W - BAT_W) / 2; // clock | MENU (92)
constexpr int CLOCK_X     = REF_W + BAT_W;
constexpr int MENU_X      = CLOCK_X + TAB_W;
constexpr int BOT_H       = 40;
constexpr int BOT_TOP     = H - BOT_H;               // 280
constexpr int BOT_C_W     = 80;                      // Contact | Atlas | Inbox
constexpr int BOT_A_W     = 64;
constexpr int BOT_I_W     = W - BOT_C_W - BOT_A_W;   // 96
constexpr int MENU_ROW_H  = 32;                        // 7 rows end at 260 (bottom bar 280)
constexpr int HEADER_Y    = TOP_H + 1;               // path / title line
constexpr int LIST_TOP    = TOP_H + 20;              // 56
constexpr int FOOT_H      = 20;
constexpr int FOOT_Y      = BOT_TOP - FOOT_H;        // 260
constexpr int ROW_H       = 44;
constexpr int VIS_ROWS    = (FOOT_Y - 2 - LIST_TOP) / ROW_H;   // 4
constexpr int STRIP_LINE  = 18;
constexpr int STRIP_Y     = BOT_TOP - STRIP_LINE * 3;          // 226

const lv_font_t* kFont = &lv_font_montserrat_16;

lv_color_t fg() { return g_cfg.dark ? lv_color_white() : lv_color_black(); }
lv_color_t bg() { return g_cfg.dark ? lv_color_black() : lv_color_white(); }

lv_obj_t* box(int x, int y, int w, int h, int border, bool filled = false,
              lv_border_side_t sides = LV_BORDER_SIDE_FULL) {
    lv_obj_t* o = lv_obj_create(g_root);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, border, 0);
    lv_obj_set_style_border_color(o, fg(), 0);
    lv_obj_set_style_border_side(o, sides, 0);
    lv_obj_set_style_bg_color(o, filled ? fg() : bg(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void fillRect(int x, int y, int w, int h, bool useFg = true) {
    if (w < 1 || h < 1) return;
    lv_obj_t* o = box(x, y, w, h, 0, useFg);
    (void)o;
}

int textW(const char* s, size_t len = 0) {
    if (!s) return 0;
    if (!len) len = strlen(s);
    int w = lv_txt_get_width(s, len, kFont, 0, LV_TEXT_FLAG_NONE);
    return w > 0 ? w : (int)len * 8;
}

// Dark theme needs an extra pass so white glyphs survive e-ink.
void text(int x, int y, const char* s, bool bold = false, int maxW = 0) {
    static const int8_t ox[] = {0, 1, 0};
    static const int8_t oy[] = {0, 0, 1};
    int passes = bold ? 2 : 1;
    if (g_cfg.dark) passes++;
    for (int k = 0; k < passes; k++) {
        lv_obj_t* o = lv_label_create(g_root);
        lv_label_set_text(o, s ? s : "");
        lv_obj_set_style_text_font(o, kFont, 0);
        lv_obj_set_style_text_color(o, fg(), 0);
        if (maxW > 0) {
            lv_obj_set_width(o, maxW);
            lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
        }
        lv_obj_set_pos(o, x + ox[k], y + oy[k]);
    }
}

// Centered label; optional underline under the first case-insensitive match of
// `hotkey` (doc: "underlines only when the action works").
void label(int x, int y, int w, int h, const char* s, bool bold = false, char hotkey = 0) {
    int tw = textW(s);
    int lx = x + (w - tw) / 2;
    if (lx < x + 2) lx = x + 2;
    int ly = y + (h - 16) / 2;
    text(lx, ly, s, bold, w - (lx - x) - 2);
    if (!hotkey) return;
    for (int i = 0; s[i]; i++) {
        if (toupper((unsigned char)s[i]) == toupper((unsigned char)hotkey)) {
            int px = i ? textW(s, i) : 0;
            int cw = textW(s + i, 1);
            fillRect(lx + px, ly + 17, cw + 1, 2);
            return;
        }
    }
}

// ---- Touch hit registry (filled while drawing) ------------------------------------
enum HitAct : uint8_t {
    H_REFRESH, H_MENU, H_MENU_GO, H_TAB, H_ROW, H_BACK, H_ANNOUNCE,
    H_TILE_AUDIO, H_UP, H_DOWN, H_CARD, H_TMENU, H_NAVKEYS
};
struct Hit { int16_t x, y, w, h; uint8_t act; int8_t arg; };
constexpr int MAX_HITS = 32;
Hit g_hits[MAX_HITS];
int g_hitCount = 0;

void addHit(int x, int y, int w, int h, HitAct act, int arg = 0) {
    if (g_hitCount >= MAX_HITS) return;
    g_hits[g_hitCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)act, (int8_t)arg};
}

// ---- Shared widgets -----------------------------------------------------------------
void navBox(int x, int y, int w, int h, const char* s, bool active, char hotkey) {
    box(x, y, w, h, active ? 3 : 1);
    label(x, y, w, h, s, active, hotkey);
}

// List / settings row: bold outline = selected. Leading row number is
// underlined when digit-select works (browse mode).
void listRow(int y, const char* title, const char* sub, bool selected, const char* right = nullptr) {
    box(2, y, W - 4, ROW_H, selected ? 3 : 1);
    const bool hasSub = sub && sub[0];
    const int ty = hasSub ? y + 4 : y + (ROW_H - 16) / 2;
    int rightW = (right && right[0]) ? textW(right) + 8 : 0;
    text(8, ty, title, selected, W - 16 - rightW);
    if (rightW) text(W - 6 - rightW + 2, ty, right, true);
    if (hasSub) text(8, y + 23, sub, false, W - 16);
    if (browseMode() && title[0] >= '0' && title[0] <= '9') {
        int nd = (title[1] >= '0' && title[1] <= '9') ? 2 : 1;
        fillRect(8, ty + 17, textW(title, nd), 2);
    }
}

// Battery icon (vertical, fills bottom->top). Bolt = charging, plug = USB idle.
void batSeg(int x, int y, int w, int h, int fillTopY) {
    if (y >= fillTopY)            fillRect(x, y, w, h, false);
    else if (y + h <= fillTopY)   fillRect(x, y, w, h, true);
    else { fillRect(x, y, w, fillTopY - y, true); fillRect(x, fillTopY, w, y + h - fillTopY, false); }
}

void drawBatteryIcon(int x, int y, int w, int h, int pct) {
    fillRect(x + w / 3, y, w / 3, 3);
    const int by = y + 3, bh = h - 5;
    box(x, by, w, bh, 1);
    int fh = 0;
    if (pct > 0) {
        fh = constrain(((bh - 4) * pct) / 100, 1, bh - 4);
        fillRect(x + 2, by + bh - 2 - fh, w - 4, fh);
    }
    const int top = by + bh - 2 - fh;
    const int cx = x + w / 2, cy = by + bh / 2;
    if (g_bat.charging) {
        batSeg(cx + 1, cy - 7, 5, 3, top); batSeg(cx - 2, cy - 4, 4, 3, top);
        batSeg(cx - 4, cy - 1, 8, 3, top); batSeg(cx - 1, cy + 2, 4, 3, top);
        batSeg(cx - 5, cy + 5, 5, 3, top);
    } else if (g_bat.usb) {
        batSeg(cx - 3, cy - 7, 2, 4, top); batSeg(cx + 1, cy - 7, 2, 4, top);
        batSeg(cx - 4, cy - 3, 8, 2, top); batSeg(cx - 3, cy - 1, 6, 7, top);
    }
}

void drawRefreshIcon(int x, int y, int w, int h) {
    box(x, y, w, h, 1);
    const int cx = x + w / 2, cy = y + h / 2;
    lv_obj_t* ring = box(cx - 8, cy - 8, 16, 16, 2);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    fillRect(cx + 3, cy - 7, 5, 3);
    fillRect(cx - 8, cy + 4, 5, 3);
}

void drawLogo(int cx, int cy) {
    const int w = RATSPEAK_LOGO_W, h = RATSPEAK_LOGO_H, rowBytes = (w + 7) / 8;
    const int x0 = cx - w / 2, y0 = cy - h / 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t b = pgm_read_byte(&RATSPEAK_LOGO_BITS[y * rowBytes + x / 8]);
            bool srcBlack = (b & (0x80 >> (x & 7))) != 0;
            bool black = g_cfg.dark ? srcBlack : !srcBlack;   // light theme: dark mark on paper
            int px = x0 + x, py = y0 + y;
            if (px >= 0 && py >= 0 && px < W && py < H) g_display.setPixel(px, py, black);
        }
    }
    g_display.markDirty();
}

void clearRoot() {
    g_hitCount = 0;
    g_display.fillScreen(g_cfg.dark);
    if (!g_root) {
        g_root = lv_scr_act();
        lv_obj_set_style_pad_all(g_root, 0, 0);
        lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clean(g_root);
    lv_obj_set_style_bg_color(g_root, bg(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
}

// =============================================================================
// 6. Screens
// =============================================================================

// ---- TCP hub (Settings > WiFi > TCP hub) -------------------------------------------
// The core's Reticulum TCP client interface (TcpClientSet on the protocol pump, as the
// Plus build uses), one endpoint: NVS rs_tcp (on, host, port), default off and
// rns.ratspeak.org:4242. It runs only while WiFi is connected. Device only: the
// simulator has no TCP stack (FreeRTOS tasks + lwIP sockets), so there it is settings only.
struct TcpCfg { bool on = false; char host[64] = ""; uint16_t port = TCP_DEFAULT_PORT; };
TcpCfg g_tcpCfg;
bool   g_tcpApplied = false;          // endpoint handed to the client set
bool   g_tcpDirty = false;            // settings changed: reload
#if !defined(SIMULATOR)
TcpClientSet g_tcp;
#endif

void tcpLoad() {
    Preferences p;
    strlcpy(g_tcpCfg.host, RATSPEAK_HUB_HOST, sizeof(g_tcpCfg.host));
    if (!p.begin("rs_tcp", true)) return;
    g_tcpCfg.on = p.getUChar("on", 0) != 0;
    String h = p.getString("host", RATSPEAK_HUB_HOST);
    strlcpy(g_tcpCfg.host, h.c_str(), sizeof(g_tcpCfg.host));
    g_tcpCfg.port = p.getUShort("port", TCP_DEFAULT_PORT);
    if (!g_tcpCfg.port) g_tcpCfg.port = TCP_DEFAULT_PORT;
    p.end();
}

void tcpSave() {
    Preferences p;
    if (!p.begin("rs_tcp", false)) return;
    p.putUChar("on", g_tcpCfg.on ? 1 : 0);
    p.putString("host", g_tcpCfg.host);
    p.putUShort("port", g_tcpCfg.port);
    p.end();
    g_tcpDirty = true;
    Serial.printf("[TCP] settings %s %s:%u\n", g_tcpCfg.on ? "on" : "off", g_tcpCfg.host, (unsigned)g_tcpCfg.port);
}

int tcpUp() {
    int up = 0;
#if !defined(SIMULATOR)
    for (auto* c : g_tcp) if (c && c->isConnected()) up++;
#endif
    return up;
}

const char* tcpStatus() {
#if defined(SIMULATOR)
    return g_tcpCfg.on ? "on (not in simulator)" : "off";
#else
    if (!g_tcpCfg.on) return "off";
    if (!g_protoReady) return "protocol off";
    if (!wifiConnected()) return "waiting for WiFi";
    return tcpUp() ? "connected" : "connecting";
#endif
}

void tcpTick() {
#if !defined(SIMULATOR)
    if (!g_protoReady) return;
    const bool want = g_tcpCfg.on && g_tcpCfg.host[0] && wifiConnected();
    if (want && (!g_tcpApplied || g_tcpDirty)) {
        std::vector<TCPEndpoint> eps(1);
        eps[0].host = g_tcpCfg.host;
        eps[0].port = g_tcpCfg.port;
        eps[0].autoConnect = true;
        g_tcpDirty = false;
        if (g_tcp.reload(g_proto.pump(), eps)) {
            g_tcpApplied = true;
            Serial.printf("[TCP] client %s:%u\n", g_tcpCfg.host, (unsigned)g_tcpCfg.port);
        } else {
            Serial.println("[TCP] endpoint rejected");
        }
    } else if (!want && g_tcpApplied) {
        g_tcp.stop();
        g_tcpApplied = false;
        Serial.println("[TCP] stopped");
    }
    g_tcp.maintain();
    if (g_tcpApplied) g_tcp.poll(20);
    static int lastUp = 0;
    const int up = tcpUp();
    if (up != lastUp) {
        lastUp = up;
        Serial.printf("[TCP] %s\n", up ? "connected" : "disconnected");
        if (g_screen == SCR_SETTINGS && g_page == PG_TCP) requestRedraw();
    }
#endif
}

// ---- Identities (Settings > Identity > Identities) ------------------------------------
// The core's IdentityManager slots (as on the Plus build): list, create, switch, erase.
// The protocol loads the active identity at boot, so a switch restarts the device.
// Destructive actions need Enter twice within 5 s.
int      g_idSel = 0;                 // slot shown on PG_ID_ONE
void popPage();
int      g_confirmKey = -1;
uint32_t g_confirmAt = 0;

bool confirmTwice(int key, const char* what) {
    if (g_confirmKey == key && millis() - g_confirmAt < 5000) { g_confirmKey = -1; return true; }
    g_confirmKey = key;
    g_confirmAt = millis();
    toast("Enter again: %s", what);
    return false;
}

void identityRestart(const char* why) {
    Serial.printf("[ID] %s - restarting\n", why);
    saveConfig();
    g_idMgr.flushPending();
    delay(200);
    ESP.restart();
}

int identityIndexOf(const std::string& hash) {
    const auto& v = g_idMgr.identities();
    for (size_t i = 0; i < v.size(); i++) if (v[i].hash == hash) return (int)i;
    return -1;
}

void identityNew() {
    const int k = g_idMgr.createIdentity(String(g_selfName));
    if (k < 0) { toast("Create failed (max 8?)"); return; }
    if (!g_idMgr.switchTo(k)) { toast("Switch failed"); return; }
    identityRestart("new identity");
}

void identitySwitch(int i) {
    if (i == g_idMgr.activeIndex()) { toast("Already active"); return; }
    if (!g_idMgr.switchTo(i)) { toast("Switch failed"); return; }
    identityRestart("identity switched");
}

// Erase a slot's key. The active one is replaced: a new identity becomes active first.
void identityErase(int i) {
    const auto& v = g_idMgr.identities();
    if (i < 0 || i >= (int)v.size()) return;
    const std::string hash = v[i].hash;
    if (i != g_idMgr.activeIndex()) {
        if (!g_idMgr.deleteIdentity(i)) { toast("Erase failed"); return; }
        Serial.printf("[ID] erased %s\n", hash.c_str());
        toast("Identity erased");
        popPage();
        return;
    }
    const int k = g_idMgr.createIdentity(String(g_selfName));
    if (k < 0 || !g_idMgr.switchTo(k)) { toast("New identity failed"); return; }
    const int old = identityIndexOf(hash);
    if (old < 0 || !g_idMgr.deleteIdentity(old)) { toast("Old key not erased"); return; }
    Serial.printf("[ID] erased active %s\n", hash.c_str());
    identityRestart("active identity erased, new one made");
}

// ---- Idle password lock (Settings > Screen > Idle splash) ----------------------------
// The lock password IS the SecretVault password: setting one makes the vault start
// locked at every boot (saved WiFi passwords stay sealed until it is typed), and idle
// mode 2 also locks after the idle timeout. LoRa / LXMF keep running while locked;
// the screen shows only the time, battery and the unread count. Wrong tries:
// 3 free, then 30 s, 60 s ... 16 min between tries (count kept in NVS rs_lock).
bool     g_locked = false;
char     g_lockBuf[33] = "";
size_t   g_lockLen = 0;
uint8_t  g_lockFails = 0;
uint32_t g_lockUntil = 0;           // millis(): no tries before this (0 = none)
constexpr uint8_t  kLockFreeTries = 3;
constexpr uint32_t kLockPromptMs = 30000;   // prompt without keys -> back to the splash
constexpr size_t   kLockPassMin = 4, kLockPassMax = 32;

bool lockHasPassword() { return !vault::usingDefaultPassword(); }
bool lockOnIdle()      { return g_cfg.idleMode == 2 && lockHasPassword(); }

uint32_t lockWaitS() {
    if (!g_lockUntil) return 0;
    const int32_t left = (int32_t)(g_lockUntil - millis());
    if (left <= 0) { g_lockUntil = 0; return 0; }
    return (uint32_t)(left + 999) / 1000;
}

void lockSaveFails() {
    Preferences p;
    if (p.begin("rs_lock", false)) { p.putUChar("fails", g_lockFails); p.end(); }
}

void lockArmDelay() {
    if (g_lockFails < kLockFreeTries) { g_lockUntil = 0; return; }
    int sh = g_lockFails - kLockFreeTries;
    if (sh > 5) sh = 5;
    g_lockUntil = millis() + (30000UL << sh);
    if (!g_lockUntil) g_lockUntil = 1;
}

void lockLoadFails() {
    Preferences p;
    if (p.begin("rs_lock", true)) { g_lockFails = p.getUChar("fails", 0); p.end(); }
    lockArmDelay();                    // a reboot does not reset the wait
}

// Wrong password typed (lock prompt or Settings "current password").
void lockNoteWrong() {
    if (g_lockFails < 250) g_lockFails++;
    lockSaveFails();
    lockArmDelay();
    Serial.printf("[LOCK] wrong password (%u in a row, wait %u s)\n", (unsigned)g_lockFails, (unsigned)lockWaitS());
}

void lockNoteRight() {
    if (g_lockFails) { g_lockFails = 0; lockSaveFails(); }
    g_lockUntil = 0;
}

void lockWipe() { memset(g_lockBuf, 0, sizeof(g_lockBuf)); g_lockLen = 0; g_showPass = false; }

void enterIdle();
void wifiLoad();

void lockNow() {
    if (!lockHasPassword()) return;
    g_locked = true;
    lockWipe();
    Serial.println("[LOCK] locked");
    enterIdle();
}

void lockTry() {
    if (lockWaitS() || !g_lockLen) { requestRedraw(); return; }
    const bool vaultWasLocked = !vault::unlocked();
    const bool ok = vault::unlock(g_lockBuf);   // derives the key, or checks it when unlocked
    lockWipe();
    if (!ok) { lockNoteWrong(); requestRedraw(); return; }
    lockNoteRight();
    g_locked = false;
    if (vaultWasLocked) wifiLoad();            // saved WiFi passwords readable now
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.println("[LOCK] unlocked");
}

void lockKey(const KeyEvent& ev) {
    if (ev.up) {                               // ^ = clear, again = back to the splash
        if (g_lockLen) lockWipe(); else enterIdle();
        requestRedraw();
        return;
    }
    if (ev.enter) { lockTry(); return; }
    if (ev.del) { if (g_lockLen) g_lockBuf[--g_lockLen] = 0; requestRedraw(); return; }
    if (ev.fn) { g_showPass = !g_showPass; requestRedraw(); return; }   // MIC = show / hide
    char c = ev.space ? ' ' : ev.character;
    if (c < 32 || c > 126) return;
    if (ev.alt && isalpha((unsigned char)c)) c = isupper((unsigned char)c) ? (char)tolower(c) : (char)toupper(c);
    if (lockWaitS()) { requestRedraw(); return; }
    if (g_lockLen >= kLockPassMax) return;
    g_lockBuf[g_lockLen++] = c;
    g_lockBuf[g_lockLen] = 0;
    requestRedraw();
}

void renderLock() {
    label(0, 80, W, 22, "RatSpeak", true);
    label(0, 102, W, 18, g_selfName);
    label(0, 128, W, 22, "LOCKED", true);
    const int unread = g_storeReady ? g_msgStore.totalUnreadCount() : 0;
    char line[48];
    if (unread > 0) snprintf(line, sizeof(line), "%d unread message%s", unread, unread == 1 ? "" : "s");
    else            snprintf(line, sizeof(line), "No new messages");
    label(0, 150, W, 18, line, unread > 0);
    label(0, 184, W, 18, "Type password, Enter");
    box(20, 206, W - 40, 30, 2);
    size_t n = g_lockLen < 24 ? g_lockLen : 24;
    for (size_t i = 0; i < n; i++) line[i] = g_showPass ? g_lockBuf[g_lockLen - n + i] : '*';
    line[n] = '_'; line[n + 1] = 0;
    label(20, 206, W - 40, 30, line, true);
    const uint32_t wait = lockWaitS();
    if (wait) {
        snprintf(line, sizeof(line), "Wrong %u times: wait %u s", (unsigned)g_lockFails, (unsigned)wait);
        g_redrawAt = millis() + (wait > 5 ? 5000 : wait * 1000 + 50);   // count down
    } else if (g_lockFails) {
        snprintf(line, sizeof(line), "Wrong password (%u)", (unsigned)g_lockFails);
    } else {
        line[0] = 0;
    }
    label(0, 244, W, 18, line, true);
    label(0, 274, W, 18, g_showPass ? "MIC = hide password" : "MIC = show password");
    label(0, 292, W, 18, "^ clear / back to splash");
}

// ---- Settings pages: single source of truth --------------------------------------
enum RowKind : uint8_t { RK_MENU, RK_TOGGLE, RK_INFO, RK_ACTION, RK_ENTRY, RK_PICK };
const char* kKindName[] = {"Menu", "Toggle", "Info", "Action", "Entry", "Pick"};

struct SetRow { char title[40]; char sub[34]; uint8_t kind; };
constexpr int MAX_SET_ROWS = kTzCount > 12 ? kTzCount : 12;
SetRow g_srows[MAX_SET_ROWS];
int g_srowCount = 0;

void srow(uint8_t kind, const char* name, const char* subFmt, ...) {
    if (g_srowCount >= MAX_SET_ROWS) return;
    SetRow& r = g_srows[g_srowCount];
    snprintf(r.title, sizeof(r.title), "%d : %s : %s", g_srowCount + 1, kKindName[kind], name);
    if (textW(r.title) > W - 16)              // too long for one line: the kind word goes
        snprintf(r.title, sizeof(r.title), "%d : %s", g_srowCount + 1, name);
    va_list ap;
    va_start(ap, subFmt);
    vsnprintf(r.sub, sizeof(r.sub), subFmt ? subFmt : "", ap);
    va_end(ap);
    r.kind = kind;
    g_srowCount++;
}

const char* pageTitle(Page p) {
    switch (p) {
        case PG_ROOT:     return "Settings";
        case PG_TIME:     return "Time & Location";
        case PG_RTC:      return "Set RTC";
        case PG_TZ:       return "Time zone";
        case PG_LOC:      return "Location";
        case PG_WIFI:     return "WiFi";
        case PG_SCREEN:   return "Screen & I/O";
        case PG_IDLE:     return "Idle splash";
        case PG_BATTERY:  return "Battery";
        case PG_BATLOG:   return "Battery log";
        case PG_IDENTITY: return "Identity";
        case PG_LORA:     return "LoRa";
        case PG_ALERTS:   return "Alerts";
        case PG_DIAG:     return "Diagnostics";
        case PG_STORAGE:  return "Storage";
        case PG_SD:       return "SD card";
        case PG_WIFI_SCAN:  return "Networks";
        case PG_WIFI_SAVED: return "Saved WiFi";
        case PG_WIFI_NET:   return "Saved network";
        case PG_WIFI_JOIN:  return "Join WiFi";
        case PG_POWER:        return "Power";
        case PG_POWER_CUSTOM: return "Custom power";
        case PG_ENERGY:       return "Energy monitor";
        case PG_ENERGY_GRAPH: return "Energy trend";
        case PG_CHECKIN:      return "Check-in";
        case PG_TCP:          return "TCP hub";
        case PG_IDS:          return "Identities";
        case PG_ID_ONE:       return "Identity slot";
        case PG_WPTS:         return "Waypoints";
        case PG_TRACK:        return "Track";
        case PG_GEOFS:        return "Arrival alerts";
        case PG_LOWBAT:       return "Low-battery notice";
        case PG_GEOF_NEW:     return "New arrival alert";
        case PG_BACKUPS:      return "Restore backup";
    }
    return "";
}

// Text shown in a row's subtitle while that row is being typed into.
const char* editingSub(Entry kind, int arg = 0) {
    static char s[34];
    if (g_entry != kind || (kind == Entry::RtcField && g_entryArg != arg)) return nullptr;
    constexpr size_t kShow = 27;                 // keep the cursor end visible
    if (passEntry(kind)) {                       // lock password: * unless MIC = show
        size_t n = g_bufLen < kShow ? g_bufLen : kShow;
        s[0] = '>';
        for (size_t i = 0; i < n; i++) s[1 + i] = g_showPass ? g_buf[g_bufLen - n + i] : '*';
        s[1 + n] = '_'; s[2 + n] = 0;
        return s;
    }
    if (g_bufLen > kShow) snprintf(s, sizeof(s), ">..%s_", g_buf + g_bufLen - (kShow - 2));
    else                  snprintf(s, sizeof(s), ">%s_", g_buf);
    return s;
}

void checkinEveryText(char* out, size_t n) {
    if (g_pw.checkinEvery % 60) snprintf(out, n, "%u min", (unsigned)g_pw.checkinEvery);
    else if (g_pw.checkinEvery == 1440) snprintf(out, n, "day");
    else snprintf(out, n, "%u h", (unsigned)(g_pw.checkinEvery / 60));
}

void buildSettingsRows(Page p) {
    g_srowCount = 0;
    char a[34], b[16];
    const char* e;
    switch (p) {
    case PG_ROOT:
        fmtTime(a, sizeof(a));
        srow(RK_MENU, "Time & Location", "%s %s", a, tzCur().name);
        wifiStatusLine(a, sizeof(a));
        srow(RK_MENU, "WiFi", "%s", a);
        srow(RK_MENU, "Screen & I/O", "%s, keys %s", g_cfg.dark ? "Black ink" : "White paper",
             g_cfg.kbLight ? "lit" : "dark");
        srow(RK_MENU, "Power", "%s%s", power::modeName(g_pw.mode), g_pwOnUsb ? " (USB: Full)" : "");
        if (g_bat.valid) srow(RK_MENU, "Battery", "%.2f V  %d%%", g_bat.volts, g_bat.pct);
        else             srow(RK_MENU, "Battery", "gauge n/a");
        srow(RK_MENU, "Identity", "%s", g_selfName);
        srow(RK_MENU, "LoRa", "%.0f MHz %s", LORA_DEFAULT_FREQ / 1e6, loraOnline() ? "online" : "offline");
        srow(RK_MENU, "Alerts", "beep %s  vibe %s", g_cfg.beep ? "on" : "off", g_cfg.vibe ? "on" : "off");
        srow(RK_MENU, "Diagnostics", "paths %u  links %u",
             g_protoReady ? (unsigned)g_proto.pathCount() : 0u,
             g_protoReady ? (unsigned)g_proto.linkCount() : 0u);
        srow(RK_MENU, "Storage", "heap %u KB", (unsigned)(ESP.getFreeHeap() / 1024));
        break;

    case PG_TIME:
        fmtTime(a, sizeof(a)); fmtDate(b, sizeof(b));
        srow(RK_MENU, "Set RTC", "%s %s", a, b);
        srow(RK_MENU, "Time zone", "%s %s", tzCur().name, tzCur().label);
        {
            int inView, heard, best;
            gpsSkySummary(inView, heard, best);
            if (g_cfg.gpsOn && !g_gpsAllowed)
                srow(RK_TOGGLE, "Enable GPS", "ON  resting (%s)", power::gpsName(g_pwEff.gps));
            else if (g_cfg.gpsOn && g_gpsBbrFault)
                srow(RK_TOGGLE, "Enable GPS", "ON  backup cell low");
            else if (g_gpsFix || !g_cfg.gpsOn)
                srow(RK_TOGGLE, "Enable GPS", "%s  %s  %u sats", g_cfg.gpsOn ? "ON" : "OFF", g_gpsStatus, g_gpsSats);
            else
                srow(RK_TOGGLE, "Enable GPS", "ON  %d/%d heard  %ddB", heard, inView, best);
        }
        if (g_cfg.hasPos) srow(RK_MENU, "Manual location", "%.4f, %.4f", g_cfg.lat, g_cfg.lon);
        else              srow(RK_MENU, "Manual location", "not set");
        srow(RK_ACTION, "Sync GPS now", g_gpsFix ? "Enter = sync" : "needs GPS fix");
        srow(RK_TOGGLE, "Auto GPS sync", g_cfg.gpsAutoSync ? "ON (boot + daily)" : "OFF");
        {
            char pos[56];
            gpsPosText(pos, sizeof(pos));
            srow(RK_INFO, "GPS position", "%s", pos);
            if (ashareActive()) srow(RK_ACTION, "Location auto-share", "%d on; Enter = stop all", ashareActive());
            else srow(RK_INFO, "Location auto-share", "off (set in a chat: MIC)");
            srow(RK_MENU, "Waypoints", "%d saved (Alt+P adds)", g_wptCount);
            srow(RK_MENU, "Track (breadcrumbs)", "%s, %d points%s", g_cfg.trackOn ? "ON" : "OFF", g_trackN,
                 g_tbOn ? ", track back" : "");
            srow(RK_PICK, "Units", "%s", g_cfg.imperial ? "ft / mi" : "m / km");
            static const char* const kFmt[3] = {"Decimal 43.64012,-70.92011", "Deg min sec 43 38'24\"N", "MGRS 19T CJ 4513 3368"};
            srow(RK_PICK, "Coordinates", "%s", kFmt[g_cfg.coordFmt % 3]);
        }
        break;

    case PG_TRACK:
        srow(RK_TOGGLE, "Record track", "%s, %d points", g_cfg.trackOn ? "ON" : "OFF", g_trackN);
        if (g_tbOn) srow(RK_ACTION, "Track back", "ON: to point %d; Enter = stop", g_tbIdx + 1);
        else srow(RK_ACTION, "Track back", "guide me back along it");
        srow(RK_ACTION, "Export as GPX", "%s", g_trackGpx[0] ? g_trackGpx : "<SD>/ratdeckpro/tracks/");
        srow(RK_ACTION, "Clear track", "Enter twice");
        srow(RK_INFO, "GPS for the track", "%s", g_pwEff.trackGps ? "wakes it every ~2 min" : "logs when it is on anyway");
        break;

    case PG_WPTS: {
        double la = 0, lo = 0;
        const bool own = ownPosition(la, lo);
        if (!g_wptCount) srow(RK_INFO, "No waypoints yet", "Alt+P on the map adds one");
        for (int i = 0; i < g_wptCount && i < MAX_SET_ROWS; i++) {
            char nm[24], d[24];
            snprintf(nm, sizeof(nm), "W%d %s", i + 1, g_wpt[i].name);
            if (own) distText(la, lo, g_wpt[i].lat, g_wpt[i].lon, d, sizeof(d));
            else snprintf(d, sizeof(d), "%.4f,%.4f", g_wpt[i].lat, g_wpt[i].lon);
            srow(RK_ACTION, nm, "%s  Enter = map", d);
        }
        break;
    }

    case PG_RTC: {
        static const char* names[5] = {"Hour (0-23)", "Minute (0-59)", "Month (1-12)", "Day (1-31)", "Year"};
        for (int i = 0; i < 5; i++) {
            e = editingSub(Entry::RtcField, i);
            if (e) srow(RK_ENTRY, names[i], "%s", e);
            else   srow(RK_ENTRY, names[i], i == 4 ? "%04d" : "%02d", g_rtc[i]);
        }
        srow(RK_ACTION, "Apply to clock", "%02d:%02d %02d/%02d/%04d", g_rtc[0], g_rtc[1], g_rtc[2], g_rtc[3], g_rtc[4]);
        break;
    }

    case PG_TZ:
        for (int i = 0; i < kTzCount; i++)
            srow(RK_PICK, kTz[i].name, "%s%s", kTz[i].label, i == g_cfg.tz ? "  (current)" : "");
        break;

    case PG_LOC:
        e = editingSub(Entry::Latitude);
        if (e) srow(RK_ENTRY, "Latitude", "%s", e);
        else   srow(RK_ENTRY, "Latitude", g_cfg.hasPos ? "%.5f" : "not set", g_cfg.lat);
        e = editingSub(Entry::Longitude);
        if (e) srow(RK_ENTRY, "Longitude", "%s", e);
        else   srow(RK_ENTRY, "Longitude", g_cfg.hasPos ? "%.5f" : "not set", g_cfg.lon);
        srow(RK_ACTION, "Clear position", g_cfg.hasPos ? "Enter = clear" : "-");
        break;

    case PG_WIFI:
        wifiStatusLine(a, sizeof(a));
        srow(RK_INFO, "Status", "%s", a);
        srow(RK_INFO, "IP address", "%s", wifiConnected() ? WiFi.localIP().toString().c_str() : "-");
        srow(RK_MENU, "Scan networks", g_wifiScanning ? "scanning..."
                                       : g_wifiScanned ? "%d found" : "Enter = scan", g_wifiSeenCount);
        srow(RK_MENU, "Saved networks", "%d saved, encrypted%s", g_wifiSavedCount,
             wifiHasAutoCreds() ? ", auto" : "");
        srow(RK_ACTION, "Disconnect", g_wifiSt == WifiSt::Off ? "-" : "Enter = off (pause auto)");
        srow(RK_MENU, "TCP hub", "%s", tcpStatus());
        break;

    case PG_TCP:
        srow(RK_TOGGLE, "TCP hub", "%s", g_tcpCfg.on ? "ON (while WiFi is up)" : "OFF");
        e = editingSub(Entry::TcpHost);
        srow(RK_ENTRY, "Host", "%s", e ? e : g_tcpCfg.host);
        e = editingSub(Entry::TcpPort);
        if (e) srow(RK_ENTRY, "Port", "%s", e); else srow(RK_ENTRY, "Port", "%u", (unsigned)g_tcpCfg.port);
        srow(RK_INFO, "Status", "%s", tcpStatus());
        srow(RK_ACTION, "Use RatSpeak hub", "%s:%u", RATSPEAK_HUB_HOST, (unsigned)TCP_DEFAULT_PORT);
        break;

    case PG_WIFI_SCAN:
        srow(RK_ACTION, "Rescan", g_wifiScanning ? "scanning..." : g_wifiSeenCount ? "Enter = scan again"
                                  : g_wifiScanned ? "none found, Enter = scan" : "Enter = scan");
        for (int i = 0; i < g_wifiSeenCount; i++) {
            const WifiSeen& s = g_wifiSeen[i];
            const bool cur = wifiConnected() && strcmp(s.ssid, g_wifiTarget) == 0;
            srow(RK_PICK, s.ssid, "%d dBm %s%s%s", (int)s.rssi, s.secure ? "locked" : "open",
                 wifiFindSaved(s.ssid) >= 0 ? " saved" : "", cur ? " (on)" : "");
        }
        break;

    case PG_WIFI_SAVED:
        if (!g_wifiSavedCount) srow(RK_INFO, "No saved networks", "use Scan networks");
        for (int i = 0; i < g_wifiSavedCount; i++) {
            const bool cur = wifiConnected() && strcmp(g_wifiSaved[i].ssid, g_wifiTarget) == 0;
            srow(RK_MENU, g_wifiSaved[i].ssid, "auto %s%s", g_wifiSaved[i].autoJoin ? "ON" : "OFF",
                 cur ? "  (connected)" : "");
        }
        break;

    case PG_WIFI_NET: {
        if (g_wifiNetIdx < 0 || g_wifiNetIdx >= g_wifiSavedCount) break;
        const WifiCred& c = g_wifiSaved[g_wifiNetIdx];
        const bool cur = wifiConnected() && strcmp(c.ssid, g_wifiTarget) == 0;
        srow(RK_INFO, "Network", "%s", c.ssid);
        srow(RK_ACTION, "Connect now", cur ? "connected" : "Enter = join");
        srow(RK_TOGGLE, "Auto-connect", "%s", c.autoJoin ? "ON" : "OFF");
        e = editingSub(Entry::WifiPass);
        if (e) srow(RK_ENTRY, "Password", "%s", e);
        else if (c.passBad) srow(RK_ENTRY, "Password", "unreadable, Enter = re-type");
        else   srow(RK_ENTRY, "Password", c.pass[0] ? "%u chars, encrypted" : "none (open)",
                    (unsigned)strlen(c.pass));
        srow(RK_ACTION, "Forget network", "Enter = forget");
        break;
    }

    case PG_WIFI_JOIN:
        srow(RK_INFO, "Network", "%s", g_joinSsid);
        e = editingSub(Entry::WifiPass);
        if (e) srow(RK_ENTRY, "Password", "%s", e);
        else   srow(RK_ENTRY, "Password", g_joinPass[0] ? "%u chars" : "Enter = type",
                    (unsigned)strlen(g_joinPass));
        srow(RK_TOGGLE, "Auto-connect", "%s", g_joinAuto ? "ON" : "OFF");
        srow(RK_ACTION, "Join", "Enter = connect + save");
        break;

    case PG_POWER: {
        // Estimated runtime on a full battery: learned capacity, else the gauge's, else 1450 mAh.
        bool capLearned = false;
        const uint16_t cap = batCapacityMah(&capLearned);
        static const char* kNote[power::MODE_CUSTOM] = {"always awake", "wake 1 s", "wake 60 s",
                                                        "LoRa only", "LoRa on key", ""};
        char rt[16], ci[24], at[12];
        fmtClockMin(g_pw.checkinStart, at, sizeof(at));
        checkinEveryText(ci, sizeof(ci));
        for (uint8_t m = power::MODE_FULL; m < power::MODE_CUSTOM; m++) {
            power::fmtRuntime(rt, sizeof(rt), cap, power::estimateMa(power::preset(m)));
            if (m == power::MODE_CHECKIN)        // a menu: the Check-in page (time, every ...)
                srow(RK_MENU, "Check-in", "%s%s, every %s", g_pw.mode == m ? "ON " : "", at, ci);
            else
                srow(RK_PICK, power::modeName(m), "%s%s, %s", g_pw.mode == m ? "ON " : "", rt, kNote[m]);
        }
        if (g_pw.customSet || g_pw.mode == power::MODE_CUSTOM) {
            power::fmtRuntime(rt, sizeof(rt), cap, power::estimateMa(g_pw.custom));
            srow(RK_MENU, "Custom", "%s%s (your settings)", g_pw.mode == power::MODE_CUSTOM ? "ON " : "", rt);
        } else {
            srow(RK_MENU, "Custom", "set your own");
        }
        srow(RK_MENU, "Check-in sched", "%s, every %s%s", at, ci, g_outboxCount ? ", msgs wait" : "");
        srow(RK_MENU, "Energy monitor", "what uses the battery");
        srow(RK_TOGGLE, "Full power on USB", "%s", g_pw.fullOnUsb ? "ON" : "OFF");
        const uint32_t tot = g_pwAwakeMs + g_pwSleepMs;
        if (g_pwOnUsb)               srow(RK_INFO, "Now", "Full (USB power)");
        else if (!g_pwEff.wakeSecs)  srow(RK_INFO, "Now", "%u MHz, never sleeps", (unsigned)g_pwEff.cpuMhz);
        else srow(RK_INFO, "Now", "%u MHz, asleep %u%%", (unsigned)g_pwEff.cpuMhz,
                  tot ? (unsigned)((uint64_t)g_pwSleepMs * 100 / tot) : 0u);
        if (!g_bat.valid)                          srow(RK_INFO, "Battery draw", "gauge n/a");
        else if (g_bat.usb && !g_benchUntil)       srow(RK_INFO, "Battery draw", "on USB (%+d mA)", (int)g_bat.avgMa);
        else if (g_bat.tteMin && g_bat.tteMin != 0xFFFF)
            srow(RK_INFO, "Battery draw", "%d mA, %u h %02u m left", (int)-g_bat.avgMa,
                 (unsigned)(g_bat.tteMin / 60), (unsigned)(g_bat.tteMin % 60));
        else                                       srow(RK_INFO, "Battery draw", "%d mA", (int)-g_bat.avgMa);
        srow(RK_INFO, "Times use", capLearned ? "%u mAh (learned)" : "%u mAh (nominal)", (unsigned)cap);
        break;
    }

    case PG_CHECKIN: {
        const power::CheckinPhase ph = checkinNow();
        char at[12], ev[16];
        fmtClockMin(g_pw.checkinStart, at, sizeof(at));
        checkinEveryText(ev, sizeof(ev));
        srow(RK_TOGGLE, "Use Check-in mode", "%s", g_pw.mode == power::MODE_CHECKIN ? "ON" : "OFF");
        e = editingSub(Entry::CheckinTime);
        if (e) srow(RK_ENTRY, "Check-in time (hhmm)", "%s", e);
        else   srow(RK_ENTRY, "Check-in time", "%s", at);
        if (g_cfg.clock12) srow(RK_PICK, "am / pm", "%s", g_pw.checkinStart < 720 ? "am" : "pm");
        else               srow(RK_INFO, "am / pm", "- (24 h clock)");
        srow(RK_PICK, "Every", "%s", ev);
        if (ph.inWindow) {
            srow(RK_INFO, "Now", "window open, LoRa on");
        } else {
            const time_t at = time(nullptr) + ph.toNextS;
            struct tm lt;
            applyTz();
            localtime_r(&at, &lt);
            char nx[12];
            fmtClockMin(lt.tm_hour * 60 + lt.tm_min, nx, sizeof(nx));
            srow(RK_INFO, "Next check-in", "%s (in %lu min)", nx, (unsigned long)(ph.toNextS / 60));
        }
        srow(RK_INFO, "Messages waiting", "%d", g_outboxCount);
        if (handheld::ClockConfidence::synchronized()) {
            srow(RK_INFO, "Clock", "GPS / network time");
        } else if (g_clkLastAt) {
            struct tm lt;
            applyTz();
            localtime_r(&g_clkLastAt, &lt);
            srow(RK_INFO, "Clock", "from peer %02d:%02d (%+ld s)", lt.tm_hour, lt.tm_min, (long)g_clkLastOff);
        } else {
            srow(RK_INFO, "Clock", "own (not synced yet)");
        }
        srow(RK_INFO, "How it works", "LoRa 1 min before, 3 after");
        break;
    }

    case PG_ENERGY: {
        const int battPct = g_bat.valid ? g_bat.pct : -1;
        const bool cur = g_energyShowCurrent || energy::count() == 0;
        const energy::Record r = cur ? energy::current(millis(), battPct) : energy::closed(0);
        srow(RK_MENU, "Trend graph", "last %d periods", energy::count() < 24 ? energy::count() : 24);
        const unsigned per = (unsigned)(energy::period() / 60);
        if (cur) srow(RK_ACTION, "View: this period", "%lu of %u min so far", (unsigned long)(r.lenS / 60), per);
        else     srow(RK_ACTION, "View: last period", "%u min, Enter = this one", per);
        float est = 0;
        for (int c = 0; c < energy::CAT_COUNT; c++) est += r.est10[c] / 10.0f;
        const float hours = r.lenS ? r.lenS / 3600.0f : 0;
        const bool measured = r.measuredS * 2 >= r.lenS && r.lenS > 0;
        if (measured)
            srow(RK_INFO, "Measured (gauge)", "%.1f mAh, avg %.1f mA", r.measured10 / 10.0f,
                 r.measuredS ? (r.measured10 / 10.0f) * 3600.0f / r.measuredS : 0.0f);
        else
            srow(RK_INFO, "Measured (gauge)", "n/a: on USB power");
        srow(RK_INFO, "Estimated total", "%.1f mAh, avg %.1f mA", est, hours > 0 ? est / hours : 0.0f);
        // Categories, largest first.
        uint8_t order[energy::CAT_COUNT];
        for (int c = 0; c < energy::CAT_COUNT; c++) order[c] = (uint8_t)c;
        for (int i = 0; i < energy::CAT_COUNT; i++)
            for (int j = i + 1; j < energy::CAT_COUNT; j++)
                if (r.est10[order[j]] > r.est10[order[i]]) { uint8_t t = order[i]; order[i] = order[j]; order[j] = t; }
        for (int i = 0; i < energy::CAT_COUNT; i++) {
            const uint8_t c = order[i];
            if (!r.est10[c]) continue;
            srow(RK_INFO, energy::catName(c), "%.1f mAh  %u%%", r.est10[c] / 10.0f,
                 est > 0 ? (unsigned)(r.est10[c] / 10.0f * 100.0f / est + 0.5f) : 0u);
        }
        if (measured) {
            const float other = r.measured10 / 10.0f - est;
            srow(RK_INFO, "Other (meas - est)", "%+.1f mAh", other);
        }
        srow(RK_INFO, "Awake / battery", "%u%% awake, battery %u%%", (unsigned)r.awakePct, (unsigned)r.battPct);
        break;
    }

    case PG_ENERGY_GRAPH:
        break;                                          // drawn by renderEnergyGraph()

    case PG_POWER_CUSTOM: {
        const power::Profile& c = g_pw.custom;
        srow(RK_PICK, "CPU speed", "%u MHz", (unsigned)c.cpuMhz);
        power::fmtWake(a, sizeof(a), c.wakeSecs);
        srow(RK_PICK, "Sleep, wake every", "%s", a);
        srow(RK_PICK, "Awake after key", c.wakeSecs ? "%u s" : "%u s (no sleep)", (unsigned)c.awakeSecs);
        srow(RK_PICK, "LoRa radio", "%s", c.lora ? "ON (messages)" : c.loraOnUse ? "2 min after a key" : "OFF (no messages)");
        srow(RK_PICK, "GPS", "%s%s", power::gpsName(c.gps), g_cfg.gpsOn ? "" : " (GPS disabled)");
        srow(RK_TOGGLE, "WiFi", "%s", c.wifi ? "allowed" : "OFF");
        srow(RK_TOGGLE, "Key backlight", "%s", c.keyLight ? "allowed" : "OFF");
        srow(RK_TOGGLE, "Touch", "%s", c.touchOnKey ? "1 min after a key" : "always on");
        srow(RK_TOGGLE, "Map wakes GPS", "%s", c.mapGps ? "ON (fix when the map opens)" : "OFF (last fix)");
        srow(RK_TOGGLE, "Track wakes GPS", "%s", c.trackGps ? "ON (every ~2 min)" : "OFF (when GPS is on)");
        srow(RK_ACTION, "Reset to Balanced", "Enter = reset");
        break;
    }

    case PG_SCREEN:
        srow(RK_TOGGLE, "Theme", "%s", g_cfg.dark ? "Black ink" : "White paper");
        srow(RK_MENU, "Idle splash", g_cfg.idleMode ? "On, %u s" : "Off", (unsigned)g_cfg.idleSecs);
        srow(RK_TOGGLE, "Key backlight", "%s", g_cfg.kbLight ? "ON" : "OFF");
        break;

    case PG_IDLE: {
        const bool pw = lockHasPassword();
        srow(RK_PICK, "Off", "%s", g_cfg.idleMode == 0 ? "(current)" : "");
        srow(RK_PICK, "On, no password", "%s", g_cfg.idleMode == 1 ? "(current)" : "");
        srow(RK_PICK, "On + password", "%s", lockOnIdle() ? "(current)" : pw ? "" : "asks for a password");
        e = editingSub(Entry::IdleSecs);
        if (e) srow(RK_ENTRY, "Timeout (10-99999 s)", "%s", e);
        else   srow(RK_ENTRY, "Timeout (10-99999 s)", "%u s", (unsigned)g_cfg.idleSecs);
        if (passEntry(g_entry)) {
            const char* t = g_entry == Entry::PassOld ? "Current password"
                          : g_entry == Entry::PassNew ? "New pw (4-32)" : "Repeat new pw";
            srow(RK_ENTRY, t, "%s", editingSub(g_entry));
        } else {
            srow(RK_ENTRY, pw ? "Change password" : "Set password", "%s",
                 pw ? "set; asked at every boot" : "none (vault default key)");
        }
        srow(RK_ACTION, "Remove password", "%s", pw ? "Enter = remove" : "-");
        srow(RK_ACTION, "Lock now", "%s", pw ? "Enter = lock" : "set a password first");
        srow(RK_PICK, "Idle screen", "%s", kIdleScreenName[g_cfg.idleScreen]);
        e = editingSub(Entry::FoundText);
        if (e) srow(RK_ENTRY, "If-found text ( ; = new line)", "%s", e);
        else   srow(RK_ENTRY, "If-found text", "%s", g_cfg.foundText[0] ? g_cfg.foundText : "none: Enter to type");
        srow(RK_TOGGLE, "If found: my address", "%s", g_cfg.foundAddr ? "shown" : "hidden");
        break;
    }

    case PG_BATTERY:
        if (g_bat.valid) srow(RK_INFO, "Voltage", "%.3f V", g_bat.volts);
        else             srow(RK_INFO, "Voltage", "n/a");
        if (g_bat.valid) srow(RK_INFO, "Charge", "%d%%  (gauge %d%%)", g_bat.pct, g_bat.rawSoc);
        else             srow(RK_INFO, "Charge", "n/a");
        srow(RK_INFO, "Avg current", "%+d mA", (int)g_bat.avgMa);
        srow(RK_INFO, "Status", "%s", g_bat.full ? "full (charge done)"
                                    : g_bat.charging ? "charging"
                                    : g_bat.usb ? "USB, not charging" : "discharging");
        srow(RK_INFO, "USB power", "%s%s", g_bat.usb ? "yes" : "no",
             g_bat.chrgStat == 0xFF ? " (charger n/a)" : "");
        srow(RK_INFO, "Gauge flags", "0x%04X", (unsigned)g_bat.flags);
        srow(RK_INFO, "Capacity", "%u / %u mAh", (unsigned)g_bat.remMah, (unsigned)g_bat.fccMah);
        srow(RK_MENU, "Battery log", "%s%s", g_batLogInternal ? "INT " : "", g_batLogSd ? "SD" : (g_batLogInternal ? "" : "off"));
        srow(RK_INFO, "Full-charge cal", g_fullSoc < 100 ? "gauge %u%% = 100%%" : "none yet (charge to full)",
             (unsigned)g_fullSoc);
        srow(RK_ACTION, "Reset calibration", g_fullSoc < 100 ? "Enter = reset" : "-");
        {
            char t[48];
            batLeftText(t, sizeof(t));
            srow(RK_INFO, "Time left (measured)", "%s", t);
        }
        srow(RK_MENU, "Low-battery notice", "%s", g_cfg.lowBatPct ? "on" : "off");
        if (g_capMah) srow(RK_INFO, "Learned capacity", "%u mAh, %u%% of new", (unsigned)g_capMah,
                           (unsigned)(g_capMah * 100u / kNominalMah));
        else srow(RK_INFO, "Learned capacity", "%s", g_capAnchor >= 0 ? "counting (needs 40% drop)" : "after a 40% drop");
        break;

    case PG_LOWBAT: {
        if (g_cfg.lowBatPct) srow(RK_PICK, "Send at", "%u %%", (unsigned)g_cfg.lowBatPct);
        else srow(RK_PICK, "Send at", "off");
        char nm[24];
        const bool to = memcmp(g_cfg.lowBatTo, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0;
        if (to) peerName(g_cfg.lowBatTo, nm, sizeof(nm)); else snprintf(nm, sizeof(nm), "nobody yet");
        srow(RK_ACTION, "Send to", "%s; Enter = pick", nm);
        srow(RK_INFO, "This discharge", "%s", g_cfg.lowBatSent ? "sent (again after charging)" : "not sent");
        srow(RK_INFO, "Message", "battery %%, last position");
        break;
    }

    case PG_BATLOG:
        srow(RK_TOGGLE, "Internal log", "%s (%u samples)", g_batLogInternal ? "ON" : "OFF", (unsigned)g_batLogCount);
        srow(RK_TOGGLE, "SD card log", "%s", g_batLogSd ? "ON " : "OFF");
        srow(RK_ACTION, "Dump to USB serial", "Enter = dump");
        srow(RK_ACTION, "Clear internal", g_batLogCount ? "Enter = clear" : "empty");
        srow(RK_MENU, "SD card", "%s", g_sdStatus);
        break;

    case PG_IDENTITY:
        srow(RK_INFO, "Destination", "%.16s", g_destHex[0] ? g_destHex : "unknown");
        srow(RK_ENTRY, "Display name", "%s", g_selfName);
        srow(RK_INFO, "LXMF address", "..%.16s", g_destHex[0] ? g_destHex + 16 : "");
        srow(RK_ACTION, "Announce ID", "last: %s", g_announceStr);
        srow(RK_MENU, "Identities", "%d saved: new/switch/erase", g_idMgr.count());
        srow(RK_ACTION, "Run setup wizard", "name, clock, energy, idle ...");
        srow(RK_ACTION, "Share all contacts", "every contact, as cards");
        srow(RK_ACTION, "Export contacts (SD)", "ratdeckpro/contacts.txt");
        srow(RK_ACTION, "Import contacts (SD)", "ratdeckpro/contacts.txt");
        break;

    case PG_IDS: {
        const auto& v = g_idMgr.identities();
        for (size_t i = 0; i < v.size() && g_srowCount < MAX_SET_ROWS - 1; i++) {
            const char* nm = v[i].displayName.length() ? v[i].displayName.c_str() : "(no name)";
            srow(RK_MENU, nm, "%.16s%s", v[i].hash.c_str(), v[i].active ? " (active)" : "");
        }
        srow(RK_ACTION, "New identity", "make + switch + restart");
        break;
    }

    case PG_ID_ONE: {
        const auto& v = g_idMgr.identities();
        if (g_idSel < 0 || g_idSel >= (int)v.size()) { srow(RK_INFO, "Gone", "^ = back"); break; }
        const bool act = v[g_idSel].active;
        srow(RK_INFO, "Hash", "%.16s", v[g_idSel].hash.c_str());
        srow(RK_INFO, "", "%s", v[g_idSel].hash.size() > 16 ? v[g_idSel].hash.c_str() + 16 : "");
        srow(RK_ACTION, "Switch to it", "%s", act ? "(active)" : "Enter x2 = switch + restart");
        srow(RK_ACTION, "Erase it", "%s", act ? "Enter x2 = new ID + restart" : "Enter x2 = delete key");
        break;
    }

    case PG_LORA:
        srow(RK_INFO, "Radio", "%s", loraOnline() ? "online" : "offline");
        srow(RK_INFO, "Frequency", "%.3f MHz", LORA_DEFAULT_FREQ / 1e6);
        srow(RK_PICK, "Link preset", "%s%s", kLoraPresets[g_loraPreset].name,
             g_loraPreset ? " (peers need same)" : "");
        srow(RK_INFO, "Last RSSI", "%d dBm", loraRssi());
        srow(RK_INFO, "Last SNR", "%.1f dB", loraSnr());
        srow(RK_ACTION, "Range test", g_rtOn ? "running; Enter = show" : "ping a radio, log to SD");
        break;

    case PG_ALERTS:
        srow(RK_TOGGLE, "Beep on message", "%s", g_cfg.beep ? "ON" : "OFF");
        srow(RK_TOGGLE, "Vibrate on message", "%s", g_cfg.vibe ? "ON" : "OFF");
        srow(RK_PICK, "Volume", "%u%%%s", (unsigned)g_cfg.volume,
             g_cfg.volume == 0 ? " (silent)" : g_cfg.volume == 80 ? " (default)" : g_cfg.volume == 100 ? " (max)" : "");
        srow(RK_ACTION, "Test alert", "Enter = play");
        srow(RK_MENU, "Arrival alerts", "%d set (contact near a place)", g_geofCount);
        break;

    case PG_GEOFS:
        for (int i = 0; i < g_geofCount; i++) {
            char t[40], sub[34];
            geofText(g_geof[i], t, sizeof(t));
            snprintf(sub, sizeof(sub), "%s; Enter twice = remove", g_geof[i].repeat ? "every time" : "once");
            srow(RK_ACTION, t, "%s", sub);
        }
        srow(RK_ACTION, "New alert", g_geofCount < kGeofMax ? "contact, place, distance" : "list full (8)");
        break;

    case PG_GEOF_NEW: {
        char nm[24], at[24], d[16];
        if (g_gnContact >= 0 && g_gnContact < g_gnContactN) peerName(g_gnContacts[g_gnContact], nm, sizeof(nm));
        else snprintf(nm, sizeof(nm), "(no saved contacts)");
        srow(RK_PICK, "Contact", "%s", nm);
        if (g_gnPlace <= 0 || g_gnPlace > g_wptCount) snprintf(at, sizeof(at), "me (where I am)");
        else snprintf(at, sizeof(at), "W%d %s", g_gnPlace, g_wpt[g_gnPlace - 1].name);
        srow(RK_PICK, "Near", "%s", at);
        fmtDist(kGeofSteps[g_gnDist], d, sizeof(d));
        srow(RK_PICK, "Within", "%s", d);
        srow(RK_TOGGLE, "Repeat", "%s", g_gnRepeat ? "every time they come back" : "once, then removed");
        srow(RK_ACTION, "Save alert", "Enter = save");
        break;
    }

    case PG_DIAG: {
        uint32_t s = millis() / 1000;
        srow(RK_INFO, "Paths / links", "%u / %u", g_protoReady ? (unsigned)g_proto.pathCount() : 0u,
             g_protoReady ? (unsigned)g_proto.linkCount() : 0u);
        srow(RK_INFO, "LXMF queue", "%d", g_protoReady ? g_proto.lxmfQueuedCount() : 0);
        srow(RK_INFO, "Peers heard", "%d", g_announceMgr.nodeCount());
        srow(RK_INFO, "Protocol", "%s", g_protoReady ? "running" : "off");
        srow(RK_INFO, "Uptime", "%lud %02lu:%02lu", (unsigned long)(s / 86400),
             (unsigned long)((s / 3600) % 24), (unsigned long)((s / 60) % 60));
        break;
    }

    case PG_STORAGE:
        srow(RK_INFO, "Free heap", "%u KB (int %u KB)", (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        srow(RK_INFO, "Free PSRAM", "%u KB", (unsigned)(ESP.getFreePsram() / 1024));
        srow(RK_INFO, "Flash", "%s", g_storeReady ? "LittleFS ok" : "unavailable");
        srow(RK_INFO, "Messages", "%u conv, %d unread",
             g_storeReady ? (unsigned)g_msgStore.totalConversations() : 0u,
             g_storeReady ? g_msgStore.totalUnreadCount() : 0);
        srow(RK_MENU, "SD card", "%s", g_sdStatus);
        srow(RK_INFO, "Secrets key", "%s (AES-256)", vault::status());
        break;

    case PG_SD:
        sdStatusRefresh();
        srow(RK_INFO, "Status", "%s", g_sdStatus);
        srow(RK_ACTION, "Mount", g_sdMounted ? "mounted" : "Enter = mount");
        srow(RK_ACTION, "Unmount", g_sdMounted ? "Enter = eject" : "-");
        srow(RK_ACTION, "Reset log file", g_sdMounted ? "Enter = reset" : "mount first");
        srow(RK_INFO, "Log file", "%s", kBatLogPath);
        srow(RK_ACTION, "Message archive", "%s%s", sdArchiveEnabled() ? "On" : "Off",
             (sdArchiveEnabled() != g_sdArchiveActive) ? " (after reboot)" : "");
        srow(RK_ACTION, "USB drive", "%s", g_usbDrive ? "on the PC" : "card on PC");
        sdToolsScan(false);
        if (!g_sdMounted) srow(RK_ACTION, "Back up now", "copy all to card");
        else if (g_bkCount) srow(RK_ACTION, "Back up now", "last %s", g_bkNames[0]);
        else srow(RK_ACTION, "Back up now", "none on card yet");
        if (!g_sdMounted) srow(RK_MENU, "Restore backup", "from the card");
        else srow(RK_MENU, "Restore backup", g_bkCount ? "%d on card" : "none on card", g_bkCount);
        if (!g_sdMounted) srow(RK_ACTION, "Update firmware", "from /firmware");
        else if (!g_fwName[0]) srow(RK_ACTION, "Update firmware", "no .bin in /firmware");
        else srow(RK_ACTION, "Update firmware", g_fwInstalled ? "%.13s (installed)" : "%.17s %u KB",
                  g_fwName, (unsigned)(g_fwSize >> 10));
        break;

    case PG_BACKUPS:
        sdToolsScan(false);
        if (!g_bkCount) srow(RK_INFO, "No backups on card", "-");
        for (int i = 0; i < g_bkCount; i++) {
            char nm[24];
            backupPretty(g_bkNames[i], nm, sizeof(nm));
            srow(RK_ACTION, nm, "%s", i ? "Enter twice = restore" : "newest; Enter twice");
        }
        break;
    }
}

int settingsRowCount() { buildSettingsRows(g_page); return g_srowCount; }

void clampScroll(int sel, int& scroll, int count) {
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + VIS_ROWS) scroll = sel - VIS_ROWS + 1;
    int maxScroll = count > VIS_ROWS ? count - VIS_ROWS : 0;
    if (scroll > maxScroll) scroll = maxScroll;
    if (scroll < 0) scroll = 0;
}

void pushPage(Page p, int sel = 0) {
    if (g_navDepth >= NAV_DEPTH) return;
    g_navStack[g_navDepth++] = {g_page, g_sel, g_scroll};
    g_page = p;
    g_sel = sel;
    g_scroll = 0;
    clampScroll(g_sel, g_scroll, settingsRowCount());
    requestRedraw();
}

void popPage() {
    if (g_navDepth == 0) return;
    const NavFrame& f = g_navStack[--g_navDepth];
    g_page = f.page;
    g_sel = f.sel;
    g_scroll = f.scroll;
    clampScroll(g_sel, g_scroll, settingsRowCount());
    requestRedraw();
}

void settingsPath(char* out, size_t n) {
    size_t len = snprintf(out, n, "S");
    for (int i = 0; i < g_navDepth && len < n; i++)
        len += snprintf(out + len, n - len, ".%d", g_navStack[i].sel + 1);
    if (entryActive() && len < n) snprintf(out + len, n - len, ".%d", g_sel + 1);
}

void loadRtcStaging() {
    struct tm t;
    if (!localNow(t)) return;
    g_rtc[0] = t.tm_hour; g_rtc[1] = t.tm_min;
    g_rtc[2] = t.tm_mon + 1; g_rtc[3] = t.tm_mday; g_rtc[4] = t.tm_year + 1900;
}

void startNameEntry(const uint8_t peer[16]);

void goScreen(ScreenId s);

void activateSetting(int idx) {
    switch (g_page) {
    case PG_ROOT: {
        static const Page kSub[10] = {PG_TIME, PG_WIFI, PG_SCREEN, PG_POWER, PG_BATTERY, PG_IDENTITY,
                                      PG_LORA, PG_ALERTS, PG_DIAG, PG_STORAGE};
        if (idx >= 0 && idx < 10) pushPage(kSub[idx], idx == 3 ? g_pw.mode : 0);
        break;
    }
    case PG_TIME:
        if (idx == 0) { loadRtcStaging(); pushPage(PG_RTC); }
        else if (idx == 1) pushPage(PG_TZ, g_cfg.tz);
        else if (idx == 2) { g_cfg.gpsOn = !g_cfg.gpsOn; saveConfig(); gpsApplyPower(); requestRedraw(); }
        else if (idx == 3) pushPage(PG_LOC);
        else if (idx == 4) { if (gpsSyncClock()) toast("Clock synced from GPS"); else toast("No GPS fix yet"); }
        else if (idx == 5) { g_cfg.gpsAutoSync = !g_cfg.gpsAutoSync; saveConfig(); requestRedraw(); }
        else if (idx == 7 && ashareActive()) { ashareStopAll(); toast("All auto-sharing stopped"); }
        else if (idx == 8) pushPage(PG_WPTS);
        else if (idx == 9) { trackLoad(); pushPage(PG_TRACK); }
        else if (idx == 10) { g_cfg.imperial = !g_cfg.imperial; saveConfig(); requestRedraw(); }
        else if (idx == 11) {
            g_cfg.coordFmt = (uint8_t)((g_cfg.coordFmt + 1) % 3);
            saveConfig();
            Serial.printf("[CFG] coordinates %s\n", g_cfg.coordFmt == 2 ? "MGRS" : g_cfg.coordFmt == 1 ? "DMS" : "decimal");
            requestRedraw();
        }
        break;

    case PG_TRACK:
        if (idx == 0) {
            g_cfg.trackOn = !g_cfg.trackOn;
            saveConfig();
            if (g_cfg.trackOn) trackLoad();
            toast(g_cfg.trackOn ? "Track on" : "Track off (kept)");
            requestRedraw();
        } else if (idx == 1) {
            if (g_tbOn) tbStop("stopped"); else if (tbStart()) openMap();
        } else if (idx == 2) {
            const int n = trackExportGpx();
            if (n >= 0) toast("%d points -> GPX", n);
            requestRedraw();
        } else if (idx == 3) {
            if (confirmTwice(1010, "clear the track")) { trackClear(); toast("Track cleared"); }
        }
        break;

    case PG_WPTS:
        if (idx >= 0 && idx < g_wptCount) {
            wptOpenFromSettings(idx);
        }
        break;

    case PG_RTC:
        if (idx >= 0 && idx < 5) startEntry(Entry::RtcField, nullptr, idx);
        else if (idx == 5) {
            if (setClockLocal(g_rtc[4], g_rtc[2], g_rtc[3], g_rtc[0], g_rtc[1])) {
                saveConfig();
                popPage();
                toast("Clock set");
            } else {
                toast("Invalid date/time");
            }
        }
        break;

    case PG_TZ:
        if (idx >= 0 && idx < kTzCount) {
            g_cfg.tz = idx;
            applyTz();
            saveConfig();
            popPage();
            toast("Zone %s", kTz[idx].name);
        }
        break;

    case PG_LOC: {
        char init[16] = "";
        if (idx == 0) { if (g_cfg.hasPos) snprintf(init, sizeof(init), "%.5f", g_cfg.lat); startEntry(Entry::Latitude, init); }
        else if (idx == 1) { if (g_cfg.hasPos) snprintf(init, sizeof(init), "%.5f", g_cfg.lon); startEntry(Entry::Longitude, init); }
        else if (idx == 2 && g_cfg.hasPos) { g_cfg.hasPos = false; g_cfg.lat = g_cfg.lon = 0; saveConfig(); toast("Position cleared"); }
        break;
    }

    case PG_WIFI:
        if (idx == 2) { if (!g_wifiScanned || !g_wifiSeenCount) wifiStartScan(false); pushPage(PG_WIFI_SCAN); }
        else if (idx == 3) pushPage(PG_WIFI_SAVED);
        else if (idx == 4 && g_wifiSt != WifiSt::Off) { wifiDisconnect(); toast("WiFi off"); }
        else if (idx == 5) pushPage(PG_TCP);
        break;

    case PG_TCP:
        if (idx == 0) {
            g_tcpCfg.on = !g_tcpCfg.on;
            tcpSave();
            if (g_tcpCfg.on && !wifiConnected()) toast("Starts when WiFi is up");
            requestRedraw();
        } else if (idx == 1) {
            startEntry(Entry::TcpHost, g_tcpCfg.host);
        } else if (idx == 2) {
            char pb[8]; snprintf(pb, sizeof(pb), "%u", (unsigned)g_tcpCfg.port); startEntry(Entry::TcpPort, pb);
        } else if (idx == 4) {
            strlcpy(g_tcpCfg.host, RATSPEAK_HUB_HOST, sizeof(g_tcpCfg.host));
            g_tcpCfg.port = TCP_DEFAULT_PORT;
            tcpSave();
            toast("RatSpeak hub set");
        }
        break;

    case PG_WIFI_SCAN: {
        if (idx == 0) { wifiStartScan(false); break; }
        if (idx < 1 || idx > g_wifiSeenCount) break;
        const WifiSeen& s = g_wifiSeen[idx - 1];
        const int k = wifiFindSaved(s.ssid);
        if (wifiConnected() && strcmp(s.ssid, g_wifiTarget) == 0) { toast("Already connected"); break; }
        if (k >= 0 && !g_wifiSaved[k].passBad) { // known: join with the stored password
            wifiConnect(g_wifiSaved[k].ssid, g_wifiSaved[k].pass, false, g_wifiSaved[k].autoJoin);
            popPage();
            toast("Joining %.16s", s.ssid);
            break;
        }
        strlcpy(g_joinSsid, s.ssid, sizeof(g_joinSsid));
        g_joinPass[0] = 0;
        g_joinSecure = s.secure;
        g_joinAuto = k >= 0 ? g_wifiSaved[k].autoJoin : true;   // default: auto-connect
        if (s.secure) { pushPage(PG_WIFI_JOIN, 1); startEntry(Entry::WifiPass); }
        else          pushPage(PG_WIFI_JOIN, 3);
        break;
    }

    case PG_WIFI_SAVED:
        if (idx >= 0 && idx < g_wifiSavedCount) { g_wifiNetIdx = idx; pushPage(PG_WIFI_NET); }
        break;

    case PG_WIFI_NET: {
        if (g_wifiNetIdx < 0 || g_wifiNetIdx >= g_wifiSavedCount) { popPage(); break; }
        WifiCred& c = g_wifiSaved[g_wifiNetIdx];
        if (idx == 1) {
            if (wifiConnected() && strcmp(c.ssid, g_wifiTarget) == 0) { toast("Already connected"); break; }
            if (c.passBad) { startEntry(Entry::WifiPass); toast("Re-type the password"); break; }
            wifiConnect(c.ssid, c.pass, false, c.autoJoin);
            toast("Joining %.16s", c.ssid);
        } else if (idx == 2) {
            c.autoJoin = !c.autoJoin;
            if (strcmp(c.ssid, g_wifiTarget) == 0) g_wifiTargetAuto = c.autoJoin;
            wifiSaveAll();
            if (c.autoJoin) g_wifiNextAuto = millis();
            requestRedraw();
        } else if (idx == 3) {
            startEntry(Entry::WifiPass);
        } else if (idx == 4) {
            char name[33];
            strlcpy(name, c.ssid, sizeof(name));
            if (strcmp(name, g_wifiTarget) == 0 && g_wifiSt != WifiSt::Off) {
                wifiRadioOff();
                g_wifiSt = WifiSt::Off;
                strlcpy(g_wifiMsg, "off", sizeof(g_wifiMsg));
            }
            wifiForget(g_wifiNetIdx);
            popPage();
            toast("Forgot %.18s", name);
        }
        break;
    }

    case PG_WIFI_JOIN:
        if (idx == 1) startEntry(Entry::WifiPass, g_joinPass);
        else if (idx == 2) { g_joinAuto = !g_joinAuto; requestRedraw(); }
        else if (idx == 3) {
            if (g_joinSecure && strlen(g_joinPass) < 8) { toast("Password: 8+ chars"); break; }
            wifiConnect(g_joinSsid, g_joinPass, true, g_joinAuto);
            popPage();                           // Join -> Networks
            popPage();                           // Networks -> WiFi (shows status)
            toast("Joining %.16s", g_joinSsid);
        }
        break;

    case PG_POWER:
        if (idx == power::MODE_CHECKIN) {
            pushPage(PG_CHECKIN);
        } else if (idx >= power::MODE_FULL && idx < power::MODE_CUSTOM) {
            g_pw.mode = (uint8_t)idx;
            power::save(g_pw);
            powerApply(true);
            toast("Power: %s", power::modeName(g_pw.mode));
        } else if (idx == power::MODE_CUSTOM) {
            if (!g_pw.customSet) {                 // start from the mode in use
                g_pw.custom = power::selected(g_pw);
                g_pw.customSet = true;
            }
            g_pw.mode = power::MODE_CUSTOM;
            power::save(g_pw);
            powerApply(true);
            pushPage(PG_POWER_CUSTOM);
        } else if (idx == power::MODE_CUSTOM + 1) {
            pushPage(PG_CHECKIN);
        } else if (idx == power::MODE_CUSTOM + 2) {
            g_energyShowCurrent = false;
            pushPage(PG_ENERGY);
        } else if (idx == power::MODE_CUSTOM + 3) {
            g_pw.fullOnUsb = !g_pw.fullOnUsb;
            power::save(g_pw);
            powerApply(true);
            requestRedraw();
        }
        break;

    case PG_CHECKIN:
        if (idx == 0) {                          // off: back to Balanced
            g_pw.mode = g_pw.mode == power::MODE_CHECKIN ? power::MODE_BALANCED : power::MODE_CHECKIN;
            toast("Power: %s", power::modeName(g_pw.mode));
        } else if (idx == 1) {
            char cur[8];
            snprintf(cur, sizeof(cur), "%02u%02u", (unsigned)(g_cfg.clock12 && g_pw.checkinStart / 60 % 12 == 0 ? 12
                                                               : g_cfg.clock12 ? g_pw.checkinStart / 60 % 12
                                                                               : g_pw.checkinStart / 60),
                     (unsigned)(g_pw.checkinStart % 60));
            startEntry(Entry::CheckinTime, cur);
            g_entryReplace = true;
            break;
        } else if (idx == 2 && g_cfg.clock12) {
            g_pw.checkinStart = (uint16_t)((g_pw.checkinStart + 720) % 1440);
        } else if (idx == 3) {
            g_pw.checkinEvery = power::nextCheckinEvery(g_pw.checkinEvery);
        } else break;
        power::save(g_pw);
        powerApply(true);
        requestRedraw();
        break;

    case PG_ENERGY:
        if (idx == 0) pushPage(PG_ENERGY_GRAPH);
        else if (idx == 1) { g_energyShowCurrent = !g_energyShowCurrent; requestRedraw(); }
        break;

    case PG_POWER_CUSTOM: {
        power::Profile& c = g_pw.custom;
        switch (idx) {
            case 0: c.cpuMhz = power::nextCpu(c.cpuMhz); break;
            case 1: c.wakeSecs = power::nextWake(c.wakeSecs); break;
            case 2: c.awakeSecs = power::nextAwake(c.awakeSecs); break;
            case 3:                                    // on -> on use -> off
                if (c.lora) { c.lora = false; c.loraOnUse = true; }
                else if (c.loraOnUse) c.loraOnUse = false;
                else c.lora = true;
                break;
            case 4: c.gps = (uint8_t)((c.gps + 1) % power::GPS_COUNT); break;
            case 5: c.wifi = !c.wifi; break;
            case 6: c.keyLight = !c.keyLight; break;
            case 7: c.touchOnKey = !c.touchOnKey; break;
            case 8: c.mapGps = !c.mapGps; break;
            case 9: c.trackGps = !c.trackGps; break;
            case 10: c = power::preset(power::MODE_BALANCED); toast("Custom = Balanced"); break;
            default: return;
        }
        g_pw.mode = power::MODE_CUSTOM;
        g_pw.customSet = true;
        power::save(g_pw);
        powerApply(true);
        requestRedraw();
        break;
    }

    case PG_SCREEN:
        if (idx == 0) {
            g_cfg.dark = !g_cfg.dark;
            saveConfig();
            g_fullRefreshReq = true;    // whole panel flips polarity
            requestRedraw();
        } else if (idx == 1) {
            pushPage(PG_IDLE, g_cfg.idleMode);
        } else if (idx == 2) {
            g_cfg.kbLight = !g_cfg.kbLight;
            applyKeyLight();
            if (g_cfg.kbLight && !g_pwEff.keyLight) toast("Key light off in %s mode", power::modeName(g_pw.mode));
            saveConfig();
            requestRedraw();
        }
        break;

    case PG_IDLE:
        if (idx == 0 || idx == 1) {
            g_cfg.idleMode = (uint8_t)idx;
            saveConfig();
            if (lockHasPassword()) toast("Boot still asks password");
            requestRedraw();
        } else if (idx == 2) {
            if (lockHasPassword()) { g_cfg.idleMode = 2; saveConfig(); toast("Locks after %u s idle", (unsigned)g_cfg.idleSecs); }
            else {                               // no password yet: new + repeat, then lock on
                g_sel = 4;                       // the password row shows the typing
                startEntry(Entry::PassNew, nullptr, 2);
                toast("Choose a password");
            }
        } else if (idx == 3) {
            char s[8]; snprintf(s, sizeof(s), "%u", (unsigned)g_cfg.idleSecs); startEntry(Entry::IdleSecs, s);
        } else if (idx == 4) {
            startEntry(lockHasPassword() ? Entry::PassOld : Entry::PassNew, nullptr, 0);
        } else if (idx == 5) {
            if (lockHasPassword()) { g_sel = 4; startEntry(Entry::PassOld, nullptr, 1); }
            else toast("No password set");
        } else if (idx == 6) {
            if (lockHasPassword()) lockNow(); else toast("Set a password first");
        } else if (idx == 7) {
            g_cfg.idleScreen = (uint8_t)((g_cfg.idleScreen + 1) % IDLE_COUNT);
            saveConfig();
            Serial.printf("[IDLE] screen %s\n", kIdleScreenName[g_cfg.idleScreen]);
            if (g_cfg.idleScreen == IDLE_FOUND && !g_cfg.foundText[0]) toast("Type your text: row 9");
            requestRedraw();
        } else if (idx == 8) {
            startEntry(Entry::FoundText, g_cfg.foundText);
            g_entryReplace = true;            // typing starts over, Backspace edits the old text
        } else if (idx == 9) {
            g_cfg.foundAddr = !g_cfg.foundAddr;
            saveConfig();
            requestRedraw();
        }
        break;

    case PG_LOWBAT:
        if (idx == 0) {
            static const uint8_t kSteps[5] = {0, 5, 10, 15, 20};
            int k = 0;
            while (k < 5 && kSteps[k] != g_cfg.lowBatPct) k++;
            g_cfg.lowBatPct = kSteps[(k + 1) % 5];
            saveConfig();
            requestRedraw();
        } else if (idx == 1) {
            goScreen(SCR_CONTACTS);
            g_shareActive = true;
            g_pickLowbat = true;
            toast("Pick who gets it");
        }
        break;

    case PG_BATTERY:
        if (idx == 11) pushPage(PG_LOWBAT);
        else if (idx == 7) pushPage(PG_BATLOG);
        else if (idx == 9 && g_fullSoc < 100) { saveBatteryCal(100); readBattery(); toast("Calibration reset"); }
        break;

    case PG_BATLOG:
        if (idx == 0) {
            g_batLogInternal = !g_batLogInternal;
            if (g_batLogInternal) batLogSample();
            requestRedraw();
        } else if (idx == 1) {
            if (!g_batLogSd && !sdMount()) { toast("SD mount failed"); break; }
            g_batLogSd = !g_batLogSd;
            if (g_batLogSd) batLogSample();
            requestRedraw();
        } else if (idx == 2) {
            batLogDump();
            toast("Dumped %u samples", (unsigned)g_batLogCount);
        } else if (idx == 3) {
            g_batLogCount = g_batLogHead = 0;
            toast("Internal log cleared");
        } else if (idx == 4) {
            pushPage(PG_SD);
        }
        break;

    case PG_IDENTITY:
        if (idx == 1 && g_haveSelf) startNameEntry(g_selfHash);
        else if (idx == 1) toast("Identity not loaded");
        else if (idx == 3) { doAnnounce(); if (g_cfg.beep) g_audio.playAnnounce(); toast("Announce: %s", g_announceStr); }
        else if (idx == 4) pushPage(PG_IDS, g_idMgr.activeIndex() > 0 ? g_idMgr.activeIndex() : 0);
        else if (idx == 5) wizStart();
        else if (idx == 6) shareAllStart();
        else if (idx == 7) { const int n = contactsExport(); if (n >= 0) toast("%d contacts -> contacts.txt", n); }
        else if (idx == 8) { const int n = contactsImport(); if (n >= 0) toast("%d contacts imported", n); }
        break;

    case PG_IDS:
        if (idx >= 0 && idx < g_idMgr.count()) { g_idSel = idx; pushPage(PG_ID_ONE, 2); }
        else if (idx == g_idMgr.count() && confirmTwice(100, "new ID + restart")) identityNew();
        break;

    case PG_ID_ONE:
        if (idx == 2 && confirmTwice(200 + g_idSel, "switch + restart")) identitySwitch(g_idSel);
        else if (idx == 3 && confirmTwice(300 + g_idSel, g_idSel == g_idMgr.activeIndex() ? "erase active ID" : "erase key"))
            identityErase(g_idSel);
        break;

    case PG_LORA:
        if (idx == 2) { applyLoraPreset(g_loraPreset + 1); requestRedraw(); }
        else if (idx == 5) {
            if (g_rtOn) { g_rtOpen = true; g_fullRefreshReq = true; requestRedraw(); break; }
            goScreen(SCR_CONTACTS);
            g_shareActive = true;
            g_pickRt = true;
            toast("Pick the other radio");
        }
        break;

    case PG_ALERTS:
        if (idx == 0) { g_cfg.beep = !g_cfg.beep; g_audio.setEnabled(g_cfg.beep); saveConfig(); requestRedraw(); }
        else if (idx == 1) { g_cfg.vibe = !g_cfg.vibe; saveConfig(); requestRedraw(); }
        else if (idx == 2) {
            g_cfg.volume = g_cfg.volume >= 100 ? 0 : (uint8_t)((g_cfg.volume / 20 + 1) * 20);   // 0..100 by 20
            g_audio.setVolume(g_cfg.volume);
            saveConfig();
            if (g_cfg.volume && g_cfg.beep) g_audio.playMessage();   // hear the new level
            requestRedraw();
        } else if (idx == 3) {
            if (g_cfg.beep) g_audio.playMessage();
            if (g_cfg.vibe) vibrate(60);
            toast("Test alert");
        } else if (idx == 4) pushPage(PG_GEOFS);
        break;

    case PG_GEOFS:
        if (idx >= 0 && idx < g_geofCount) {
            if (confirmTwice(2000 + idx, "remove this alert")) { geofRemove(idx); toast("Alert removed"); }
        } else if (idx == g_geofCount && g_geofCount < kGeofMax) {
            geofNewStart(nullptr);
        }
        break;

    case PG_GEOF_NEW:
        if (idx == 0 && g_gnContactN) g_gnContact = (g_gnContact + 1) % g_gnContactN;
        else if (idx == 1) g_gnPlace = (g_gnPlace + 1) % (g_wptCount + 1);
        else if (idx == 2) g_gnDist = (g_gnDist + 1) % kGeofStepN;
        else if (idx == 3) g_gnRepeat = !g_gnRepeat;
        else if (idx == 4) {
            if (!g_gnContactN) { toast("Save a contact first"); break; }
            if (geofAdd(g_gnContacts[g_gnContact], g_gnPlace, kGeofSteps[g_gnDist], g_gnRepeat)) { toast("Alert saved"); popPage(); }
            else toast("List full (8)");
        }
        requestRedraw();
        break;

    case PG_STORAGE:
        if (idx == 4) pushPage(PG_SD);
        break;

    case PG_SD:
        if (idx == 1) { if (sdMount()) toast("SD mounted"); else toast("SD mount failed"); }
        else if (idx == 2) { sdUnmount(); toast("SD ejected"); }
        else if (idx == 3) {
            if (!g_sdMounted) { toast("Mount SD first"); break; }
            g_sdStore.remove(kBatLogPath);
            File f = g_sdStore.openFile(kBatLogPath, FILE_WRITE);
            if (f) { f.println(kBatLogHeader); f.close(); }
            toast("Log file reset");
        } else if (idx == 6) {
            usbDriveStart();
        } else if (idx == 7) {
            backupNow();
        } else if (idx == 8) {
            backupsOpen();
        } else if (idx == 9) {
            fwUpdateKey();
        } else if (idx == 5) {
            sdArchiveSet(!sdArchiveEnabled());
            toast(sdArchiveEnabled() ? "Archive on: reboot to apply" : "Archive off: reboot to apply");
        }
        break;

    case PG_BACKUPS:
        backupPick(idx);
        break;

    default:
        break;
    }
}

// ---- List screens (Inbox / Contacts / Peers) ------------------------------------
// Lists are shown in pages that follow the selection: Contacts 10 rows (digits 0-9),
// Inbox / Peers 9 rows (digits 1-9). Digits pick within the selected row's page;
// N / B jump a page, G / V move across page edges.
int pageSize(ScreenId s) { return s == SCR_CONTACTS ? 10 : 9; }
int rowNumber(ScreenId s, int idx) { return s == SCR_CONTACTS ? idx % 10 : idx % 9 + 1; }

void rememberSelection() {
    if (g_listSel >= 0 && g_listSel < g_rowCount) {
        memcpy(g_rememberHash[g_screen], g_rows[g_listSel].peer, 16);
        g_rememberValid[g_screen] = true;
    }
}

void loadRows(ScreenId s) {
    g_rowCount = 0;
    if (s == SCR_INBOX) {
        appendConversations(MAX_ROWS);
        appendAnnounced(false, false);     // so a new peer can be messaged
    } else if (s == SCR_CONTACTS) {
        if (g_haveSelf) {
            if (PeerRow* r = addRow(g_selfHash)) {
                snprintf(r->name, sizeof(r->name), "MyID");
                snprintf(r->sub, sizeof(r->sub), "%.23s", g_selfName);
            }
        }
        appendAnnounced(true, false);      // saved / named contacts
    } else if (s == SCR_PEERS) {
        appendAnnounced(false, true);
    }

    // Restore selection by hash, else clamp.
    int sel = -1;
    if (g_rememberValid[s]) sel = findRow(g_rememberHash[s]);
    if (sel < 0) sel = g_listSel;
    g_listSel = g_rowCount ? constrain(sel, 0, g_rowCount - 1) : 0;
    clampScroll(g_listSel, g_listScroll, g_rowCount);
}

void drawFooter() {
    const char* third = "";
    if (g_toast[0]) third = g_toast;
    if (!browseMode()) {
        const char* hint = "Enter=set  ^=cancel";
        if (g_entry == Entry::Latitude || g_entry == Entry::Longitude) hint = "SPC=.  SYM+U=-";
        else if (g_entry == Entry::WifiPass) hint = "Alt+key=case  Enter=ok";
        else if (g_entry == Entry::Search) hint = "Enter=keep  ^=clear";
        else if (passEntry(g_entry)) hint = g_showPass ? "MIC=hide  Alt=case" : "MIC=show  Alt=case";
        box(0, FOOT_Y, W, FOOT_H, 1);
        label(0, FOOT_Y - 1, W, FOOT_H, third[0] ? third : hint);
        return;
    }
    int count = g_screen == SCR_SETTINGS ? g_srowCount : g_rowCount;
    int scroll = g_screen == SCR_SETTINGS ? g_scroll : g_listScroll;
    bool moreDn = scroll + VIS_ROWS < count;
    if (g_toast[0]) {
        box(0, FOOT_Y, W, FOOT_H, 1);
        label(0, FOOT_Y - 1, W, FOOT_H, g_toast);
        return;
    }
    box(0, FOOT_Y, BOT_C_W, FOOT_H, 1);
    label(0, FOOT_Y - 1, BOT_C_W, FOOT_H, "V=Down", false, 'V');
    addHit(0, FOOT_Y, BOT_C_W, FOOT_H, H_DOWN);
    box(BOT_C_W, FOOT_Y, BOT_A_W, FOOT_H, 1);
    label(BOT_C_W, FOOT_Y - 1, BOT_A_W, FOOT_H, "G=up", false, 'G');
    addHit(BOT_C_W, FOOT_Y, BOT_A_W, FOOT_H, H_UP);
    box(BOT_C_W + BOT_A_W, FOOT_Y, BOT_I_W, FOOT_H, 1);
    if (moreDn) {
        label(BOT_C_W + BOT_A_W, FOOT_Y - 1, BOT_I_W, FOOT_H, "v more");
        addHit(BOT_C_W + BOT_A_W, FOOT_Y, BOT_I_W, FOOT_H, H_DOWN);
    }
}

void drawHeader(const char* left, bool moreUp) {
    text(4, HEADER_Y, left, true, moreUp ? MENU_X - 8 : W - 8);
    if (moreUp) {
        // Hangs off MENU: same left/right edges, bottom edge = top line of
        // the first list row, so it reads like the footer's "v more" cell.
        const int h = LIST_TOP - (TOP_H - 1) + 1;
        box(MENU_X, TOP_H - 1, TAB_W, h, 1);
        label(MENU_X, TOP_H - 2, TAB_W, h, "^ more");
        addHit(MENU_X, TOP_H - 1, TAB_W, h, H_UP);
    }
}

void renderList() {
    char hdr[48];
    const int ps = pageSize(g_screen), pages = (g_rowCount + ps - 1) / ps;
    char pg[12] = "";
    if (pages > 1) snprintf(pg, sizeof(pg), " p%d/%d", g_listSel / ps + 1, pages);
    // The window never mixes pages: it scrolls inside the selected row's page.
    const int pStart = (g_listSel / ps) * ps, pEnd = std::min(pStart + ps, g_rowCount);
    if (g_listScroll < pStart) g_listScroll = pStart;
    if (g_listSel >= g_listScroll + VIS_ROWS) g_listScroll = g_listSel - VIS_ROWS + 1;
    if (g_listScroll > std::max(pStart, pEnd - VIS_ROWS)) g_listScroll = std::max(pStart, pEnd - VIS_ROWS);
    if (g_entry == Entry::Search) {
        snprintf(hdr, sizeof(hdr), "Find: %s_ (%d)", g_buf, g_rowCount);
    } else if (g_filter[0] && !g_shareActive) {
        snprintf(hdr, sizeof(hdr), "\"%.10s\" %d%s", g_filter, g_rowCount, pg);
    } else if (g_screen == SCR_INBOX) {
        int unread = g_storeReady ? g_msgStore.totalUnreadCount() : 0;
        if (pages > 1) snprintf(hdr, sizeof(hdr), "Inbox%s new %d", pg, unread > 99 ? 99 : unread);
        else           snprintf(hdr, sizeof(hdr), "Inbox  unread %02d", unread > 99 ? 99 : unread);
    } else if (g_shareActive && g_pickRt) {
        snprintf(hdr, sizeof(hdr), "Range test with:");
    } else if (g_shareActive && g_pickLowbat) {
        snprintf(hdr, sizeof(hdr), "Low battery notice to:");
    } else if (g_shareActive && g_shareAll) {
        snprintf(hdr, sizeof(hdr), "All contacts to:");
    } else if (g_shareActive) {
        char nm[24];
        peerName(g_sharePeer, nm, sizeof(nm));
        snprintf(hdr, sizeof(hdr), "Send %.12s to:", nm);
    } else {
        snprintf(hdr, sizeof(hdr), pages > 1 ? "%s %d%s" : "%s (%d)%s", screenName(g_screen), g_rowCount, pg);
    }
    drawHeader(hdr, g_listScroll > 0);

    if (g_rowCount == 0) {
        const char* empty = g_filter[0] || g_entry == Entry::Search ? "No match"
                          : g_screen == SCR_CONTACTS ? "No contacts - see Peers"
                          : g_screen == SCR_PEERS    ? "No peers - Announce on Home"
                                                     : "No chats yet";
        text(8, LIST_TOP + 8, empty);
    }
    for (int i = g_listScroll; i < pEnd && i < g_listScroll + VIS_ROWS; i++) {
        const PeerRow& r = g_rows[i];
        const int y = LIST_TOP + (i - g_listScroll) * ROW_H;
        char title[36];
        snprintf(title, sizeof(title), "%d : %s%s", rowNumber(g_screen, i), r.name,
                 (g_screen == SCR_PEERS && r.saved) ? " *" : "");
        char right[12] = "";
        if (g_screen == SCR_INBOX && r.unread)
            snprintf(right, sizeof(right), "unrd %02u", r.unread > 99 ? 99u : (unsigned)r.unread);
        listRow(y, title, r.sub, i == g_listSel, right);
        addHit(2, y, W - 4, ROW_H, H_ROW, i);
    }
    drawFooter();
}

// ---- Thread (entry mode: typing goes to compose) ---------------------------------
void drawBubble(int y, const char* s, bool incoming, const char* status) {
    int tw = textW(s) + 16;
    if (tw > W - 28) tw = W - 28;
    if (tw < 40) tw = 40;
    const int x = incoming ? (W - 4 - tw) : 4;   // sent left, received right
    lv_obj_t* b = box(x, y, tw, 24, 2);
    lv_obj_set_style_radius(b, 4, 0);
    text(x + 6, y + 4, s, false, tw - 10);
    if (status && status[0]) text(x + 2, y + 25, status);
}

extern bool g_tmenuOpen;
void renderThreadMenu(int y);
uint8_t ashareMinsFor(const uint8_t dest[16]);
int ashareActive();

void renderThread() {
    char name[24], hdr[48];
    peerName(g_threadPeer, name, sizeof(name));
    const uint8_t am = ashareMinsFor(g_threadPeer);
    if (g_tmenuOpen && g_toast[0]) snprintf(hdr, sizeof(hdr), "%s", g_toast);   // menu: no strip
    else if (g_tmenuOpen) snprintf(hdr, sizeof(hdr), "1-5 / Enter, MIC or ^ = back");
    else if (am) snprintf(hdr, sizeof(hdr), ">>> %.14s [loc %um]", name, (unsigned)am);
    else snprintf(hdr, sizeof(hdr), ">>> %s", name);
    text(4, HEADER_Y, hdr, true, W - 8);
    int y = HEADER_Y + 20;
    if (g_tmenuOpen) {                       // MIC: location menu instead of the history
        renderThreadMenu(y);                 // 5 rows down to y + 220 (= 277)
        return;
    }
    if (g_txStr[0]) {
        char st[32];
        snprintf(st, sizeof(st), "last send: %s", g_txStr);
        text(4, y, st);
        y += 18;
    }
    int yMax = STRIP_Y - 2;
    // Check-in outbox messages for this contact are the newest: they sit under the
    // history (as sent bubbles, "queued HH:MM") and win the space first.
    constexpr int kQueuedH = 42;
    int queued[kOutboxCap], nQueued = 0;
    for (int i = 0; i < g_outboxCount; i++)
        if (!memcmp(g_outbox[i].dest, g_threadPeer, 16)) queued[nQueued++] = i;
    int qFirst = 0;
    while (nQueued - qFirst > 0 && y + (nQueued - qFirst) * kQueuedH > yMax) qFirst++;
    yMax -= (nQueued - qFirst) * kQueuedH;
    if (g_histCount == 0 && !nQueued) {
        text(4, y, g_histNote[0] ? g_histNote : "(no messages yet)");
    } else if (g_histCount) {
        // Newest messages win the space; older ones drop off the top.
        auto hOf = [](int i) { return (!g_hist[i].incoming && g_hist[i].status[0]) ? 42 : 28; };
        int start = g_histCount, used = 0;
        while (start > 0 && y + used + hOf(start - 1) <= yMax) { used += hOf(start - 1); start--; }
        for (int i = start; i < g_histCount; i++) {
            drawBubble(y, g_hist[i].text, g_hist[i].incoming, g_hist[i].incoming ? nullptr : g_hist[i].status);
            y += hOf(i);
        }
    }
    if (nQueued - qFirst > 0) {
        char st[24];
        const power::CheckinPhase ph = checkinNow();
        const time_t at = time(nullptr) + ph.toNextS;
        struct tm lt;
        applyTz();
        localtime_r(&at, &lt);
        if (g_pwEff.checkin) snprintf(st, sizeof(st), "queued %02d:%02d", lt.tm_hour, lt.tm_min);
        else snprintf(st, sizeof(st), "queued");
        for (int k = qFirst; k < nQueued; k++) {
            drawBubble(y, g_outbox[queued[k]].text, false, st);
            y += kQueuedH;
        }
    }

    // 3-line compose strip: show the tail so the cursor is always visible.
    box(2, STRIP_Y, W - 4, BOT_TOP - STRIP_Y, 2);
    constexpr size_t LINE = 26;
    const char* tail = g_buf;
    if (g_bufLen > LINE * 2) tail = g_buf + (g_bufLen - LINE * 2);
    char l1[LINE + 2] = "", l2[LINE + 2] = "";
    size_t tl = strlen(tail);
    snprintf(l1, sizeof(l1), "%.*s", (int)(tl < LINE ? tl : LINE), tail);
    if (tl > LINE) snprintf(l2, sizeof(l2), "%.*s", (int)(tl - LINE), tail + LINE);
    // Cursor on the last line in use
    if (tl < LINE) strncat(l1, "_", sizeof(l1) - strlen(l1) - 1);
    else           strncat(l2, "_", sizeof(l2) - strlen(l2) - 1);
    text(6, STRIP_Y + 1, l1, false, W - 12);
    text(6, STRIP_Y + STRIP_LINE, l2, false, W - 12);
    char hint[40] = "[Enter] send  [MIC] location";
    const bool offer = !g_bufLen && (locOffered() || cardOffered());
    if (!g_bufLen && locOffered()) snprintf(hint, sizeof(hint), "[Enter] map %.3f,%.3f", g_threadLoc.lat, g_threadLoc.lon);
    else if (!g_bufLen && cardOffered()) snprintf(hint, sizeof(hint), "[Enter] save card: %.14s", g_threadCard.name);
    text(6, STRIP_Y + STRIP_LINE * 2, g_toast[0] ? g_toast : hint, !g_toast[0] && offer, W - 12);
}

// ---- Rename (entry mode) ---------------------------------------------------------
void renderRename() {
    const bool self = isSelf(g_nameTarget);
    char hex[33], cur[24], line[40];
    hashToHex(g_nameTarget, hex);
    peerName(g_nameTarget, cur, sizeof(cur));
    int y = HEADER_Y + 4;
    text(6, y, self ? "Name THIS radio" : "Name contact", true); y += 26;
    snprintf(line, sizeof(line), "Now : %s", cur);
    text(6, y, line, false, W - 12); y += 22;
    snprintf(line, sizeof(line), "ID  : %.16s", hex);
    text(6, y, line); y += 30;
    box(2, y - 4, W - 4, 30, 2);
    snprintf(line, sizeof(line), "%.22s_", g_buf);
    text(8, y + 2, line, true, W - 16); y += 40;
    text(6, y, "Enter = save   ^ = cancel"); y += 20;
    text(6, y, "Alt = change case");
    if (g_toast[0]) text(6, y + 24, g_toast, true);
}

// ---- Home -------------------------------------------------------------------------
void renderHome() {
    constexpr int ID_H = 56, TILE_GAP = 2, TILE_W = (W - TILE_GAP * 2) / 3, TILE_H = 42, ANN_H = 34;
    const int y0 = TOP_H - 1;
    const int divX = REF_W + BAT_W;

    // Identity card; open at the top under the nav battery so % joins it.
    box(0, y0, W, ID_H, 2, false,
        (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM));
    fillRect(divX, y0, W - divX, 2);
    fillRect(divX - 1, y0, 1, ID_H);
    char pct[8];
    if (g_bat.valid) snprintf(pct, sizeof(pct), "%d%%", g_bat.pct); else snprintf(pct, sizeof(pct), "--%%");
    label(0, y0 + 2, divX, 16, pct);
    label(0, y0 + 18, divX, 16, g_bat.charging ? "Charg" : (g_bat.usb ? "Plug" : "Batt"));
    label(0, y0 + 34, divX, 16, loraOnline() ? "Online" : "Offline");
    text(divX + 4, y0 + 6, "LXMF");
    char l1[20], l2[20];
    snprintf(l1, sizeof(l1), "%.16s", g_destHex[0] ? g_destHex : "--");
    snprintf(l2, sizeof(l2), "%.16s", g_destHex[0] ? g_destHex + 16 : "");
    text(divX + 4, y0 + 22, l1);
    text(divX + 4, y0 + 38, l2);

    // 3x2 status tiles
    const int gy = TOP_H + ID_H;
    char loraV[14], pathV[10], linkV[10], peerV[14], qV[8];
    if (loraOnline()) { int r = loraRssi(); if (r) snprintf(loraV, sizeof(loraV), "%d dBm", r); else snprintf(loraV, sizeof(loraV), "ON"); }
    else snprintf(loraV, sizeof(loraV), "OFF");
    snprintf(pathV, sizeof(pathV), "P%u", g_protoReady ? (unsigned)g_proto.pathCount() : 0u);
    snprintf(linkV, sizeof(linkV), "L%u", g_protoReady ? (unsigned)g_proto.linkCount() : 0u);
    snprintf(peerV, sizeof(peerV), "%d/%u", g_announceMgr.nodeCount(),
             g_storeReady ? (unsigned)g_msgStore.totalConversations() : 0u);
    snprintf(qV, sizeof(qV), "%d", g_protoReady ? g_proto.lxmfQueuedCount() : 0);
    struct { const char* t; const char* v; } tiles[6] = {
        {"LORA", loraV}, {"PATH", pathV}, {"LINK", linkV},
        {"PEERS", peerV}, {"AUDIO", g_cfg.beep ? "ON" : "OFF"}, {"LXMFQ", qV},
    };
    for (int i = 0; i < 6; i++) {
        const int tx = (i % 3) * (TILE_W + TILE_GAP), ty = gy + (i / 3) * (TILE_H + TILE_GAP);
        box(tx, ty, TILE_W, TILE_H, 2);
        label(tx, ty + 1, TILE_W, 16, tiles[i].t, true);
        label(tx, ty + TILE_H / 2, TILE_W, 18, tiles[i].v);
    }
    addHit(TILE_W + TILE_GAP, gy + TILE_H + TILE_GAP, TILE_W, TILE_H, H_TILE_AUDIO);

    const int sy = gy + 2 * (TILE_H + TILE_GAP) + 2;
    text(4, sy, "Listening for peers");
    char ann[28];
    snprintf(ann, sizeof(ann), "ID chirp: %s", g_announceStr);
    text(4, sy + 16, ann);
    const int ay = sy + 34;
    box(0, ay, W, ANN_H, 2);
    label(0, ay, W, ANN_H, "Announce ID", true, browseMode() ? 'N' : 0);
    addHit(0, ay, W, ANN_H, H_ANNOUNCE);
    int ny = ay + ANN_H + 4;
    if (ny > BOT_TOP - 18) ny = BOT_TOP - 18;
    label(0, ny, W, 16, g_selfName, true);
}

// ---- Settings ---------------------------------------------------------------------
void renderEnergyGraph();

void renderSettings() {
    if (g_page == PG_ENERGY_GRAPH) { renderEnergyGraph(); return; }
    buildSettingsRows(g_page);
    clampScroll(g_sel, g_scroll, g_srowCount);
    char path[24], hdr[48];
    settingsPath(path, sizeof(path));
    snprintf(hdr, sizeof(hdr), "%s %s", path, pageTitle(g_page));
    drawHeader(hdr, g_scroll > 0);
    for (int i = g_scroll; i < g_srowCount && i < g_scroll + VIS_ROWS; i++) {
        const int y = LIST_TOP + (i - g_scroll) * ROW_H;
        listRow(y, g_srows[i].title, g_srows[i].sub, i == g_sel);
        if (browseMode()) addHit(2, y, W - 4, ROW_H, H_ROW, i);
    }
    drawFooter();
}

void smallText(int x, int y, const char* s, bool bold);

// ---- Energy trend graph ------------------------------------------------------------
// Time labels under the bars, rotated (read bottom to top) in a 5x7 font stamped after the
// LVGL frame: the clock time at the start of the bar ("7:30", 12 h: "7:30a"), "-4.5h" when
// the clock is not set, "now" under the newest.
const char kTinyChars[] = "0123456789.:-hapnow";
const uint8_t kTiny5x7[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},   // . :
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11},   // - h
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},   // a p
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}, {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},   // n o
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}};                                              // w
struct GraphLabel { int16_t x, y; char t[8]; };
GraphLabel g_graphLabels[25];
int g_graphLabelN = 0;

// Rotated 90 degrees counter-clockwise: the text starts at (x, yBottom) and runs upwards;
// a character takes 6 px up and 7 px across.
void stampTinyUp(int x, int yBottom, const char* t) {
    const bool ink = !g_cfg.dark;
    for (int k = 0; t[k]; k++) {
        const char* f = strchr(kTinyChars, t[k]);
        if (!f) continue;
        const uint8_t* g = kTiny5x7[f - kTinyChars];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c)) {
                    const int px = x + r, py = yBottom - (k * 6 + c);
                    if (px >= 0 && py >= 0 && px < W && py < H) g_display.setPixel(px, py, ink);
                }
    }
}

void stampGraphLabels() {
    for (int i = 0; i < g_graphLabelN; i++) stampTinyUp(g_graphLabels[i].x, g_graphLabels[i].y, g_graphLabels[i].t);
    if (g_graphLabelN) g_display.markDirty();
}

// Average battery current per period (newest right). Filled bar = gauge measurement;
// outlined bar = estimate only (period mostly on USB). The rightmost bar is the period in
// progress. Below: the largest consumers over the shown periods.
void renderEnergyGraph() {
    g_srowCount = 0;
    char path[24], hdr[48];
    settingsPath(path, sizeof(path));
    snprintf(hdr, sizeof(hdr), "%s %s", path, pageTitle(g_page));
    drawHeader(hdr, false);

    constexpr int kBars = 24;
    float ma[kBars + 1];
    bool meas[kBars + 1];
    uint32_t startEpoch[kBars + 1];         // wall clock at the start of each bar (0 = unknown)
    const time_t nowT = time(nullptr);
    int n = energy::count() < kBars ? energy::count() : kBars;
    float sumCat[energy::CAT_COUNT] = {0};
    float maxMa = 10.0f;
    for (int i = 0; i <= n; i++) {          // i == n: the current period
        const energy::Record r = i < n ? energy::closed(n - 1 - i) : energy::current(millis(), -1);
        float est = 0;
        for (int c = 0; c < energy::CAT_COUNT; c++) { est += r.est10[c] / 10.0f; sumCat[c] += r.est10[c] / 10.0f; }
        meas[i] = r.measuredS * 2 >= r.lenS && r.measuredS > 0;
        if (i == n) startEpoch[i] = nowT > 1600000000 ? (uint32_t)(nowT - r.lenS) : 0;
        else startEpoch[i] = r.epoch > 1600000000 ? r.epoch - r.lenS : 0;
        const float hrs = (meas[i] ? r.measuredS : r.lenS) / 3600.0f;
        ma[i] = hrs > 0 ? (meas[i] ? r.measured10 / 10.0f : est) / hrs : 0;
        if (ma[i] > maxMa) maxMa = ma[i];
    }
    // Left axis: mA, ticks at 0, half and the top (the largest bar rounded up to 1/2/2.5/5 x 10^n).
    {
        float step = 1.0f;
        while (step * 10.0f < maxMa) step *= 10.0f;
        const float nice[4] = {1.0f, 2.0f, 2.5f, 5.0f};
        float top = step * 10.0f;
        for (float f : nice) if (step * f >= maxMa) { top = step * f; break; }
        maxMa = top;
    }
    const int gx = 30, gy = LIST_TOP + 18, gw = W - gx - 6, gh = 92;   // axis left, time labels below
    char t[40];
    smallText(2, LIST_TOP + 2, "mA", true);
    snprintf(t, sizeof(t), "%lu min/bar", (unsigned long)(energy::period() / 60));
    text(W - 6 - textW(t), LIST_TOP, t);
    box(gx, gy, gw, gh + 2, 1);
    for (int k = 0; k <= 2; k++) {                  // 0, max/2, max
        const float v = maxMa * k / 2.0f;
        const int ty = gy + 1 + gh - (int)(gh * k / 2.0f + 0.5f);
        fillRect(gx - 4, ty, 4, 1);
        if (k == 1) for (int x = gx + 2; x < gx + gw - 2; x += 4) fillRect(x, ty, 1, 1);   // dotted half line
        snprintf(t, sizeof(t), v >= 10 || v == 0 ? "%.0f" : "%.1f", v);
        const int w = lv_txt_get_width(t, strlen(t), &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE);
        smallText(gx - 6 - w, ty - 7, t, false);
    }
    const int slots = n + 1;
    const int bw = (gw - 4) / (slots < 8 ? 8 : slots);
    for (int i = 0; i < slots; i++) {
        int h = (int)(ma[i] / maxMa * gh + 0.5f);
        if (ma[i] > 0 && h < 1) h = 1;
        const int x = gx + 2 + i * bw, y = gy + 1 + gh - h;
        if (h <= 0) continue;
        if (meas[i]) fillRect(x, y, bw - 2 > 1 ? bw - 2 : 1, h);
        else         box(x, y, bw - 2 > 2 ? bw - 2 : 2, h, 1);
    }
    // A tick under every bar, a time label under every k-th one (k so labels are >= 9 px apart),
    // always under the newest ("now").
    g_graphLabelN = 0;
    const int every = bw >= 9 ? 1 : (9 + bw - 1) / bw;
    const int labelTop = gy + gh + 7;
    for (int i = 0; i < slots; i++) {
        const int cx = gx + 2 + i * bw + (bw - 2) / 2;
        fillRect(cx, gy + gh + 2, 1, 3);
        if (i != n && (n - i) % every) continue;
        if (i == n - 1 && every > 1) continue;           // keep "now" clear of its neighbour
        GraphLabel& L = g_graphLabels[g_graphLabelN++];
        if (i == n) snprintf(L.t, sizeof(L.t), "now");
        else if (startEpoch[i]) {
            const time_t st = (time_t)startEpoch[i];
            struct tm lt;
            applyTz();
            localtime_r(&st, &lt);
            if (g_cfg.clock12) snprintf(L.t, sizeof(L.t), "%d:%02d%c", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min,
                                        lt.tm_hour < 12 ? 'a' : 'p');
            else snprintf(L.t, sizeof(L.t), "%d:%02d", lt.tm_hour, lt.tm_min);
        } else {
            const uint32_t backS = (uint32_t)(n - i) * energy::period();
            if (backS % 3600 == 0) snprintf(L.t, sizeof(L.t), "-%luh", (unsigned long)(backS / 3600));
            else snprintf(L.t, sizeof(L.t), "-%.1fh", backS / 3600.0f);
        }
        L.x = (int16_t)(cx - 3);
        L.y = (int16_t)(labelTop + (int)strlen(L.t) * 6);
    }

    // Top consumers over the shown periods.
    float total = 0;
    for (int c = 0; c < energy::CAT_COUNT; c++) total += sumCat[c];
    int y = gy + gh + 50;
    for (int k = 0; k < 2 && total > 0; k++) {
        char line[48] = "";
        size_t len = 0;
        for (int j = 0; j < 2; j++) {
            int best = -1;
            for (int c = 0; c < energy::CAT_COUNT; c++)
                if (sumCat[c] > 0 && (best < 0 || sumCat[c] > sumCat[best])) best = c;
            if (best < 0) break;
            len += snprintf(line + len, sizeof(line) - len, "%s%s %u%%", j ? ", " : "", energy::catName(best),
                            (unsigned)(sumCat[best] * 100.0f / total + 0.5f));
            sumCat[best] = -1;
        }
        if (len) text(gx, y, line, false, W - 12);
        y += 18;
    }
    drawFooter();
}

// ---- SD tools: back up / restore, firmware update (S.10.5 rows 8-10) ------------------------
// Backup: /backups/<YYYYMMDD-HHMM>/ on the card holds a copy of every file in the unit's
// flash (flash/...), the settings, keys and message index from NVS (nvs.txt: one
// "namespace<TAB>key<TAB>type<TAB>hex" per line) and info.txt, written last: a backup
// without "complete 1" is never restored. The backup holds the identity's private key in
// the clear: whoever has the card can be this unit.
// Restore runs at the next boot, before anything opens the flash (restoreAtBoot): flash
// files and our NVS namespaces are wiped, the backup is copied back, the unit restarts.
// Settings tied to this unit's chip (Wi-Fi passwords sealed with the chip MAC, the password
// lock, battery calibration) go back only onto the same unit.
// Firmware update: the newest *.bin in /firmware is written to the other app slot
// (hal/SysImage). The new firmware confirms itself 10 s after "[BOOT] ready"; if it crashes
// or is reset before that, the bootloader starts the old one again.
constexpr const char* kBackupRoot = "/backups";
constexpr const char* kFwDir = "/firmware";
constexpr const char* kFwMarker = "RatSpeak T-Deck Pro - standalone";   // boot banner, in every build
constexpr size_t kSysBuf = 8192;
uint32_t g_sdToolsScanAt = 0;
uint32_t g_markValidAt = 0;

bool backupNs(const char* ns) {
    if (!strcmp(ns, "rs_restore") || !strcmp(ns, "rs_ota")) return false;
    return !strncmp(ns, "rs_", 3) || !strcmp(ns, "gps") || !strcmp(ns, NVS_NS_IDENTITY) || !strcmp(ns, NVS_NS_MSG);
}

bool unitOnlyNs(const char* ns) {
    return !strcmp(ns, "rs_vault") || !strcmp(ns, "rs_wifi") || !strcmp(ns, "rs_bat") || !strcmp(ns, "rs_lock");
}

void macHex(char out[13]) {
    uint8_t m[6] = {};
    esp_efuse_mac_get_default(m);
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

uint8_t* sysBuf() {
    static uint8_t* b = nullptr;
    if (!b) b = (uint8_t*)heap_caps_malloc(kSysBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) b = (uint8_t*)malloc(kSysBuf);
    return b;
}

// Second Enter on the same row within 5 s (keys 1009 = firmware, 1100 + i = backup i).
bool askTwice(int what, const char* msg) {
    const bool yes = confirmTwice(1000 + what, msg);
    requestRedraw();
    return yes;
}

// "20261113-1405" -> "2026-11-13 14:05" (other names as they are).
void backupPretty(const char* n, char* out, size_t cap) {
    bool stamp = strlen(n) >= 13 && n[8] == '-';
    for (int i = 0; stamp && i < 13; i++) if (i != 8 && !isdigit((unsigned char)n[i])) stamp = false;
    if (stamp) snprintf(out, cap, "%.4s-%.2s-%.2s %.2s:%.2s%s", n, n + 4, n + 6, n + 9, n + 11, n + 13);
    else snprintf(out, cap, "%s", n);
}

void sdToolsScan(bool force) {
    if (!force && g_sdToolsScanAt && millis() - g_sdToolsScanAt < 5000) return;
    g_sdToolsScanAt = millis() ? millis() : 1;
    g_bkCount = 0;
    g_fwName[0] = 0;
    g_fwSize = 0;
    g_fwInstalled = false;
    if (g_usbDrive || !g_sdMounted || !g_sdStore.isReady()) return;
    SharedSPILock lock;
    if (File d = g_sdStore.openDir(kBackupRoot)) {
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            const char* nm = e.name();
            if (e.isDirectory() && nm && *nm && strlen(nm) < sizeof(g_bkNames[0])) {
                int pos = 0;                                   // newest (largest name) first
                while (pos < g_bkCount && strcmp(nm, g_bkNames[pos]) < 0) pos++;
                if (pos < kMaxBackups) {
                    const int last = g_bkCount < kMaxBackups ? g_bkCount : kMaxBackups - 1;
                    for (int i = last; i > pos; i--) memcpy(g_bkNames[i], g_bkNames[i - 1], sizeof(g_bkNames[0]));
                    snprintf(g_bkNames[pos], sizeof(g_bkNames[0]), "%s", nm);
                    if (g_bkCount < kMaxBackups) g_bkCount++;
                }
            }
            e.close();
        }
        d.close();
    }
    if (File d = g_sdStore.openDir(kFwDir)) {
        time_t best = 0;
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            const char* nm = e.name();
            const size_t len = nm ? strlen(nm) : 0;
            if (!e.isDirectory() && len > 4 && len < sizeof(g_fwName) && !strcasecmp(nm + len - 4, ".bin")) {
                const time_t t = e.getLastWrite();
                if (!g_fwName[0] || t > best) {
                    best = t;
                    snprintf(g_fwName, sizeof(g_fwName), "%s", nm);
                    g_fwSize = (uint32_t)e.size();
                }
            }
            e.close();
        }
        d.close();
    }
    if (g_fwName[0]) {
        Preferences p;
        if (p.begin("rs_ota", true)) {
            char last[40] = "";
            p.getString("file", last, sizeof(last));
            g_fwInstalled = !strcmp(last, g_fwName) && p.getUInt("fsize", 0) == g_fwSize;
            p.end();
        }
    }
}

// Full-screen status while a long job blocks the loop (e-ink keeps it after a restart).
void sysScreen(const char* title, const char* l1, const char* l2, const char* l3) {
    clearRoot();
    drawHeader(title, false);
    int y = LIST_TOP + 8;
    if (l1) { text(8, y, l1, true, W - 16); y += 26; }
    if (l2) { text(8, y, l2, false, W - 16); y += 20; }
    if (l3) { text(8, y, l3, false, W - 16); }
    lv_refr_now(nullptr);
    g_display.refreshIfDirty();
}

struct CopyStats {
    uint32_t files = 0;
    uint64_t bytes = 0;
    uint64_t total = 0;          // for the progress line (0 = no progress shown)
    int shown = 0;
    const char* title = nullptr;
};

void copyProgress(CopyStats& st) {
    if (!st.total || !st.title) return;
    int pct = (int)(st.bytes * 100 / st.total);
    if (pct > 99) pct = 99;
    if (pct / 25 <= st.shown / 25) return;
    st.shown = pct;
    char l[32];
    snprintf(l, sizeof(l), "%d%%  (%lu files)", pct, (unsigned long)st.files);
    sysScreen(st.title, "Backing up...", l, "Keep the card in");
}

// One open file into another, 8 KB at a time; the SPI lock is taken per chunk so the
// radio keeps its turns on the shared bus.
bool copyStream(File& in, File& out, CopyStats& st) {
    uint8_t* b = sysBuf();
    if (!b) return false;
    for (;;) {
        size_t n;
        { SharedSPILock l; n = in.read(b, kSysBuf); }
        if (!n) break;
        size_t w;
        { SharedSPILock l; w = out.write(b, n); }
        if (w != n) return false;
        st.bytes += n;
        copyProgress(st);
        yield();
    }
    st.files++;
    return true;
}

void joinPath(char* out, size_t cap, const char* dir, const char* name) {
    const size_t dl = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", name);
}

// Unit flash -> card, recursive.
bool copyFlashToSd(const char* fdir, const char* sdir, CopyStats& st) {
    File d = g_flash.openDir(fdir);
    if (!d || !d.isDirectory()) return false;
    bool ok = true;
    for (File e = d.openNextFile(); e && ok; e = d.openNextFile()) {
        char fp[160], sp[192];
        const char* nm = e.name();
        joinPath(fp, sizeof(fp), fdir, nm);
        joinPath(sp, sizeof(sp), sdir, nm);
        if (e.isDirectory()) {
            e.close();
            ok = g_sdStore.ensureDir(sp) && copyFlashToSd(fp, sp, st);
        } else {
            File out = g_sdStore.openFile(sp, FILE_WRITE);
            ok = out && copyStream(e, out, st);
            if (out) { SharedSPILock l; out.close(); }
            e.close();
            if (!ok) Serial.printf("[BACKUP] copy failed: %s\n", fp);
        }
    }
    d.close();
    return ok;
}

// Card -> unit flash, recursive.
bool copySdToFlash(const char* sdir, const char* fdir, CopyStats& st) {
    File d = g_sdStore.openDir(sdir);
    if (!d || !d.isDirectory()) return false;
    bool ok = true;
    for (File e = d.openNextFile(); e && ok; e = d.openNextFile()) {
        char fp[160], sp[192];
        const char* nm = e.name();
        joinPath(sp, sizeof(sp), sdir, nm);
        joinPath(fp, sizeof(fp), fdir, nm);
        if (e.isDirectory()) {
            e.close();
            ok = g_flash.ensureDir(fp) && copySdToFlash(sp, fp, st);
        } else {
            File out = g_flash.openFile(fp, FILE_WRITE);
            ok = out && copyStream(e, out, st);
            if (out) out.close();
            e.close();
            if (!ok) Serial.printf("[RESTORE] copy failed: %s\n", fp);
        }
    }
    d.close();
    return ok;
}

// Every file and folder under fdir (the folder itself stays). One entry per pass, so
// nothing is deleted while a listing is open.
bool wipeFlashDir(const char* fdir) {
    for (;;) {
        char victim[160] = "";
        bool isDir = false;
        if (File d = g_flash.openDir(fdir)) {
            if (File e = d.openNextFile()) {
                joinPath(victim, sizeof(victim), fdir, e.name());
                isDir = e.isDirectory();
                e.close();
            }
            d.close();
        }
        if (!victim[0]) return true;
        const bool gone = isDir ? (wipeFlashDir(victim) && g_flash.removeDir(victim)) : g_flash.remove(victim);
        if (!gone) { Serial.printf("[RESTORE] cannot remove %s\n", victim); return false; }
    }
}

struct NvsOut { File* f; int n; };

bool nvsLine(const char* ns, const char* key, uint8_t type, const uint8_t* d, size_t len, void* ctx) {
    auto* o = (NvsOut*)ctx;
    if (!backupNs(ns)) return true;
    static const char* hx = "0123456789abcdef";
    SharedSPILock l;
    o->f->printf("%s\t%s\t%u\t", ns, key, (unsigned)type);
    char chunk[130];
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        chunk[k++] = hx[d[i] >> 4];
        chunk[k++] = hx[d[i] & 15];
        if (k >= 128) { chunk[k] = 0; o->f->print(chunk); k = 0; }
    }
    chunk[k++] = '\n';
    chunk[k] = 0;
    o->f->print(chunk);
    o->n++;
    return true;
}

void backupNow() {
    if (g_usbDrive) { toast("USB drive mode is on"); return; }
    if (!sdMount()) { toast("No SD card"); return; }
    char stamp[24], dir[48], sub[64];
    struct tm t;
    if (localNow(t) && t.tm_year > 120)
        snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    else
        snprintf(stamp, sizeof(stamp), "backup");
    snprintf(dir, sizeof(dir), "%s/%s", kBackupRoot, stamp);
    for (int k = 2; g_sdStore.exists(dir) && k < 100; k++) snprintf(dir, sizeof(dir), "%s/%s-%d", kBackupRoot, stamp, k);
    const uint64_t used = g_flash.usedBytes();
    if (g_sdStore.totalBytes() - g_sdStore.usedBytes() < used + (1u << 20)) { toast("SD card full"); return; }
    snprintf(sub, sizeof(sub), "%s/flash", dir);
    if (!g_sdStore.ensureDir(sub)) { toast("Cannot write the card"); return; }

    const uint32_t t0 = millis();
    CopyStats st;
    st.total = used;
    st.title = "Back up";
    sysScreen("Back up", "Backing up...", "0%", "Keep the card in");
    bool ok = copyFlashToSd("/", sub, st);
    NvsOut no{nullptr, 0};
    if (ok) {
        char path[64];
        snprintf(path, sizeof(path), "%s/nvs.txt", dir);
        File f = g_sdStore.openFile(path, FILE_WRITE);
        no.f = &f;
        ok = f && sysimg::nvsForEach(nvsLine, &no) >= 0;
        if (f) { SharedSPILock l; f.close(); }
    }
    if (ok) {
        char path[64], mac[13];
        snprintf(path, sizeof(path), "%s/info.txt", dir);
        macHex(mac);
        File f = g_sdStore.openFile(path, FILE_WRITE);
        if (f) {
            SharedSPILock l;
            f.printf("ratspeak-backup 1\nname %s\nmac %s\nbuilt %s %s\nfiles %lu\nbytes %llu\nsettings %d\ncomplete 1\n",
                     g_selfName, mac, __DATE__, __TIME__, (unsigned long)st.files, (unsigned long long)st.bytes, no.n);
            f.close();
        } else ok = false;
    }
    Serial.printf("[BACKUP] %s %s: %lu files, %lu KB, %d settings, %lu ms\n", ok ? "ok" : "FAILED", dir,
                  (unsigned long)st.files, (unsigned long)(st.bytes >> 10), no.n, (unsigned long)(millis() - t0));
    g_sdToolsScanAt = 0;
    g_fullRefreshReq = true;
    requestRedraw();
    if (ok) toast("Backed up: %s", dir + strlen(kBackupRoot) + 1);
    else toast("Backup failed");
}

void backupsOpen() {
    if (!sdMount()) { toast("No SD card"); return; }
    sdToolsScan(true);
    if (!g_bkCount) { toast("No backups in /backups"); return; }
    pushPage(PG_BACKUPS);
}

[[noreturn]] void restoreRequest(int i) {
    char dir[48];
    snprintf(dir, sizeof(dir), "%s/%s", kBackupRoot, g_bkNames[i]);
    Preferences p;
    if (p.begin("rs_restore", false)) { p.putString("dir", dir); p.end(); }
    Serial.printf("[RESTORE] %s at the next boot: restarting\n", dir);
    char nm[24];
    backupPretty(g_bkNames[i], nm, sizeof(nm));
    sysScreen("Restore backup", "Restoring...", nm, "Restarts by itself when done");
    Serial.flush();
    delay(100);
    ESP.restart();
    for (;;) delay(1000);
}

void backupPick(int i) {
    if (i < 0 || i >= g_bkCount) return;
    char path[64];
    snprintf(path, sizeof(path), "%s/%s/info.txt", kBackupRoot, g_bkNames[i]);
    if (g_sdStore.readString(path).indexOf("complete 1") < 0) { toast("Backup incomplete: skip"); return; }
    if (!askTwice(100 + i, "replaces all")) return;
    restoreRequest(i);
}

// Restore asked for before the restart: runs before the flash or NVS is used, then restarts.
void restoreAtBoot() {
    char dir[48] = "";
    {
        Preferences p;
        if (!p.begin("rs_restore", false)) return;
        p.getString("dir", dir, sizeof(dir));
        if (dir[0]) p.remove("dir");                  // once only, even if this fails half-way
        p.end();
    }
    if (!dir[0]) return;
    Serial.printf("[RESTORE] %s\n", dir);
    const uint32_t t0 = millis();
    char msg[48] = "Restore failed";
    char path[80];
    CopyStats st;
    int put = 0;
    bool same = false;
    do {
        if (!sdMount()) { snprintf(msg, sizeof(msg), "Restore: no SD card"); break; }
        snprintf(path, sizeof(path), "%s/info.txt", dir);
        const String info = g_sdStore.readString(path);
        if (info.indexOf("complete 1") < 0) { snprintf(msg, sizeof(msg), "Restore: backup incomplete"); break; }
        char mine[13];
        macHex(mine);
        const int mi = info.indexOf("mac ");
        same = mi >= 0 && info.substring(mi + 4, mi + 16).equalsIgnoreCase(mine);
        if (!g_flash.begin() && !recoverCorruptLittleFS()) { snprintf(msg, sizeof(msg), "Restore: flash failed"); break; }

        // 1. Files: wipe, copy back.
        if (!wipeFlashDir("/")) { snprintf(msg, sizeof(msg), "Restore: flash wipe failed"); break; }
        snprintf(path, sizeof(path), "%s/flash", dir);
        if (!copySdToFlash(path, "/", st)) { snprintf(msg, sizeof(msg), "Restore: copy failed"); break; }

        // 2. NVS: wipe our namespaces (not the unit-only ones on another unit), then put back.
        char names[32][16];
        int nn = 0;
        struct Ctx { char (*names)[16]; int* nn; } c{names, &nn};
        sysimg::nvsForEach([](const char* ns, const char*, uint8_t, const uint8_t*, size_t, void* v) {
            auto* c = (Ctx*)v;
            for (int i = 0; i < *c->nn; i++) if (!strcmp(c->names[i], ns)) return true;
            if (*c->nn < 32) snprintf(c->names[(*c->nn)++], 16, "%s", ns);
            return true;
        }, &c);
        for (int i = 0; i < nn; i++)
            if (backupNs(names[i]) && (same || !unitOnlyNs(names[i]))) sysimg::nvsEraseNamespace(names[i]);
        snprintf(path, sizeof(path), "%s/nvs.txt", dir);
        File f = g_sdStore.openFile(path, FILE_READ);
        if (!f) { snprintf(msg, sizeof(msg), "Restore: no settings file"); break; }
        for (;;) {
            String line;
            { SharedSPILock l; if (!f.available()) break; line = f.readStringUntil('\n'); }
            const int t1 = line.indexOf('\t'), t2 = line.indexOf('\t', t1 + 1), t3 = line.indexOf('\t', t2 + 1);
            if (t1 <= 0 || t2 <= t1 || t3 <= t2) continue;
            const String ns = line.substring(0, t1), key = line.substring(t1 + 1, t2);
            if (!backupNs(ns.c_str()) || (!same && unitOnlyNs(ns.c_str()))) continue;
            const uint8_t type = (uint8_t)line.substring(t2 + 1, t3).toInt();
            const char* hex = line.c_str() + t3 + 1;
            size_t hl = strlen(hex);
            while (hl && (hex[hl - 1] == '\r' || hex[hl - 1] == ' ')) hl--;
            const size_t len = hl / 2;
            uint8_t* buf = (uint8_t*)malloc(len ? len : 1);
            if (!buf) continue;
            bool good = true;
            for (size_t i = 0; i < len && good; i++) {
                int v = 0;
                for (int k = 0; k < 2; k++) {
                    const char ch = hex[2 * i + k];
                    const int d = isdigit((unsigned char)ch) ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
                    if (d < 0) good = false;
                    v = v * 16 + d;
                }
                buf[i] = (uint8_t)v;
            }
            if (good && sysimg::nvsPut(ns.c_str(), key.c_str(), type, buf, len)) put++;
            else Serial.printf("[RESTORE] setting not restored: %s/%s\n", ns.c_str(), key.c_str());
            free(buf);
        }
        { SharedSPILock l; f.close(); }
        snprintf(msg, sizeof(msg), same ? "Backup restored" : "Restored (Wi-Fi pw not moved)");
    } while (0);
    Serial.printf("[RESTORE] %s: %lu files, %lu KB, %d settings, %s unit, %lu ms\n", msg, (unsigned long)st.files,
                  (unsigned long)(st.bytes >> 10), put, same ? "same" : "another", (unsigned long)(millis() - t0));
    Preferences p;
    if (p.begin("rs_restore", false)) { p.putString("msg", msg); p.end(); }
    Serial.println("[RESTORE] restarting");
    Serial.flush();
    delay(50);
    ESP.restart();
}

// After "[BOOT] ready": the result of a restore that ran before the last restart.
void restoreReport() {
    Preferences p;
    if (!p.begin("rs_restore", false)) return;
    char msg[48] = "";
    p.getString("msg", msg, sizeof(msg));
    if (msg[0]) { p.remove("msg"); toast("%s", msg); Serial.printf("[RESTORE] report: %s\n", msg); }
    p.end();
}

bool hasSeq(const uint8_t* h, size_t n, const char* s, size_t m) {
    if (n < m) return false;
    for (size_t i = 0; i + m <= n; i++) {
        const uint8_t* p = (const uint8_t*)memchr(h + i, s[0], n - m + 1 - i);
        if (!p) return false;
        i = p - h;
        if (!memcmp(p, s, m)) return true;
    }
    return false;
}

void fwUpdate() {
    if (g_usbDrive) { toast("USB drive mode is on"); return; }
    if (!g_fwName[0]) { toast("No .bin in /firmware"); Serial.println("[OTA] no .bin in /firmware"); return; }
    if (g_bat.valid && !g_bat.usb && g_bat.pct < 30) { toast("Charge or plug in first"); return; }
    char why[40] = "";
    if (!sysimg::otaCheck(g_fwSize, why, sizeof(why))) { toast("Update: %s", why); Serial.printf("[OTA] refused: %s\n", why); return; }
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", kFwDir, g_fwName);
    File f = g_sdStore.openFile(path, FILE_READ);
    uint8_t* b = sysBuf();
    if (!f || !b) { toast("Cannot read %s", g_fwName); return; }
    const uint32_t t0 = millis();
    sysScreen("Update firmware", "Updating...", g_fwName, "Do not turn off");
    bool ok = sysimg::otaBegin(g_fwSize, why, sizeof(why));
    const size_t ml = strlen(kFwMarker);
    uint8_t edge[128];                               // end of the last chunk + start of this one
    size_t tail = 0;
    bool marker = false;
    uint32_t done = 0;
    int shown = 0;
    while (ok && done < g_fwSize) {
        size_t n;
        { SharedSPILock l; n = f.read(b, kSysBuf); }
        if (!n) { snprintf(why, sizeof(why), "read error"); ok = false; break; }
        // An app image: magic E9, and the app description (magic ABCD5432) right after the
        // first segment header. A merged image (bootloader first) or other file fails here.
        if (done == 0 && !(n >= 36 && b[0] == 0xE9 && b[32] == 0x32 && b[33] == 0x54 && b[34] == 0xCD && b[35] == 0xAB)) {
            snprintf(why, sizeof(why), "not an app image");
            ok = false;
            break;
        }
        if (!marker) {
            const size_t head = n < ml - 1 ? n : ml - 1;
            memcpy(edge + tail, b, head);
            marker = hasSeq(edge, tail + head, kFwMarker, ml) || hasSeq(b, n, kFwMarker, ml);
            tail = n < ml - 1 ? n : ml - 1;
            memcpy(edge, b + n - tail, tail);
        }
        if (!sysimg::otaWrite(b, n)) { snprintf(why, sizeof(why), "flash write failed"); ok = false; break; }
        done += n;
        const int pct = (int)((uint64_t)done * 100 / g_fwSize);
        if (pct / 20 > shown / 20 && pct < 100) {
            shown = pct;
            char l[16];
            snprintf(l, sizeof(l), "%d%%", pct);
            sysScreen("Update firmware", "Updating...", l, "Do not turn off");
        }
        yield();
    }
    { SharedSPILock l; f.close(); }
    if (ok && !marker) { snprintf(why, sizeof(why), "not T-Deck Pro firmware"); ok = false; }
    if (ok) ok = sysimg::otaEnd(why, sizeof(why));
    else sysimg::otaAbort();
    Serial.printf("[OTA] %s: %s, %lu ms\n", ok ? "installed" : "FAILED", ok ? g_fwName : why, (unsigned long)(millis() - t0));
    if (!ok) {
        g_fullRefreshReq = true;
        requestRedraw();
        toast("Update failed: %s", why);
        return;
    }
    Preferences p;
    if (p.begin("rs_ota", false)) { p.putString("file", g_fwName); p.putUInt("fsize", g_fwSize); p.end(); }
    sysScreen("Update firmware", "Installed.", "Restarting...", "Old one returns if it fails");
    Serial.println("[OTA] restarting into the new firmware");
    Serial.flush();
    delay(200);
    ESP.restart();
}

void fwUpdateKey() {
    if (!sdMount()) { toast("No SD card"); return; }
    sdToolsScan(true);
    if (!g_fwName[0]) { toast("Put a .bin in /firmware"); return; }
    char why[40];
    if (!sysimg::otaCheck(g_fwSize, why, sizeof(why))) { toast("Update: %s", why); return; }
    if (!askTwice(9, g_fwInstalled ? "install again" : "install it")) return;
    fwUpdate();
}

// ---- USB drive mode (S.10.5 row 6) -------------------------------------------------------
// The SD card becomes a USB drive on the PC (hal/UsbDrive). The firmware unmounts the card
// and keeps its hands off it (sdMount() refuses) until the restart that ends the mode:
// Eject on the PC (restarts by itself), or Enter / ^ on the unit. Messages still arrive and
// are kept in flash; the COM port is away until the restart.
uint32_t g_usbEjectAt = 0;

void usbDriveStart() {
    if (g_usbDrive) return;
    if (g_sdArchiveActive) { toast("Archive on: turn off, reboot"); return; }
    if (g_bat.valid && !g_bat.usb) { toast("Plug into a PC first"); return; }
    sdUnmount();
    g_usbDrive = true;                  // sdMount() now refuses
    if (!usbdrive::start(SD_CS)) {
        g_usbDrive = false;
        toast("USB drive: %s", usbdrive::message());
        return;
    }
    g_fullRefreshReq = true;
    requestRedraw();
}

[[noreturn]] void usbDriveStop() {
    Serial.println("[USB] drive mode off: restarting");
    clearRoot();
    drawHeader("USB drive", false);
    text(8, LIST_TOP + 8, "Restarting...", true, W - 16);
    lv_refr_now(nullptr);
    g_display.refreshIfDirty();
    usbdrive::restartToNormal();
}

void usbDriveTick() {
    if (!g_usbDrive) return;
    static usbdrive::State shown = usbdrive::OFF;
    const usbdrive::State st = usbdrive::state();
    if (st != shown) { shown = st; requestRedraw(); }
    if (st == usbdrive::EJECTED) {
        if (!g_usbEjectAt) g_usbEjectAt = millis() ? millis() : 1;
        else if (millis() - g_usbEjectAt > 2000) usbDriveStop();   // PC ejected: back to normal
    } else {
        g_usbEjectAt = 0;
    }
    static uint32_t lastKb = 0, lastDraw = 0;     // live counters, at most every 5 s (e-ink)
    const uint32_t kb = usbdrive::readKb() + usbdrive::writtenKb();
    if (kb != lastKb && millis() - lastDraw > 5000) { lastKb = kb; lastDraw = millis(); requestRedraw(); }
}

void renderUsbDrive() {
    drawHeader("USB drive", false);
    int y = LIST_TOP + 4;
    char t[48];
    const usbdrive::State st = usbdrive::state();
    snprintf(t, sizeof(t), "SD card %lu MB", (unsigned long)(usbdrive::sectors() / 2048));
    text(8, y, t, true, W - 16); y += 24;
    if (st == usbdrive::EJECTED) {
        text(8, y, "Ejected. Restarting...", false, W - 16); y += 28;
    } else {
        text(8, y, "It is a drive on the PC now.", false, W - 16); y += 20;
        text(8, y, "Copy files, then Eject it", false, W - 16); y += 20;
        text(8, y, "on the PC: the unit restarts.", false, W - 16); y += 28;
    }
    const uint32_t r = usbdrive::readKb(), w = usbdrive::writtenKb();
    if (r >= 10240 || w >= 10240) snprintf(t, sizeof(t), "read %lu MB, written %lu MB", (unsigned long)(r >> 10), (unsigned long)(w >> 10));
    else snprintf(t, sizeof(t), "read %lu KB, written %lu KB", (unsigned long)r, (unsigned long)w);
    text(8, y, t, false, W - 16); y += 28;
    text(8, y, "Messages still arrive.", false, W - 16); y += 20;
    text(8, y, "No COM port until restart.", false, W - 16);
    box(0, FOOT_Y, W, FOOT_H, 1);
    const bool recent = usbdrive::lastWriteMs() && millis() - usbdrive::lastWriteMs() < 3000;
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : recent ? "Writing... wait" : "Enter = stop (eject first)");
}

void usbDriveKey(const KeyEvent& ev) {
    if (!(ev.enter || ev.up)) return;
    static uint32_t warnedAt = 0;
    // Not ejected and written recently: the PC may still hold data; ask once more.
    if (usbdrive::state() != usbdrive::EJECTED && usbdrive::writtenKb() &&
        (!warnedAt || millis() - warnedAt > 10000)) {
        warnedAt = millis();
        toast("Eject on PC first; again=stop");
        requestRedraw();
        return;
    }
    usbDriveStop();
}

// ---- Chrome: top bar, bottom bar, menu overlay -----------------------------------
void drawTopBar() {
    drawRefreshIcon(0, 0, REF_W, TOP_H);
    addHit(0, 0, REF_W, TOP_H, H_REFRESH);

    const bool homeOpen = g_screen == SCR_HOME && !g_menuOpen;
    box(REF_W, 0, BAT_W, TOP_H, 1, false, homeOpen
        ? (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT)
        : LV_BORDER_SIDE_FULL);
    drawBatteryIcon(REF_W + 5, 3, BAT_W - 10, TOP_H - 6, g_bat.valid ? g_bat.pct : -1);

    box(CLOCK_X, 0, TAB_W, TOP_H, 1);
    char d[12], t[12];                       // "11:30 PM" with the 12 h clock
    fmtDate(d, sizeof(d));                   // MM/DD/YYYY
    fmtTime(t, sizeof(t));
    if (textW(d) <= TAB_W - 4) {
        label(CLOCK_X, 1, TAB_W, 16, d);
    } else {                                 // wide digits (08/08/2028): the 12 px font
        lv_obj_t* o = lv_label_create(g_root);
        lv_label_set_text(o, d);
        lv_obj_set_style_text_font(o, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(o, fg(), 0);
        const int w = lv_txt_get_width(d, strlen(d), &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE);
        lv_obj_set_pos(o, CLOCK_X + (TAB_W - w) / 2, 3);
    }
    label(CLOCK_X, 17, TAB_W, 16, t);
    if (ashareActive()) {                    // location auto-share on: small pin, top left
        fillRect(CLOCK_X + 4, 4, 7, 7);
        fillRect(CLOCK_X + 6, 11, 3, 4);
    }

    navBox(MENU_X, 0, TAB_W, TOP_H, "MENU", g_menuOpen, browseMode() ? 'M' : 0);
    if (browseMode()) addHit(MENU_X, 0, TAB_W, TOP_H, H_MENU);
}

// ---- Nav keys (K, or the middle bottom tab, on the Atlas) -------------------------------
// A reference card of the Atlas keys over the lower part of the map; the map stays usable
// above it, so the keys can be tried with the card showing. K / the tab / ^ hide it.
// Atlas letters (also used by mapKey and wptMapKey).
// Alt+P adds a waypoint (and marks a trap in the game); Alt+SYM deletes the selected waypoint.
constexpr char kKeyNav = 'K', kKeyFind = 'L', kKeyWpt = 'P', kKeyContact = 'N';
constexpr char kKeyGo = 'G', kKeyBack = 'B';   // bearing + distance screen, track back
bool isAltSym(const KeyEvent& ev) {
    return ev.alt && ev.opt && !ev.character && !ev.enter && !ev.del && !ev.space && !ev.fn;
}
struct NavKeyRow { const char* keys; const char* what; };
const NavKeyRow kAtlasKeys[] = {                 // two columns of 7, the 12 px font
    {"E" LV_SYMBOL_UP " S" LV_SYMBOL_LEFT " X" LV_SYMBOL_DOWN " F" LV_SYMBOL_RIGHT, ""},
    {"D", "centre on me"},   {"Z", "zoom in"},       {"W", "zoom out"},
    {"L", "locate a place"}, {"K", "hide keys"},     {"M ^", "menu, close"},
    {"N", "next contact"},   {"P", "next waypoint"}, {"Alt+P", "add waypoint"},
    {"Alt+SYM", "delete waypt"}, {"G", "arrow + distance"}, {"B", "track back"},
    {"12 Enter", "chat #12"},
};
// Library reader and list (K there too).
const NavKeyRow kReaderKeys[] = {
    {"Space N", "next page"}, {"B", "previous page"}, {"E X", "half page"},
    {"W", "wrap on / off"},   {"S F", "pan left/right"}, {"D", "left edge"}, {"", ""},
    {"L", "locate (find)"},   {"K", "hide keys"},      {"M", "menu"},
    {"^", "back to the list"}, {"", ""},              {"", ""}, {"", ""},
};
const NavKeyRow kLibListKeys[] = {
    {"1-4", "open that row"}, {"Enter", "open selected"}, {"G V", "up / down"},
    {"K", "hide keys"},       {"", ""},                {"", ""}, {"", ""},
    {"M", "menu"},            {"^", "close"},          {"", ""},
    {"", ""},                 {"", ""},                {"", ""}, {"", ""},
};
const NavKeyRow kGameKeys[] = {
    {"E" LV_SYMBOL_UP " S" LV_SYMBOL_LEFT " X" LV_SYMBOL_DOWN " F" LV_SYMBOL_RIGHT, ""},
    {"D Enter", "open the square"}, {"Alt+P", "mark a trap"}, {"Q", "new game"},
    {"K", "hide keys"},       {"", ""},                {"", ""},
    {"M", "menu"},            {"^", "back to Library"}, {"", ""},
    {"", ""},                 {"", ""},                {"", ""}, {"", ""},
};
constexpr int kNavRowH = 14, kNavRows = 7;
constexpr int kNavH = kNavRows * kNavRowH + 6;   // 90 px: the map keeps the rest
bool g_navKeysOpen = false;
extern bool g_libOpen, g_gameOpen, g_chessOpen;
void mapKey(const KeyEvent& ev);

void navKeysToggle() {
    g_navKeysOpen = !g_navKeysOpen;
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.printf("[NAV] keys %s\n", g_navKeysOpen ? "shown" : "hidden");
}

void smallText(int x, int y, const char* s, bool bold) {
    for (int k = 0; k < (bold ? 2 : 1); k++) {
        lv_obj_t* o = lv_label_create(g_root);
        lv_label_set_text(o, s);
        lv_obj_set_style_text_font(o, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(o, fg(), 0);
        lv_obj_set_pos(o, x + k, y);
    }
}

// Drawn at the bottom of the map area (y0 .. y0 + kNavH).
void drawNavKeys(int y0, const NavKeyRow* rows = kAtlasKeys) {
    fillRect(0, y0, W, kNavH, false);
    box(0, y0, W, kNavH, 1);
    const int colW = W / 2;
    for (int i = 0; i < 2 * kNavRows; i++) {
        const int x = 4 + (i / kNavRows) * colW, y = y0 + 3 + (i % kNavRows) * kNavRowH;
        if (!rows[i].keys[0]) continue;
        smallText(x, y, rows[i].keys, true);
        const int kw = lv_txt_get_width(rows[i].keys, strlen(rows[i].keys), &lv_font_montserrat_12, 0,
                                        LV_TEXT_FLAG_NONE);
        if (rows[i].what[0]) smallText(x + (kw + 6 > 22 ? kw + 6 : 22), y, rows[i].what, false);
    }
}

void drawBottomBar() {
    if (g_wizOpen && g_entry != Entry::Name) {
        box(0, BOT_TOP, W, BOT_H, 1);
        label(0, BOT_TOP + 2, W, 18, "Enter next   ^ back");
        label(0, BOT_TOP + 20, W, 18, "0 = skip setup (defaults)");
        return;
    }
    if (g_threadOpen || g_entry == Entry::Name) {
        char back[32];
        ScreenId to = g_threadOpen ? SCR_INBOX : g_screen;
        snprintf(back, sizeof(back), "^ BACK UP TO %s", screenName(to));
        for (char* p = back; *p; p++) *p = (char)toupper((unsigned char)*p);
        navBox(0, BOT_TOP, W, BOT_H, back, true, '^');
        addHit(0, BOT_TOP, W, BOT_H, H_BACK);
        return;
    }
    const bool hk = browseMode();
    navBox(0, BOT_TOP, BOT_C_W, BOT_H, "Contact", g_screen == SCR_CONTACTS, hk ? 'C' : 0);
    // Middle tab: "Nav / keys" (K) on the Atlas, where it shows the key card; else Atlas (L,
    // the offline map) when the card has maps, else Peers (P). L and P work either way.
    if (mapview::isOpen() || g_libOpen || g_gameOpen || g_chessOpen) {
        box(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, g_navKeysOpen ? 3 : 1);
        label(BOT_C_W, BOT_TOP + 2, BOT_A_W, 16, "Nav", g_navKeysOpen);
        label(BOT_C_W, BOT_TOP + 19, BOT_A_W, 16, "keys", g_navKeysOpen, kKeyNav);
        addHit(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, H_NAVKEYS);
    } else if (g_haveMaps) {
        navBox(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, "Atlas", false, hk ? 'A' : 0);
        if (hk) addHit(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, H_MENU_GO, kMenuMap);
    } else {
        navBox(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, "Peers", g_screen == SCR_PEERS, hk ? 'P' : 0);
        if (hk) addHit(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, H_TAB, SCR_PEERS);
    }
    navBox(BOT_C_W + BOT_A_W, BOT_TOP, BOT_I_W, BOT_H, "Inbox", g_screen == SCR_INBOX, hk ? 'I' : 0);
    if (hk) {
        addHit(0, BOT_TOP, BOT_C_W, BOT_H, H_TAB, SCR_CONTACTS);
        addHit(BOT_C_W + BOT_A_W, BOT_TOP, BOT_I_W, BOT_H, H_TAB, SCR_INBOX);
    }
}

void drawMenuOverlay() {
    if (!g_menuOpen) return;
    for (int i = 0; i < kMenuCount; i++) {
        const int y = TOP_H + i * MENU_ROW_H;
        navBox(MENU_X, y, TAB_W, MENU_ROW_H, kMenuLabels[i], i == g_menuSel, kMenuKeys[i]);
        addHit(MENU_X, y, TAB_W, MENU_ROW_H, H_MENU_GO, i);
    }
}

// ---- Offline map (MapView.cpp; tiles from the SD card) ----------------------------------
void gpsRequestVisit(const char* why = "location share");
void mapGpsAsk(bool opening);
bool gpsFresh();
bool g_mapSdTried = false;
File g_mapFile;                  // pack file kept open between reads
char g_mapFilePath[80] = "";
uint32_t g_looseChecked = 0, g_looseHave = 0, g_looseAt = 0;   // per zoom: /maps/<z> looked up / exists

size_t mapReadTile(const char* rel, uint32_t offset, uint8_t* buf, size_t len) {
    if (!g_sdMounted) {
        if (g_mapSdTried) return 0;          // one mount attempt per map visit
        g_mapSdTried = true;
        if (!sdMount()) return 0;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s%s", SD_PATH_ROOT, rel);
    // Pack files are read many times in a row (header, index, then one piece per tile):
    // keep the last one open, since a fresh exists() + open costs ~45 ms on the card.
    const bool pack = strncmp(rel, "/maps/packs/", 12) == 0;
    if (!pack) {
        // Loose tile /maps/<z>/<x>/<y>.rmt: a card with packs only has no /maps/<z> folder,
        // so remember per zoom whether it exists (checked again after 60 s) instead of an
        // exists() per missing tile (~80 ms each).
        int z = -1;
        if (sscanf(rel, "/maps/%d/", &z) == 1 && z >= 0 && z < 32) {
            uint32_t& checked = g_looseChecked; uint32_t& have = g_looseHave;
            if (millis() - g_looseAt > 60000) { checked = 0; g_looseAt = millis(); }
            if (!(checked & 1u << z)) {
                char dir[48];
                snprintf(dir, sizeof(dir), "%s/maps/%d", SD_PATH_ROOT, z);
                checked |= 1u << z;
                if (g_sdStore.exists(dir)) have |= 1u << z; else have &= ~(1u << z);
            }
            if (!(have & 1u << z)) return 0;
        }
        if (!g_sdStore.exists(path)) return 0;
        File f = g_sdStore.openFile(path, FILE_READ);
        if (!f) return 0;
        size_t n = 0;
        if (offset < f.size() && (offset == 0 || f.seek(offset))) n = f.read(buf, len);
        f.close();
        return n;
    }
    if (!g_mapFile || strcmp(path, g_mapFilePath) != 0) {
        mapReadDone();
        if (!g_sdStore.exists(path)) return 0;
        File f = g_sdStore.openFile(path, FILE_READ);
        if (!f) return 0;
        g_mapFile = f;
        snprintf(g_mapFilePath, sizeof(g_mapFilePath), "%s", path);
    }
    if (offset >= g_mapFile.size() || !g_mapFile.seek(offset)) return 0;
    return g_mapFile.read(buf, len);
}

// Closes the pack file mapReadTile keeps open (map closed, card unmounted, file rewritten).
void mapReadDone() {
    if (g_mapFile) g_mapFile.close();
    g_mapFile = File();
    g_mapFilePath[0] = 0;
    g_looseChecked = 0;                     // files may have changed: look again
}

void mapPixel(int x, int y, bool black) {
    if (x >= 0 && y >= 0 && x < W && y < H) g_display.setPixel(x, y, black);
}

// Own marker: the GPS fix (fresh, else the last one: the newest real position), else the
// saved position S.2.
void mapFeedPosition() {
    if (g_gpsPosValid) mapview::setPosition(true, g_gpsLat, g_gpsLon);
    else if (g_cfg.hasPos) mapview::setPosition(true, g_cfg.lat, g_cfg.lon);
    else mapview::setPosition(false, 0, 0);
}

extern int g_mapSel;

void mapKeyHint();

void openMapAt(double lat, double lon) {
    if (g_menuOpen) g_menuOpen = false;
    g_cardOpen = false;                      // the map replaces a contact page
    g_mapSel = -1;
    mapview::setReader(mapReadTile);
    g_mapSdTried = false;
    mapFeedPosition();
    mapGpsAsk(true);                        // the own marker too
    mapview::open(lat, lon, true, false);   // explicit place: no follow
    mapview::fitZoom();                     // deepest zoom with a tile there
    g_fullRefreshReq = true;                // a map is mostly new pixels: avoid ghosting
    requestRedraw();
    Serial.printf("[MAP] open %.5f,%.5f\n", lat, lon);
    mapKeyHint();
}

int cposWithPos();

// Key hints on the map's bottom line for 6 s after it opens (keys are not written anywhere).
void mapKeyHint() {
    const int n = cposWithPos();
    if (n) toast("K = nav keys  N %d contact%s", n, n == 1 ? "" : "s");   // fits 240 px
    else toast("K = nav keys");
    g_toastUntil = millis() + 6000;
}

void openMap() {
    g_cardOpen = false;
    g_mapSel = -1;                           // callers that show a contact select it after
    mapview::setReader(mapReadTile);
    g_mapSdTried = false;
    mapFeedPosition();
    mapGpsAsk(true);
    // Centre on the newest position: a fresh fix, the last fix, the saved position (S.2).
    double lat = 0, lon = 0; bool have = false;
    const char* how = "no position";
    if (g_gpsPosValid) { lat = g_gpsLat; lon = g_gpsLon; have = true; how = gpsFresh() ? "GPS" : "last fix"; }
    else if (g_cfg.hasPos) { lat = g_cfg.lat; lon = g_cfg.lon; have = true; how = "saved position"; }
    if (g_menuOpen) g_menuOpen = false;
    // No position: the card's own start view, maps/home.txt = "lat lon zoom"
    // (written by tools/maptiles/make_tiles.py), so the first view is not empty sea.
    bool cardHome = false;
    int homeZoom = 0;
    if (!have) {
        char txt[48] = "";
        const size_t n = mapReadTile("/maps/home.txt", 0, (uint8_t*)txt, sizeof(txt) - 1);
        txt[n < sizeof(txt) ? n : sizeof(txt) - 1] = 0;
        if (n && sscanf(txt, "%lf %lf %d", &lat, &lon, &homeZoom) >= 2) { cardHome = true; how = "card home"; }
    }
    mapview::open(lat, lon, have || cardHome, true);
    if (homeZoom > 0) mapview::setZoom(homeZoom);
    if (mapview::fitZoom()) Serial.printf("[MAP] zoom %d (deepest tiles here)\n", mapview::zoom());
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.printf("[MAP] open (%s)\n", how);
    mapKeyHint();
}

void closeMap() {
    g_navKeysOpen = false;
    g_navOpen = false;
    mapview::close();
    mapReadDone();
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.println("[MAP] closed");
}

// Flash space notices (the storage worker removes the oldest messages from 85% used and
// refuses new ones at 92%, see MessageTransactions::makeRoom).
void flashSpaceTick() {
    static uint32_t lastRemoved = 0, fullAt = 0;
    static bool toldRemoved = false;
    auto& sp = handheld::storage::MessageTransactions::spaceStats();
    if (sp.full) {
        if (!fullAt || millis() - fullAt >= 600000) {
            // Deleting conversations frees space without a new save to notice it: re-check.
            const uint64_t total = sp.effectiveTotal(g_flash.totalBytes()), used = g_flash.usedBytes();
            if (total && (uint64_t)used * 100 / total < handheld::storage::MessageTransactions::SpaceRefusePct) {
                sp.full = false; sp.usedPct = (uint8_t)((uint64_t)used * 100 / total);
            } else { fullAt = millis() | 1; toast("Storage full: msgs refused"); return; }
        } else return;
    }
    fullAt = 0;
    if (sp.removed != lastRemoved) {
        lastRemoved = sp.removed;
        if (!toldRemoved) { toldRemoved = true; toast("Oldest msgs auto-removed"); }
    }
}

// Stress-test heartbeat ("hb <s>"): one line with memory, queue and storage state, so a
// PC logger can spot slow leaks, stuck outboxes and restarts over long runs.
uint32_t g_hbEveryMs = 0;
void heartbeatPrint() {
    const auto& sp = handheld::storage::MessageTransactions::spaceStats();
    Serial.printf("[HB] up=%lu heap=%u int=%u intmin=%u intblk=%u psram=%u psmin=%u q=%d ob=%d unread=%d "
                  "conv=%u inc=%lu flash=%u rm=%lu full=%d",
                  (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                  g_protoReady ? g_proto.lxmfQueuedCount() : -1, g_outboxCount,
                  g_storeReady ? g_msgStore.totalUnreadCount() : -1,
                  g_storeReady ? (unsigned)g_msgStore.totalConversations() : 0u,
                  g_storeReady ? (unsigned long)g_msgStore.incomingCommitted() : 0ul,
                  (unsigned)sp.usedPct, (unsigned long)sp.removed, sp.full ? 1 : 0);
#if defined(SIMULATOR)
    Serial.printf(" proc=%u", (unsigned)(::sim::processBytes() >> 10));
    ::sim::wearDump();                       // flash wear counters up to now (tools/stress/wear.py)
#endif
    Serial.printf("\n");
}
void heartbeatTick() {
    static uint32_t at = 0;
    if (!g_hbEveryMs || (at && millis() - at < g_hbEveryMs)) return;
    at = millis() | 1;
    heartbeatPrint();
}

bool gpsFresh();

// The map wants the own position: without a fresh fix ask for a GPS visit when it opens and
// again each minute while it stays open and in use (keys in the last 5 min), so Balanced does
// not leave it on a 15-min-old (or no) fix. Only in profiles with mapGps (Full, Balanced,
// Custom if chosen): the saving modes keep their GPS schedule. A visit indoors ends after 60 s.
void mapGpsAsk(bool opening) {
    static uint32_t asked = 0;
    if (!g_cfg.gpsOn || g_pwEff.gps == power::GPS_OFF || gpsFresh()) return;
    if (!g_pwEff.mapGps) {                        // saving modes (Custom: "Map wakes GPS")
        if (opening) Serial.println("[GPS] map: no visit in this power mode (last fix shown)");
        return;
    }
    if (!power::gpsIntervalMs(g_pwEff.gps) && g_pwEff.gps != power::GPS_ON_USE) return;   // always on
    if (!opening && (!g_lastInputMs || millis() - g_lastInputMs > 300000)) return;
    if (asked && millis() - asked < 60000) return;
    asked = millis();
    gpsRequestVisit("map");
}

void mapTick() {
    if (!mapview::isOpen()) return;
    mapGpsAsk(false);
    mapFeedPosition();
    static bool lastFresh = false;
    const bool fresh = gpsFresh();
    if (mapview::positionNeedsRedraw() || fresh != lastFresh) requestRedraw();   // footer text
    lastFresh = fresh;
}

constexpr int MAP_FOOT_H = 18;               // GPS position line under the map

bool mapContactInfo(char* out, size_t n);   // contacts on the map (below)
extern int g_mapNum;
void mapContactsStamp();
void mapContactsFeed();

extern int g_mapSel;
extern int g_wptSel;
bool wptInfo(char* out, size_t n);
extern bool g_placeOpen;
extern int g_placeN;
void renderPlaces();
constexpr int kPlaceShowFwd = 6;

void renderMapHeader() {
    char st[64];
    if (g_placeOpen) snprintf(st, sizeof(st), "Places: 1-%d go  ^ back", g_placeN < kPlaceShowFwd ? g_placeN : kPlaceShowFwd);
    else if (g_tbOn) {
        double la, lo; char d[20] = "?";
        if (ownPosition(la, lo) && g_tbIdx < g_trackN) fmtDist(metersBetween(la, lo, g_track[2 * g_tbIdx], g_track[2 * g_tbIdx + 1]), d, sizeof(d));
        snprintf(st, sizeof(st), "Back: %s  G arrow  B stop", d);
    }
    else if (g_mapSel >= 0) snprintf(st, sizeof(st), "N next  #+Enter chat  D me");   // what to press
    else if (g_wptSel >= 0) snprintf(st, sizeof(st), "P next  Alt+SYM del  D me");
    else mapview::status(st, sizeof(st));
    drawHeader(st, false);
    char pos[56];
    if (g_placeOpen) renderPlaces();
    if (g_entry == Entry::WptName) snprintf(pos, sizeof(pos), "Name: %s_", g_buf);
    else if (g_entry == Entry::PlaceFind) snprintf(pos, sizeof(pos), "Find place: %s_", g_buf);
    else if (g_toast[0]) snprintf(pos, sizeof(pos), "%s", g_toast);               // key hints
    else if (wptInfo(pos, sizeof(pos))) {}
    else if (g_mapNum) snprintf(pos, sizeof(pos), "open chat #%d_  (Enter)", g_mapNum);
    else if (!mapContactInfo(pos, sizeof(pos))) gpsPosText(pos, sizeof(pos));
    box(0, BOT_TOP - MAP_FOOT_H, W, MAP_FOOT_H, 1);
    text(4, BOT_TOP - MAP_FOOT_H + 1, pos, false, W - 8);
    if (g_navKeysOpen) drawNavKeys(BOT_TOP - MAP_FOOT_H - kNavH);
}

void stampMap() {
    mapContactsFeed();
    const int h = BOT_TOP - MAP_FOOT_H - LIST_TOP - (g_navKeysOpen ? kNavH : 0);
    mapview::draw(mapPixel, 0, LIST_TOP, W, h, g_cfg.dark);
    mapContactsStamp();                      // name tags over the markers
    g_display.markDirty();
}


// ---- Location sharing ------------------------------------------------------------------
// Sent as plain LXMF text any client can read (rsLXMF has no field API on the send path):
//   manual  "My location: 43.64012, -70.92011 (+-10 m, 14:32)"
//   no fix  "My last location: 43.64012, -70.92011 (35 min old)"
//   auto    "Loc: 43.64012, -70.92011 14:32"
// A RatSpeak unit shows "[loc] ..." in the thread; Enter on an empty draft opens it on the
// map. Auto-share: per contact, OFF by default, NVS rs_ashare; sends only with a fresh fix,
// only after it moved > 50 m or two intervals passed; Check-in mode queues to the window.
constexpr const char* kLocPrefix = "My location: ";
constexpr const char* kLocLastPrefix = "My last location: ";
constexpr const char* kLocAutoPrefix = "Loc: ";
constexpr uint32_t kFreshFixMs = 120000;
constexpr int kAutoShareMax = 8;
const uint8_t kAutoMins[] = {0, 5, 15, 30, 60};           // 0 = off
const uint16_t kAutoForMin[] = {60, 240, 0};               // 0 = until turned off
const char* const kAutoForName[] = {"1 h", "4 h", "until off"};

struct AutoShare {
    uint8_t dest[16];
    uint8_t mins;            // interval
    uint8_t pad[3];
    uint32_t until;          // epoch s, 0 = until turned off
    uint32_t lastAt;         // epoch s of the last update sent
    float lastLat, lastLon;
};
AutoShare g_ashare[kAutoShareMax];
int g_ashareCount = 0;
uint8_t g_autoForSel = 1;    // duration picked in the thread menu (4 h)

bool gpsFresh() { return g_gpsFix && g_gpsPosValid && millis() - g_gpsPosAt < kFreshFixMs; }

void fmtAge(uint32_t s, char* out, size_t n) {
    if (s < 90) snprintf(out, n, "%u s", (unsigned)s);
    else if (s < 5400) snprintf(out, n, "%u min", (unsigned)((s + 30) / 60));
    else snprintf(out, n, "%u h", (unsigned)((s + 1800) / 3600));
}

// ---- Coordinate formats (S.1.12) ---------------------------------------------------
// Decimal degrees "43.64012,-70.92011", DMS "43 38'24"N 70 55'12"W", or MGRS to 10 m
// "19T CJ 4513 3368" (WGS84 / UTM; Norway + Svalbard zone exceptions). Messages always
// carry decimal degrees (machine-readable); MGRS is added to manual shares as text.
void mgrsText(double lat, double lon, char* out, size_t n, int prec = 4) {
    if (lat < -80 || lat > 84) { snprintf(out, n, "%.4f,%.4f", lat, lon); return; }   // UPS: not handled
    const double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f), ep2 = e2 / (1 - e2), k0 = 0.9996;
    int zone = (int)floor((lon + 180) / 6) + 1;
    if (lat >= 56 && lat < 64 && lon >= 3 && lon < 12) zone = 32;
    if (lat >= 72 && lat < 84) {
        if (lon >= 0 && lon < 9) zone = 31; else if (lon >= 9 && lon < 21) zone = 33;
        else if (lon >= 21 && lon < 33) zone = 35; else if (lon >= 33 && lon < 42) zone = 37;
    }
    const double r = 3.14159265358979 / 180.0;
    const double lon0 = ((zone - 1) * 6 - 180 + 3) * r, phi = lat * r, lam = lon * r;
    const double sp = sin(phi), cp = cos(phi), tp = tan(phi);
    const double N = a / sqrt(1 - e2 * sp * sp), T = tp * tp, C = ep2 * cp * cp, A = cp * (lam - lon0);
    const double e4 = e2 * e2, e6 = e4 * e2;
    const double M = a * ((1 - e2 / 4 - 3 * e4 / 64 - 5 * e6 / 256) * phi - (3 * e2 / 8 + 3 * e4 / 32 + 45 * e6 / 1024) * sin(2 * phi) +
                          (15 * e4 / 256 + 45 * e6 / 1024) * sin(4 * phi) - (35 * e6 / 3072) * sin(6 * phi));
    const double E = k0 * N * (A + (1 - T + C) * pow(A, 3) / 6 + (5 - 18 * T + T * T + 72 * C - 58 * ep2) * pow(A, 5) / 120) + 500000;
    double Nn = k0 * (M + N * tp * (A * A / 2 + (5 - T + 9 * C + 4 * C * C) * pow(A, 4) / 24 +
                                    (61 - 58 * T + T * T + 600 * C - 330 * ep2) * pow(A, 6) / 720));
    if (lat < 0) Nn += 10000000;
    const char band = "CDEFGHJKLMNPQRSTUVWXX"[(int)floor((lat + 80) / 8)];
    static const char* const kCol[3] = {"ABCDEFGH", "JKLMNPQR", "STUVWXYZ"};
    const int set = zone % 3;                // zones 1,4 -> A..H; 2,5 -> J..R; 3,6 -> S..Z
    const int ci = (int)floor(E / 100000) - 1;
    const char col = (ci >= 0 && ci < 8) ? kCol[set == 1 ? 0 : set == 2 ? 1 : 2][ci] : '?';
    static const char* const kRow = "ABCDEFGHJKLMNPQRSTUV";
    const char row = kRow[((int)floor(Nn / 100000) + (zone % 2 == 0 ? 5 : 0)) % 20];
    long e = (long)fmod(E, 100000), nn = (long)fmod(Nn, 100000);
    long d = 1;
    for (int k = prec; k < 5; k++) d *= 10;
    snprintf(out, n, "%d%c %c%c %0*ld %0*ld", zone, band, col, row, prec, e / d, prec, nn / d);
}

void fmtCoord(double lat, double lon, char* out, size_t n) {
    if (g_cfg.coordFmt == 2) { mgrsText(lat, lon, out, n); return; }
    if (g_cfg.coordFmt == 1) {
        auto dms = [](double v, int& d, int& m, int& s) {
            v = fabs(v); d = (int)v; const double mm = (v - d) * 60; m = (int)mm; s = (int)((mm - m) * 60 + 0.5);
            if (s == 60) { s = 0; m++; } if (m == 60) { m = 0; d++; }
        };
        int ad, am, as, od, om, os;
        dms(lat, ad, am, as); dms(lon, od, om, os);
        snprintf(out, n, "%d %02d'%02d\"%c %d %02d'%02d\"%c", ad, am, as, lat < 0 ? 'S' : 'N', od, om, os, lon < 0 ? 'W' : 'E');
        return;
    }
    snprintf(out, n, "%.5f,%.5f", lat, lon);
}

// One short line (fits the 240 px map footer): position, fix, age.
// What the GPS receiver is doing now: "GPS off", "GPS backup fault", "GPS on 45s 3/9 sats"
// (seconds into this visit, satellites heard / in view) or "GPS next 12m" between visits.
void gpsStateText(char* out, size_t n) {
    if (!g_cfg.gpsOn) { snprintf(out, n, "GPS off"); return; }
    if (g_pwEff.gps == power::GPS_OFF) { snprintf(out, n, "GPS off (power)"); return; }
    if (g_gpsBbrFault) { snprintf(out, n, "GPS backup fault"); return; }
    int inView = 0, heard = 0, best = 0;
    gpsSkySummary(inView, heard, best);
    if (g_gpsAllowed) {
        if (g_gpsVisitStart) snprintf(out, n, "GPS on %lus %d/%d sats",
                                      (unsigned long)((millis() - g_gpsVisitStart) / 1000), heard, inView);
        else snprintf(out, n, "GPS on %d/%d sats", heard, inView);
        return;
    }
    const int32_t left = (int32_t)(g_gpsNextVisit - millis());
    if (g_gpsNextVisit && left > 0) snprintf(out, n, "GPS next %lum", (unsigned long)((left + 59999) / 60000));
    else snprintf(out, n, "GPS idle");
}

void gpsPosText(char* out, size_t n) {
    if (gpsFresh()) {
        char c[40];
        fmtCoord(g_gpsLat, g_gpsLon, c, sizeof(c));
        snprintf(out, n, "%s %usat", c, (unsigned)g_gpsSats);
        return;
    }
    // No fresh fix: what the receiver is doing, after the age of the last fix (or "set" =
    // the saved position S.2 is what the map shows). Fits the 240 px footer.
    char st[28];
    gpsStateText(st, sizeof(st));
    if (g_gpsPosValid) {
        const uint32_t a = (millis() - g_gpsPosAt) / 1000;
        char age[8];
        if (a < 90) snprintf(age, sizeof(age), "%us", (unsigned)a);
        else if (a < 5400) snprintf(age, sizeof(age), "%um", (unsigned)((a + 30) / 60));
        else snprintf(age, sizeof(age), "%uh", (unsigned)((a + 1800) / 3600));
        snprintf(out, n, "fix %s old, %s", age, st);
    } else if (g_cfg.hasPos) {
        snprintf(out, n, "saved pos, %s", st);
    } else {
        snprintf(out, n, "no fix, %s", st);
    }
}

bool locParse(const char* s, size_t len, double& lat, double& lon, bool* autoUpdate) {
    char t[80];
    if (len >= sizeof(t)) len = sizeof(t) - 1;
    memcpy(t, s, len);
    t[len] = 0;
    const char* p = nullptr;
    bool au = false;
    if (!strncmp(t, kLocPrefix, strlen(kLocPrefix))) p = t + strlen(kLocPrefix);
    else if (!strncmp(t, kLocLastPrefix, strlen(kLocLastPrefix))) p = t + strlen(kLocLastPrefix);
    else if (!strncmp(t, kLocAutoPrefix, strlen(kLocAutoPrefix))) { p = t + strlen(kLocAutoPrefix); au = true; }
    if (!p || sscanf(p, "%lf, %lf", &lat, &lon) != 2) return false;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    if (autoUpdate) *autoUpdate = au;
    return true;
}

void localHHMM(char* out, size_t n) {
    const time_t now = time(nullptr);
    struct tm lt;
    applyTz();
    localtime_r(&now, &lt);
    snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// Location sends: queue for the next check-in window, else send now or queue in the outbox
// when the radio is busy (sendOrQueue).
SendResult locSendOrQueue(const uint8_t dest[16], const char* body) {
    const power::CheckinPhase ph = checkinNow();
    if (g_pwEff.checkin && !(ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS))
        return outboxAdd(dest, body) ? SendResult::Queued : SendResult::Failed;
    const SendResult r = sendOrQueue(dest, body);
    requestRedraw();                         // sendMessage cleared the LVGL widgets
    return r;
}

void shareMyLocation(const uint8_t dest[16]) {
    char body[96], hhmm[8], age[16];
    if (gpsFresh()) {
        localHHMM(hhmm, sizeof(hhmm));
        const int acc = g_gpsHdop > 0 ? (int)(g_gpsHdop * 5.0f + 0.5f) : 0;
        char grid[28] = "";
        if (g_cfg.coordFmt == 2) { mgrsText(g_gpsLat, g_gpsLon, grid, sizeof(grid)); strlcat(grid, ", ", sizeof(grid)); }
        if (acc > 0) snprintf(body, sizeof(body), "%s%.5f, %.5f (%s+-%d m, %s)", kLocPrefix, g_gpsLat, g_gpsLon, grid, acc < 3 ? 3 : acc, hhmm);
        else snprintf(body, sizeof(body), "%s%.5f, %.5f (%s%s)", kLocPrefix, g_gpsLat, g_gpsLon, grid, hhmm);
    } else if (g_gpsPosValid) {
        fmtAge((millis() - g_gpsPosAt) / 1000, age, sizeof(age));
        snprintf(body, sizeof(body), "%s%.5f, %.5f (%s old)", kLocLastPrefix, g_gpsLat, g_gpsLon, age);
    } else if (g_cfg.hasPos) {
        snprintf(body, sizeof(body), "%s%.5f, %.5f (set by hand)", kLocLastPrefix, g_cfg.lat, g_cfg.lon);
    } else {
        toast(g_cfg.gpsOn ? "No GPS position yet" : "GPS is off (S.1)");
        return;
    }
    const SendResult r = locSendOrQueue(dest, body);
    Serial.printf("[LOC] share %s: %s\n", r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "queued" : "failed", body);
    if (!gpsFresh() && g_cfg.gpsOn) gpsRequestVisit();       // the next share can be fresh
    toast(r == SendResult::Sent ? (gpsFresh() ? "Location sent" : "Last location sent") : r == SendResult::Queued ? "Location queued" : "Not sent: %s", g_txStr);
}

// ---- Auto-share list ---------------------------------------------------------------
void ashareSave() {
    Preferences p;
    if (!p.begin("rs_ashare", false)) return;
    p.putBytes("list", g_ashare, sizeof(AutoShare) * g_ashareCount);
    p.putUChar("n", (uint8_t)g_ashareCount);
    p.end();
}

void ashareLoad() {
    Preferences p;
    g_ashareCount = 0;
    if (!p.begin("rs_ashare", true)) return;
    const int n = p.getUChar("n", 0);
    if (n > 0 && n <= kAutoShareMax && p.getBytes("list", g_ashare, sizeof(AutoShare) * n) == sizeof(AutoShare) * (size_t)n)
        g_ashareCount = n;
    p.end();
}

int ashareFind(const uint8_t dest[16]) {
    for (int i = 0; i < g_ashareCount; i++) if (!memcmp(g_ashare[i].dest, dest, 16)) return i;
    return -1;
}

uint8_t ashareMinsFor(const uint8_t dest[16]) { const int i = ashareFind(dest); return i >= 0 ? g_ashare[i].mins : 0; }
int ashareActive() { return g_ashareCount; }

void ashareRemove(int i) {
    for (int k = i; k < g_ashareCount - 1; k++) g_ashare[k] = g_ashare[k + 1];
    g_ashareCount--;
}

void ashareStopAll() {
    g_ashareCount = 0;
    ashareSave();
    Serial.println("[LOC] auto-share: all stopped");
    requestRedraw();
}

// Next interval for this contact (off -> 5 -> 15 -> 30 -> 60 -> off).
void ashareCycle(const uint8_t dest[16]) {
    int i = ashareFind(dest);
    uint8_t cur = i >= 0 ? g_ashare[i].mins : 0;
    size_t k = 0;
    while (k < sizeof(kAutoMins) && kAutoMins[k] != cur) k++;
    const uint8_t next = kAutoMins[(k + 1) % sizeof(kAutoMins)];
    if (!next) {
        if (i >= 0) ashareRemove(i);
        toast("Auto-share off");
    } else {
        if (i < 0) {
            if (g_ashareCount >= kAutoShareMax) { toast("Max %d auto-shares", kAutoShareMax); return; }
            i = g_ashareCount++;
            memset(&g_ashare[i], 0, sizeof(AutoShare));
            memcpy(g_ashare[i].dest, dest, 16);
        }
        g_ashare[i].mins = next;
        const uint16_t forMin = kAutoForMin[g_autoForSel];
        const time_t now = time(nullptr);
        g_ashare[i].until = forMin ? (uint32_t)now + forMin * 60UL : 0;
        if (!g_cfg.gpsOn) toast("Every %u min; GPS is OFF (S.1)", (unsigned)next);
        else toast("Every %u min, %s", (unsigned)next, kAutoForName[g_autoForSel]);
    }
    ashareSave();
    Serial.printf("[LOC] auto-share %u min (%d on)\n", (unsigned)next, g_ashareCount);
    requestRedraw();
}

float metersBetween(double la1, double lo1, double la2, double lo2) {
    const double k = 3.14159265358979 / 180.0;
    const double x = (lo2 - lo1) * k * cos((la1 + la2) * 0.5 * k), y = (la2 - la1) * k;
    return (float)(sqrt(x * x + y * y) * 6371000.0);
}

void autoShareTick() {
    static uint32_t last = 0;
    if (!g_ashareCount || millis() - last < 5000) return;
    last = millis();
    const time_t now = time(nullptr);
    if (now < 1600000000) return;            // clock not set yet
    for (int i = 0; i < g_ashareCount; i++) {
        AutoShare& a = g_ashare[i];
        if (a.until && (uint32_t)now >= a.until) {
            char nm[24]; peerName(a.dest, nm, sizeof(nm));
            Serial.printf("[LOC] auto-share to %s ended\n", nm);
            toast("Auto-share to %.12s ended", nm);
            ashareRemove(i--);
            ashareSave();
            requestRedraw();
            continue;
        }
        const uint32_t iv = a.mins * 60UL;
        if (a.lastAt && (uint32_t)now - a.lastAt < iv) continue;
        if (!gpsFresh()) {                   // no fresh fix: power the GPS for a visit
            static uint32_t asked = 0;
            if (g_cfg.gpsOn && (!asked || millis() - asked > 180000)) { asked = millis(); gpsRequestVisit(); }
            continue;
        }
        const bool moved = !a.lastAt || metersBetween(a.lastLat, a.lastLon, g_gpsLat, g_gpsLon) > 50.0f;
        if (!moved && (uint32_t)now - a.lastAt < 2 * iv) continue;
        char body[64], hhmm[8];
        localHHMM(hhmm, sizeof(hhmm));
        snprintf(body, sizeof(body), "%s%.5f, %.5f %s", kLocAutoPrefix, g_gpsLat, g_gpsLon, hhmm);
        const SendResult r = locSendOrQueue(a.dest, body);
        Serial.printf("[LOC] auto %s: %s\n", r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "queued" : "failed", body);
        if (r != SendResult::Failed) {
            a.lastAt = (uint32_t)now;
            a.lastLat = (float)g_gpsLat;
            a.lastLon = (float)g_gpsLon;
            ashareSave();
            if (g_threadOpen && !memcmp(g_threadPeer, a.dest, 16)) loadHistory(g_threadPeer);
        }
        break;                               // one send per tick
    }
}

// ---- Contacts' last positions (map markers) ---------------------------------------------
// Every incoming location message (manual or auto share, any client that uses the same
// text) updates that contact's last position: NVS rs_cpos, newest 16 contacts. Fed by
// the protocol's committed-message notice (queued, read in the main loop) and by
// loadHistory() for messages that arrived before. Shown as markers on the map.
// num = the contact's map number (1-99): set by hand on the contact page (4 Map number),
// else the lowest free one when their first position arrives. hasPos = 0: a number set
// before any position (no marker yet).
struct CPos { uint8_t dest[16]; float lat, lon; uint32_t at; uint8_t num, hasPos, pad[2]; };
struct CPosV1 { uint8_t dest[16]; float lat, lon; uint32_t at; };   // NVS before 2026-11-12
constexpr int kCPosMax = 16;
CPos g_cpos[kCPosMax];
int g_cposCount = 0;
handheld::storage::RecordKey g_inQ[8];
int g_inQn = 0;

void cposSave() {
    Preferences p;
    if (!p.begin("rs_cpos", false)) return;
    p.putBytes("list", g_cpos, sizeof(CPos) * g_cposCount);
    p.putUChar("n", (uint8_t)g_cposCount);
    p.end();
}

void cposLoad() {
    Preferences p;
    g_cposCount = 0;
    if (!p.begin("rs_cpos", true)) return;
    const int n = p.getUChar("n", 0);
    const size_t len = p.getBytesLength("list");
    if (n > 0 && n <= kCPosMax && len == sizeof(CPos) * (size_t)n) {
        if (p.getBytes("list", g_cpos, len) == len) g_cposCount = n;
    } else if (n > 0 && n <= kCPosMax && len == sizeof(CPosV1) * (size_t)n) {   // migrate
        CPosV1 old[kCPosMax];
        if (p.getBytes("list", old, len) == len) {
            for (int i = 0; i < n; i++) {
                memset(&g_cpos[i], 0, sizeof(CPos));
                memcpy(g_cpos[i].dest, old[i].dest, 16);
                g_cpos[i].lat = old[i].lat; g_cpos[i].lon = old[i].lon; g_cpos[i].at = old[i].at;
                g_cpos[i].num = (uint8_t)(i + 1);
                g_cpos[i].hasPos = 1;
            }
            g_cposCount = n;
        }
    }
    p.end();
}

int cposFind(const uint8_t dest[16]) {
    for (int i = 0; i < g_cposCount; i++) if (!memcmp(g_cpos[i].dest, dest, 16)) return i;
    return -1;
}

int cposByNum(int num) {
    for (int i = 0; i < g_cposCount; i++) if (g_cpos[i].num == num) return i;
    return -1;
}

int cposWithPos() {
    int n = 0;
    for (int i = 0; i < g_cposCount; i++) n += g_cpos[i].hasPos ? 1 : 0;
    return n;
}

uint8_t cposFreeNum() {
    for (int n = 1; n < 100; n++) if (cposByNum(n) < 0) return (uint8_t)n;
    return 99;
}

// Slot for a contact (new one: lowest free number; full: replaces the oldest position).
int cposSlot(const uint8_t dest[16]) {
    int i = cposFind(dest);
    if (i >= 0) return i;
    if (g_cposCount < kCPosMax) i = g_cposCount++;
    else {
        i = 0;
        for (int k = 1; k < kCPosMax; k++) if (g_cpos[k].at < g_cpos[i].at) i = k;
    }
    memset(&g_cpos[i], 0, sizeof(CPos));
    memcpy(g_cpos[i].dest, dest, 16);
    g_cpos[i].num = cposFreeNum();
    return i;
}

// ---- Arrival alerts (Settings S.8 Alerts > Arrival alerts) ---------------------------------
// "Tell me when <contact> is within <distance> of <me | waypoint>": checked whenever their
// shared position arrives. A beep (+ vibration) and a 15 s message; once = then removed,
// repeat = again after they have been 25 % further away. Up to 8, NVS rs_geof.
// (the data is next to the waypoint data near the top)

void geofSave() {
    Preferences p;
    if (!p.begin("rs_geof", false)) return;
    p.putBytes("list", g_geof, sizeof(Geof) * g_geofCount);
    p.putUChar("n", (uint8_t)g_geofCount);
    p.end();
}

void geofLoad() {
    Preferences p;
    if (!p.begin("rs_geof", true)) return;
    const int n = p.getUChar("n", 0);
    g_geofCount = n <= kGeofMax && p.getBytesLength("list") == sizeof(Geof) * (size_t)n ? n : 0;
    if (g_geofCount) p.getBytes("list", g_geof, sizeof(Geof) * g_geofCount);
    p.end();
}

void geofText(const Geof& g, char* out, size_t n) {
    char nm[24], d[16];
    peerName(g.dest, nm, sizeof(nm));
    fmtDist(g.radius, d, sizeof(d));
    if (g.place) snprintf(out, n, "%.10s %s of %.10s", nm, d, g.wname);
    else snprintf(out, n, "%.12s within %s of me", nm, d);
}

// place 0 = me (wherever I am when their position arrives), n = waypoint n (its spot now).
bool geofAdd(const uint8_t dest[16], int place, uint16_t radius, bool repeat) {
    if (g_geofCount >= kGeofMax || radius < 10 || place < 0 || place > g_wptCount) return false;
    Geof& g = g_geof[g_geofCount];
    memset(&g, 0, sizeof(g));
    memcpy(g.dest, dest, 16);
    g.place = (uint8_t)place;
    if (place) {
        g.lat = g_wpt[place - 1].lat; g.lon = g_wpt[place - 1].lon;
        snprintf(g.wname, sizeof(g.wname), "W%d %s", place, g_wpt[place - 1].name);
    }
    g.radius = radius;
    g.repeat = repeat;
    g.armed = 1;
    g_geofCount++;
    geofSave();
    char t[40];
    geofText(g, t, sizeof(t));
    Serial.printf("[GEOF] new: %s%s\n", t, repeat ? " (repeat)" : "");
    return true;
}

void geofRemove(int i) {
    if (i < 0 || i >= g_geofCount) return;
    for (int k = i; k < g_geofCount - 1; k++) g_geof[k] = g_geof[k + 1];
    g_geofCount--;
    geofSave();
}

void geofCheck(const uint8_t dest[16], double lat, double lon) {
    bool changed = false;
    for (int i = 0; i < g_geofCount; i++) {
        Geof& g = g_geof[i];
        if (memcmp(g.dest, dest, 16) != 0) continue;
        double tla = g.lat, tlo = g.lon;
        if (!g.place && !ownPosition(tla, tlo)) continue;   // "near me" needs my position
        const float d = metersBetween(lat, lon, tla, tlo);
        if (!g.armed) {                                     // repeat: re-arm once well outside
            if (d > g.radius * 1.25f) { g.armed = 1; changed = true; }
            continue;
        }
        if (d > g.radius) continue;
        char nm[24], dd[16];
        peerName(dest, nm, sizeof(nm));
        fmtDist(d, dd, sizeof(dd));
        Serial.printf("[GEOF] %s is %s from %s\n", nm, dd, g.place ? g.wname : "me");
        if (g_cfg.beep) g_audio.playMessage();
        if (g_cfg.vibe) vibrate(200);
        toast("%.10s: %s from %.10s", nm, dd, g.place ? g.wname : "you");
        g_toastUntil = millis() + 15000;
        if (g.repeat) { g.armed = 0; changed = true; }
        else { geofRemove(i); i--; changed = false; }
        requestRedraw();
    }
    if (changed) geofSave();
}

// The New alert page, for a contact (nullptr = the first saved one).
void geofNewStart(const uint8_t* dest) {
    loadRows(SCR_CONTACTS);
    g_gnContactN = 0;
    g_gnContact = 0;
    for (int i = 0; i < g_rowCount && g_gnContactN < MAX_ROWS; i++) {
        if (isSelf(g_rows[i].peer)) continue;
        if (dest && !memcmp(dest, g_rows[i].peer, 16)) g_gnContact = g_gnContactN;
        memcpy(g_gnContacts[g_gnContactN++], g_rows[i].peer, 16);
    }
    g_gnPlace = g_wptSel >= 0 ? g_wptSel + 1 : 0;
    g_gnDist = 2;
    g_gnRepeat = false;
    pushPage(PG_GEOF_NEW);
}

void cposNote(const uint8_t dest[16], double lat, double lon, uint32_t at) {
    if (isSelf(dest)) return;
    const int found = cposFind(dest);
    if (found >= 0 && g_cpos[found].hasPos && g_cpos[found].at > at) return;   // have a newer one
    const int i = cposSlot(dest);
    g_cpos[i].lat = (float)lat;
    g_cpos[i].lon = (float)lon;
    g_cpos[i].at = at;
    g_cpos[i].hasPos = 1;
    cposSave();
    char nm[24];
    peerName(dest, nm, sizeof(nm));
    Serial.printf("[LOC] %s at %.5f,%.5f (#%u)\n", nm, lat, lon, (unsigned)g_cpos[i].num);
    geofCheck(dest, lat, lon);
    if (mapview::isOpen()) requestRedraw();
}

// Contact page "4 Map number": give this contact number n; whoever had it takes this
// contact's old number (or the lowest free one).
void cposAssign(const uint8_t dest[16], int n) {
    const int i = cposSlot(dest);
    const int other = cposByNum(n);
    if (other >= 0 && other != i) {
        g_cpos[other].num = g_cpos[i].num;
        if (g_cpos[other].num == n || !g_cpos[other].num) g_cpos[other].num = cposFreeNum();
    }
    g_cpos[i].num = (uint8_t)n;
    cposSave();
    char nm[24];
    peerName(dest, nm, sizeof(nm));
    Serial.printf("[LOC] %s is map #%d\n", nm, n);
}

// Text of a stored message (content only, NUL-terminated); false if busy / missing.
bool readRecordText(const handheld::storage::RecordKey& key, char* out, size_t cap, double* ts) {
    using namespace handheld::storage;
    auto rs = g_msgStore.requestRecord(key, 0, 256);
    if (!rs.accepted()) return false;
    MessageStore::Result rr;
    uint8_t buf[256];
    size_t got = 0;
    if (waitResult(rs.ticket, rr) && rr.outcome == Outcome::Committed && rr.error == Error::None) {
        got = rr.length < sizeof(buf) ? rr.length : sizeof(buf);
        if (!g_msgStore.readPayload(rs.ticket, buf, got)) got = 0;
    }
    g_msgStore.releaseResult(rs.ticket);
    if (got < sizeof(StoredRecordHeader)) return false;
    StoredRecordHeader hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    size_t off = sizeof(hdr) + hdr.titleLength;
    if (off > got) off = got;
    size_t clen = hdr.contentLength;
    if (off + clen > got) clen = got - off;
    if (clen >= cap) clen = cap - 1;
    memcpy(out, buf + off, clen);
    out[clen] = 0;
    if (ts) *ts = hdr.timestamp;
    return true;
}

// When a position is from: the sender's timestamp, but never later than our own clock (the
// two units' clocks differ; a "future" time would read as old / unknown age on the map).
uint32_t msgEpoch(double ts) {
    const time_t now = time(nullptr);
    const uint32_t mine = now > 1600000000 ? (uint32_t)now : 0;
    if (ts > 1.6e9 && ts < (double)now + 86400.0) return mine && (uint32_t)ts > mine ? mine : (uint32_t)ts;
    return mine;
}

// Called by the protocol (inside g_proto.loop()): only queue the key.
void onCommittedMessage(const LXMFManager::CommittedMessage& m) {
    if (!m.key.incoming || g_inQn >= (int)(sizeof(g_inQ) / sizeof(g_inQ[0]))) return;
    g_inQ[g_inQn++] = m.key;
}

void inboundTick() {
    if (!g_inQn || !g_storeReady) return;
    const handheld::storage::RecordKey key = g_inQ[0];
    char text[96];
    double ts = 0;
    if (!readRecordText(key, text, sizeof(text), &ts)) {
        static uint8_t tries = 0;
        if (++tries < 20) return;            // store busy: try again next loop
        tries = 0;
    } else {
        double la, lo;
        if (locParse(text, strlen(text), la, lo)) cposNote(key.peer, la, lo, msgEpoch(ts));
        wizCardIn(text);
    }
    for (int k = 1; k < g_inQn; k++) g_inQ[k - 1] = g_inQ[k];
    g_inQn--;
}

// ---- Contacts on the map ---------------------------------------------------------------
// Map key T: next contact with a known position (newest first), centred, its marker
// selected; the footer shows "Name 12.4km NE 8m ago". C / Enter: back to own position.
int g_mapSel = -1;                           // index into g_cpos, -1 = none

int cposOrder(int* idx) {                    // newest first
    int n = 0;
    for (int i = 0; i < g_cposCount; i++) if (g_cpos[i].hasPos) idx[n++] = i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && g_cpos[idx[b]].at > g_cpos[idx[b - 1]].at; b--) std::swap(idx[b], idx[b - 1]);
    return n;
}

bool ownPosition(double& lat, double& lon) {
    if (g_gpsPosValid) { lat = g_gpsLat; lon = g_gpsLon; return true; }
    if (g_cfg.hasPos) { lat = g_cfg.lat; lon = g_cfg.lon; return true; }
    return false;
}

const char* compass8(double deg) {
    static const char* k[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return k[(int)((deg + 22.5) / 45.0) & 7];
}

double bearingDeg(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979 / 180.0;
    const double y = sin((lo2 - lo1) * r) * cos(la2 * r);
    const double x = cos(la1 * r) * sin(la2 * r) - sin(la1 * r) * cos(la2 * r) * cos((lo2 - lo1) * r);
    double b = atan2(y, x) / r;
    return b < 0 ? b + 360.0 : b;
}

bool mapContactInfo(char* out, size_t n) {
    if (g_mapSel < 0 || g_mapSel >= g_cposCount || !g_cpos[g_mapSel].hasPos) return false;
    const CPos& c = g_cpos[g_mapSel];
    char nm[24], age[12] = "?", dist[24] = "";
    peerName(c.dest, nm, sizeof(nm));
    const time_t now = time(nullptr);
    if (c.at && now > (time_t)c.at) {
        const uint32_t a = (uint32_t)(now - c.at);
        if (a < 90) snprintf(age, sizeof(age), "%us", (unsigned)a);
        else if (a < 5400) snprintf(age, sizeof(age), "%um", (unsigned)((a + 30) / 60));
        else if (a < 172800) snprintf(age, sizeof(age), "%uh", (unsigned)((a + 1800) / 3600));
        else snprintf(age, sizeof(age), "%ud", (unsigned)((a + 43200) / 86400));
    }
    double la, lo;
    if (ownPosition(la, lo)) {
        const double m = metersBetween(la, lo, c.lat, c.lon);
        const char* dir = compass8(bearingDeg(la, lo, c.lat, c.lon));
        if (m < 1000) snprintf(dist, sizeof(dist), "%dm %s", (int)m, dir);
        else if (m < 100000) snprintf(dist, sizeof(dist), "%.1fkm %s", m / 1000.0, dir);
        else snprintf(dist, sizeof(dist), "%dkm %s", (int)(m / 1000.0), dir);
    } else {
        snprintf(dist, sizeof(dist), "%.3f,%.3f", c.lat, c.lon);
    }
    snprintf(out, n, "#%u %.9s %s %s ago", (unsigned)c.num, nm, dist, age);
    return true;
}

void mapCycleContact() {
    int idx[kCPosMax];
    const int n = cposOrder(idx);
    if (!n) { toast("No contact positions yet"); return; }
    int k = 0;
    while (k < n && idx[k] != g_mapSel) k++;
    g_mapSel = (k >= n) ? idx[0] : (k + 1 < n ? idx[k + 1] : -1);   // after the last: none
    if (g_mapSel >= 0) {
        mapview::centreOn(g_cpos[g_mapSel].lat, g_cpos[g_mapSel].lon);
        mapview::fitZoom();
        char info[56];
        mapContactInfo(info, sizeof(info));
        Serial.printf("[MAP] contact %s\n", info);
    } else {
        mapview::key(0, true);               // back to own position (follow)
        Serial.println("[MAP] contact: none");
    }
    g_fullRefreshReq = true;
    requestRedraw();
}

// Markers for every known contact position; name tags (LVGL labels on a white box)
// drawn over the stamped map, so only those small areas are redrawn.
int wptFeed(mapview::Marker* m, int n);

void mapContactsFeed() {
    mapview::Marker m[mapview::kMaxMarkers];
    const time_t now = time(nullptr);
    int n = 0;
    for (int i = 0; i < g_cposCount; i++) {
        if (!g_cpos[i].hasPos) continue;
        const bool fresh = g_cpos[i].at && now >= (time_t)g_cpos[i].at && now - (time_t)g_cpos[i].at < 3600;
        m[n++] = {g_cpos[i].lat, g_cpos[i].lon, i == g_mapSel, g_cpos[i].num, fresh, false, 0};
    }
    n = wptFeed(m, n);                       // waypoints (diamonds), track, guide line
    mapview::setMarkers(m, n);
}

// ---- Waypoints + breadcrumb track ------------------------------------------------------
// Waypoints: named places (up to 32, NVS rs_wpt), drawn as diamonds. Map keys: P = mark a
// waypoint at the map centre (name it, Enter), N = next waypoint (centred, selected, a
// dashed guide line from your position, footer "W2 Ballfield 2.1km SW"), Del twice =
// remove the selected one. Settings S.1.9 lists them (Enter = show on the map).
// Track: Settings S.1.10 on/off. Records a point when the GPS fix moved > 25 m (fresh fix
// only; in Balanced / Saver it asks the GPS for a visit every ~2 min, so points are
// sparser and the battery cost stays low). Last 1000 points in RAM, all of them appended
// to <SD>/ratdeckpro/tracks/track.bin (uint32 epoch, float lat, float lon); dotted line on
// the map. S.1.11 clears it.

void wptSave() {
    Preferences p;
    if (!p.begin("rs_wpt", false)) return;
    p.putBytes("list", g_wpt, sizeof(Wpt) * g_wptCount);
    p.putUChar("n", (uint8_t)g_wptCount);
    p.end();
}

void wptLoad() {
    Preferences p;
    g_wptCount = 0;
    if (!p.begin("rs_wpt", true)) return;
    const int n = p.getUChar("n", 0);
    if (n > 0 && n <= kWptMax && p.getBytes("list", g_wpt, sizeof(Wpt) * n) == sizeof(Wpt) * (size_t)n)
        g_wptCount = n;
    p.end();
}

double g_wptNewLat = 0, g_wptNewLon = 0;     // where P was pressed (while the name is typed)

void wptStartMark() {
    if (g_wptCount >= kWptMax) { toast("Max %d waypoints", kWptMax); return; }
    mapview::getCentre(g_wptNewLat, g_wptNewLon);
    char def[16];
    if (g_placeLast[0]) { strlcpy(def, g_placeLast, sizeof(def)); g_placeLast[0] = 0; }   // saved from F
    else snprintf(def, sizeof(def), "WP%d", g_wptCount + 1);
    startEntry(Entry::WptName, def);
    g_entryReplace = true;                   // Enter keeps "WP3", the first key replaces it
}

void wptAdd(const char* name) {
    if (g_wptCount >= kWptMax) return;
    Wpt& w = g_wpt[g_wptCount++];
    memset(&w, 0, sizeof(w));
    w.lat = (float)g_wptNewLat;
    w.lon = (float)g_wptNewLon;
    strlcpy(w.name, name[0] ? name : "WP", sizeof(w.name));
    const time_t now = time(nullptr);
    w.at = now > 1600000000 ? (uint32_t)now : 0;
    wptSave();
    g_wptSel = g_wptCount - 1;
    g_mapSel = -1;
    Serial.printf("[WPT] W%d %s at %.5f,%.5f\n", g_wptCount, w.name, w.lat, w.lon);
    requestRedraw();
}

void wptRemove(int i) {
    if (i < 0 || i >= g_wptCount) return;
    Serial.printf("[WPT] removed W%d %s\n", i + 1, g_wpt[i].name);
    for (int k = i; k < g_wptCount - 1; k++) g_wpt[k] = g_wpt[k + 1];
    g_wptCount--;
    g_wptSel = -1;
    wptSave();
    requestRedraw();
}

void distText(double la, double lo, double tla, double tlo, char* out, size_t n) {
    const double m = metersBetween(la, lo, tla, tlo);
    const char* dir = compass8(bearingDeg(la, lo, tla, tlo));
    if (m < 1000) snprintf(out, n, "%dm %s", (int)m, dir);
    else if (m < 100000) snprintf(out, n, "%.1fkm %s", m / 1000.0, dir);
    else snprintf(out, n, "%dkm %s", (int)(m / 1000.0), dir);
}

bool wptInfo(char* out, size_t n) {
    if (g_wptSel < 0 || g_wptSel >= g_wptCount) return false;
    const Wpt& w = g_wpt[g_wptSel];
    char d[24] = "";
    double la, lo;
    if (ownPosition(la, lo)) distText(la, lo, w.lat, w.lon, d, sizeof(d));
    else snprintf(d, sizeof(d), "%.4f,%.4f", w.lat, w.lon);
    snprintf(out, n, "W%d %.11s %s", g_wptSel + 1, w.name, d);
    return true;
}

void wptSelect(int i) {
    if (i < 0 || i >= g_wptCount) return;
    g_wptSel = i;
    g_mapSel = -1;
    mapview::centreOn(g_wpt[i].lat, g_wpt[i].lon);
    mapview::fitZoom();
    char info[64];
    wptInfo(info, sizeof(info));
    Serial.printf("[MAP] waypoint %s\n", info);
    g_fullRefreshReq = true;
    requestRedraw();
}

void wptCycle() {
    if (!g_wptCount) { toast("No waypoints: Alt+P marks one"); return; }
    const int next = g_wptSel + 1;
    if (next >= g_wptCount) {                // after the last: own position
        g_wptSel = -1;
        mapview::key(0, true);
        Serial.println("[MAP] waypoint: none");
        g_fullRefreshReq = true;
        requestRedraw();
        return;
    }
    wptSelect(next);
}

// ---- Track ---------------------------------------------------------------------------
constexpr const char* kTrackFile = SD_PATH_ROOT "/tracks/track.bin";
uint32_t g_trackLastAt = 0;                  // millis of the last point (every-10-min rule)
char g_trackGpx[40] = "";                    // last GPX export (forward-declared above) (shown on the Track page)

void trackPushRam(float la, float lo) {
    if (g_trackN >= kTrackRam) {             // drop the oldest
        memmove(g_track, g_track + 2, sizeof(float) * 2 * (kTrackRam - 1));
        g_trackN = kTrackRam - 1;
    }
    g_track[2 * g_trackN] = la;
    g_track[2 * g_trackN + 1] = lo;
    g_trackN++;
}

void trackLoad() {                           // last kTrackRam points from the SD file
    if (g_trackLoaded) return;
    g_trackLoaded = true;
    g_trackN = 0;
    if (!sdMount()) return;
    File f = g_sdStore.openFile(kTrackFile, FILE_READ);
    if (!f) return;
    const size_t n = f.size() / 12;
    const size_t first = n > (size_t)kTrackRam ? n - kTrackRam : 0;
    f.seek(first * 12);
    uint8_t rec[12];
    while (f.read(rec, 12) == 12) {
        float la, lo;
        memcpy(&la, rec + 4, 4);
        memcpy(&lo, rec + 8, 4);
        trackPushRam(la, lo);
    }
    f.close();
    Serial.printf("[TRACK] loaded %d of %u points\n", g_trackN, (unsigned)n);
}

void trackClear() {
    g_trackN = 0;
    if (sdMount()) {
        g_sdStore.remove(kTrackFile);
    }
    Serial.println("[TRACK] cleared");
    requestRedraw();
}

void trackAdd(double la, double lo) {
    trackPushRam((float)la, (float)lo);
    g_trackLastAt = millis() ? millis() : 1;
    if (sdMount()) {
        g_sdStore.ensureDir(SD_PATH_ROOT "/tracks");
        File f = g_sdStore.openFile(kTrackFile, FILE_APPEND);
        if (f) {
            const time_t now = time(nullptr);
            const uint32_t t = now > 1600000000 ? (uint32_t)now : 0;
            const float fla = (float)la, flo = (float)lo;
            uint8_t rec[12];
            memcpy(rec, &t, 4); memcpy(rec + 4, &fla, 4); memcpy(rec + 8, &flo, 4);
            f.write(rec, 12);
            f.close();
        }
    }
    Serial.printf("[TRACK] point %d %.5f,%.5f\n", g_trackN, la, lo);
    if (mapview::isOpen()) requestRedraw();
}

void trackTick() {
    if (!g_cfg.trackOn) return;
    static uint32_t last = 0, asked = 0;
    if (millis() - last < 5000) return;
    last = millis();
    if (!g_trackLoaded) trackLoad();
    if (!gpsFresh()) {                       // only modes that allow it wake the GPS for the track
        if (g_cfg.gpsOn && g_pwEff.trackGps && power::gpsIntervalMs(g_pwEff.gps) &&
            (!asked || millis() - asked > 120000)) { asked = millis(); gpsRequestVisit("track"); }
        return;
    }
    // A point when moved >= 20 m, or every 10 min while standing (shows where you waited).
    const bool moved = !g_trackN ||
        metersBetween(g_track[2 * g_trackN - 2], g_track[2 * g_trackN - 1], g_gpsLat, g_gpsLon) >= 20.0;
    const bool due = g_trackLastAt && millis() - g_trackLastAt >= 600000UL;
    if (!moved && !due) return;
    trackAdd(g_gpsLat, g_gpsLon);
}

// Markers + track for the map (called before every map draw).
int wptFeed(mapview::Marker* m, int n) {
    for (int i = 0; i < g_wptCount && n < mapview::kMaxMarkers; i++)
        m[n++] = {g_wpt[i].lat, g_wpt[i].lon, i == g_wptSel, (uint8_t)(i + 1), true, false, 1};
    if (g_cfg.trackOn && !g_trackLoaded) trackLoad();
    mapview::setTrack(g_track, g_trackN);
    if (g_tbOn && g_tbIdx >= 0 && g_tbIdx < g_trackN) mapview::setGuide(true, g_track[2 * g_tbIdx], g_track[2 * g_tbIdx + 1]);
    else if (g_wptSel >= 0 && g_wptSel < g_wptCount) mapview::setGuide(true, g_wpt[g_wptSel].lat, g_wpt[g_wptSel].lon);
    else mapview::setGuide(false);
    return n;
}

// ---- Place search (map key R) ----------------------------------------------------------
// Index on the SD card: <SD>/ratdeckpro/places/<first letter>.txt, lines
// "Name|lat|lon|K|ST" sorted by name (tools/places/make_places.py, USGS GNIS: every US
// town + summits, lakes, islands ... for the route states). R on the map: type the start
// of a name, Enter: the 8 nearest matches (to you, else the map centre) as a list; a digit
// jumps there; Q right after saves it as a waypoint with that name. ^ closes the list.
struct Place { char name[28]; float lat, lon; char kind, st[3]; };
constexpr int kPlaceShow = 6, kPlaceKeep = 48;
Place g_placeRes[kPlaceKeep];
int g_placeN = 0;
bool g_placeOpen = false;
char g_placeLast[28] = "";                   // name of the last place jumped to (P default)

const char* placeKindName(char k) {
    switch (k) {
        case 'P': return "town"; case 'S': return "summit"; case 'L': return "lake"; case 'I': return "island";
        case 'B': return "bay"; case 'F': return "falls"; case 'G': return "gap"; case 'C': return "cape";
        case 'H': return "beach"; default: return "";
    }
}

void placeSearch(const char* q0) {
    g_placeN = 0;
    // "knoxville tn": a last word of two letters filters by state.
    char q[28], stateF[3] = "";
    strlcpy(q, q0, sizeof(q));
    size_t ql = strlen(q);
    while (ql && q[ql - 1] == ' ') q[--ql] = 0;
    if (ql > 3 && q[ql - 3] == ' ' && isalpha((unsigned char)q[ql - 2]) && isalpha((unsigned char)q[ql - 1])) {
        stateF[0] = (char)toupper((unsigned char)q[ql - 2]);
        stateF[1] = (char)toupper((unsigned char)q[ql - 1]);
        q[ql - 3] = 0;
        ql -= 3;
    }
    if (!ql) return;
    char low[28];
    size_t k = 0;
    for (; k < ql && k < sizeof(low) - 1; k++) low[k] = (char)tolower((unsigned char)q[k]);
    low[k] = 0;
    const char c0 = (low[0] >= 'a' && low[0] <= 'z') ? low[0] : '0';
    char path[64];
    snprintf(path, sizeof(path), "%s/places/%c.txt", SD_PATH_ROOT, c0);
    if (!sdMount()) { toast("No SD card"); return; }
    File f = g_sdStore.openFile(path, FILE_READ);
    if (!f) { toast("No place index on the card"); return; }
    double ola = 0, olo = 0;
    if (!ownPosition(ola, olo)) mapview::getCentre(ola, olo);
    char line[96];
    const uint32_t t0 = millis();
    static uint8_t rb[2048];                 // buffered line reader (works on the simulator too)
    int rpos = 0, rlen = 0;
    auto readLine = [&](char* out, size_t cap) -> bool {
        size_t n = 0;
        for (;;) {
            if (rpos >= rlen) {
                rlen = f.read(rb, sizeof(rb));
                rpos = 0;
                if (rlen <= 0) { out[n] = 0; return n > 0; }
            }
            const char ch = (char)rb[rpos++];
            if (ch == '\n') { out[n] = 0; return true; }
            if (n < cap - 1) out[n++] = ch;
        }
    };
    while (readLine(line, sizeof(line))) {
        size_t i = 0;                        // compare the name's start, case-insensitive
        while (i < k && line[i] && line[i] != '|' && tolower((unsigned char)line[i]) == low[i]) i++;
        if (i < k) {
            // sorted by name: once past the query, stop
            if (line[i] && line[i] != '|' && tolower((unsigned char)line[i]) > low[i]) break;
            continue;
        }
        Place p{};
        char* s1 = strchr(line, '|');
        if (!s1) continue;
        *s1 = 0;
        float la = 0, lo = 0;
        char kind = 0, st[3] = "";
        if (sscanf(s1 + 1, "%f|%f|%c|%2s", &la, &lo, &kind, st) < 3) continue;
        if (stateF[0] && strcmp(st, stateF) != 0) continue;
        strlcpy(p.name, line, sizeof(p.name));
        p.lat = la; p.lon = lo; p.kind = kind;
        strlcpy(p.st, st, sizeof(p.st));
        if (g_placeN < kPlaceKeep) g_placeRes[g_placeN++] = p;
        else {                               // keep the nearest kPlaceKeep
            int far = 0;
            double fd = -1;
            for (int j = 0; j < kPlaceKeep; j++) {
                const double d = metersBetween(ola, olo, g_placeRes[j].lat, g_placeRes[j].lon);
                if (d > fd) { fd = d; far = j; }
            }
            if (metersBetween(ola, olo, la, lo) < fd) g_placeRes[far] = p;
        }
        if ((g_placeN & 63) == 0) yield();
    }
    f.close();
    for (int a2 = 1; a2 < g_placeN; a2++)    // nearest first
        for (int b = a2; b > 0 && metersBetween(ola, olo, g_placeRes[b].lat, g_placeRes[b].lon) <
                                      metersBetween(ola, olo, g_placeRes[b - 1].lat, g_placeRes[b - 1].lon); b--)
            std::swap(g_placeRes[b], g_placeRes[b - 1]);
    Serial.printf("[PLACE] \"%s\"%s%s: %d match%s in %lu ms\n", q, stateF[0] ? " in " : "", stateF, g_placeN,
                  g_placeN == 1 ? "" : "es",
                  (unsigned long)(millis() - t0));
    if (!g_placeN) { toast("No place: %.16s", q); return; }
    g_placeOpen = true;
    g_fullRefreshReq = true;
    requestRedraw();
}

void placeJump(int i) {
    if (i < 0 || i >= g_placeN || i >= kPlaceShow) return;
    const Place& p = g_placeRes[i];
    g_placeOpen = false;
    g_mapSel = -1;
    g_wptSel = -1;
    mapview::centreOn(p.lat, p.lon);
    mapview::fitZoom();
    strlcpy(g_placeLast, p.name, sizeof(g_placeLast));
    Serial.printf("[PLACE] go %s %s %.5f,%.5f\n", p.name, p.st, p.lat, p.lon);
    toast("%.16s %s  (P = save)", p.name, p.st);
    g_fullRefreshReq = true;
    requestRedraw();
}

void placeKey(const KeyEvent& ev) {
    const char c = ev.character;
    if (ev.up) { g_placeOpen = false; g_fullRefreshReq = true; requestRedraw(); return; }
    if (c >= '1' && c <= '6') { placeJump(c - '1'); return; }
    if (ev.enter) { placeJump(0); return; }
}

// Results list in place of the map (renderMapHeader draws it; stampMap skips the map).
void renderPlaces() {
    double ola = 0, olo = 0;
    const bool own = ownPosition(ola, olo);
    const int rowH = 34;                     // 6 x 34 = 204 px: two lines per place
    for (int i = 0; i < g_placeN && i < kPlaceShow; i++) {
        const Place& p = g_placeRes[i];
        char t[48], d[24] = "", k[40];
        if (own) distText(ola, olo, p.lat, p.lon, d, sizeof(d));
        snprintf(t, sizeof(t), "%d %s, %s", i + 1, p.name, p.st);
        snprintf(k, sizeof(k), "   %s  %s", placeKindName(p.kind), d);
        const int y = LIST_TOP + i * rowH;
        text(4, y, t, true, W - 8);
        text(4, y + 16, k, false, W - 8);
        fillRect(4, y + rowH - 1, W - 8, 1);
    }
}

// Map keys for waypoints. Returns true if used.
bool wptMapKey(const KeyEvent& ev) {
    const char c = (char)toupper((unsigned char)ev.character);
    if (c == kKeyWpt && ev.alt) { wptStartMark(); return true; }   // Alt+P: add one here
    if (c == kKeyWpt) { wptCycle(); return true; }                 // P: next waypoint
    if (c == kKeyContact) g_wptSel = -1;     // contacts take over
    if (isAltSym(ev)) {                      // Alt+SYM twice within 3 s: delete the selected one
        if (g_wptSel < 0) { toast("P selects a waypoint first"); return true; }
        if (g_wptDelAt && millis() - g_wptDelAt < 3000) { g_wptDelAt = 0; wptRemove(g_wptSel); toast("Waypoint removed"); }
        else { g_wptDelAt = millis(); toast("Alt+SYM again: remove W%d", g_wptSel + 1); }
        return true;
    }
    if (c == 'D' || ev.enter) g_wptSel = -1;
    return false;
}


// The marker carries the contact's number (no name tags: the screen is small); the
// selected contact's name, distance and age are on the map's bottom line.
void mapContactsStamp() {}

int g_mapNum = 0;                            // digits typed on the map: open that chat

void openThread(const uint8_t peer[16]);
void openMap();
void closeThread();

// Map centred on a contact's last position, its marker selected (lists: L, chat menu 5).
bool mapShowContact(const uint8_t dest[16]) {
    const int i = cposFind(dest);
    if (i < 0 || !g_cpos[i].hasPos) { toast("No position from them yet"); return false; }
    if (!mapview::isOpen()) openMap();
    g_mapSel = i;
    mapview::centreOn(g_cpos[i].lat, g_cpos[i].lon);
    mapview::fitZoom();
    g_fullRefreshReq = true;
    requestRedraw();
    char info[64];
    mapContactInfo(info, sizeof(info));
    Serial.printf("[MAP] contact %s\n", info);
    return true;
}

// Digits + Enter on the map: open contact #n's chat. Returns true if the key was used.
bool mapNumberKey(const KeyEvent& ev) {
    const char c = ev.character;
    if (c >= '0' && c <= '9' && !ev.enter) {
        g_mapNum = (g_mapNum * 10 + (c - '0')) % 100;
        requestRedraw();
        return true;
    }
    if (ev.del && g_mapNum) { g_mapNum /= 10; requestRedraw(); return true; }
    if (ev.enter && g_mapNum) {
        const int n = g_mapNum;
        g_mapNum = 0;
        const int k = cposByNum(n);
        if (k < 0) { toast("No contact #%d on the map", n); requestRedraw(); return true; }
        uint8_t peer[16];
        memcpy(peer, g_cpos[k].dest, 16);
        Serial.printf("[MAP] open chat #%d\n", n);
        closeMap();
        openThread(peer);
        return true;
    }
    return false;
}

// ---- Thread menu (MIC in a conversation) -----------------------------------------------
bool g_tmenuOpen = false;
int  g_tmenuSel = 0;
constexpr int kTmenuRows = 5;

void tmenuAct(int i) {
    if (i == 0) { g_tmenuOpen = false; shareMyLocation(g_threadPeer); loadHistory(g_threadPeer); }
    else if (i == 1) ashareCycle(g_threadPeer);
    else if (i == 2) {
        g_autoForSel = (uint8_t)((g_autoForSel + 1) % 3);
        const int k = ashareFind(g_threadPeer);
        if (k >= 0) {                        // applies to a running share too
            const uint16_t m = kAutoForMin[g_autoForSel];
            g_ashare[k].until = m ? (uint32_t)time(nullptr) + m * 60UL : 0;
            ashareSave();
        }
    } else if (i == 3) { if (g_ashareCount) { ashareStopAll(); toast("All auto-sharing stopped"); } else toast("Nothing to stop"); }
    else if (i == 4) {
        uint8_t peer[16];
        memcpy(peer, g_threadPeer, 16);
        if (cposFind(peer) < 0 || !g_cpos[cposFind(peer)].hasPos) { toast("No position from them yet"); return; }
        g_tmenuOpen = false;
        closeThread();
        mapShowContact(peer);
        return;
    }
    requestRedraw();
}

void tmenuKey(const KeyEvent& ev) {
    if (ev.up || ev.fn) { g_tmenuOpen = false; requestRedraw(); return; }
    if (ev.enter) { tmenuAct(g_tmenuSel); return; }
    const char c = (char)toupper((unsigned char)ev.character);
    if (c >= '1' && c <= '0' + kTmenuRows) { g_tmenuSel = c - '1'; tmenuAct(g_tmenuSel); return; }
    if (c == 'G' && g_tmenuSel > 0) g_tmenuSel--;
    if (c == 'V' && g_tmenuSel < kTmenuRows - 1) g_tmenuSel++;
    requestRedraw();
}

void renderThreadMenu(int y) {
    char sub[40], a[40];
    const int k = ashareFind(g_threadPeer);
    gpsPosText(a, sizeof(a));
    snprintf(sub, sizeof(sub), "%s", a);
    listRow(y, "1 Share my location", sub, g_tmenuSel == 0);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 0); y += ROW_H;
    if (k >= 0) {
        char left[16] = "until off";
        if (g_ashare[k].until) {
            const int32_t s = (int32_t)(g_ashare[k].until - (uint32_t)time(nullptr));
            fmtAge(s > 0 ? (uint32_t)s : 0, left, sizeof(left));
            strlcat(left, " left", sizeof(left));
        }
        snprintf(sub, sizeof(sub), "every %u min, %s", (unsigned)g_ashare[k].mins, left);
    } else {
        snprintf(sub, sizeof(sub), "off (Enter: 5/15/30/60 min)");
    }
    listRow(y, "2 Auto-share location", sub, g_tmenuSel == 1);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 1); y += ROW_H;
    listRow(y, "3 Auto-share for", kAutoForName[g_autoForSel], g_tmenuSel == 2);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 2); y += ROW_H;
    snprintf(sub, sizeof(sub), "%d contact%s on", g_ashareCount, g_ashareCount == 1 ? "" : "s");
    listRow(y, "4 Stop all auto-sharing", sub, g_tmenuSel == 3);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 3); y += ROW_H;
    const int ci = cposFind(g_threadPeer);
    if (ci >= 0 && g_cpos[ci].hasPos) {
        const time_t now = time(nullptr);
        const uint32_t a = (g_cpos[ci].at && now > (time_t)g_cpos[ci].at) ? (uint32_t)(now - g_cpos[ci].at) : 0;
        char age[12];
        fmtAge(a, age, sizeof(age));
        snprintf(sub, sizeof(sub), "#%u, %s old", (unsigned)g_cpos[ci].num, g_cpos[ci].at ? age : "?");
    } else {
        snprintf(sub, sizeof(sub), "no position from them yet");
    }
    listRow(y, "5 Show them on the map", sub, g_tmenuSel == 4);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 4);
}

void openMenu();

// ---- Library (reference texts on the SD card) -----------------------------------------
// MENU -> Library: the files in <SD>/ratdeckpro/library/ (plain text: frequency card,
// GhostNet / JS8Call notes, weather radio list ...). Enter / digit opens one. Reader keys:
// Space / N / V next page, B / G previous, E / X up / down half a page, W wrap on/off (off
// for .csv: each file line whole, cut at the screen edge), S / F pan left / right when wrap
// is off, D back to the left edge, R find (case-insensitive, from the next page on), K the
// keys card, ^ back to the list / close. Copy files with tools/sdcard/push_files.py.
constexpr const char* kLibDir = SD_PATH_ROOT "/library";
constexpr int kLibMaxFiles = 24, kLibLines = 12, kLibLineH = 17;
struct LibFile { char name[40]; uint32_t size; };
LibFile g_libFiles[kLibMaxFiles];
int g_libCount = 0, g_libSel = 0, g_libScroll = 0;
bool g_libOpen = false, g_libReading = false;
char g_libPath[96] = "";
uint32_t g_libSize = 0;
std::vector<uint32_t> g_libPages;            // start offset of every page read so far
constexpr int kLibLineMax = 256;             // chars kept per line (wrap off: room to pan)
char g_libText[kLibLines][kLibLineMax];      // the current page
uint32_t g_libLineOff[kLibLines];            // file offset where each shown line starts
int g_libTextN = 0;
uint32_t g_libNext = 0;                      // offset after the current page
bool g_libWrap = true;                       // false: one file line per screen line, panned
int g_libCol = 0;                            // wrap off: first character shown
constexpr int kLibPanCols = 12;              // S / F: about half a screen

void libScan() {
    g_libCount = 0;
    if (!sdMount()) return;
    g_sdStore.ensureDir(kLibDir);
    File d = g_sdStore.openDir(kLibDir);
    if (!d) return;
    for (File e = d.openNextFile(); e && g_libCount < kLibMaxFiles; e = d.openNextFile()) {
        if (!e.isDirectory()) {
            const char* nm = strrchr(e.name(), '/');
            nm = nm ? nm + 1 : e.name();
            if (nm[0] != '.') {
                strlcpy(g_libFiles[g_libCount].name, nm, sizeof(g_libFiles[0].name));
                g_libFiles[g_libCount].size = (uint32_t)e.size();
                g_libCount++;
            }
        }
        e.close();
    }
    d.close();
    for (int a = 1; a < g_libCount; a++)     // by name
        for (int b = a; b > 0 && strcasecmp(g_libFiles[b].name, g_libFiles[b - 1].name) < 0; b--)
            std::swap(g_libFiles[b], g_libFiles[b - 1]);
}

// UTF-8 punctuation the font may not have -> ASCII; other non-ASCII -> '?'.
int libAscii(const uint8_t* p, int n, char& out) {
    if (p[0] < 0x80) { out = (char)p[0]; return 1; }
    if (n >= 3 && p[0] == 0xE2 && p[1] == 0x80) {
        const uint8_t c = p[2];
        out = (c == 0x98 || c == 0x99) ? '\'' : (c == 0x9C || c == 0x9D) ? '"' : (c == 0x93 || c == 0x94) ? '-' :
              (c == 0xA2) ? '*' : (c == 0xA6) ? '.' : '?';
        return 3;
    }
    int len = (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 1;
    if (len > n) len = n;
    out = '?';
    return len;
}

// Lay out text from `off`: up to maxLines lines (wrap on: at most W-12 px, broken at the last
// space; wrap off: one file line each, the first kLibLineMax-1 chars kept). Line i's text
// goes to text[i] (if text) and its file offset to starts[i]; returns the line count and
// sets *next to the offset after the last line.
int libLayout(uint32_t off, int maxLines, char (*text)[kLibLineMax], uint32_t* starts, uint32_t* next) {
    *next = off;
    File f = g_sdStore.openFile(g_libPath, FILE_READ);
    if (!f) return 0;
    f.seek(off);
    static uint8_t buf[2048];
    const int n = f.read(buf, sizeof(buf));
    f.close();
    if (n <= 0) return 0;
    const int maxW = W - 12;
    int count = 0, i = 0, ll = 0, lastSpace = -1, lastSpaceIn = 0, lineStartIn = 0;
    char line[kLibLineMax];
    auto emit = [&](int len) {
        line[len] = 0;
        if (text) strlcpy(text[count], line, kLibLineMax);
        if (starts) starts[count] = off + (uint32_t)lineStartIn;
        count++;
    };
    while (i < n && count < maxLines) {
        char c;
        const int used = libAscii(buf + i, n - i, c);
        if (c == '\r') { i += used; continue; }
        if (c == '\t') c = ' ';
        if (c == '\n') {
            emit(ll);
            i += used;
            ll = 0; lastSpace = -1;
            lineStartIn = i;
            continue;
        }
        if (!g_libWrap) {                         // keep the start of a long line, skip the rest
            if (ll < kLibLineMax - 1) line[ll++] = c;
            i += used;
            continue;
        }
        line[ll] = c;
        line[ll + 1] = 0;
        if (ll >= 39 || textW(line) > maxW) {     // wrap at the last space, else hard
            if (lastSpace > 0) { emit(lastSpace); i = lastSpaceIn + 1; }
            else emit(ll);
            ll = 0; lastSpace = -1;
            lineStartIn = i;
            continue;
        }
        if (c == ' ') { lastSpace = ll; lastSpaceIn = i; }
        ll++;
        i += used;
    }
    if (count < maxLines && ll > 0 && i >= n) {   // end of the chunk (= end of file if short)
        if (n < (int)sizeof(buf) || count == 0) { emit(ll); lineStartIn = i; }
    }
    *next = off + (uint32_t)lineStartIn;
    if (count == 0 && n == (int)sizeof(buf)) *next = off + (uint32_t)n;   // one line longer than the chunk
    return count;
}

void libLoadPage(uint32_t off) {
    g_libTextN = libLayout(off, kLibLines, g_libText, g_libLineOff, &g_libNext);
}

void libOpenFile(int k) {
    if (k < 0 || k >= g_libCount) return;
    snprintf(g_libPath, sizeof(g_libPath), "%s/%s", kLibDir, g_libFiles[k].name);
    g_libSize = g_libFiles[k].size;
    const char* dot = strrchr(g_libFiles[k].name, '.');
    g_libWrap = !(dot && !strcasecmp(dot, ".csv"));   // tables: no wrap, pan instead
    g_libCol = 0;
    g_libPages.clear();
    g_libPages.push_back(0);
    libLoadPage(0);
    g_libReading = true;
    Serial.printf("[LIB] open %s (%u bytes)\n", g_libFiles[k].name, (unsigned)g_libSize);
    g_fullRefreshReq = true;
    requestRedraw();
}

void libOpen() {
    if (g_menuOpen) g_menuOpen = false;
    libScan();
    g_libOpen = true;
    g_libReading = false;
    g_libSel = 0;
    g_libScroll = 0;
    Serial.printf("[LIB] %d files\n", g_libCount);
    g_fullRefreshReq = true;
    requestRedraw();
}

// The page being read, for the idle screen "Library page".
void libRemember() {
    if (!g_libReading || g_libPages.empty()) return;
    const char* nm = strrchr(g_libPath, '/');
    strlcpy(g_cfg.libLast, nm ? nm + 1 : g_libPath, sizeof(g_cfg.libLast));
    g_cfg.libLastOff = g_libPages.back();
    g_cfg.libLastWrap = g_libWrap;
    saveConfig();
}

void libClose() {
    libRemember();
    g_libOpen = false; g_libReading = false; g_navKeysOpen = false;
    g_libPages.clear(); g_libPages.shrink_to_fit();
    requestRedraw();
}

// E / X: half a page (6 lines) up / down. Down pushes the 7th line's offset; up lays out
// again from the previous stop and goes back 6 lines from the top one.
void libHalf(int dir) {
    const int half = kLibLines / 2;
    if (dir > 0) {
        if (g_libTextN <= half || g_libNext >= g_libSize) { toast("End of file"); return; }
        g_libPages.push_back(g_libLineOff[half]);
    } else {
        if (g_libPages.size() <= 1) { toast("Start of file"); return; }
        const uint32_t top = g_libPages.back();
        const uint32_t prev = g_libPages[g_libPages.size() - 2];
        static uint32_t st[64];
        uint32_t at = prev;
        int n = 0;
        while (at < top && n < 64) {              // line starts from prev up to top
            uint32_t nx;
            const int got = libLayout(at, 64 - n, nullptr, st + n, &nx);
            if (!got || nx <= at) break;
            int keep = got;
            while (keep > 0 && st[n + keep - 1] >= top) keep--;
            n += keep;
            if (keep < got) break;
            at = nx;
        }
        g_libPages.pop_back();
        const uint32_t to = n > half ? st[n - half] : prev;
        if (to > prev) g_libPages.push_back(to);
    }
    libLoadPage(g_libPages.back());
    requestRedraw();
}

void libPage(int dir) {
    if (dir > 0) {
        if (g_libNext >= g_libSize || g_libNext <= g_libPages.back()) { toast("End of file"); return; }
        g_libPages.push_back(g_libNext);
    } else {
        if (g_libPages.size() <= 1) { toast("Start of file"); return; }
        g_libPages.pop_back();
    }
    libLoadPage(g_libPages.back());
    requestRedraw();
}

// Find (case-insensitive) after the current page; jumps to the line that holds it.
void libFind(const char* what) {
    const size_t wl = strlen(what);
    if (!wl) return;
    File f = g_sdStore.openFile(g_libPath, FILE_READ);
    if (!f) return;
    static uint8_t buf[2048 + 64];
    uint32_t pos = g_libPages.back() + 1;
    int32_t found = -1;
    while (pos < g_libSize && found < 0) {
        f.seek(pos);
        const int n = f.read(buf, 2048 + 63);
        if (n <= 0) break;
        for (int i = 0; i + (int)wl <= n; i++) {
            if (!strncasecmp((const char*)buf + i, what, wl)) { found = (int32_t)(pos + i); break; }
        }
        if (n < (int)wl) break;
        pos += (uint32_t)(n - (int)wl + 1);
        if (n < 2048) break;
    }
    if (found < 0) { f.close(); toast("Not found: %.14s", what); return; }
    uint32_t ls = (uint32_t)found;           // back to the start of that line (max 200 bytes)
    for (int k = 0; k < 200 && ls > 0; k++) {
        f.seek(ls - 1);
        if (f.read() == '\n') break;
        ls--;
    }
    f.close();
    g_libPages.push_back(ls);
    libLoadPage(ls);
    Serial.printf("[LIB] found \"%s\" at %ld\n", what, (long)found);
    requestRedraw();
}

bool gameOpen();
bool chessOpen();
void libKey(const KeyEvent& ev) {
    const char c = (char)toupper((unsigned char)ev.character);
    if (c == kKeyNav) { navKeysToggle(); return; }
    if (g_libReading) {
        if (c == 'N' || c == 'V' || ev.space) libPage(+1);
        else if (c == 'B' || c == 'G') libPage(-1);
        else if (c == 'X') libHalf(+1);
        else if (c == 'E') libHalf(-1);
        else if (c == 'L') startEntry(Entry::LibFind);
        else if (c == 'W') {
            g_libWrap = !g_libWrap;
            g_libCol = 0;
            libLoadPage(g_libPages.back());
            toast(g_libWrap ? "Wrap on" : "Wrap off: S / F pan");
            Serial.printf("[LIB] wrap %s\n", g_libWrap ? "on" : "off");
            requestRedraw();
        } else if ((c == 'S' || c == 'F') && !g_libWrap) {
            g_libCol += c == 'F' ? kLibPanCols : -kLibPanCols;
            if (g_libCol < 0) g_libCol = 0;
            if (g_libCol > kLibLineMax - 2) g_libCol = kLibLineMax - 2;
            Serial.printf("[LIB] column %d\n", g_libCol);
            requestRedraw();
        } else if (c == 'D') { g_libCol = 0; requestRedraw(); }
        else if (c == 'M') { libClose(); openMenu(); }
        return;
    }
    auto pick = [](int k) {                      // the rows after the files are the games
        if (k == g_libCount) { if (gameOpen()) { g_libOpen = false; } else requestRedraw(); }
        else if (k == g_libCount + 1) { if (chessOpen()) g_libOpen = false; }
        else libOpenFile(k);
    };
    if (ev.enter) { pick(g_libSel); return; }
    if (c >= '1' && c <= '9') { const int k = g_libScroll + (c - '1'); if (k <= g_libCount + 1) { g_libSel = k; pick(k); } return; }
    if (c == 'V' && g_libSel < g_libCount + 1) g_libSel++;
    if (c == 'G' && g_libSel > 0) g_libSel--;
    if (c == 'M') { libClose(); openMenu(); return; }
    if (g_libSel < g_libScroll) g_libScroll = g_libSel;
    if (g_libSel >= g_libScroll + 4) g_libScroll = g_libSel - 3;
    requestRedraw();
}

void renderLibrary() {
    char hdr[48];
    if (!g_libReading) {
        drawHeader("Library", false);
        if (!g_libCount) {
            text(8, LIST_TOP + ROW_H + 8, "No files in ratdeckpro/library", false, W - 16);
            text(8, LIST_TOP + ROW_H + 30, "on the SD card (push_files.py)", false, W - 16);
        }
        for (int r = 0; r < 4 && g_libScroll + r <= g_libCount + 1; r++) {
            const int k = g_libScroll + r;
            char t[48], sub[24];
            if (k == g_libCount + 1) {
                snprintf(t, sizeof(t), "%d Chess (game)", r + 1);
                listRow(LIST_TOP + r * ROW_H, t, "play the unit; 4 levels", k == g_libSel);
                continue;
            }
            if (k == g_libCount) {
                snprintf(t, sizeof(t), "%d Rat Trap (game)", r + 1);
                listRow(LIST_TOP + r * ROW_H, t, "a short puzzle; battery 25%+", k == g_libSel);
                continue;
            }
            snprintf(t, sizeof(t), "%d %s", r + 1, g_libFiles[k].name);
            const uint32_t sz = g_libFiles[k].size;
            if (sz < 2048) snprintf(sub, sizeof(sub), "%u bytes", (unsigned)sz);
            else snprintf(sub, sizeof(sub), "%u KB", (unsigned)((sz + 512) / 1024));
            listRow(LIST_TOP + r * ROW_H, t, sub, k == g_libSel);
        }
        box(0, FOOT_Y, W, FOOT_H, 1);
        label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : "1-4 / Enter  G/V  K keys");
        if (g_navKeysOpen) drawNavKeys(FOOT_Y - kNavH, kLibListKeys);
        return;
    }
    const char* nm = strrchr(g_libPath, '/');
    nm = nm ? nm + 1 : g_libPath;
    const unsigned pct = g_libSize ? (unsigned)((uint64_t)g_libPages.back() * 100 / g_libSize) : 0;
    if (g_libCol) snprintf(hdr, sizeof(hdr), "p%u %u%% >%d %s", (unsigned)g_libPages.size(), pct, g_libCol, nm);
    else snprintf(hdr, sizeof(hdr), "p%u %u%% %s", (unsigned)g_libPages.size(), pct, nm);
    drawHeader(hdr, false);
    for (int l = 0; l < g_libTextN; l++) {
        const char* t = g_libText[l];
        const int len = (int)strlen(t);
        text(6, LIST_TOP + l * kLibLineH, g_libWrap ? t : (g_libCol < len ? t + g_libCol : ""), false, W - 10);
    }
    box(0, FOOT_Y, W, FOOT_H, 1);
    if (g_entry == Entry::LibFind) {
        char t[48];
        snprintf(t, sizeof(t), "Find: %s_", g_buf);
        label(0, FOOT_Y - 1, W, FOOT_H, t);
    } else {
        label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : "K keys  L find  ^ list");
    }
    if (g_navKeysOpen) drawNavKeys(FOOT_Y - kNavH, kReaderKeys);
}

// ---- Rat Trap (a small puzzle game, MENU -> Library, last row) -----------------------
// A 10 x 9 field hides 12 traps. Open every safe square; a number tells how many traps
// touch that square (8 neighbours). The Atlas keys: E S X F move, D (Enter / Space) opens,
// Alt+P marks a trap, Q new game, K the keys card,
// ^ back. The first square opened is never a trap. Turn-based, so the e-ink
// only refreshes on a key. Not offered on battery below 25 % (messages come first).
constexpr int kGameCols = 10, kGameRows = 9, kGameTraps = 12, kGameCell = 22;
constexpr int kGameX = (W - kGameCols * kGameCell) / 2;
constexpr int kGameY = LIST_TOP + 2;
constexpr int kGameMinBattery = 25;
enum : uint8_t { GC_TRAP = 1, GC_OPEN = 2, GC_FLAG = 4 };
uint8_t g_gameCell[kGameRows][kGameCols];
bool g_gameOpen = false, g_gamePlaced = false, g_gameOver = false, g_gameWon = false;
int g_gameCx = 0, g_gameCy = 0, g_gameFlags = 0, g_gameOpened = 0;
uint32_t g_gameStart = 0, g_gameEnd = 0, g_gameBest = 0, g_gameWins = 0;

int gameAround(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            const int ax = x + dx, ay = y + dy;
            if ((dx || dy) && ax >= 0 && ay >= 0 && ax < kGameCols && ay < kGameRows && (g_gameCell[ay][ax] & GC_TRAP)) n++;
        }
    return n;
}

void gameNew() {
    memset(g_gameCell, 0, sizeof(g_gameCell));
    g_gamePlaced = g_gameOver = g_gameWon = false;
    g_gameCx = kGameCols / 2;
    g_gameCy = kGameRows / 2;
    g_gameFlags = g_gameOpened = 0;
    g_gameStart = g_gameEnd = 0;
    g_fullRefreshReq = true;
    requestRedraw();
}

// Traps go in after the first open, never on or next to that square.
void gamePlace(int sx, int sy) {
    int placed = 0;
    while (placed < kGameTraps) {
        const int x = (int)(esp_random() % kGameCols), y = (int)(esp_random() % kGameRows);
        if (abs(x - sx) <= 1 && abs(y - sy) <= 1) continue;
        if (g_gameCell[y][x] & GC_TRAP) continue;
        g_gameCell[y][x] |= GC_TRAP;
        placed++;
    }
    g_gamePlaced = true;
    g_gameStart = millis();
}

void gameOpenCell(int x, int y) {
    // Flood fill without recursion: a small stack of squares with no traps around.
    static uint8_t stack[kGameRows * kGameCols];
    int sp = 0;
    auto push = [&](int ax, int ay) {
        uint8_t& c = g_gameCell[ay][ax];
        if (c & (GC_OPEN | GC_FLAG | GC_TRAP)) return;
        c |= GC_OPEN;
        g_gameOpened++;
        if (!gameAround(ax, ay)) stack[sp++] = (uint8_t)(ay * kGameCols + ax);
    };
    push(x, y);
    while (sp) {
        const int p = stack[--sp], px = p % kGameCols, py = p / kGameCols;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                const int ax = px + dx, ay = py + dy;
                if (ax >= 0 && ay >= 0 && ax < kGameCols && ay < kGameRows) push(ax, ay);
            }
    }
}

void gameSaveScore() {
    Preferences p;
    if (!p.begin("rs_game", false)) return;
    p.putUInt("best", g_gameBest);
    p.putUInt("wins", g_gameWins);
    p.end();
}

bool gameOpen() {
    if (!g_bat.usb && g_bat.valid && g_bat.pct < kGameMinBattery) {
        toast("Battery under %d%%: no game", kGameMinBattery);
        return false;
    }
    Preferences p;
    if (p.begin("rs_game", true)) {
        g_gameBest = p.getUInt("best", 0);
        g_gameWins = p.getUInt("wins", 0);
        p.end();
    }
    g_gameOpen = true;
    gameNew();
    Serial.println("[GAME] Rat Trap open");
    return true;
}

void gameClose() { g_gameOpen = false; g_navKeysOpen = false; g_fullRefreshReq = true; requestRedraw(); }

void gameDig() {
    if (g_gameOver) return;
    uint8_t& c = g_gameCell[g_gameCy][g_gameCx];
    if (c & (GC_OPEN | GC_FLAG)) return;
    if (!g_gamePlaced) gamePlace(g_gameCx, g_gameCy);
    if (c & GC_TRAP) {
        g_gameOver = true;
        g_gameEnd = millis();
        Serial.println("[GAME] trap - game over");
        return;
    }
    gameOpenCell(g_gameCx, g_gameCy);
    if (g_gameOpened == kGameCols * kGameRows - kGameTraps) {
        g_gameOver = g_gameWon = true;
        g_gameEnd = millis();
        const uint32_t s = (g_gameEnd - g_gameStart + 999) / 1000;
        g_gameWins++;
        if (!g_gameBest || s < g_gameBest) g_gameBest = s;
        gameSaveScore();
        Serial.printf("[GAME] cleared in %lu s (best %lu, wins %lu)\n", (unsigned long)s,
                      (unsigned long)g_gameBest, (unsigned long)g_gameWins);
    }
}

// The Atlas keys: E S X F move, D (or Enter / Space) opens, Alt+P marks, Q new game, K keys.
void gameKey(const KeyEvent& ev) {
    const char c = (char)toupper((unsigned char)ev.character);
    if (c == 'M') { gameClose(); openMenu(); return; }
    if (c == kKeyNav) { navKeysToggle(); return; }
    if (c == 'Q') { gameNew(); return; }
    if (c == 'E' && g_gameCy > 0) g_gameCy--;
    else if (c == 'X' && g_gameCy < kGameRows - 1) g_gameCy++;
    else if (c == 'S' && g_gameCx > 0) g_gameCx--;
    else if (c == 'F' && g_gameCx < kGameCols - 1) g_gameCx++;
    else if (c == 'D' || ev.enter || ev.space) gameDig();
    else if (c == kKeyWpt && ev.alt && !g_gameOver) {
        uint8_t& cell = g_gameCell[g_gameCy][g_gameCx];
        if (!(cell & GC_OPEN)) { cell ^= GC_FLAG; g_gameFlags += (cell & GC_FLAG) ? 1 : -1; }
    } else return;
    requestRedraw();
}

void renderGame() {
    char hdr[48];
    const uint32_t end = g_gameEnd ? g_gameEnd : millis();
    const uint32_t s = g_gameStart ? (end - g_gameStart) / 1000 : 0;
    snprintf(hdr, sizeof(hdr), "Rat Trap  traps %d  %lu:%02lu", kGameTraps - g_gameFlags,
             (unsigned long)(s / 60), (unsigned long)(s % 60));
    drawHeader(hdr, false);
    for (int y = 0; y < kGameRows; y++)
        for (int x = 0; x < kGameCols; x++) {
            const int px = kGameX + x * kGameCell, py = kGameY + y * kGameCell;
            const uint8_t c = g_gameCell[y][x];
            const bool cur = x == g_gameCx && y == g_gameCy;
            box(px, py, kGameCell + 1, kGameCell + 1, cur ? 3 : 1);
            if (c & GC_OPEN) {
                const int n = gameAround(x, y);
                if (n) { char t[2] = {(char)('0' + n), 0}; label(px, py + 1, kGameCell, kGameCell, t, n >= 3); }
            } else if ((c & GC_TRAP) && g_gameOver) {
                label(px, py + 1, kGameCell, kGameCell, g_gameWon ? "o" : "X", true);
            } else if (c & GC_FLAG) {
                label(px, py + 1, kGameCell, kGameCell, "F", true);
            } else {
                fillRect(px + 5, py + 5, kGameCell - 9, kGameCell - 9);   // still covered
            }
        }
    box(0, FOOT_Y, W, FOOT_H, 1);
    char foot[48];
    if (g_toast[0]) snprintf(foot, sizeof(foot), "%s", g_toast);
    else if (g_gameWon) snprintf(foot, sizeof(foot), "Cleared! best %lus  Q new", (unsigned long)g_gameBest);
    else if (g_gameOver) snprintf(foot, sizeof(foot), "Trapped!  Q new game  ^ back");
    else snprintf(foot, sizeof(foot), "K keys  D open  Alt+P mark");
    label(0, FOOT_Y - 1, W, FOOT_H, foot);
    if (g_navKeysOpen) drawNavKeys(FOOT_Y - kNavH, kGameKeys);
}

// ---- Chess (MENU -> Library, last row) ------------------------------------------------
// Play against the unit (Chess.cpp: rules + engine). The engine thinks in a task on
// core 0, so the radio and keys keep working; light sleep waits for it. The game is kept
// in NVS (rs_chess: moves, level, your colour, score) and goes on where it was left.
// Keys: E S X F / trackball move the cursor, D / Enter / Space pick up and put down,
// 1-4 choose a promotion, U takes back, L level, C swap sides, Q new game (twice), K keys.
constexpr int kChessSq = 28, kChessX = (W - 8 * kChessSq) / 2, kChessY = LIST_TOP;
constexpr int kChessMaxMoves = 400;
const NavKeyRow kChessKeys[] = {
    {"E" LV_SYMBOL_UP " S" LV_SYMBOL_LEFT " X" LV_SYMBOL_DOWN " F" LV_SYMBOL_RIGHT, ""},
    {"D Enter", "pick / drop"}, {"U", "take back"}, {"L", "level 1-4"},
    {"C", "swap sides"},      {"Q Q", "new game"},      {"K", "hide keys"},
    {"M", "menu"},            {"^", "back to Library"}, {"", ""},
    {"", ""},                 {"", ""},                {"", ""}, {"", ""},
};

bool g_chessOpen = false;
chess::Pos g_cPos;
chess::Move* g_cMoves = nullptr;          // kChessMaxMoves each, in PSRAM (chessAlloc)
uint64_t* g_cKeys = nullptr;              // position before each move (repetition)
int g_cN = 0;
int8_t g_cUser = 1;                       // your colour: +1 white, -1 black
uint8_t g_cLevel = 2;
int g_cCur = 12, g_cSel = -1;             // cursor / picked-up square
int g_cPromoFrom = -1, g_cPromoTo = -1;   // waiting for 1-4
uint8_t g_cResult = chess::ONGOING;
char g_cLastName[16] = "";
uint32_t g_cWins = 0, g_cLosses = 0, g_cDraws = 0;
// Engine task
volatile bool g_cThinking = false, g_cDone = false, g_cStop = false;
chess::Pos g_cJobPos;
chess::Move g_cReply;
chess::ThinkInfo g_cInfo;

const char* chessResultText(uint8_t r, int8_t winner) {
    switch (r) {
    case chess::CHECKMATE: return winner == g_cUser ? "Checkmate - you win" : "Checkmate - you lose";
    case chess::STALEMATE: return "Stalemate - draw";
    case chess::DRAW_50: return "50 moves - draw";
    case chess::DRAW_MATERIAL: return "No mate possible - draw";
    case chess::DRAW_REPEAT: return "Repetition - draw";
    }
    return "";
}

void chessSave() {
    Preferences p;
    if (!p.begin("rs_chess", false)) return;
    uint8_t buf[kChessMaxMoves * 3];
    for (int i = 0; i < g_cN; i++) {
        buf[i * 3] = g_cMoves[i].from;
        buf[i * 3 + 1] = g_cMoves[i].to;
        buf[i * 3 + 2] = (uint8_t)g_cMoves[i].promo;
    }
    if (g_cN) p.putBytes("moves", buf, g_cN * 3);
    else p.remove("moves");
    p.putUChar("lvl", g_cLevel);
    p.putChar("user", g_cUser);
    p.putUInt("w", g_cWins);
    p.putUInt("l", g_cLosses);
    p.putUInt("d", g_cDraws);
    p.end();
}

bool chessAlloc() {
    if (g_cMoves && g_cKeys) return true;
    const size_t bytes = kChessMaxMoves * (sizeof(chess::Move) + sizeof(uint64_t));
    uint8_t* m = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) m = (uint8_t*)malloc(bytes);
    if (!m) return false;
    g_cKeys = (uint64_t*)m;
    g_cMoves = (chess::Move*)(m + kChessMaxMoves * sizeof(uint64_t));
    return true;
}

// Position after the first n moves of the list (from the start position).
void chessReplay(int n) {
    chess::startPos(g_cPos);
    chess::Move legal[chess::kMaxMoves];
    int ok = 0;
    for (; ok < n; ok++) {
        const int k = chess::legalMoves(g_cPos, legal);
        int hit = -1;
        for (int j = 0; j < k; j++)
            if (legal[j].from == g_cMoves[ok].from && legal[j].to == g_cMoves[ok].to && legal[j].promo == g_cMoves[ok].promo) hit = j;
        if (hit < 0) break;                       // a saved list that does not replay: stop there
        g_cMoves[ok] = legal[hit];
        g_cKeys[ok] = g_cPos.key;
        chess::make(g_cPos, legal[hit]);
    }
    g_cN = ok;
    g_cResult = chess::result(g_cPos, g_cKeys, g_cN);
    g_cSel = -1;
    g_cPromoFrom = g_cPromoTo = -1;
    g_cLastName[0] = 0;
    if (g_cN) {
        chess::Pos before;
        chess::startPos(before);
        for (int i = 0; i < g_cN - 1; i++) chess::make(before, g_cMoves[i]);
        chess::moveName(before, g_cMoves[g_cN - 1], g_cLastName, sizeof(g_cLastName));
    }
}

bool chessLoad() {
    if (!chessAlloc()) return false;
    Preferences p;
    g_cN = 0;
    if (p.begin("rs_chess", true)) {
        uint8_t buf[kChessMaxMoves * 3];
        const size_t len = p.getBytes("moves", buf, sizeof(buf));
        for (size_t i = 0; i + 2 < len + 0 && (int)(i / 3) < kChessMaxMoves; i += 3) {
            g_cMoves[i / 3].from = buf[i];
            g_cMoves[i / 3].to = buf[i + 1];
            g_cMoves[i / 3].promo = (int8_t)buf[i + 2];
            g_cN = (int)(i / 3) + 1;
        }
        g_cLevel = p.getUChar("lvl", 2);
        g_cUser = p.getChar("user", 1) < 0 ? -1 : 1;
        g_cWins = p.getUInt("w", 0);
        g_cLosses = p.getUInt("l", 0);
        g_cDraws = p.getUInt("d", 0);
        p.end();
    }
    if (g_cLevel < 1 || g_cLevel > 4) g_cLevel = 2;
    chessReplay(g_cN);
    return true;
}

void chessTask(void*) {
    static const uint32_t kMs[5] = {0, 1500, 2000, 4000, 8000};
    g_cReply = chess::think(g_cJobPos, g_cLevel, kMs[g_cLevel], g_cKeys, g_cN, &g_cInfo, &g_cStop);
    g_cDone = true;
    vTaskDelete(nullptr);
}

void chessThink() {
    if (g_cThinking || g_cResult != chess::ONGOING || g_cPos.side == g_cUser) return;
    g_cJobPos = g_cPos;
    g_cDone = g_cStop = false;
    g_cThinking = true;
    // Priority 0 (idle's): the idle task on core 0 still runs, so its watchdog stays fed.
    if (xTaskCreatePinnedToCore(chessTask, "chess", 32768, nullptr, 0, nullptr, 0) != pdPASS) {
        g_cThinking = false;
        toast("Chess: no memory to think");
    }
}

// The game has ended with this move: count it once.
void chessScore() {
    if (g_cResult == chess::ONGOING) return;
    if (g_cResult == chess::CHECKMATE) { if (-g_cPos.side == g_cUser) g_cWins++; else g_cLosses++; }
    else g_cDraws++;
    Serial.printf("[CHESS] %s after %d moves (won %lu, lost %lu, drawn %lu)\n", chessResultText(g_cResult, -g_cPos.side),
                  g_cN, (unsigned long)g_cWins, (unsigned long)g_cLosses, (unsigned long)g_cDraws);
}

void chessPlay(const chess::Move& m) {
    if (g_cN >= kChessMaxMoves) { toast("Game too long: Q Q new"); return; }
    chess::moveName(g_cPos, m, g_cLastName, sizeof(g_cLastName));
    g_cMoves[g_cN] = m;
    g_cKeys[g_cN] = g_cPos.key;
    g_cN++;
    chess::make(g_cPos, m);
    g_cResult = chess::result(g_cPos, g_cKeys, g_cN);
    Serial.printf("[CHESS] %d. %s%s\n", (g_cN + 1) / 2, g_cN % 2 ? "" : "... ", g_cLastName);
    chessScore();
    chessSave();
    requestRedraw();
}

void chessTick() {
    if (!g_cThinking || !g_cDone) return;
    g_cThinking = false;
    if (g_cStop || !g_cReply.valid()) return;          // stopped (take back, new game, swap)
    Serial.printf("[CHESS] engine L%u: depth %d, %lu nodes, %lu ms, score %d\n", (unsigned)g_cLevel, g_cInfo.depth,
                  (unsigned long)g_cInfo.nodes, (unsigned long)g_cInfo.ms, g_cInfo.score);
    chessPlay(g_cReply);
    if (g_chessOpen) {
        g_cCur = g_cReply.to;
        g_audio.requestMessage();
    }
}

void chessStopThinking() {
    if (!g_cThinking) return;
    g_cStop = true;
    while (!g_cDone) delay(5);                  // the engine checks every 1024 nodes
    g_cThinking = false;
}

void chessNew() {
    chessStopThinking();
    g_cN = 0;
    chessReplay(0);
    g_cCur = g_cUser > 0 ? 12 : 52;             // e2 / e7
    chessSave();
    Serial.printf("[CHESS] new game, you play %s, level %u\n", g_cUser > 0 ? "white" : "black", (unsigned)g_cLevel);
    chessThink();
    requestRedraw();
}

bool chessOpen() {
    if (!chessLoad()) { toast("Chess: no memory"); return false; }
    g_chessOpen = true;
    g_cCur = g_cN && g_cMoves[g_cN - 1].to < 64 ? g_cMoves[g_cN - 1].to : (g_cUser > 0 ? 12 : 52);
    Serial.printf("[CHESS] open: %d moves, you play %s, level %u\n", g_cN, g_cUser > 0 ? "white" : "black",
                  (unsigned)g_cLevel);
    g_fullRefreshReq = true;
    chessThink();
    requestRedraw();
    return true;
}

void chessClose() { g_chessOpen = false; g_navKeysOpen = false; g_fullRefreshReq = true; requestRedraw(); }

void chessPick() {
    if (g_cThinking) { toast("Thinking..."); return; }
    if (g_cResult != chess::ONGOING) { toast("Game over: Q Q new game"); return; }
    if (g_cPos.side != g_cUser) { chessThink(); return; }
    const int v = g_cPos.sq[g_cCur];
    if (g_cSel < 0 || (v * g_cUser > 0 && g_cCur != g_cSel)) {     // pick up (or pick another)
        if (v * g_cUser <= 0) return;
        g_cSel = g_cCur;
        return;
    }
    if (g_cCur == g_cSel) { g_cSel = -1; return; }                  // put it back
    chess::Move legal[chess::kMaxMoves];
    const int n = chess::legalMoves(g_cPos, legal);
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (legal[i].from == g_cSel && legal[i].to == g_cCur) { hit = i; break; }
    if (hit < 0) { toast("Not a legal move"); return; }
    if (legal[hit].promo) { g_cPromoFrom = g_cSel; g_cPromoTo = g_cCur; return; }
    g_cSel = -1;
    chessPlay(legal[hit]);
    chessThink();
}

void chessPromote(int8_t piece) {
    chess::Move legal[chess::kMaxMoves];
    const int n = chess::legalMoves(g_cPos, legal);
    for (int i = 0; i < n; i++) {
        if (legal[i].from == g_cPromoFrom && legal[i].to == g_cPromoTo && legal[i].promo == piece) {
            g_cPromoFrom = g_cPromoTo = -1;
            g_cSel = -1;
            chessPlay(legal[i]);
            chessThink();
            return;
        }
    }
}

void chessKey(const KeyEvent& ev) {
    const char c = (char)toupper((unsigned char)ev.character);
    if (c == 'M') { chessClose(); openMenu(); return; }
    if (c == kKeyNav) { navKeysToggle(); return; }
    if (g_cPromoFrom >= 0) {                                      // promotion: 1-4, anything else cancels
        static const int8_t kPick[4] = {chess::QUEEN, chess::ROOK, chess::BISHOP, chess::KNIGHT};
        if (c >= '1' && c <= '4') chessPromote(kPick[c - '1']);
        else { g_cPromoFrom = g_cPromoTo = -1; }
        requestRedraw();
        return;
    }
    // Cursor moves as the board is shown (turned round when you play black).
    const int fl = g_cUser > 0 ? 1 : -1;
    int f = chess::file(g_cCur), r = chess::rank(g_cCur);
    if (c == 'E' || ev.up) r += fl;
    else if (c == 'X' || ev.down) r -= fl;
    else if (c == 'S' || ev.left) f -= fl;
    else if (c == 'F' || ev.right) f += fl;
    else if (c == 'D' || ev.enter || ev.space) chessPick();
    else if (c == 'U') {
        if (!g_cN) return;
        chessStopThinking();
        int n = g_cN - 1;                                         // back to your turn, one of your moves undone
        if ((n % 2 == 0) != (g_cUser > 0)) n--;
        if (n < 0) n = 0;
        chessReplay(n);
        chessSave();
        toast("Taken back");
        chessThink();
    } else if (c == 'L') {
        g_cLevel = g_cLevel % 4 + 1;
        chessSave();
        toast("Level %u%s", (unsigned)g_cLevel, g_cLevel == 1 ? " (easy)" : g_cLevel == 4 ? " (hard)" : "");
    } else if (c == 'C') {
        chessStopThinking();
        g_cUser = (int8_t)-g_cUser;
        g_cSel = -1;
        chessSave();
        toast("You play %s", g_cUser > 0 ? "white" : "black");
        chessThink();
    } else if (c == 'Q') {
        static uint32_t askedAt = 0;
        if (!askedAt || millis() - askedAt > 5000) { askedAt = millis(); toast("Q again: new game"); requestRedraw(); return; }
        askedAt = 0;
        chessNew();
    } else return;
    if (f < 0) f = 0;
    if (f > 7) f = 7;
    if (r < 0) r = 0;
    if (r > 7) r = 7;
    g_cCur = r * 8 + f;
    requestRedraw();
}

void renderChess() {
    char hdr[48], sq[3] = {(char)('a' + chess::file(g_cCur)), (char)('1' + chess::rank(g_cCur)), 0};
    if (g_toast[0]) snprintf(hdr, sizeof(hdr), "%s", g_toast);
    else if (g_cPromoFrom >= 0) snprintf(hdr, sizeof(hdr), "Promote: 1Q 2R 3B 4N");
    else if (g_cResult != chess::ONGOING) snprintf(hdr, sizeof(hdr), "%s", chessResultText(g_cResult, -g_cPos.side));
    else if (g_cThinking) snprintf(hdr, sizeof(hdr), "%s%sThinking... L%u", g_cLastName, g_cLastName[0] ? "  " : "", (unsigned)g_cLevel);
    else if (g_cPos.side == g_cUser && g_cLastName[0])
        snprintf(hdr, sizeof(hdr), "Bot played %s%s  L%u", g_cLastName, chess::inCheck(g_cPos) ? " check" : "",
                 (unsigned)g_cLevel);
    else if (g_cPos.side == g_cUser) snprintf(hdr, sizeof(hdr), "Your move (%s)  L%u", sq, (unsigned)g_cLevel);
    else snprintf(hdr, sizeof(hdr), "Bot to move  L%u  Enter", (unsigned)g_cLevel);
    drawHeader(hdr, false);
    if (g_navKeysOpen) drawNavKeys(BOT_TOP - kNavH, kChessKeys);
}

// ---- Board pixels (after LVGL; 20x20 pieces, my own drawings) ----------------------------
const char* const kChessArt[6][20] = {
    {   // pawn
        "....................", "....................", "....................", "........XXXX........",
        ".......XXXXXX.......", ".......XXXXXX.......", ".......XXXXXX.......", "........XXXX........",
        "......XXXXXXXX......", "........XXXX........", "........XXXX........", ".......XXXXXX.......",
        ".......XXXXXX.......", "......XXXXXXXX......", ".....XXXXXXXXXX.....", "....XXXXXXXXXXXX....",
        "...XXXXXXXXXXXXXX...", "...XXXXXXXXXXXXXX...", "....................", "...................."},
    {   // knight
        "....................", ".......XX.X.........", "......XXXXXX........", ".....XXXXXXXX.......",
        "....XXXXXXXXXX......", "...XXXX.XXXXXXX.....", "..XXXXXXXXXXXXXX....", "..XXXXXXXXXXXXXXX...",
        "...XXXX.XXXXXXXXX...", "....XX..XXXXXXXXX...", ".......XXXXXXXXXX...", "......XXXXXXXXXXX...",
        ".....XXXXXXXXXXX....", ".....XXXXXXXXXXX....", "....XXXXXXXXXXXX....", "....XXXXXXXXXXXX....",
        "...XXXXXXXXXXXXXX...", "..XXXXXXXXXXXXXXXX..", "..XXXXXXXXXXXXXXXX..", "...................."},
    {   // bishop
        "....................", ".........XX.........", "........XXXX........", ".......XXXXXX.......",
        "......XXXX.XXX......", "......XXX.XXXX......", "......XX.XXXXX......", "......XXXXXXXX......",
        ".......XXXXXX.......", "........XXXX........", ".......XXXXXX.......", "........XXXX........",
        "........XXXX........", ".......XXXXXX.......", "......XXXXXXXX......", ".....XXXXXXXXXX.....",
        "...XXXXXXXXXXXXXX...", "..XXXXXXXXXXXXXXXX..", "..XXXXXXXXXXXXXXXX..", "...................."},
    {   // rook
        "....................", "...XXX..XXXX..XXX...", "...XXX..XXXX..XXX...", "...XXXXXXXXXXXXXX...",
        "...XXXXXXXXXXXXXX...", "....XXXXXXXXXXXX....", ".....XXXXXXXXXX.....", ".....XXXXXXXXXX.....",
        ".....XXXXXXXXXX.....", ".....XXXXXXXXXX.....", ".....XXXXXXXXXX.....", ".....XXXXXXXXXX.....",
        ".....XXXXXXXXXX.....", ".....XXXXXXXXXX.....", "....XXXXXXXXXXXX....", "...XXXXXXXXXXXXXX...",
        "...XXXXXXXXXXXXXX...", "..XXXXXXXXXXXXXXXX..", "..XXXXXXXXXXXXXXXX..", "...................."},
    {   // queen
        "....................", "..X......XX......X..", "..XX....XXXX....XX..", "..XX.X...XX...X.XX..",
        "..XXXXX.XXXX.XXXXX..", "...XXXXXXXXXXXXXX...", "...XXXXXXXXXXXXXX...", "....XXXXXXXXXXXX....",
        "....XXXXXXXXXXXX....", ".....XXXXXXXXXX.....", "......XXXXXXXX......", "......XXXXXXXX......",
        "......XXXXXXXX......", ".....XXXXXXXXXX.....", "....XXXXXXXXXXXX....", "....XXXXXXXXXXXX....",
        "...XXXXXXXXXXXXXX...", "..XXXXXXXXXXXXXXXX..", "..XXXXXXXXXXXXXXXX..", "...................."},
    {   // king
        ".........XX.........", ".........XX.........", ".......XXXXXX.......", ".........XX.........",
        "........XXXX........", "...XXX.XXXXXX.XXX...", "..XXXXXXXXXXXXXXXX..", "..XXXXXXXXXXXXXXXX..",
        "..XXXXXXXXXXXXXXXX..", "...XXXXXXXXXXXXXX...", "....XXXXXXXXXXXX....", ".....XXXXXXXXXX.....",
        "......XXXXXXXX......", "......XXXXXXXX......", ".....XXXXXXXXXX.....", "....XXXXXXXXXXXX....",
        "...XXXXXXXXXXXXXX...", "..XXXXXXXXXXXXXXXX..", "..XXXXXXXXXXXXXXXX..", "...................."},
};

inline bool chessArt(int t, int x, int y) {
    return x >= 0 && y >= 0 && x < 20 && y < 20 && kChessArt[t][y][x] == 'X';
}

void chessPx(int x, int y, bool black) { mapPixel(x, y, g_cfg.dark ? !black : black); }

void chessFrame(int x0, int y0, int inset, int width) {
    for (int k = 0; k < width; k++) {
        const int a = inset + k, b = kChessSq - 1 - inset - k;
        for (int i = a; i <= b; i++) {
            chessPx(x0 + i, y0 + a, true); chessPx(x0 + i, y0 + b, true);
            chessPx(x0 + a, y0 + i, true); chessPx(x0 + b, y0 + i, true);
        }
    }
}

void stampChess() {
    const int yMax = g_navKeysOpen ? BOT_TOP - kNavH : BOT_TOP;
    const bool flip = g_cUser < 0;
    chess::Move legal[chess::kMaxMoves];
    const int nLegal = g_cSel >= 0 ? chess::legalMoves(g_cPos, legal) : 0;
    const chess::Move* last = g_cN ? &g_cMoves[g_cN - 1] : nullptr;
    for (int vr = 0; vr < 8; vr++) {
        for (int vc = 0; vc < 8; vc++) {
            const int r = flip ? vr : 7 - vr, f = flip ? 7 - vc : vc, sq = r * 8 + f;
            const int x0 = kChessX + vc * kChessSq, y0 = kChessY + vr * kChessSq;
            if (y0 + kChessSq > yMax) continue;
            const bool dark = ((r + f) & 1) == 0;
            const int v = g_cPos.sq[sq], t = v < 0 ? -v : v;
            for (int y = 0; y < kChessSq; y++)
                for (int x = 0; x < kChessSq; x++) {
                    bool ink = dark && !((x0 + x) & 1) && !((y0 + y) & 1);    // dark square: 25 % dots
                    if (t) {
                        const int px = x - 4, py = y - 4;
                        const bool in = chessArt(t - 1, px, py);
                        bool edge = false, near = false;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                const bool o = chessArt(t - 1, px + dx, py + dy);
                                if (in && !o && (dx == 0 || dy == 0)) edge = true;
                                if (!in && o) near = true;
                            }
                        if (in) ink = v < 0 ? true : edge;                    // black filled, white outlined
                        else if (near) ink = false;                          // white halo
                    }
                    chessPx(x0 + x, y0 + y, ink);
                }
            if (last && (sq == last->from || sq == last->to)) {               // last move: corner ticks
                for (int i = 0; i < 6; i++)
                    for (int k = 0; k < 2; k++) {
                        chessPx(x0 + 2 + i, y0 + 2 + k, true); chessPx(x0 + 2 + k, y0 + 2 + i, true);
                        chessPx(x0 + kChessSq - 3 - i, y0 + kChessSq - 3 - k, true);
                        chessPx(x0 + kChessSq - 3 - k, y0 + kChessSq - 3 - i, true);
                    }
            }
            for (int i = 0; i < nLegal; i++) {                              // where the picked piece can go
                if (legal[i].from != g_cSel || legal[i].to != sq) continue;
                if (v) {
                    for (int y = 0; y < 5; y++)
                        for (int x = 0; x < 5 - y; x++) {
                            chessPx(x0 + x, y0 + y, true); chessPx(x0 + kChessSq - 1 - x, y0 + y, true);
                            chessPx(x0 + x, y0 + kChessSq - 1 - y, true);
                            chessPx(x0 + kChessSq - 1 - x, y0 + kChessSq - 1 - y, true);
                        }
                } else {
                    for (int y = 10; y < 18; y++)
                        for (int x = 10; x < 18; x++)
                            chessPx(x0 + x, y0 + y, !(x == 10 || x == 17 || y == 10 || y == 17));
                }
                break;
            }
            if (sq == g_cSel) chessFrame(x0, y0, 1, 2);
            if (sq == g_cCur) chessFrame(x0, y0, 0, g_cSel == sq ? 1 : 3);
        }
    }
    for (int i = -1; i <= 8 * kChessSq; i++) chessPx(kChessX + i, kChessY - 1, true);   // board edge
    for (int y = kChessY - 1; y < yMax; y++) {
        chessPx(kChessX - 1, y, true);
        chessPx(kChessX + 8 * kChessSq, y, true);
    }
    g_display.markDirty();
}

// ---- Contact page + share picker ----------------------------------------------------
const char* const kCardActs[4] = {"Message", "Rename", "Share", "Map number"};
const char* const kCardSubs[4] = {"open the chat", "name it on this radio", "send it to a contact", ""};

void openCard(const uint8_t peer[16]) {
    memcpy(g_cardPeer, peer, 16);
    g_cardOpen = true;
    g_cardSel = 0;
    requestRedraw();
}

void closeCard() { g_cardOpen = false; requestRedraw(); }

void renderCard() {
    char name[24], hex[33], line[40];
    peerName(g_cardPeer, name, sizeof(name));
    hashToHex(g_cardPeer, hex);
    snprintf(line, sizeof(line), "%s%s", isSelf(g_cardPeer) ? "MyID: " : "", name);
    text(4, HEADER_Y, line, true, W - 8);
    snprintf(line, sizeof(line), "%.20s..", hex);         // one line: room for 4 rows
    text(6, HEADER_Y + 19, line);
    int y = HEADER_Y + 39;
    for (int i = 0; i < 4; i++) {
        char sub[40];
        snprintf(sub, sizeof(sub), "%s", kCardSubs[i]);
        if (i == 3) {
            const int ci = cposFind(g_cardPeer);
            if (g_entry == Entry::MapNum) snprintf(sub, sizeof(sub), ">%s_  (1-99, Enter)", g_buf);
            else if (ci >= 0) snprintf(sub, sizeof(sub), "#%u on the map%s", (unsigned)g_cpos[ci].num,
                                       g_cpos[ci].hasPos ? "" : " (no position yet)");
            else snprintf(sub, sizeof(sub), "none yet (1 = shown first)");
        }
        snprintf(line, sizeof(line), "%d : %s", i + 1, kCardActs[i]);
        listRow(y, line, sub, i == g_cardSel);
        addHit(2, y, W - 4, ROW_H, H_CARD, i);
        y += ROW_H;
    }
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : "1-4 / Enter    ^ = back");
}

void startNameEntry(const uint8_t peer[16]);
void openThread(const uint8_t peer[16]);
void goScreen(ScreenId s);
void openMenu();

void cardAct(int i) {
    uint8_t peer[16];
    memcpy(peer, g_cardPeer, 16);
    switch (i) {
        case 0:
            if (isSelf(peer)) { toast("That is this radio"); return; }
            closeCard();
            openThread(peer);
            break;
        case 1:
            closeCard();
            startNameEntry(peer);
            break;
        case 2:
            closeCard();
            goScreen(SCR_CONTACTS);
            memcpy(g_sharePeer, peer, 16);
            g_shareActive = true;
            toast("Pick who gets it");
            Serial.println("[CARD] pick recipient");
            break;
        case 3: {                                   // map number (1 = wins at a shared spot)
            if (isSelf(peer)) { toast("That is this radio"); return; }
            const int ci = cposFind(peer);
            char cur[4] = "";
            if (ci >= 0) snprintf(cur, sizeof(cur), "%u", (unsigned)g_cpos[ci].num);
            startEntry(Entry::MapNum, cur);
            break;
        }
    }
}

void cardKey(const KeyEvent& ev) {
    if (ev.enter) { cardAct(g_cardSel); return; }
    const char raw = ev.character;
    if (raw >= '1' && raw <= '4') { g_cardSel = raw - '1'; cardAct(g_cardSel); return; }
    switch (toupper((unsigned char)raw)) {
        case 'G': if (g_cardSel > 0) g_cardSel--; requestRedraw(); break;
        case 'V': if (g_cardSel < 3) g_cardSel++; requestRedraw(); break;
        case 'M': closeCard(); openMenu(); break;
        default: break;
    }
}

void shareAllTo(const uint8_t to[16]);

void shareSendTo(int idx) {
    if (g_pickLowbat) {
        if (isSelf(g_rows[idx].peer)) { toast("Pick another radio"); return; }
        memcpy(g_cfg.lowBatTo, g_rows[idx].peer, 16);
        if (!g_cfg.lowBatPct) g_cfg.lowBatPct = 10;
        saveConfig();
        g_shareActive = false;
        g_pickLowbat = false;
        char nm[24];
        peerName(g_cfg.lowBatTo, nm, sizeof(nm));
        toast("Low battery -> %.14s at %u%%", nm, (unsigned)g_cfg.lowBatPct);
        Serial.printf("[LOWBAT] to %s at %u%%\n", nm, (unsigned)g_cfg.lowBatPct);
        goScreen(SCR_SETTINGS);
        return;
    }
    if (g_pickRt) {
        uint8_t to[16];
        memcpy(to, g_rows[idx].peer, 16);
        if (isSelf(to)) { toast("Pick another radio"); return; }
        g_shareActive = false;
        g_pickRt = false;
        rtStart(to, 0);
        return;
    }
    if (g_shareAll) {
        uint8_t to[16];
        memcpy(to, g_rows[idx].peer, 16);
        if (isSelf(to)) { toast("Pick someone else"); return; }
        shareAllTo(to);
        return;
    }
    const uint8_t* to = g_rows[idx].peer;
    if (isSelf(to) || !memcmp(to, g_sharePeer, 16)) { toast("Pick someone else"); return; }
    char nm[24], toName[24], body[96];
    peerName(g_sharePeer, nm, sizeof(nm));
    peerName(to, toName, sizeof(toName));
    cardFormat(g_sharePeer, nm, body, sizeof(body));
    g_shareActive = false;
    const power::CheckinPhase ph = checkinNow();
    if (g_pwEff.checkin && !(ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS)) {
        if (outboxAdd(to, body)) toast("Card queued (check-in)"); else toast("Outbox full (%d)", kOutboxCap);
    } else if (sendMessage(to, body)) {
        Serial.printf("[CARD] sent \"%s\" to %s\n", nm, toName);
        toast("Card sent to %.12s", toName);
    } else {
        toast("Not sent: %s", g_txStr);
    }
    loadRows(g_screen);
    requestRedraw();
}

void saveThreadCard() {
    if (!cardOffered()) return;
    saveContactName(g_threadCard.hash, g_threadCard.name);
    char hex[33];
    hashToHex(g_threadCard.hash, hex);
    Serial.printf("[CARD] saved \"%s\" %s\n", g_threadCard.name, hex);
    toast("Saved %.18s", g_threadCard.name);
}

// ---- Contacts: share all, export / import (Settings S.6, setup wizard) -----------------
constexpr const char* kContactsFile = SD_PATH_ROOT "/contacts.txt";   // "name<TAB>32 hex" per line

// Every saved contact as a contact card to one radio (it saves them while its setup wizard
// listens, or one by one from the chat). Check-in mode: they wait for the window.
void shareAllTo(const uint8_t to[16]) {
    g_shareActive = false;
    g_shareAll = false;
    loadRows(SCR_CONTACTS);
    static uint8_t peers[MAX_ROWS][16];
    static char names[MAX_ROWS][24];
    int n = 0;
    for (int i = 0; i < g_rowCount && n < MAX_ROWS; i++) {
        const uint8_t* h = g_rows[i].peer;
        if (isSelf(h) || !memcmp(h, to, 16)) continue;
        memcpy(peers[n], h, 16);
        peerName(h, names[n], sizeof(names[n]));
        n++;
    }
    // The first goes now; the rest wait in the outbox (one send at a time, 3 s apart).
    // Check-in mode: all of them wait for the window.
    int sent = 0, queued = 0;
    const power::CheckinPhase ph = checkinNow();
    const bool queue = g_pwEff.checkin && !(ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS);
    for (int i = 0; i < n; i++) {
        char body[96];
        cardFormat(peers[i], names[i], body, sizeof(body));
        if (queue) { if (outboxAdd(to, body)) queued++; continue; }
        const SendResult r = sendOrQueue(to, body);
        if (r == SendResult::Sent) sent++;
        else if (r == SendResult::Queued) queued++;
    }
    char toName[24];
    peerName(to, toName, sizeof(toName));
    Serial.printf("[CARD] shareall %d of %d to %s (%d now, %d queued)\n", sent + queued, n, toName, sent, queued);
    if (sent + queued < n) toast("%d of %d cards: outbox full", sent + queued, n);
    else toast("%d cards to %.12s", n, toName);
    loadRows(g_screen);
    requestRedraw();
}

void shareAllStart() {
    goScreen(SCR_CONTACTS);
    memset(g_sharePeer, 0, sizeof(g_sharePeer));
    g_shareActive = true;
    g_shareAll = true;
    toast("Pick who gets all contacts");
    Serial.println("[CARD] pick recipient (all contacts)");
}

int contactsExport() {
    if (!sdMount()) { toast("No SD card"); return -1; }
    File f = g_sdStore.openFile(kContactsFile, FILE_WRITE);
    if (!f) { toast("Cannot write contacts.txt"); return -1; }
    loadRows(SCR_CONTACTS);
    int n = 0;
    for (int i = 0; i < g_rowCount; i++) {
        if (isSelf(g_rows[i].peer)) continue;
        char nm[24], hex[33];
        peerName(g_rows[i].peer, nm, sizeof(nm));
        hashToHex(g_rows[i].peer, hex);
        f.printf("%s\t%s\n", nm, hex);
        n++;
    }
    f.close();
    loadRows(g_screen);
    Serial.printf("[CONTACTS] %d -> contacts.txt\n", n);
    return n;
}

int contactsImport() {
    if (!sdMount()) { toast("No SD card"); return -1; }
    File f = g_sdStore.openFile(kContactsFile, FILE_READ);
    if (!f) { toast("No ratdeckpro/contacts.txt"); return -1; }
    char line[96];
    int len = 0, n = 0;
    auto take = [&]() {
        line[len] = 0;
        len = 0;
        char* tab = strchr(line, '\t');
        if (!tab) return;
        *tab = 0;
        const char* hex = tab + 1;
        uint8_t h[16];
        if (strlen(hex) < 32 || !hexToHash(hex, h) || isSelf(h) || !line[0]) return;
        saveContactName(h, line);
        n++;
    };
    while (f.available()) {
        const int c = f.read();
        if (c < 0) break;
        if (c == '\n') take();
        else if (c != '\r' && len < (int)sizeof(line) - 1) line[len++] = (char)c;
    }
    if (len) take();
    f.close();
    if (g_screen == SCR_CONTACTS || g_screen == SCR_PEERS) loadRows(g_screen);
    Serial.printf("[CONTACTS] imported %d from contacts.txt\n", n);
    return n;
}

// ---- Setup wizard (fresh device, or Settings S.6 "Run setup wizard") --------------------
// One screen per step, "Step 3/10" in the header. Enter keeps what is shown and goes on, ^
// goes back a step, 0 finishes with the rest left as it is. A digit picks a row: on a
// choice step it also goes on.
enum WizStep : uint8_t { WZ_WELCOME, WZ_NAME, WZ_TIME, WZ_CLOCK, WZ_POWER, WZ_IDLE, WZ_MODE, WZ_LOCK,
                         WZ_IMPORT, WZ_DONE, WZ_COUNT };
const char* const kWizTitle[WZ_COUNT] = {"Welcome", "Device name", "Time zone", "Clock", "Energy mode",
                                         "Idle screen", "Simple / Advanced", "Password lock",
                                         "Import contacts", "Done"};
const uint8_t kWizModes[4] = {power::MODE_FULL, power::MODE_BALANCED, power::MODE_SAVER, power::MODE_CHECKIN};

void wizStart() {
    g_menuOpen = false;
    g_wizOpen = true;
    g_wizStep = WZ_WELCOME;
    g_wizListen = false;
    g_wizGot = 0;
    g_cfg.wizDone = false;                   // a setting saved meanwhile keeps it pending (reboot: again)
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.println("[WIZ] start");
}

void wizListen(bool on) {
    g_wizListen = on;
    if (on) g_wizGot = 0;
    Serial.printf("[WIZ] listen %s\n", on ? "on" : "off");
    requestRedraw();
}

// A contact card that arrived while the wizard listens: saved at once.
void wizCardIn(const char* text) {
    if (!g_wizOpen || !g_wizListen) return;
    uint8_t h[16];
    char nm[24];
    if (!cardParse(text, strlen(text), h, nm, sizeof(nm)) || isSelf(h)) return;
    saveContactName(h, nm);
    g_wizGot++;
    Serial.printf("[WIZ] contact saved \"%s\" (%d)\n", nm, g_wizGot);
    requestRedraw();
}

void wizFinish(bool announce) {
    if (!g_wizOpen && g_cfg.wizDone) return;
    g_wizOpen = false;
    g_wizListen = false;
    g_cfg.wizDone = true;
    saveConfig();
    Serial.printf("[WIZ] %s\n", announce ? "done" : "skipped");
    goScreen(SCR_HOME);
    if (announce) { doAnnounce(); toast("Announced: others can see you"); }
    g_fullRefreshReq = true;
    requestRedraw();
}

void wizNext() {
    if (g_wizStep == WZ_IMPORT && g_wizListen) wizListen(false);
    if (g_wizStep >= WZ_DONE) { wizFinish(true); return; }
    g_wizStep++;
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.printf("[WIZ] step %d %s\n", g_wizStep + 1, kWizTitle[g_wizStep]);
}

// Digit k (1-based) on the current step. true = a choice was made: go on.
bool wizPick(int k) {
    switch (g_wizStep) {
        case WZ_NAME:
            if (k == 1 && g_haveSelf) startNameEntry(g_selfHash);
            return false;
        case WZ_TIME:
            if (k == 1) { g_cfg.tz = (g_cfg.tz + 1) % kTzCount; saveConfig(); applyTz(); requestRedraw(); }
            return false;
        case WZ_CLOCK:
            if (k < 1 || k > 2) return false;
            g_cfg.clock12 = k == 2; saveConfig();
            return true;
        case WZ_POWER:
            if (k < 1 || k > 4) return false;
            g_pw.mode = kWizModes[k - 1]; power::save(g_pw); powerApply(true);
            return true;
        case WZ_IDLE:
            if (k < 1 || k > IDLE_COUNT) return false;
            g_cfg.idleScreen = (uint8_t)(k - 1); saveConfig();
            if (g_cfg.idleScreen == IDLE_FOUND) {      // the text right away
                startEntry(Entry::FoundText, g_cfg.foundText);
                g_entryReplace = true;
                return false;
            }
            return true;
        case WZ_MODE:
            if (k < 1 || k > 2) return false;
            g_cfg.simple = k == 1; saveConfig();
            return true;
        case WZ_LOCK:
            if (k == 1) return true;
            if (k == 2) { startEntry(Entry::PassNew, nullptr, 2); toast("Choose a password"); }
            return false;
        case WZ_IMPORT:
            if (k == 1) wizListen(!g_wizListen);
            else if (k == 2) { const int n = contactsImport(); if (n >= 0) { g_wizGot += n; toast("%d contacts imported", n); } }
            requestRedraw();
            return false;
        default:
            return false;
    }
}

void wizKey(const KeyEvent& ev) {
    if (ev.up) {
        if (g_wizStep > WZ_WELCOME) {
            if (g_wizStep == WZ_IMPORT && g_wizListen) wizListen(false);
            g_wizStep--;
            g_fullRefreshReq = true;
            requestRedraw();
        }
        return;
    }
    if (ev.enter) { wizNext(); return; }
    const char c = ev.character;
    if (c == '0') { wizFinish(false); return; }
    if (c >= '1' && c <= '9' && wizPick(c - '0')) wizNext();
}

void renderWizard() {
    char hdr[40], a[48], b[48];
    snprintf(hdr, sizeof(hdr), "Setup %d/%d  %s", g_wizStep + 1, WZ_COUNT, kWizTitle[g_wizStep]);
    drawHeader(hdr, false);
    int y = LIST_TOP;
    auto row = [&](const char* t, const char* sub, bool sel) { listRow(y, t, sub, sel); y += ROW_H; };
    auto line = [&](const char* t, bool bold) { label(0, y, W, 18, t, bold); y += 20; };
    switch (g_wizStep) {
        case WZ_WELCOME:
            y += 6;
            line("Welcome to RatSpeak", true);
            line("Text messages over LoRa", false);
            line("radio: no phone network,", false);
            line("no internet needed.", false);
            y += 8;
            line("A few questions follow.", false);
            line("Enter = next, ^ = back", true);
            line("a digit picks a row", false);
            line("0 = skip setup", false);
            break;
        case WZ_NAME:
            snprintf(a, sizeof(a), "1 Name: %s", g_selfName);
            row(a, "1 = change it, Enter = keep", false);
            y += 6;
            line("Others see this name", false);
            line("when you announce.", false);
            break;
        case WZ_TIME: {
            snprintf(a, sizeof(a), "1 %s %s", tzCur().name, tzCur().label);
            row(a, "1 = next time zone", false);
            char d[12], t[12];
            fmtDate(d, sizeof(d)); fmtTime(t, sizeof(t));
            snprintf(a, sizeof(a), "Clock: %s %s", d, t);
            row(a, "sets itself from GPS / radios", false);
            y += 6;
            line("By hand later: Settings", false);
            line("S.1 Time > Set RTC", false);
            break;
        }
        case WZ_CLOCK:
            row("1 24 h", "13:45", !g_cfg.clock12);
            row("2 12 h", "1:45 PM", g_cfg.clock12);
            break;
        case WZ_POWER:
            for (int i = 0; i < 4; i++) {
                snprintf(a, sizeof(a), "%d %s", i + 1, power::modeName(kWizModes[i]));
                if (kWizModes[i] == power::MODE_CHECKIN) {
                    char at[12], ev[16];
                    fmtClockMin(g_pw.checkinStart, at, sizeof(at));
                    checkinEveryText(ev, sizeof(ev));
                    snprintf(b, sizeof(b), "LoRa at %s, every %s", at, ev);
                    row(a, b, g_pw.mode == kWizModes[i]);
                } else {
                    row(a, power::modeBlurb(kWizModes[i]), g_pw.mode == kWizModes[i]);
                }
            }
            break;
        case WZ_IDLE:
            for (int i = 0; i < IDLE_COUNT; i++) {
                snprintf(a, sizeof(a), "%d %s", i + 1, kIdleScreenName[i]);
                static const char* kSub[IDLE_COUNT] = {"logo, battery, time", "the map around you",
                                                       "the page you last read", "who to return it to"};
                row(a, kSub[i], g_cfg.idleScreen == i);
            }
            if (g_entry == Entry::FoundText) { snprintf(b, sizeof(b), "%s_", g_buf); line(b, false); }
            break;
        case WZ_MODE:
            row("1 Simple", "fewer options (coming later)", g_cfg.simple);
            row("2 Advanced", "everything (today's screens)", !g_cfg.simple);
            y += 6;
            line("Simple mode: coming later", false);
            break;
        case WZ_LOCK:
            row(lockHasPassword() ? "1 Keep the password" : "1 No password", "anyone can use it", false);
            row("2 Set a password", "asked after idle and at boot", lockOnIdle());
            if (passEntry(g_entry)) {
                snprintf(b, sizeof(b), "%s: %s", g_entry == Entry::PassConfirm ? "Again" : "Password",
                         editingSub(g_entry));
                line(b, false);
            }
            break;
        case WZ_IMPORT:
            snprintf(b, sizeof(b), g_wizListen ? "listening: %d received" : "the other: S.6 Share all", g_wizGot);
            row(g_wizListen ? "1 Stop listening" : "1 Listen over the radio", b, g_wizListen);
            row("2 Read contacts.txt (SD)", "ratdeckpro/contacts.txt", false);
            y += 6;
            snprintf(a, sizeof(a), "%d contact%s added", g_wizGot, g_wizGot == 1 ? "" : "s");
            line(a, g_wizGot > 0);
            break;
        case WZ_DONE:
            snprintf(a, sizeof(a), "Name: %s", g_selfName); line(a, false);
            snprintf(a, sizeof(a), "Time: %s, %s", tzCur().name, g_cfg.clock12 ? "12 h" : "24 h"); line(a, false);
            snprintf(a, sizeof(a), "Energy: %s", power::modeName(g_pw.mode)); line(a, false);
            snprintf(a, sizeof(a), "Idle: %s", kIdleScreenName[g_cfg.idleScreen]); line(a, false);
            snprintf(a, sizeof(a), "Lock: %s", lockHasPassword() ? "password" : "none"); line(a, false);
            y += 8;
            line("Enter = announce + finish", true);
            line("so others see this radio", false);
            break;
    }
}

// ---- Track back, GPX export -------------------------------------------------------------
// Track back (Atlas B, Settings S.1.10): from the track point nearest to you, walk the
// recorded points backwards; the guide line (and G's arrow) points at the next one, which
// moves on when you are within 25 m; at the first point: "Back at the start".
constexpr float kTbReach = 25.0f, kTbStep = 30.0f;

int tbStepBack(int from) {                  // the first earlier point at least kTbStep away
    for (int j = from - 1; j >= 0; j--)
        if (metersBetween(g_track[2 * j], g_track[2 * j + 1], g_track[2 * from], g_track[2 * from + 1]) >= kTbStep)
            return j;
    return 0;
}

bool tbStart() {
    trackLoad();
    double la, lo;
    if (g_trackN < 2) { toast("No track yet (S.1.10)"); return false; }
    if (!ownPosition(la, lo)) { toast("No position yet"); return false; }
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < g_trackN; i++) {
        const float d = metersBetween(la, lo, g_track[2 * i], g_track[2 * i + 1]);
        if (d < bd) { bd = d; best = i; }
    }
    if (best == 0 && bd < kTbReach) { toast("You are at the start"); return false; }
    g_tbIdx = bd < kTbReach ? tbStepBack(best) : best;
    g_tbOn = true;
    Serial.printf("[TRACK] back on: point %d of %d (%.0f m off the track)\n", g_tbIdx + 1, g_trackN, bd);
    toast("Track back: follow the line");
    requestRedraw();
    return true;
}

void tbStop(const char* why) {
    if (!g_tbOn) return;
    g_tbOn = false;
    Serial.printf("[TRACK] back %s\n", why);
    toast("Track back %s", why);
    requestRedraw();
}

void tbTick() {
    if (!g_tbOn) return;
    double la, lo;
    if (!ownPosition(la, lo) || g_tbIdx >= g_trackN) return;
    if (metersBetween(la, lo, g_track[2 * g_tbIdx], g_track[2 * g_tbIdx + 1]) > kTbReach) return;
    if (g_tbIdx == 0) {
        if (g_cfg.beep) g_audio.playAnnounce();
        tbStop("done: at the start");
        return;
    }
    g_tbIdx = tbStepBack(g_tbIdx);
    Serial.printf("[TRACK] back: next point %d\n", g_tbIdx + 1);
    requestRedraw();
}

// The whole SD track as GPX 1.1 in <SD>/ratdeckpro/tracks/track-YYYYMMDD-HHMM.gpx.
int trackExportGpx() {
    if (!sdMount()) { toast("No SD card"); return -1; }
    File in = g_sdStore.openFile(kTrackFile, FILE_READ);
    if (!in) { toast("No track on the card"); return -1; }
    struct tm t;
    char name[40] = "track.gpx";
    if (localNow(t)) snprintf(name, sizeof(name), "track-%04d%02d%02d-%02d%02d.gpx", t.tm_year + 1900, t.tm_mon + 1,
                              t.tm_mday, t.tm_hour, t.tm_min);
    char path[96];
    snprintf(path, sizeof(path), "%s/tracks/%s", SD_PATH_ROOT, name);
    File out = g_sdStore.openFile(path, FILE_WRITE);
    if (!out) { in.close(); toast("Cannot write the GPX"); return -1; }
    out.print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<gpx version=\"1.1\" creator=\"RatSpeak T-Deck Pro\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
              "<trk><name>RatSpeak track</name><trkseg>\n");
    uint8_t rec[12];
    int n = 0;
    while (in.read(rec, 12) == 12) {
        uint32_t e; float la, lo;
        memcpy(&e, rec, 4); memcpy(&la, rec + 4, 4); memcpy(&lo, rec + 8, 4);
        if (e) {
            const time_t tt = (time_t)e;
            struct tm u;
            gmtime_r(&tt, &u);
            out.printf("<trkpt lat=\"%.6f\" lon=\"%.6f\"><time>%04d-%02d-%02dT%02d:%02d:%02dZ</time></trkpt>\n", la, lo,
                       u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
        } else {
            out.printf("<trkpt lat=\"%.6f\" lon=\"%.6f\"></trkpt>\n", la, lo);
        }
        n++;
    }
    out.print("</trkseg></trk>\n</gpx>\n");
    out.close();
    in.close();
    snprintf(g_trackGpx, sizeof(g_trackGpx), "tracks/%s", name);
    Serial.printf("[TRACK] %d points -> %s\n", n, path);
    return n;
}

// ---- Bearing + distance (G on the Atlas) ------------------------------------------------
// For when the map is too much: a big arrow to the selected contact, waypoint or track-back
// point, the distance and the age of a contact's fix. No compass on board: while you move
// (GPS speed over 1 knot) the arrow is relative to your course over ground ("ahead" = up),
// standing still it is north-up.
enum NavKind : uint8_t { NAV_CONTACT, NAV_WPT, NAV_TRACK };
NavKind g_navKind = NAV_CONTACT;
int     g_navIdx = 0;
bool    g_navArrowOk = false;
constexpr int kNavCy = 116, kNavR = 54, kNavNumY = 184;   // ring centre / radius, distance digits
char    g_navNum[12] = "";                  // the distance number, drawn big by stampNav
int     g_navNumX = 0;
// 5x7 digits (and '.') at scale 5 for the distance.
const uint8_t kBig5x7[11][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}};                       // [10] = '.'
constexpr int kBigScale = 4;
int navBigWidth(const char* s) {
    int w = 0;
    for (; *s; s++) w += (*s == '.' ? 3 : 6) * kBigScale;
    return w;
}
void navBigStamp(int x, int y, const char* s) {
    const bool ink = !g_cfg.dark;
    for (; *s; s++) {
        const int g = *s == '.' ? 10 : (*s >= '0' && *s <= '9') ? *s - '0' : -1;
        const int cols = *s == '.' ? 3 : 6;
        if (g >= 0)
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (kBig5x7[g][r] & (0x10 >> c))
                        for (int dy = 0; dy < kBigScale; dy++)
                            for (int dx = 0; dx < kBigScale; dx++) {
                                const int px = x + (c - (g == 10 ? 2 : 0)) * kBigScale + dx, py = y + r * kBigScale + dy;
                                if (px >= 0 && py >= 0 && px < W && py < H) g_display.setPixel(px, py, ink);
                            }
        x += cols * kBigScale;
    }
}
float   g_navArrowDeg = 0;                 // screen angle: 0 = up, clockwise
float   g_navShownM = -1, g_navShownBrg = -1000;
uint32_t g_navShownMin = 0;

void fmtDist(double m, char* out, size_t n) {
    if (g_cfg.imperial) {
        const double ft = m * 3.28084;
        if (ft < 1000) snprintf(out, n, "%d ft", (int)(ft + 0.5));
        else if (m < 160934) snprintf(out, n, "%.2f mi", m / 1609.344);
        else snprintf(out, n, "%d mi", (int)(m / 1609.344));
    } else {
        if (m < 1000) snprintf(out, n, "%d m", (int)(m + 0.5));
        else if (m < 100000) snprintf(out, n, "%.2f km", m / 1000.0);
        else snprintf(out, n, "%d km", (int)(m / 1000.0));
    }
}

bool navTarget(double& la, double& lo, char* name, size_t n) {
    switch (g_navKind) {
        case NAV_TRACK:
            if (!g_tbOn || g_navIdx >= g_trackN) return false;
            g_navIdx = g_tbIdx;
            la = g_track[2 * g_navIdx]; lo = g_track[2 * g_navIdx + 1];
            snprintf(name, n, "Track back, point %d", g_navIdx + 1);
            return true;
        case NAV_WPT:
            if (g_navIdx < 0 || g_navIdx >= g_wptCount) return false;
            la = g_wpt[g_navIdx].lat; lo = g_wpt[g_navIdx].lon;
            snprintf(name, n, "W%d %s", g_navIdx + 1, g_wpt[g_navIdx].name);
            return true;
        default:
            if (g_navIdx < 0 || g_navIdx >= g_cposCount || !g_cpos[g_navIdx].hasPos) return false;
            la = g_cpos[g_navIdx].lat; lo = g_cpos[g_navIdx].lon;
            char nm[24];
            peerName(g_cpos[g_navIdx].dest, nm, sizeof(nm));
            snprintf(name, n, "#%u %s", (unsigned)g_cpos[g_navIdx].num, nm);
            return true;
    }
}

void navOpen() {
    if (g_tbOn) { g_navKind = NAV_TRACK; g_navIdx = g_tbIdx; }
    else if (g_mapSel >= 0) { g_navKind = NAV_CONTACT; g_navIdx = g_mapSel; }
    else if (g_wptSel >= 0) { g_navKind = NAV_WPT; g_navIdx = g_wptSel; }
    else { toast("Pick a contact (N) or waypoint (P) first"); requestRedraw(); return; }
    g_navOpen = true;
    g_navKeysOpen = false;
    g_navShownM = -1;
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.printf("[NAV] open kind=%d idx=%d\n", (int)g_navKind, g_navIdx);
}

void navClose() {
    g_navOpen = false;
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.println("[NAV] closed");
}

void navKey(const KeyEvent& ev) {
    const char c = (char)toupper((unsigned char)ev.character);
    if (c == kKeyGo || ev.enter) { navClose(); return; }
    if (c == 'M') { navClose(); openMenu(); return; }
    if (c == kKeyBack) { if (g_tbOn) tbStop("stopped"); navClose(); return; }
}

bool navMoving() { return gpsFresh() && g_gpsCourseOk && g_gpsSpeedKn >= 1.0f && millis() - g_gpsCourseAt < 10000; }

void renderNav() {
    double tla, tlo, la, lo;
    char name[40], line[64];
    g_navArrowOk = false;
    if (!navTarget(tla, tlo, name, sizeof(name))) { navClose(); return; }
    drawHeader(name, false);
    const bool own = ownPosition(la, lo);
    if (!own) {
        label(0, 120, W, 20, "No own position yet", true);
        label(0, 142, W, 18, "(GPS or saved position S.1)");
    } else {
        const double m = metersBetween(la, lo, tla, tlo);
        const double brg = bearingDeg(la, lo, tla, tlo);
        const bool moving = navMoving();
        g_navArrowDeg = (float)(moving ? fmod(brg - g_gpsCourse + 720.0, 360.0) : brg);
        g_navArrowOk = true;
        label(0, kNavCy - kNavR - 18, W, 16, moving ? "ahead" : "N", true);
        char d[24];                                       // "1.24 km": big digits, then the unit
        fmtDist(m, d, sizeof(d));
        const char* sp = strchr(d, ' ');
        const int nl0 = sp ? (int)(sp - d) : (int)strlen(d);
        snprintf(g_navNum, sizeof(g_navNum), "%.*s", nl0, d);
        const int unitX = (W - navBigWidth(g_navNum) - 34) / 2 + navBigWidth(g_navNum) + 6;
        g_navNumX = (W - navBigWidth(g_navNum) - 34) / 2;
        text(unitX, kNavNumY + 14, sp ? sp + 1 : "", true);
        snprintf(line, sizeof(line), "%s %d deg%s", compass8(brg), (int)(brg + 0.5), moving ? "" : "  (north up)");
        label(0, kNavNumY + 40, W, 16, line);
        if (g_navKind == NAV_CONTACT) {
            const uint32_t now = (uint32_t)time(nullptr), at = g_cpos[g_navIdx].at;
            const uint32_t age = (at && now > at) ? (now - at) / 60 : 0;
            if (age < 120) snprintf(line, sizeof(line), "their fix %lu min old", (unsigned long)age);
            else snprintf(line, sizeof(line), "their fix %lu h old", (unsigned long)(age / 60));
        } else if (moving) {
            const float kmh = g_gpsSpeedKn * 1.852f;
            if (g_cfg.imperial) snprintf(line, sizeof(line), "you: %.0f mph, course %d deg", kmh / 1.609f, (int)g_gpsCourse);
            else snprintf(line, sizeof(line), "you: %.0f km/h, course %d deg", kmh, (int)g_gpsCourse);
        } else {
            snprintf(line, sizeof(line), "you: standing (no course)");
        }
        label(0, kNavNumY + 58, W, 16, line);
        g_navShownM = (float)m;
        g_navShownBrg = (float)brg;
        g_navShownMin = millis() / 60000UL;
    }
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : (g_tbOn ? "G map  B stop track back" : "G or ^ back to the map"));
}

// Thick line straight into the frame buffer (after LVGL's frame, like the map).
void navLine(float x0, float y0, float x1, float y1, int thick) {
    const bool ink = !g_cfg.dark;
    const float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const int steps = (int)len + 1;
    for (int i = 0; i <= steps; i++) {
        const float x = x0 + (x1 - x0) * i / steps, y = y0 + (y1 - y0) * i / steps;
        for (int dy = -thick / 2; dy <= thick / 2; dy++)
            for (int dx = -thick / 2; dx <= thick / 2; dx++) {
                const int px = (int)lroundf(x) + dx, py = (int)lroundf(y) + dy;
                if (px >= 0 && py >= 0 && px < W && py < H) g_display.setPixel(px, py, ink);
            }
    }
}

void stampNav() {
    if (!g_navArrowOk) return;
    navBigStamp(g_navNumX, kNavNumY, g_navNum);
    const float cx = W / 2.0f, cy = kNavCy, r = kNavR;
    for (int a = 0; a < 360; a += 2) {                      // the ring
        const float t = a * 3.14159265f / 180.0f;
        navLine(cx + r * sinf(t), cy - r * cosf(t), cx + r * sinf(t), cy - r * cosf(t), 2);
    }
    const float t = g_navArrowDeg * 3.14159265f / 180.0f;
    const float tipX = cx + (r - 6) * sinf(t), tipY = cy - (r - 6) * cosf(t);
    const float tailX = cx - (r - 18) * sinf(t), tailY = cy + (r - 18) * cosf(t);
    navLine(tailX, tailY, tipX, tipY, 5);
    for (int side = -1; side <= 1; side += 2) {             // the head
        const float h = t + side * 2.6f;
        navLine(tipX, tipY, tipX + 26 * sinf(h), tipY - 26 * cosf(h), 5);
    }
    g_display.markDirty();
}

// Redraw when the numbers change (position, their fix, course) or each minute (ages).
void navTick() {
    tbTick();
    if (!g_navOpen) return;
    static uint32_t last = 0;
    if (millis() - last < 2000) return;
    last = millis();
    double tla, tlo, la, lo;
    char name[40];
    if (!navTarget(tla, tlo, name, sizeof(name)) || !ownPosition(la, lo)) return;
    const float m = metersBetween(la, lo, tla, tlo);
    const float brg = (float)bearingDeg(la, lo, tla, tlo);
    const bool far = g_navShownM < 0 || fabsf(m - g_navShownM) > (g_navShownM * 0.02f + 3.0f);
    const bool turned = fabsf(brg - g_navShownBrg) > 5.0f;
    if (far || turned || millis() / 60000UL != g_navShownMin || navMoving()) requestRedraw();
}

// ---- Range test (Settings S.7 LoRa > Range test) -----------------------------------------
// Two units in range-test mode ping each other: every N s this one sends a ping to the chosen
// partner; a unit in range-test mode answers a ping meant for it with a pong carrying the
// RSSI / SNR it heard. Both log every event to <SD>/ratdeckpro/rangetest/rt-<date>-<time>.csv
// (time, event, seq, peer, rssi, snr, peer_rssi, peer_snr, lat, lon, fix). Frames are raw LoRa
// (first byte 0xFF = an "access code" frame other Reticulum nodes drop), so nothing reaches
// the inbox. PC: tools/rangetest/rangetest_map.py makes a map / plot from the CSV.
constexpr uint8_t kRtMagic[4] = {0xFF, 'R', 'T', '1'};
uint8_t  g_rtPeer[16];
uint16_t g_rtEvery = 15, g_rtSeq = 0;
uint32_t g_rtSent = 0, g_rtGot = 0, g_rtLost = 0, g_rtHeard = 0, g_rtNextAt = 0;
bool     g_rtWaiting = false;                // the last ping has no pong yet
int      g_rtRssi = 0, g_rtPeerRssi = 0, g_rtHeardRssi = 0;
float    g_rtSnr = 0, g_rtPeerSnr = 0, g_rtHeardSnr = 0;
char     g_rtFile[40] = "";
uint32_t g_rtShownAt = 0;

void rtLog(const char* ev, uint16_t seq, int rssi, float snr, int prssi, float psnr) {
    if (!g_rtFile[0] || !sdMount()) return;
    char path[96];
    snprintf(path, sizeof(path), "%s/rangetest/%s", SD_PATH_ROOT, g_rtFile);
    File f = g_sdStore.openFile(path, FILE_APPEND);
    if (!f) return;
    char peer[33], ts[24] = "";
    hashToHex(g_rtPeer, peer);
    const time_t now = time(nullptr);
    if (now > 1600000000) { struct tm u; gmtime_r(&now, &u); strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &u); }
    const bool fix = gpsFresh();
    double la = 0, lo = 0;
    ownPosition(la, lo);
    f.printf("%s,%s,%u,%.8s,%d,%.1f,%d,%.1f,%.6f,%.6f,%d\n", ts, ev, (unsigned)seq, peer, rssi, snr, prssi, psnr,
             la, lo, fix ? 1 : 0);
    f.close();
}

void rtSend(char type, uint16_t seq, const uint8_t to[16], int rssi, float snr) {
    uint8_t b[18];
    memcpy(b, kRtMagic, 4);
    b[4] = (uint8_t)type;
    b[5] = (uint8_t)(seq >> 8); b[6] = (uint8_t)seq;
    memcpy(b + 7, g_selfHash, 4);
    memcpy(b + 11, to, 4);
    b[15] = (uint8_t)(int8_t)constrain(rssi, -127, 0);
    b[16] = (uint8_t)(int8_t)constrain((int)lroundf(snr * 4), -127, 127);
    b[17] = 0;
    if (g_loraIf) g_loraIf->sendRaw(b, sizeof(b));
}

bool rtRxTap(const uint8_t* d, size_t n, int rssi, float snr) {
    if (n != 18 || memcmp(d, kRtMagic, 4) != 0) return false;
    if (!g_rtOn || memcmp(d + 11, g_selfHash, 4) != 0) return true;   // ours, but not for us: drop
    const uint16_t seq = (uint16_t)(d[5] << 8 | d[6]);
    if (d[4] == 'P') {                        // a ping: answer with what we heard
        g_rtHeard++;
        g_rtHeardRssi = rssi; g_rtHeardSnr = snr;
        rtLog("ping-heard", seq, rssi, snr, 0, 0);
        uint8_t from[16] = {0};
        memcpy(from, d + 7, 4);
        // The reply goes to the sender's prefix (it only checks the first 4 bytes).
        rtSend('O', seq, from, rssi, snr);
        Serial.printf("[RT] ping %u heard %d dBm %.1f dB\n", (unsigned)seq, rssi, snr);
    } else if (d[4] == 'O' && memcmp(d + 7, g_rtPeer, 4) == 0) {
        if (seq == g_rtSeq && g_rtWaiting) { g_rtWaiting = false; g_rtGot++; }
        g_rtRssi = rssi; g_rtSnr = snr;
        g_rtPeerRssi = (int8_t)d[15]; g_rtPeerSnr = (int8_t)d[16] / 4.0f;
        rtLog("pong", seq, rssi, snr, g_rtPeerRssi, g_rtPeerSnr);
        Serial.printf("[RT] pong %u: here %d dBm %.1f dB, there %d dBm %.1f dB\n", (unsigned)seq, rssi, snr,
                      g_rtPeerRssi, g_rtPeerSnr);
    }
    if (g_rtOpen) requestRedraw();
    return true;
}

void rtStart(const uint8_t peer[16], int everySecs) {
    memcpy(g_rtPeer, peer, 16);
    if (everySecs >= 5 && everySecs <= 600) g_rtEvery = (uint16_t)everySecs;
    g_rtOn = true;
    g_rtOpen = true;
    g_rtSeq = 0;
    g_rtSent = g_rtGot = g_rtLost = g_rtHeard = 0;
    g_rtWaiting = false;
    g_rtRssi = g_rtPeerRssi = g_rtHeardRssi = 0;
    g_rtNextAt = millis() + 2000;
    struct tm t;
    if (localNow(t)) snprintf(g_rtFile, sizeof(g_rtFile), "rt-%04d%02d%02d-%02d%02d.csv", t.tm_year + 1900, t.tm_mon + 1,
                              t.tm_mday, t.tm_hour, t.tm_min);
    else snprintf(g_rtFile, sizeof(g_rtFile), "rt-%lu.csv", (unsigned long)(millis() / 1000));
    if (sdMount()) {
        g_sdStore.ensureDir(SD_PATH_ROOT "/rangetest");
        char path[96];
        snprintf(path, sizeof(path), "%s/rangetest/%s", SD_PATH_ROOT, g_rtFile);
        File f = g_sdStore.openFile(path, FILE_WRITE);
        if (f) { f.print("time_utc,event,seq,peer,rssi,snr,peer_rssi,peer_snr,lat,lon,fix\n"); f.close(); }
    } else {
        g_rtFile[0] = 0;
        toast("No SD card: stats only");
    }
    char nm[24];
    peerName(peer, nm, sizeof(nm));
    Serial.printf("[RT] start with %s every %u s, file %s\n", nm, (unsigned)g_rtEvery, g_rtFile[0] ? g_rtFile : "-");
    g_fullRefreshReq = true;
    requestRedraw();
}

void rtStop() {
    if (!g_rtOn && !g_rtOpen) return;
    if (g_rtOn) rtLog("stop", g_rtSeq, 0, 0, 0, 0);
    g_rtOn = false;
    g_rtOpen = false;
    Serial.printf("[RT] stop: sent %lu replies %lu lost %lu heard %lu\n", (unsigned long)g_rtSent,
                  (unsigned long)g_rtGot, (unsigned long)g_rtLost, (unsigned long)g_rtHeard);
    g_fullRefreshReq = true;
    requestRedraw();
}

void rtTick() {
    if (!g_rtOn) return;
    if ((int32_t)(millis() - g_rtNextAt) < 0) return;
    g_rtNextAt = millis() + g_rtEvery * 1000UL;
    if (g_rtWaiting) {                        // no pong for the last one
        g_rtLost++;
        rtLog("lost", g_rtSeq, 0, 0, 0, 0);
        Serial.printf("[RT] ping %u lost\n", (unsigned)g_rtSeq);
    }
    g_rtSeq++;
    g_rtSent++;
    g_rtWaiting = true;
    rtSend('P', g_rtSeq, g_rtPeer, 0, 0);
    rtLog("ping", g_rtSeq, 0, 0, 0, 0);
    Serial.printf("[RT] ping %u sent\n", (unsigned)g_rtSeq);
    if (g_rtOpen && millis() - g_rtShownAt > 5000) requestRedraw();
}

void rtKey(const KeyEvent& ev) {
    const char c = ev.character;
    static const uint16_t kEvery[4] = {10, 15, 30, 60};
    if (c >= '1' && c <= '4') {
        g_rtEvery = kEvery[c - '1'];
        g_rtNextAt = millis() + g_rtEvery * 1000UL;
        toast("Every %u s", (unsigned)g_rtEvery);
        requestRedraw();
        return;
    }
    if (toupper((unsigned char)c) == 'M') { g_rtOpen = false; openMenu(); return; }   // keeps running
}

void renderRangeTest() {
    char nm[24], a[56];
    peerName(g_rtPeer, nm, sizeof(nm));
    snprintf(a, sizeof(a), "Range test: %.14s", nm);
    drawHeader(a, false);
    int y = LIST_TOP;
    auto line = [&](const char* t, bool bold) { label(0, y, W, 18, t, bold); y += 20; };
    const unsigned pct = g_rtSent ? (unsigned)((g_rtLost * 100 + g_rtSent / 2) / g_rtSent) : 0;
    snprintf(a, sizeof(a), "pings %lu  replies %lu", (unsigned long)g_rtSent, (unsigned long)g_rtGot); line(a, true);
    snprintf(a, sizeof(a), "lost %lu (%u%%)  every %u s", (unsigned long)g_rtLost, pct, (unsigned)g_rtEvery); line(a, false);
    y += 4;
    line("Last reply heard here:", false);
    if (g_rtGot) { snprintf(a, sizeof(a), "%d dBm  SNR %.1f dB", g_rtRssi, g_rtSnr); line(a, true); }
    else line("-", false);
    line("They heard our ping:", false);
    if (g_rtGot) { snprintf(a, sizeof(a), "%d dBm  SNR %.1f dB", g_rtPeerRssi, g_rtPeerSnr); line(a, true); }
    else line("-", false);
    snprintf(a, sizeof(a), "their pings heard: %lu", (unsigned long)g_rtHeard); line(a, false);
    double la, lo;
    if (gpsFresh() && ownPosition(la, lo)) snprintf(a, sizeof(a), "me %.5f,%.5f", la, lo);
    else snprintf(a, sizeof(a), "me: no GPS fix (logged as such)");
    line(a, false);
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : "^ stop  1-4: 10/15/30/60 s");
    g_rtShownAt = millis();
}

void render() {
    const uint32_t t0 = millis();
    clearRoot();
    if (g_locked) {                    // nothing else is drawn (no touch targets either)
        renderLock();
        lv_refr_now(nullptr);
        drawLogo(W / 2, 36);
        g_redraw = false;
        if (g_fullRefreshReq) { g_fullRefreshReq = false; g_display.fullRefresh(); }
        else g_display.refreshIfDirty();
        keyboardSetNumLayer(false);
        Serial.printf("[UI] lock frame %ums\n", (unsigned)(millis() - t0));
        return;
    }
    if (g_usbDrive)                    renderUsbDrive();
    else if (g_threadOpen)             renderThread();
    else if (g_entry == Entry::Name)   renderRename();
    else if (g_wizOpen)                renderWizard();
    else if (g_navOpen)                renderNav();
    else if (g_rtOpen)                 renderRangeTest();
    else if (g_cardOpen)               renderCard();
    else if (g_libOpen)                renderLibrary();
    else if (g_gameOpen)               renderGame();
    else if (g_chessOpen)              renderChess();
    else if (mapview::isOpen())        renderMapHeader();
    else switch (g_screen) {
        case SCR_HOME:     renderHome(); break;
        case SCR_SETTINGS: renderSettings(); break;
        default:           renderList(); break;
    }
    drawTopBar();
    drawBottomBar();
    drawMenuOverlay();
    lv_refr_now(nullptr);
    if (mapview::isOpen() && !g_threadOpen && !g_menuOpen && !g_libOpen && !g_gameOpen && !g_chessOpen && !g_placeOpen && !g_navOpen)
        stampMap();   // after LVGL's clear
    if (g_chessOpen && !g_menuOpen && !g_threadOpen && !g_libOpen && !g_usbDrive && !g_cardOpen && !g_navOpen && !g_rtOpen &&
        !g_wizOpen && g_entry != Entry::Name)
        stampChess();
    if (g_navOpen && !g_menuOpen) stampNav();
    if (g_screen == SCR_SETTINGS && g_page == PG_ENERGY_GRAPH && !g_menuOpen && !g_threadOpen && !mapview::isOpen() &&
        !g_libOpen && !g_gameOpen && !g_chessOpen && !g_wizOpen && !g_rtOpen)
        stampGraphLabels();
    const uint32_t t1 = millis();
    // Clear the request first: keys queued during the panel refresh may ask again.
    g_redraw = false;
    g_redrawAt = 0;
    if (g_fullRefreshReq) { g_fullRefreshReq = false; g_display.fullRefresh(); }
    else g_display.refreshIfDirty();
    syncNumLayer();
    Serial.printf("[UI] frame %ums (build %u, panel %u)\n", (unsigned)(millis() - t0),
                  (unsigned)(t1 - t0), (unsigned)g_display.lastRefreshMs());
}

// ---- Idle splash -----------------------------------------------------------------
int g_idleShownUnread = -1, g_idleShownPct = -1;
uint32_t g_idleShownMin = 0;

void spaced(const char* in, char* out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < n; i++) {
        out[j++] = in[i];
        if (in[i + 1]) out[j++] = ' ';
    }
    out[j] = 0;
}

bool paintIdleMap();

bool paintIdleLibrary();
void paintIdleFound();
constexpr int kIdleMapTop = 38, kIdleMapBottom = 300;

// Every idle screen: time, battery and unread across the top, the date and the radio's name
// under them, and the wake / LOCKED line at the bottom.
void drawIdleTop() {
    char t[12], d[12], line[48];
    fmtTime(t, sizeof(t));
    fmtDate(d, sizeof(d));
    const int pct = g_bat.valid ? g_bat.pct : -1;
    const int unread = g_storeReady ? g_msgStore.totalUnreadCount() : 0;
    box(0, 0, W, kIdleMapTop, 1);
    char left[10] = "";
    const int h10 = g_bat.usb ? -1 : batHoursLeft10(nullptr);
    if (h10 >= 0) snprintf(left, sizeof(left), h10 < 480 ? " ~%dh" : " ~%dd", h10 < 480 ? (h10 + 5) / 10 : (h10 + 120) / 240);
    if (unread > 0) snprintf(line, sizeof(line), "%s  %d%%%s%s  %d NEW", t, pct, g_bat.charging ? "+" : "", left, unread);
    else snprintf(line, sizeof(line), "%s  %d%%%s%s  no new", t, pct, g_bat.charging ? "+" : "", left);
    label(0, 0, W, 20, line, true);
    snprintf(line, sizeof(line), "%s  %s", d, g_selfName);
    label(0, 18, W, 18, line);
    g_idleShownUnread = unread;
    g_idleShownPct = pct;
    g_idleShownMin = millis() / 60000UL;
}

void drawIdleFoot(const char* unlockedText) {
    box(0, kIdleMapBottom, W, H - kIdleMapBottom, 1);
    label(0, kIdleMapBottom, W, H - kIdleMapBottom, g_locked ? "LOCKED | press a key" : unlockedText, g_locked);
}

void paintIdle() {
    // Locked: the map (your position) and a library page are not shown; If found is.
    uint8_t sc = g_cfg.idleScreen;
    if (g_locked && (sc == IDLE_MAP || sc == IDLE_LIBRARY)) sc = IDLE_SPLASH;
    if (sc == IDLE_MAP && paintIdleMap()) return;
    if (sc == IDLE_LIBRARY && paintIdleLibrary()) return;
    if (sc == IDLE_FOUND) { paintIdleFound(); return; }
    clearRoot();
    drawIdleTop();
    const int y0 = 140, step = 20;
    label(0, y0, W, 20, "RatSpeak", true);
    char line[40];
    label(0, y0 + step, W, 18,
          g_bat.charging ? (g_bat.usb ? "charging | plugged in" : "charging")
                         : (g_bat.usb ? "plugged in" : "unplugged"));
    uint32_t s = millis() / 1000UL;
    label(0, y0 + 2 * step + 6, W, 16, "uptime");
    snprintf(line, sizeof(line), "%05luD : %02luH : %02luM", (unsigned long)(s / 86400UL),
             (unsigned long)((s / 3600UL) % 24UL), (unsigned long)((s / 60UL) % 60UL));
    label(0, y0 + 3 * step + 6, W, 16, line);
    label(0, y0 + 4 * step + 14, W, 18, tzCur().name);
    drawIdleFoot("press a KEY to wake");
    lv_refr_now(nullptr);
    drawLogo(W / 2, kIdleMapTop + 50);       // stamped after LVGL so the clear does not wipe it
}

// Idle screen "Library page": the page last read in the Library (else the first page of the
// first file), with the reader's wrap setting. false = no card / no files: the splash.
bool paintIdleLibrary() {
    if (!sdMount()) {
        clearRoot();
        drawIdleTop();
        label(0, 140, W, 20, "No library page", true);
        label(0, 162, W, 18, "(no SD card)");
        drawIdleFoot("press a KEY to wake");
        lv_refr_now(nullptr);
        return true;
    }
    char path[96];
    uint32_t off = 0;
    bool wrap = true;
    if (g_cfg.libLast[0]) {
        snprintf(path, sizeof(path), "%s/%s", kLibDir, g_cfg.libLast);
        off = g_cfg.libLastOff;
        wrap = g_cfg.libLastWrap;
    }
    if (!g_cfg.libLast[0] || !g_sdStore.exists(path)) {
        libScan();
        if (!g_libCount) {
            clearRoot();
            drawIdleTop();
            label(0, 140, W, 20, "No library page", true);
            label(0, 162, W, 18, "(ratdeckpro/library on the SD card)");
            drawIdleFoot("press a KEY to wake");
            lv_refr_now(nullptr);
            return true;
        }
        snprintf(path, sizeof(path), "%s/%s", kLibDir, g_libFiles[0].name);
        off = 0;
        const char* dot = strrchr(g_libFiles[0].name, '.');
        wrap = !(dot && !strcasecmp(dot, ".csv"));
    }
    char keepPath[96];
    strlcpy(keepPath, g_libPath, sizeof(keepPath));
    const bool keepWrap = g_libWrap;
    strlcpy(g_libPath, path, sizeof(g_libPath));
    g_libWrap = wrap;
    static char lines[kLibLines][kLibLineMax];
    uint32_t next;
    const int n = libLayout(off, kLibLines, lines, nullptr, &next);
    strlcpy(g_libPath, keepPath, sizeof(g_libPath));
    g_libWrap = keepWrap;
    clearRoot();
    drawIdleTop();
    const char* nm = strrchr(path, '/');
    label(0, kIdleMapTop + 1, W, 18, nm ? nm + 1 : path, true);
    for (int l = 0; l < n; l++) text(6, kIdleMapTop + 22 + l * kLibLineH, lines[l], false, W - 10);
    drawIdleFoot("press a KEY to wake");
    lv_refr_now(nullptr);
    return true;
}

// Idle screen "If found": who to return the radio to, as the owner typed it (';' = new
// line), and this radio's name + LXMF address so a finder with RatSpeak can message it.
// Also the locked screen; e-ink keeps it readable when the battery is flat.
void paintIdleFound() {
    clearRoot();
    drawIdleTop();
    int y = kIdleMapTop + 6;
    label(0, y, W, 20, "IF FOUND, PLEASE", true);
    label(0, y + 20, W, 20, "RETURN TO:", true);
    y += 48;
    if (g_cfg.foundText[0]) {
        char t[sizeof(g_cfg.foundText)];
        strlcpy(t, g_cfg.foundText, sizeof(t));
        int lines = 0;
        for (char* tok = strtok(t, ";|"); tok && lines < 7; tok = strtok(nullptr, ";|"), lines++) {
            while (*tok == ' ') tok++;
            label(0, y, W, 20, tok, lines == 0);
            y += 22;
        }
    } else {
        label(0, y, W, 20, "(owner text not set:");
        label(0, y + 22, W, 20, "Settings S.3.2)");
        y += 44;
    }
    if (g_cfg.foundAddr) {
        y = y < 226 ? 226 : y + 4;
        label(0, y, W, 18, g_selfName, true);
        char hex[33];
        strlcpy(hex, g_destHex[0] ? g_destHex : "--", sizeof(hex));
        lv_obj_t* o = lv_label_create(g_root);       // 32 hex digits fit in the 12 px font
        lv_label_set_text(o, hex);
        lv_obj_set_style_text_font(o, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(o, fg(), 0);
        const int w = lv_txt_get_width(hex, strlen(hex), &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE);
        lv_obj_set_pos(o, (W - w) / 2, y + 20);
        label(0, y + 34, W, 16, "message me on RatSpeak");
    }
    drawIdleFoot("press a KEY to wake");
    lv_refr_now(nullptr);
}

// Idle screen "Map" (S.3.2 "Idle screen"): the map centred on the GPS fix, else the
// saved position, else the card's home.txt, at zoom 13 (or the deepest level with tiles),
// with time / battery / unread across the top. false = no position: the splash is shown.
constexpr int kIdleMapZoom = 13;

bool paintIdleMap() {
    double lat = 0, lon = 0;
    bool have = false;
    if (g_gpsPosValid) { lat = g_gpsLat; lon = g_gpsLon; have = true; }
    else if (g_cfg.hasPos) { lat = g_cfg.lat; lon = g_cfg.lon; have = true; }
    mapview::setReader(mapReadTile);
    g_mapSdTried = false;
    if (!have) {
        char txt[48] = "";
        int z = 0;
        const size_t n = mapReadTile("/maps/home.txt", 0, (uint8_t*)txt, sizeof(txt) - 1);
        txt[n < sizeof(txt) ? n : sizeof(txt) - 1] = 0;
        have = n && sscanf(txt, "%lf %lf %d", &lat, &lon, &z) >= 2;
    }
    if (!have) return false;
    const bool wasOpen = mapview::isOpen();
    if (!wasOpen) {
        mapFeedPosition();
        mapview::open(lat, lon, true, g_gpsPosValid);
        mapview::setZoom(kIdleMapZoom);
        mapview::fitZoom();
    }

    clearRoot();
    drawIdleTop();
    char pos[56];
    gpsPosText(pos, sizeof(pos));
    drawIdleFoot(pos);
    lv_refr_now(nullptr);
    mapContactsFeed();                       // contacts' last positions on the idle map too
    mapview::draw(mapPixel, 0, kIdleMapTop, W, kIdleMapBottom - kIdleMapTop, g_cfg.dark);
    g_display.markDirty();
    if (!wasOpen) { mapview::close(); mapReadDone(); }
    return true;
}

void enterIdle() {
    g_idleActive = true;
    g_menuOpen = false;
    paintIdle();
    g_display.fullRefresh();
    Serial.println("[IDLE] enter");
}

void updateIdle() {
    const int pct = g_bat.valid ? g_bat.pct : -1;
    const int unread = g_storeReady ? g_msgStore.totalUnreadCount() : 0;
    if (pct == g_idleShownPct && unread == g_idleShownUnread && millis() / 60000UL == g_idleShownMin) return;
    paintIdle();
    g_display.refreshIfDirty();
}

void drawBootSplash() {
    clearRoot();
    label(0, 130, W, 22, "RatSpeak", true);
    label(0, 160, W, 18, "T-Deck Pro");
    label(0, 184, W, 18, "LoRa / LXMF mesh");
    label(0, 240, W, 18, "BOOTING...", true);
    lv_refr_now(nullptr);
    drawLogo(W / 2, 64);
    g_display.refreshIfDirty();
}

// =============================================================================
// 7. Input — handlers only mutate state and request redraws
// =============================================================================

void noteInput() { g_lastInputMs = millis() ? millis() : 1; }

void openThread(const uint8_t peer[16]) {
    if (isSelf(peer)) { toast("Pick a remote peer"); return; }
    memcpy(g_threadPeer, peer, 16);
    g_threadOpen = true;
    g_tmenuOpen = false;
    g_txStr[0] = 0;
    loadHistory(peer);
    startEntry(Entry::Message);
}

void closeThread() {
    g_threadOpen = false;
    endEntry();
    loadRows(SCR_INBOX);           // unread counts changed
    requestRedraw();
}

void startNameEntry(const uint8_t peer[16]) {
    memcpy(g_nameTarget, peer, 16);
    char cur[24] = "";
    if (isSelf(peer)) snprintf(cur, sizeof(cur), "%s", g_selfName);
    else {
        peerName(peer, cur, sizeof(cur));
        char hex[33];
        hashToHex(peer, hex);
        if (!strncmp(cur, hex, strlen(cur))) cur[0] = 0;   // hex placeholder: start empty
    }
    startEntry(Entry::Name, cur);
}

void goScreen(ScreenId s) {
    g_menuOpen = false;
    g_libOpen = false;
    g_gameOpen = false;
    g_chessOpen = false;
    g_cardOpen = false;
    g_shareActive = false;
    if (mapview::isOpen()) { mapview::close(); mapReadDone(); }
    if (g_threadOpen) { g_threadOpen = false; }
    if (entryActive()) endEntry();
    if (s == SCR_SETTINGS) {          // doc: M -> S always lands on the Settings root
        g_navDepth = 0;
        g_page = PG_ROOT;
        g_sel = 0;
        g_scroll = 0;
    }
    if (g_screen != s) { g_listScroll = 0; g_filter[0] = 0; }
    g_screen = s;
    if (s == SCR_INBOX || s == SCR_CONTACTS || s == SCR_PEERS) loadRows(s);
    requestRedraw();
    Serial.printf("[UI] -> %s\n", screenName(s));
}

// Settings > Waypoints > row: leave Settings, map over the Inbox, that waypoint selected.
void wptOpenFromSettings(int i) {
    goScreen(SCR_INBOX);
    openMap();
    wptSelect(i);
}

void openMenu() {
    if (g_wizOpen) return;                   // finish or skip the wizard first
    g_menuOpen = true;
    g_menuSel = 0;
    for (int i = 0; i < kMenuMap; i++) if (kMenuScreens[i] == g_screen) g_menuSel = i;   // not Map / Library
    requestRedraw();
}

void closeMenu() { g_menuOpen = false; requestRedraw(); }

bool isListScreen(ScreenId s) { return s == SCR_INBOX || s == SCR_CONTACTS || s == SCR_PEERS; }

void pageJump(int dir) {
    if (!isListScreen(g_screen) || !g_rowCount) return;
    const int ps = pageSize(g_screen), pages = (g_rowCount + ps - 1) / ps;
    const int p = g_listSel / ps + dir;
    if (p < 0 || p >= pages) { toast(dir > 0 ? "Last page" : "First page"); return; }
    g_listSel = p * ps;
    g_listScroll = g_listSel;
    clampScroll(g_listSel, g_listScroll, g_rowCount);
    rememberSelection();
    requestRedraw();
}

void moveSelection(int delta) {
    if (g_screen == SCR_SETTINGS) {
        int n = settingsRowCount();
        if (!n) return;
        g_sel = constrain(g_sel + delta, 0, n - 1);
        clampScroll(g_sel, g_scroll, n);
    } else if (g_screen != SCR_HOME && g_rowCount) {
        g_listSel = constrain(g_listSel + delta, 0, g_rowCount - 1);
        clampScroll(g_listSel, g_listScroll, g_rowCount);
        rememberSelection();
    } else {
        return;
    }
    requestRedraw();
}

void activateListRow(int idx) {
    if (idx < 0 || idx >= g_rowCount) return;
    g_listSel = idx;
    rememberSelection();
    if (g_screen == SCR_INBOX) openThread(g_rows[idx].peer);
    else if (g_shareActive) shareSendTo(idx);  // picking who gets a contact card
    else openCard(g_rows[idx].peer);           // Contacts + Peers: Message / Rename / Share
}

void activateCurrent() {
    switch (g_screen) {
        case SCR_HOME:
            doAnnounce();
            if (g_cfg.beep) g_audio.playAnnounce();
            requestRedraw();
            break;
        case SCR_SETTINGS:
            if (settingsRowCount()) activateSetting(g_sel);
            break;
        default:
            activateListRow(g_listSel);
            break;
    }
}

// Digits: lists select (Enter activates); Settings select + activate.
void digitPressed(int d) {
    if (g_screen == SCR_SETTINGS) {
        int idx = d == 0 ? 9 : d - 1;           // 1..9 = rows 1..9, 0 = row 10
        int n = settingsRowCount();
        if (idx < 0 || idx >= n) return;           // no clamping to "nearest" row
        g_sel = idx;
        clampScroll(g_sel, g_scroll, n);
        activateSetting(idx);
        requestRedraw();
        return;
    }
    if (g_screen == SCR_HOME) return;
    const int ps = pageSize(g_screen);
    int idx = (g_listSel / ps) * ps + ((g_screen == SCR_CONTACTS) ? d : d - 1);
    if (d == 0 && g_screen != SCR_CONTACTS) return;
    if (idx < 0 || idx >= g_rowCount) return;
    g_listSel = idx;
    clampScroll(g_listSel, g_listScroll, g_rowCount);
    rememberSelection();
    requestRedraw();
}

void cancelEntry() {
    if (g_entry == Entry::Message) { closeThread(); return; }
    if (g_entry == Entry::Search) { g_filter[0] = 0; endEntry(); loadRows(g_screen); return; }
    endEntry();
}

void goBack() {
    if (g_menuOpen)              { closeMenu(); return; }
    if (g_navOpen && !entryActive()) { navClose(); return; }
    if (g_navKeysOpen && (mapview::isOpen() || g_libOpen || g_gameOpen || g_chessOpen) && !entryActive()) { navKeysToggle(); return; }
    if (g_cardOpen && !entryActive()) { closeCard(); return; }
    if (g_rtOpen && !entryActive()) { rtStop(); return; }
    if (g_shareActive && !entryActive()) {
        g_shareActive = false; g_shareAll = false; g_pickRt = false; g_pickLowbat = false;
        toast("Cancelled");
        return;
    }
    if (g_placeOpen && !entryActive()) { g_placeOpen = false; g_fullRefreshReq = true; requestRedraw(); return; }
    if (g_libOpen && !entryActive()) {
        if (g_libReading) { libRemember(); g_libReading = false; requestRedraw(); } else libClose();
        return;
    }
    if (g_gameOpen && !entryActive()) { gameClose(); libOpen(); return; }   // back to the Library list
    if (g_chessOpen && !entryActive()) { chessClose(); libOpen(); return; }
    if (mapview::isOpen() && !entryActive()) { closeMap(); return; }    // map over a list: map first
    if (g_filter[0] && !entryActive() && isListScreen(g_screen)) { g_filter[0] = 0; loadRows(g_screen); requestRedraw(); return; }
    if (entryActive())           { cancelEntry(); return; }
    if (g_screen == SCR_SETTINGS && g_navDepth > 0) { popPage(); return; }
}

bool parseLong(const char* s, long& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    out = strtol(s, &end, 10);
    return end && *end == 0;
}

bool parseFloat(const char* s, float& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    out = strtof(s, &end);
    return end && *end == 0;
}

void commitEntry() {
    switch (g_entry) {
    case Entry::Message: {
        if (!g_bufLen) {                         // Enter on an empty draft: map or save card
            if (locOffered()) {
                const double la = g_threadLoc.lat, lo = g_threadLoc.lon;
                uint8_t peer[16];
                memcpy(peer, g_threadPeer, 16);
                closeThread();
                openMapAt(la, lo);
                g_mapSel = cposFind(peer);            // their marker selected (name + age below)
                Serial.printf("[LOC] show %.5f,%.5f on the map\n", la, lo);
            } else {
                saveThreadCard();
            }
            return;
        }
        char body[BUF_CAP + 1];
        memcpy(body, g_buf, g_bufLen + 1);
        const power::CheckinPhase ph = checkinNow();
        if (g_pwEff.checkin && !(ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS)) {
            if (outboxAdd(g_threadPeer, body)) {
                bufClear();
                const time_t at = time(nullptr) + ph.toNextS;
                struct tm lt;
                applyTz();
                localtime_r(&at, &lt);
                toast("Queued for %02d:%02d check-in", lt.tm_hour, lt.tm_min);
            } else {
                toast("Outbox full (%d)", kOutboxCap);
            }
        } else {
            const SendResult r = sendOrQueue(g_threadPeer, body);
            if (r == SendResult::Sent) {
                bufClear();
                if (g_cfg.beep) g_audio.playMessage();
            } else if (r == SendResult::Queued) {
                bufClear();
                toast("Queued: radio busy");
            } else {
                toast("Not sent: %s", g_txStr);   // keep the draft for retry
            }
        }
        loadHistory(g_threadPeer);
        requestRedraw();
        return;
    }
    case Entry::Name: {
        if (!g_bufLen) { toast("Type a name first"); return; }
        char name[BUF_CAP + 1];
        memcpy(name, g_buf, g_bufLen + 1);
        saveContactName(g_nameTarget, name);
        endEntry();
        if (g_screen == SCR_CONTACTS || g_screen == SCR_PEERS) loadRows(g_screen);
        toast("Saved \"%.16s\"", name);
        return;
    }
    case Entry::RtcField: {
        static const int lo[5] = {0, 0, 1, 1, 2000}, hi[5] = {23, 59, 12, 31, 2099};
        long v;
        if (!parseLong(g_buf, v)) { toast("Type digits"); return; }
        if (g_entryArg == 4 && v < 100) v += 2000;
        if (v < lo[g_entryArg] || v > hi[g_entryArg]) { toast("Out of range %d-%d", lo[g_entryArg], hi[g_entryArg]); return; }
        g_rtc[g_entryArg] = (int)v;
        endEntry();
        return;
    }
    case Entry::PlaceFind: {
        char what[28];
        strlcpy(what, g_buf, sizeof(what));
        endEntry();
        placeSearch(what);
        return;
    }
    case Entry::LibFind: {
        char what[40];
        strlcpy(what, g_buf, sizeof(what));
        endEntry();
        libFind(what);
        return;
    }
    case Entry::WptName: {
        char nm[16];
        strlcpy(nm, g_buf, sizeof(nm));
        endEntry();
        wptAdd(nm);
        toast("Waypoint W%d", g_wptCount);
        return;
    }
    case Entry::MapNum: {
        long v;
        if (!parseLong(g_buf, v) || v < 1 || v > 99) { toast("Map number 1-99"); return; }
        endEntry();
        cposAssign(g_cardPeer, (int)v);
        toast("Map #%ld", v);
        requestRedraw();
        return;
    }
    case Entry::CheckinTime: {               // hhmm or hmm; 12 h clock: 1-12 + the am / pm row
        long v;
        if (!parseLong(g_buf, v) || g_bufLen < 3 || g_bufLen > 4) { toast("Type hhmm, e.g. 0730"); return; }
        int h = (int)(v / 100), m = (int)(v % 100);
        if (m > 59 || h > 23 || (g_cfg.clock12 && (h < 1 || h > 12))) { toast(g_cfg.clock12 ? "1:00 - 12:59" : "00:00 - 23:59"); return; }
        if (g_cfg.clock12) h = h % 12 + (g_pw.checkinStart >= 720 ? 12 : 0);
        g_pw.checkinStart = (uint16_t)(h * 60 + m);
        power::save(g_pw);
        powerApply(true);
        endEntry();
        char at[12];
        fmtClockMin(g_pw.checkinStart, at, sizeof(at));
        toast("Check-in at %s", at);
        Serial.printf("[CHECKIN] time %s\n", at);
        return;
    }
    case Entry::FoundText:
        strlcpy(g_cfg.foundText, g_buf, sizeof(g_cfg.foundText));
        saveConfig();
        endEntry();
        toast(g_cfg.foundText[0] ? "If-found text saved" : "If-found text cleared");
        return;
    case Entry::IdleSecs: {
        long v;
        if (!parseLong(g_buf, v) || v < 10 || v > 99999) { toast("Out of range 10-99999"); return; }
        g_cfg.idleSecs = (uint32_t)v;
        saveConfig();
        endEntry();
        return;
    }
    case Entry::Latitude:
    case Entry::Longitude: {
        const bool isLat = g_entry == Entry::Latitude;
        float v;
        const float lim = isLat ? 90.0f : 180.0f;
        if (!parseFloat(g_buf, v) || v < -lim || v > lim) { toast(isLat ? "Lat -90..90" : "Lon -180..180"); return; }
        if (!g_cfg.hasPos) { g_cfg.lat = 0; g_cfg.lon = 0; }
        if (isLat) g_cfg.lat = v; else g_cfg.lon = v;
        g_cfg.hasPos = true;
        saveConfig();
        endEntry();
        return;
    }
    case Entry::TcpHost: {
        if (!g_bufLen || g_bufLen >= sizeof(g_tcpCfg.host) || strchr(g_buf, ' ')) { toast("Host: 1-63 chars, no spaces"); return; }
        strlcpy(g_tcpCfg.host, g_buf, sizeof(g_tcpCfg.host));
        endEntry();
        tcpSave();
        return;
    }
    case Entry::TcpPort: {
        long v;
        if (!parseLong(g_buf, v) || v < 1 || v > 65535) { toast("Port 1-65535"); return; }
        g_tcpCfg.port = (uint16_t)v;
        endEntry();
        tcpSave();
        return;
    }
    case Entry::Search:                          // keep the filter, back to browsing
        endEntry();
        Serial.printf("[LIST] filter \"%s\": %d rows\n", g_filter, g_rowCount);
        return;
    case Entry::PassOld: {                       // arg 0 = change, 1 = remove
        if (lockWaitS()) { toast("Wrong too often: wait %u s", (unsigned)lockWaitS()); return; }
        if (!vault::checkPassword(g_buf)) { lockNoteWrong(); endEntry(); toast("Wrong password"); return; }
        lockNoteRight();
        if (g_entryArg == 1) {
            endEntry();
            if (!vault::setPassword(nullptr)) { toast("Vault error"); return; }
            wifiSaveAll();                       // re-seal saved WiFi passwords with the new key
            if (g_cfg.idleMode == 2) { g_cfg.idleMode = 1; saveConfig(); }
            Serial.println("[LOCK] password removed");
            toast("Password removed");
            return;
        }
        const int arg = g_entryArg;
        startEntry(Entry::PassNew, nullptr, arg);
        return;
    }
    case Entry::PassNew: {                       // arg 0 = set/change, 2 = then lock on idle
        if (g_bufLen < kLockPassMin) { toast("Password: 4+ chars"); return; }
        char keep[33];
        strlcpy(keep, g_buf, sizeof(keep));
        const int arg = g_entryArg;
        startEntry(Entry::PassConfirm, nullptr, arg);   // startEntry keeps g_passNew
        memcpy(g_passNew, keep, sizeof(g_passNew));
        memset(keep, 0, sizeof(keep));
        return;
    }
    case Entry::PassConfirm: {
        if (strcmp(g_buf, g_passNew) != 0) {
            const int arg = g_entryArg;
            endEntry();
            startEntry(Entry::PassNew, nullptr, arg);
            toast("No match: type it again");
            return;
        }
        char pw[33];
        strlcpy(pw, g_passNew, sizeof(pw));
        const int arg = g_entryArg;
        endEntry();
        const bool ok = vault::setPassword(pw);
        memset(pw, 0, sizeof(pw));
        if (!ok) { toast("Vault error"); return; }
        wifiSaveAll();                           // re-seal saved WiFi passwords with the new key
        lockNoteRight();
        if (arg == 2) { g_cfg.idleMode = 2; saveConfig(); }
        Serial.println("[LOCK] password set");
        toast(arg == 2 ? "Password set; lock on" : "Password set");
        return;
    }
    case Entry::WifiPass: {
        if (g_bufLen > 0 && g_bufLen < 8) { toast("Password: 8+ chars"); return; }
        if (g_bufLen > WIFI_PASS_MAX) { toast("Max 63 chars"); return; }
        if (g_page == PG_WIFI_NET && g_wifiNetIdx >= 0 && g_wifiNetIdx < g_wifiSavedCount) {
            if (!g_bufLen) { endEntry(); toast("Password unchanged"); return; }
            WifiCred& c = g_wifiSaved[g_wifiNetIdx];
            strlcpy(c.pass, g_buf, sizeof(c.pass));
            c.passBad = false;
            wifiSaveAll();
            endEntry();
            toast("Password saved");
            return;
        }
        if (g_page == PG_WIFI_JOIN) {
            if (!g_bufLen && g_joinSecure) { toast("Type the password"); return; }
            strlcpy(g_joinPass, g_buf, sizeof(g_joinPass));
            endEntry();
            activateSetting(3);                  // Enter after the password = Join
            return;
        }
        endEntry();
        return;
    }
    default:
        endEntry();
        return;
    }
}

void bufAppend(char c) {
    if (g_bufLen >= BUF_CAP) { toast("Full (%u chars)", (unsigned)BUF_CAP); return; }
    g_buf[g_bufLen++] = c;
    g_buf[g_bufLen] = 0;
}

void searchChanged() {
    strlcpy(g_filter, g_buf, sizeof(g_filter));
    g_listSel = 0;
    g_listScroll = 0;
    g_rememberValid[g_screen] = false;
    loadRows(g_screen);
}

void entryKey(const KeyEvent& ev) {
    if (ev.enter) { commitEntry(); return; }
    if (ev.del) {
        if (g_entry == Entry::FoundText) g_entryReplace = false;   // editing the old text
        if (g_bufLen) g_buf[--g_bufLen] = 0;
        if (g_entry == Entry::Search) searchChanged();
        requestRedraw();
        return;
    }
    char c = 0;
    if (ev.space) {
        if (g_entry == Entry::Latitude || g_entry == Entry::Longitude) c = '.';
        else if (g_entry == Entry::Message || g_entry == Entry::Name || g_entry == Entry::WifiPass || passEntry(g_entry) ||
                 g_entry == Entry::Search || g_entry == Entry::WptName || g_entry == Entry::LibFind ||
                 g_entry == Entry::PlaceFind || g_entry == Entry::FoundText) c = ' ';
    } else if (ev.fn) {
        if (passEntry(g_entry)) { g_showPass = !g_showPass; requestRedraw(); return; }   // MIC = show / hide
        if (g_entry == Entry::Message && g_threadOpen) { g_tmenuOpen = true; g_tmenuSel = 0; requestRedraw(); return; }
        if (entryNumeric()) c = '0';          // MIC (+SYM) types 0 in number fields
    } else {
        c = ev.character;
    }
    if (c < 32 || c > 126) return;

    if (entryNumeric()) {
        const bool decimal = g_entry == Entry::Latitude || g_entry == Entry::Longitude;
        const bool ok = (c >= '0' && c <= '9') ||
                        (decimal && (c == '.' || (c == '-' && g_bufLen == 0)));
        if (!ok) { toast("Numbers only"); return; }
    } else if (ev.alt && isalpha((unsigned char)c)) {
        c = isupper((unsigned char)c) ? (char)tolower(c) : (char)toupper(c);   // Alt = case
    }
    if (g_entry == Entry::Name && g_bufLen >= 23) { toast("Max 23 chars"); return; }
    if (g_entry == Entry::WifiPass && g_bufLen >= WIFI_PASS_MAX) { toast("Max 63 chars"); return; }
    if (passEntry(g_entry) && g_bufLen >= kLockPassMax) { toast("Max 32 chars"); return; }
    if (g_entry == Entry::Search && g_bufLen >= sizeof(g_filter) - 1) return;
    if ((g_entry == Entry::WptName || g_entry == Entry::FoundText || g_entry == Entry::CheckinTime) &&
        g_entryReplace) {                                                                  // typing replaces the default
        bufClear(); g_entryReplace = false;
    }
    if (g_entry == Entry::WptName && g_bufLen >= 15) { toast("Max 15 chars"); return; }
    if (g_entry == Entry::TcpHost && g_bufLen >= sizeof(g_tcpCfg.host) - 1) { toast("Max 63 chars"); return; }
    if (g_entry == Entry::FoundText && g_bufLen >= sizeof(g_cfg.foundText) - 1) { toast("Max 120 chars"); return; }
    bufAppend(c);
    if (g_entry == Entry::Search) searchChanged();
    // Show it on the next frame. Keys typed while that frame is on the panel are
    // queued by onPanelBusy() and land together in the frame after.
    requestRedraw();
}

void libOpen();

void menuGo(int i) {
    if (i == kMenuMap) { closeMenu(); openMap(); return; }
    if (i == kMenuLib) { closeMenu(); libOpen(); return; }
    goScreen(kMenuScreens[i]);
}

void menuKey(const KeyEvent& ev) {
    if (ev.enter) { menuGo(g_menuSel); return; }
    const char c = (char)toupper((unsigned char)ev.character);
    if (c == 'M') { closeMenu(); return; }
    if (c == 'G') { if (g_menuSel > 0) g_menuSel--; requestRedraw(); return; }
    if (c == 'V') { if (g_menuSel < kMenuCount - 1) g_menuSel++; requestRedraw(); return; }
    for (int i = 0; i < kMenuCount; i++)
        if (c == kMenuKeys[i]) { menuGo(i); return; }
}

void browseKey(const KeyEvent& ev) {
    if (g_cardOpen) { cardKey(ev); return; }
    if (ev.enter) { activateCurrent(); return; }
    if (ev.del || ev.space) return;
    const char raw = ev.character;
    if (raw >= '0' && raw <= '9') { digitPressed(raw - '0'); return; }
    switch (toupper((unsigned char)raw)) {
        case 'G': moveSelection(-1); break;
        case 'V': moveSelection(+1); break;
        case 'M': openMenu(); break;
        case 'H': goScreen(SCR_HOME); break;
        // 'S' is intentionally absent: Settings is reached only via M -> S (guide §7).
        case 'C': goScreen(SCR_CONTACTS); break;
        case 'P': goScreen(SCR_PEERS); break;
        case 'I': goScreen(SCR_INBOX); break;
        case 'A':                                   // Atlas; on a list row with a known position: there
            if (isListScreen(g_screen) && g_listSel < g_rowCount && cposFind(g_rows[g_listSel].peer) >= 0 &&
                g_cpos[cposFind(g_rows[g_listSel].peer)].hasPos)
                mapShowContact(g_rows[g_listSel].peer);
            else openMap();
            break;
        case 'L':                                   // locate: type to filter the list
            if (isListScreen(g_screen)) { startEntry(Entry::Search, g_filter); }
            break;
        case 'N':                                   // Home: Announce ID; lists: next page
            if (g_screen == SCR_HOME) activateCurrent();
            else pageJump(+1);
            break;
        case 'B': pageJump(-1); break;
        default: break;
    }
}

void handleKey(const KeyEvent& ev) {
    if (g_idleActive) {                 // any key wakes; the key itself is consumed
        g_idleActive = false;
        noteInput();
        g_fullRefreshReq = true;
        requestRedraw();
        Serial.println(g_locked ? "[IDLE] wake (locked)" : "[IDLE] wake");
        return;
    }
    noteInput();
    if (g_locked) { lockKey(ev); return; }
    if (g_usbDrive) { usbDriveKey(ev); return; }
    if (g_threadOpen && g_tmenuOpen) { tmenuKey(ev); return; }
    if (g_wizOpen && !entryActive()) { wizKey(ev); return; }   // setup wizard (^ = step back)
    if (ev.up) { goBack(); return; }                          // lone Shift = ^
    if (g_menuOpen) { menuKey(ev); return; }
    if (entryActive()) { entryKey(ev); return; }
    if (g_libOpen) { libKey(ev); return; }
    if (g_gameOpen) { gameKey(ev); return; }
    if (g_chessOpen) { chessKey(ev); return; }
    if (ev.fn) { g_fullRefreshReq = true; requestRedraw(); return; }   // MIC = full refresh
    if (g_rtOpen) { rtKey(ev); return; }
    if (g_navOpen) { navKey(ev); return; }
    if (mapview::isOpen()) { mapKey(ev); return; }
    browseKey(ev);
}

// A key on the Atlas. M C I H leave the map like everywhere else (M menu, C contacts,
// I inbox, H home); P is next waypoint here (Peers: M then P). A does nothing (it is next to
// S, so a slip while panning is harmless).
void mapKey(const KeyEvent& ev) {
    const char mc = (char)toupper((unsigned char)ev.character);
    if (g_placeOpen) { placeKey(ev); return; }            // place results list (digits = go)
    if (mapNumberKey(ev)) return;                         // "12" Enter: open chat #12
    if (mc == kKeyNav) { navKeysToggle(); return; }
    if (mc == 'M') { openMenu(); return; }
    if (mc == 'C' || mc == 'I' || mc == 'H') {
        closeMap();
        goScreen(mc == 'C' ? SCR_CONTACTS : mc == 'I' ? SCR_INBOX : SCR_HOME);
        return;
    }
    if (mc == 'A') return;
    if (mc == kKeyBack) { if (g_tbOn) tbStop("stopped"); else tbStart(); requestRedraw(); return; }
    if (mc == kKeyGo) { navOpen(); return; }
    if (mc == kKeyFind) { startEntry(Entry::PlaceFind); return; }   // locate a place
    if (wptMapKey(ev)) return;                            // P next / Alt+P add / Alt+SYM delete
    if (mc == kKeyContact) { mapCycleContact(); return; } // next contact's last position
    if (mc == 'D' || ev.enter) g_mapSel = -1;             // back to own position
    if (mapview::key(ev.character, ev.enter)) requestRedraw();
}

// ---- Key queue --------------------------------------------------------------------
// The TCA8418 FIFO holds only 10 events (5 key presses). A partial e-ink refresh
// blocks ~0.7 s, so the keyboard is also read from the panel's busy-wait callback
// into this queue; nothing is dropped and keys are handled right after the frame.
constexpr uint8_t KEYQ_CAP = 48;
KeyEvent g_keyq[KEYQ_CAP];
uint8_t  g_keyqHead = 0, g_keyqTail = 0;

void readKeyboardIntoQueue() {
    if (!g_kbOk) return;
    for (int n = 0; n < 10; n++) {
        g_kb.update();
        if (!g_kb.hasEvent()) break;
        uint8_t next = (uint8_t)((g_keyqHead + 1) % KEYQ_CAP);
        if (next == g_keyqTail) break;                  // queue full: leave rest in FIFO
        g_keyq[g_keyqHead] = g_kb.getEvent();
        g_keyqHead = next;
    }
}

void processKeys() {
    readKeyboardIntoQueue();
    while (g_keyqTail != g_keyqHead) {
        KeyEvent ev = g_keyq[g_keyqTail];
        g_keyqTail = (uint8_t)((g_keyqTail + 1) % KEYQ_CAP);
        handleKey(ev);
    }
}

// Runs while the panel is busy refreshing. Must only read input, never draw.
void onPanelBusy(const void*) {
    static uint32_t last = 0;
    if (millis() - last >= 8) {
        last = millis();
        readKeyboardIntoQueue();
        gpsPoll();                  // drain NMEA too, so no sentences are lost to a refresh
    }
    delay(1);
}

// ---- Touch (secondary) --------------------------------------------------------------
bool cst328Read(int16_t& x, int16_t& y) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write((uint8_t)0xD0);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)TOUCH_I2C_ADDR, 7) < 5) return false;
    uint8_t d0 = Wire.read(), d1 = Wire.read(), d2 = Wire.read(), d3 = Wire.read();
    while (Wire.available()) (void)Wire.read();
    if ((d0 & 0x0F) != 0x06) return false;
    x = (int16_t)((d1 << 4) | ((d3 >> 4) & 0x0F));
    y = (int16_t)((d2 << 4) | (d3 & 0x0F));
    return true;
}

void handleTouch(int16_t x, int16_t y) {
    if (g_idleActive || g_locked) return;     // doc: key-only wake; no touch while locked
    noteInput();
    const Hit* hit = nullptr;
    for (int i = g_hitCount - 1; i >= 0; i--) {      // last drawn = topmost
        const Hit& h = g_hits[i];
        if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) { hit = &h; break; }
    }
    if (g_menuOpen && (!hit || (hit->act != H_MENU_GO && hit->act != H_MENU))) { closeMenu(); return; }
    if (!hit) return;
    Serial.printf("[TOUCH] %d,%d act=%u arg=%d\n", x, y, hit->act, hit->arg);
    switch (hit->act) {
        case H_REFRESH:    g_fullRefreshReq = true; requestRedraw(); break;
        case H_MENU:       if (g_menuOpen) closeMenu(); else openMenu(); break;
        case H_MENU_GO:    menuGo(hit->arg); break;
        case H_TAB:        goScreen((ScreenId)hit->arg); break;
        case H_BACK:       goBack(); break;
        case H_ANNOUNCE:   activateCurrent(); break;
        case H_UP:         moveSelection(-1); break;
        case H_DOWN:       moveSelection(+1); break;
        case H_CARD:       if (g_cardOpen) { g_cardSel = hit->arg; cardAct(hit->arg); } break;
        case H_TMENU:      if (g_tmenuOpen) { g_tmenuSel = hit->arg; tmenuAct(hit->arg); } break;
        case H_NAVKEYS:    navKeysToggle(); break;
        case H_TILE_AUDIO:
            g_cfg.beep = !g_cfg.beep;
            g_audio.setEnabled(g_cfg.beep);
            saveConfig();
            requestRedraw();
            break;
        case H_ROW:
            if (g_screen == SCR_SETTINGS) {          // settings: tap = activate
                g_sel = hit->arg;
                activateSetting(hit->arg);
                requestRedraw();
            } else if (hit->arg == g_listSel) {      // lists: tap selected row = Enter
                activateListRow(hit->arg);
            } else {                                 // lists: first tap selects
                g_listSel = hit->arg;
                rememberSelection();
                requestRedraw();
            }
            break;
    }
}

void pollTouch() {
    static int lastInt = HIGH;
    static uint32_t lastMs = 0;
    int v = digitalRead(TOUCH_INT);
    if (lastInt == HIGH && v == LOW && millis() - lastMs > 350) {
        lastMs = millis();
        int16_t x, y;
        if (g_touchOk && cst328Read(x, y)) handleTouch(x, y);
    }
    lastInt = v;
}

// ---- Energy modes: apply, GPS schedule, light sleep -----------------------------------
// The effective profile (power/PowerModes.h) is re-evaluated every second; changes are
// applied to the hardware here and nowhere else.

bool chargerUpdateReg(uint8_t reg, uint8_t mask, uint8_t value) {
    Wire.beginTransmission(BQ25896_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ25896_I2C_ADDR, 1) != 1) return false;
    const uint8_t r = (uint8_t)((Wire.read() & ~mask) | (value & mask));
    Wire.beginTransmission(BQ25896_I2C_ADDR);
    Wire.write(reg);
    Wire.write(r);
    return Wire.endTransmission() == 0;
}

// BQ25896 REG00 bit 7 EN_HIZ: input off, the system runs from the battery although USB
// is plugged in. Bench use only (console "bench"), so the gauge shows the real draw.
// The charger's I2C watchdog (REG07 bits 5:4, default 40 s) puts EN_HIZ back to 0 when it
// expires, which happens while the ESP32 light-sleeps, so it is off during a bench run and
// restored (40 s) when the run ends and at every boot.
bool chargerSetHiz(bool on) {
    chargerUpdateReg(0x07, 0x30, on ? 0x00 : 0x10);
    return chargerUpdateReg(0x00, 0x80, on ? 0x80 : 0x00);
}

bool powerUsbPowered() { return g_bat.usb && !g_benchUntil; }

void powerApply(bool force) {
    const bool usb = powerUsbPowered();
    power::Profile want = power::effective(g_pw, usb);
    if (!want.lora && want.loraOnUse && (millis() < power::kLoraOnUseMs ||
                                         (g_lastInputMs && millis() - g_lastInputMs < power::kLoraOnUseMs)))
        want.lora = true;                        // on demand: LoRa on for 2 min after a key
    if (want.checkin && !want.lora && checkinNow().inWindow) want.lora = true;   // check-in window
    const bool onUsb = usb && g_pw.fullOnUsb && g_pw.mode != power::MODE_FULL;
    if (!force && power::sameProfile(want, g_pwEff) && onUsb == g_pwOnUsb) return;
    const power::Profile old = g_pwEff;
    g_pwEff = want;
    g_pwOnUsb = onUsb;

#if !defined(SIMULATOR)
    if (getCpuFrequencyMhz() != g_pwEff.cpuMhz) setCpuFrequencyMhz(g_pwEff.cpuMhz);
#endif
    // LoRa RX duty cycle whenever the profile sleeps (Full keeps continuous receive).
    if (g_radio) g_radio->setRxDutyCycle(g_pw.rxDc && g_pwEff.wakeSecs != 0, g_pw.rxDc ? g_pw.rxDc : 4);
    if (g_loraIf && g_radio) {
        if (g_pwEff.lora && !g_loraIf->isOnline()) {
            if (g_loraIf->start()) Serial.println("[POWER] LoRa receiver on");
        } else if (!g_pwEff.lora && g_loraIf->isOnline()) {
            g_loraIf->stop();
            g_radio->sleep();
            Serial.println("[POWER] LoRa radio asleep (no messages)");
        }
    }
    if (!g_pwEff.wifi && (g_wifiSt != WifiSt::Off || g_wifiScanning || WiFi.getMode() != WIFI_OFF)) {
        g_wifiJoinAfterScan = false;
        wifiRadioOff();
        g_wifiScanning = false;
        g_wifiSt = WifiSt::Off;
        strlcpy(g_wifiMsg, "off (power mode)", sizeof(g_wifiMsg));
        wifiUiChanged();
    } else if (g_pwEff.wifi && !old.wifi) {
        strlcpy(g_wifiMsg, "off", sizeof(g_wifiMsg));
        g_wifiNextAuto = millis() + 2000;
    }
    applyKeyLight();
    if (old.gps != g_pwEff.gps || force) {
        g_gpsVisitStart = 0;
        g_gpsNextVisit = 0;                      // periodic: first visit now
    }
    char w[16];
    power::fmtWake(w, sizeof(w), g_pwEff.wakeSecs);
    Serial.printf("[POWER] %s%s: cpu %u MHz, sleep %s, awake %us, lora %s, gps %s, wifi %s, keylight %s\n",
                  power::modeName(g_pw.mode), g_pwOnUsb ? " (USB -> Full)" : "", (unsigned)g_pwEff.cpuMhz, w,
                  (unsigned)g_pwEff.awakeSecs, g_pwEff.lora ? "on" : "off", power::gpsName(g_pwEff.gps),
                  g_pwEff.wifi ? "allowed" : "off", g_pwEff.keyLight ? "allowed" : "off");
    if (g_screen == SCR_SETTINGS) requestRedraw();
}

// Periodic GPS: power the receiver for a visit (until a fix has held 20 s, or 3 min),
// then rest until the next interval. The backup cell keeps ephemeris, so visits after
// the first are usually hot starts of a few seconds.

bool g_gpsVisitAsked = false;     // a feature (location auto-share) needs a fix now

// Start a GPS visit at the next tick even between scheduled ones (any mode but GPS off).
void gpsRequestVisit(const char* why) {
    if (g_gpsVisitStart) return;
    g_gpsVisitAsked = true;
    g_gpsNextVisit = millis();
    Serial.printf("[GPS] visit requested (%s)\n", why);
}

void powerGpsTick() {
    const uint32_t now = millis();
    const uint32_t ivl = power::gpsIntervalMs(g_pwEff.gps);
    bool want;
    if (!g_cfg.gpsOn || g_pwEff.gps == power::GPS_OFF) {
        want = false;
    } else if (!ivl) {
        want = true;
    } else if (g_gpsVisitStart) {
        static uint32_t fixSince = 0;
        if (!g_gpsFix) fixSince = 0;
        else if (!fixSince) fixSince = now ? now : 1;
        // A visit ends after a fix has held 20 s, after 3 min, or early when nothing can work:
        // no satellite heard within 60 s (indoors) or a receiver backup-supply fault.
        int inView = 0, heard = 0, best = 0;
        gpsSkySummary(inView, heard, best);
        const bool hopeless = g_gpsBbrFault || (now - g_gpsVisitStart >= 60000 && heard == 0 && !g_gpsFix);
        const bool done = (fixSince && now - fixSince >= 20000) || now - g_gpsVisitStart >= power::kGpsWindowMs ||
                          hopeless;
        if (done) {
            Serial.printf("[GPS] visit done after %lus (%s), next in %lu min\n",
                          (unsigned long)((now - g_gpsVisitStart) / 1000), g_gpsFix ? "fix" : "no fix",
                          (unsigned long)(ivl / 60000));
            g_gpsVisitStart = 0;
            fixSince = 0;
            g_gpsNextVisit = now + ivl;
            want = false;
        } else {
            want = true;
        }
    } else if ((!g_gpsNextVisit || (int32_t)(now - g_gpsNextVisit) >= 0) &&
               (g_pwEff.gps != power::GPS_ON_USE || g_gpsVisitAsked ||
                (g_lastInputMs && now - g_lastInputMs < 10000))) {
        g_gpsVisitAsked = false;
        g_gpsVisitStart = now ? now : 1;
        Serial.println("[GPS] scheduled visit: receiver on");
        want = true;
    } else {
        want = false;
    }
    if (want != g_gpsAllowed) {
        g_gpsAllowed = want;
        gpsApplyPower();
        if (g_screen == SCR_SETTINGS && g_page == PG_TIME) requestRedraw();
    }
}

// Check-in window work: announce at +5 s (peers learn paths and, if synchronized, give
// their time), then send the outbox from +kCheckinSendS, one message per ~3.5 s.
void checkinTick() {
    static bool wasIn = false;
    static uint8_t announced = 0;
    static uint32_t lastTry = 0;
    const power::CheckinPhase ph = checkinNow();
    // Out of Check-in mode (switched away with messages still queued): LoRa is on as
    // usual, so send the outbox now instead of leaving it waiting for a window.
    const bool flushNow = !g_pwEff.checkin && g_outboxCount && loraOnline();
    if (!g_pwEff.checkin && !flushNow) { wasIn = false; return; }
    if (!flushNow) {
        if (ph.inWindow != wasIn) {
            wasIn = ph.inWindow;
            announced = 0;
            Serial.printf("[CHECKIN] window %s (%d queued)\n", ph.inWindow ? "open" : "closed", g_outboxCount);
        }
        if (!ph.inWindow) return;
        // Announce at +5 s and again at +90 s: a peer whose clock is up to ~1.5 min late still
        // hears one (and, if this clock is synchronized, takes its time from it).
        const uint8_t due = ph.sinceS >= 90 ? 2 : ph.sinceS >= 5 ? 1 : 0;
        if (announced < due && g_loraIf && g_loraIf->isOnline()) {
            announced = due;
            doAnnounce();
        }
        if (!g_outboxCount || ph.sinceS < (int32_t)power::kCheckinSendS) return;
    }
    if (millis() - lastTry < (flushNow ? 3100u : 3500u)) return;   // 3 s send spacing
    lastTry = millis();
    OutboxItem& o = g_outbox[0];
    const bool sent = sendMessage(o.dest, o.text);
    // "rejected 1" (busy) and "rejected 8" (store still recovering after boot) are
    // temporary: keep the message for the next try instead of dropping it.
    const bool transient = g_txTransient;
    const bool hopeless = !sent && !transient &&
                          (!strncmp(g_txStr, "rejected", 8) || !strcmp(g_txStr, "invalid") ||
                           !strcmp(g_txStr, "pick remote"));
    Serial.printf("[CHECKIN] outbox \"%s\": %s\n", o.text, sent ? "sent" : g_txStr);
    if (sent || hopeless) {
        memmove(&g_outbox[0], &g_outbox[1], sizeof(OutboxItem) * (size_t)(g_outboxCount - 1));
        g_outboxCount--;
        outboxSave();
    }
    requestRedraw();                             // sendMessage cleared the widget tree
}

void powerTick() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    if (g_usbSleepUntil && (int32_t)(millis() - g_usbSleepUntil) >= 0) g_usbSleepUntil = 0;
    if (g_benchUntil) {
        static uint32_t lastHiz = 0;
        if ((int32_t)(millis() - g_benchUntil) >= 0 || !g_bat.valid || g_bat.volts < 3.55f) {
            chargerSetHiz(false);
            g_benchUntil = 0;
            Serial.println("[BENCH] done - charger input back on");
        } else {
            if (millis() - lastHiz >= 5000) { lastHiz = millis(); chargerSetHiz(true); }
            Serial.printf("[BENCH] %s cpu=%u ma=%d mv=%u tte=%u\n", power::modeName(g_pw.mode),
                          (unsigned)g_pwEff.cpuMhz, (int)g_bat.avgMa, (unsigned)(g_bat.volts * 1000 + 0.5f),
                          (unsigned)g_bat.tteMin);
        }
    }
    static uint32_t decayAt = 0;
    if (millis() - decayAt >= 600000UL) {          // statistics describe the last ~10-20 min
        decayAt = millis();
        g_pwAwakeMs /= 2; g_pwSleepMs /= 2;
    }
    powerApply(false);
#if defined(SIMULATOR)
    {
        // Side panel line: what the energy mode is doing right now.
        char line[128];
        const uint32_t tot = g_pwAwakeMs + g_pwSleepMs;
        snprintf(line, sizeof(line), "%s%s | asleep %u%% (%lu sleeps) | %u MHz | LoRa %s",
                 power::modeName(g_pw.mode), g_pwOnUsb ? " (USB -> Full)" : "",
                 tot ? (unsigned)((uint64_t)g_pwSleepMs * 100 / tot) : 0u, (unsigned long)g_pwSleeps,
                 (unsigned)g_pwEff.cpuMhz, loraOnline() ? "on" : "off");
        ::sim::setPowerInfo(line);
    }
#endif
    checkinTick();
    usbDriveTick();
    chessTick();
    txLateTick();
    if (g_markValidAt && (int32_t)(millis() - g_markValidAt) >= 0) { g_markValidAt = 0; sysimg::markValid(); }
    peerClockTick();
    mapTick();
    flashSpaceTick();
    heartbeatTick();
    powerGpsTick();
    powerTouchTick();
    energyAccountAwake();
    static uint32_t rtcAt = 0;
    if (!rtcAt || millis() - rtcAt >= 10000) {     // RTC log: one entry per ~10 s awake or per wake
        rtcAt = millis() ? millis() : 1;
        if (g_bat.valid)
            rtclog::sample((uint16_t)(g_bat.volts * 1000 + 0.5f), g_bat.avgMa, g_bat.usb, g_pwSleeps,
                           g_pwSleepTotalMs / 1000);
    }
}

// Touch gating: the CST328 is held in reset (TOUCH_RST low) while the idle splash shows and,
// with touchOnKey, except for 60 s after a key or touch; a key brings it back (~50 ms).
bool g_touchGated = false;
void powerTouchTick() {
    // Idle splash: touch is off in every mode (key-only wake, no accidental touches).
    const bool want = !g_idleActive && (!g_pwEff.touchOnKey || millis() - g_lastInputMs < 60000UL);
    if (want == !g_touchGated) return;
    g_touchGated = !want;
    digitalWrite(TOUCH_RST, want ? HIGH : LOW);
    Serial.printf("[TOUCH] %s\n", want ? "on" : "off until a key is pressed");
}

// ---- Energy monitor accounting ----------------------------------------------------
uint32_t g_enLastMs = 0, g_enLastMeasMs = 0, g_enLastTxAir = 0, g_enLastEink = 0;

energy::State energyState(bool awake) {
    energy::State st = {};
    st.awake = awake;
    st.cpuMhz = g_pwEff.cpuMhz;
    st.loraRx = g_loraIf && g_loraIf->isOnline();
    if (st.loraRx && g_radio && g_radio->rxDutyCycleActive()) {
        const uint32_t on = g_radio->rxDcListenUs(), off = g_radio->rxDcSleepUs();
        st.loraDcPct = (uint8_t)(on + off ? (on * 100 + off + on - 1) / (on + off) : 0);
    }
    st.gps = g_gpsSerial != nullptr;
    st.wifi = WiFi.getMode() != WIFI_OFF;
    st.keyLight = g_kbOk && g_kb.backlightIsLit();
    st.touch = !g_touchGated;
    st.audio = g_audio.isPowered();
    st.sd = g_sdMounted;
    return st;
}

// Awake time since the last accounting point, plus event costs and the gauge reading.
void energyAccountAwake() {
    const uint32_t now = millis();
    if (!g_enLastMs) { g_enLastMs = g_enLastMeasMs = now; }
    energy::account(now - g_enLastMs, energyState(true));
    g_enLastMs = now;
    if (g_radio) {                                   // LoRa TX: airtime x PA current
        const uint32_t air = g_radio->txAirMs - g_enLastTxAir;
        g_enLastTxAir = g_radio->txAirMs;
        const int p = loraTxPower();
        const float txMa = p >= 17 ? 118.0f : p >= 10 ? 60.0f : p >= 0 ? 35.0f : 20.0f;
        energy::addEvent(energy::E_LORA_TX, txMa * air / 1000.0f);
    }
    const uint32_t ein = g_display.totalRefreshMs() - g_enLastEink;   // e-ink refresh ~8 mA
    g_enLastEink = g_display.totalRefreshMs();
    energy::addEvent(energy::E_DISPLAY, 8.0f * ein / 1000.0f);
    if (g_bat.valid && (!g_bat.usb || g_benchUntil))  // gauge average over the time since the last reading
        energy::addMeasured(g_bat.avgMa, now - g_enLastMeasMs);
    if (g_bat.valid && !g_bat.usb && g_bat.avgMa < 0)   // learned capacity: the same count, battery only
        g_capMas += -g_bat.avgMa * ((now - g_enLastMeasMs) / 1000.0);
    g_enLastMeasMs = now;
    const time_t t = time(nullptr);
    energy::tick(now, t > 100000 ? (uint32_t)t : 0, g_bat.valid ? g_bat.pct : -1);
}

void energyAfterSleep(uint32_t sleptMs) {
    energy::account(sleptMs, energyState(false));
    g_enLastMs = millis();
}

// Why the CPU must stay awake right now (nullptr = it may sleep).
const char* powerSleepBlocker() {
    const uint32_t now = millis();
    if (!g_pwEff.wakeSecs) return "mode";
#if defined(RS_LIGHT_SLEEP) && !RS_LIGHT_SLEEP
    return "light sleep disabled";
#endif
    if (now < 15000) return "boot";
    if (now - g_lastInputMs < (uint32_t)g_pwEff.awakeSecs * 1000UL) return "recent input";
    if (g_usbDrive) return "USB drive";
    if (g_cThinking) return "chess engine";
    // While the idle splash shows, loop() does not render, so a pending redraw waits for the wake
    // key anyway and must not keep the CPU awake.
    if (!g_idleActive && (g_redraw || g_redrawAt)) return "redraw";
    if (g_keyqTail != g_keyqHead) return "keys";
    if (g_pwEff.checkin && g_outboxCount) {
        const power::CheckinPhase ph = checkinNow();
        if (ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS) return "check-in send";
    }
    if (HWCDC::isPlugged() && !g_usbSleepUntil) return "usb host";   // console drops while asleep
    if (mirror::active()) return "mirror";
    if (g_wifiSt != WifiSt::Off || g_wifiScanning || WiFi.getMode() != WIFI_OFF) return "wifi";
    if (g_gpsSerial) return "gps";
    if (g_protoReady && g_proto.lxmfQueuedCount() > 0) return "lxmf queue";
    if (g_radio && (g_radio->packetAvailable || g_radio->isTxBusy())) return "lora busy";
    if (g_loraIf && !g_loraIf->txIdle()) return "lora tx queue";
    // A received message is proven only after it is durably stored, and the write queue's
    // worker task is frozen in light sleep: stay awake until pending writes are done (sleeping
    // in short slices starved the worker and no proofs went out).
    if (g_storeReady && g_msgStore.writeQueue().drainCount() > 0) return "storage write";
    if (g_pwLoraWakeMs && now - g_pwLoraWakeMs < 3000) return "lora exchange";   // let replies go out
    return nullptr;
}

// End of each loop pass: either a short idle delay or a light sleep until the next
// wake-up (timer, key, touch or LoRa packet).
void powerIdle() {
    if (g_redraw && !g_idleActive) return;
    static uint32_t accountedAt = 0;
    const char* why = powerSleepBlocker();
    if (why) { delay(g_pwEff.cpuMhz >= 240 ? 2 : 5); return; }

    uint32_t ms = (uint32_t)g_pwEff.wakeSecs * 1000UL;
    const time_t t = time(nullptr);
    if (t > 100000 && g_pwEff.wakeSecs >= 60) {        // land on the minute (5 min): the clock stays right
        const uint32_t step = g_pwEff.wakeSecs >= 300 ? 300 : 60;
        const uint32_t toMinute = (uint32_t)(step - t % step) * 1000UL + 250;
        if (toMinute < ms) ms = toMinute;
    }
    if (g_pwEff.checkin) {                             // check-in: wake at the window edges
        const power::CheckinPhase ph = checkinNow();
        uint32_t s = ph.toChangeS;
        if (ph.inWindow && ph.sinceS < 5) s = (uint32_t)(5 - ph.sinceS);   // announce at +5 s
        else if (ph.inWindow && ph.sinceS < 90 && s > (uint32_t)(90 - ph.sinceS)) s = (uint32_t)(90 - ph.sinceS);
        else if (ph.inWindow && g_outboxCount && ph.sinceS < (int32_t)power::kCheckinSendS)
            s = (uint32_t)(power::kCheckinSendS - ph.sinceS);
        const uint32_t cap = s * 1000UL + 300;
        if (cap < ms) ms = cap;
    }
    if (g_pwEff.loraOnUse && g_pwEff.lora && g_lastInputMs) {   // on demand: wake to turn LoRa off
        const uint32_t on = millis() - g_lastInputMs;
        const uint32_t left = on < power::kLoraOnUseMs ? power::kLoraOnUseMs - on + 100 : 100;
        if (left < ms) ms = left;
    }
    if (g_toast[0]) {
        const int32_t left = (int32_t)(g_toastUntil - millis());
        ms = left <= 0 ? 1 : ((uint32_t)left < ms ? (uint32_t)left : ms);
    }
    if (g_protoReady) {                                // wake for a scheduled path response
        const uint32_t due = g_proto.msUntilScheduledTx();
        if (due < 50) { delay(5); return; }           // due now: stay awake and send it
        if (due < ms) ms = due;
    }

    // Acknowledge the keyboard interrupt; if the line stays low a key is waiting.
    if (g_kbOk) g_kb.clearInterrupt();
    if (digitalRead(KB_INT) == LOW) { delay(2); return; }

    power::WakePins pins;
    pins.key = KB_INT;
    pins.touch = (g_idleActive || g_touchGated) ? -1 : TOUCH_INT;   // idle / gated: key-only wake
    pins.lora = (g_radio && g_loraIf && g_loraIf->isOnline()) ? LORA_IRQ : -1;

    if (g_radio && pins.lora >= 0) g_radio->rxHeal();   // resume RX duty cycle after SPI reads
    if (g_epdOffInSleep == 2) g_display.hibernate();     // controller deep sleep; re-init on next refresh
    else if (g_epdOffInSleep) g_display.powerOff();     // panel charge pump off; image stays
    if (accountedAt) g_pwAwakeMs += millis() - accountedAt;
    energyAccountAwake();
    uint32_t slept = 0;
    const power::Wake w = power::lightSleep(ms, pins, slept);
    {
        static const char* const kWakeTag[] = {"wake-none", "wake-timer", "wake-key", "wake-touch", "wake-lora", "wake-other"};
        evtrace::markRun(w < 6 ? kWakeTag[w] : "wake-?", (int32_t)slept);   // value = ms slept (summed over a run)
    }
    energyAfterSleep(slept);
    accountedAt = millis();
    g_pwSleepMs += slept;
    g_pwSleepTotalMs += slept;
    g_pwSleeps++;
    if (w < 6) g_pwWakes[w]++;
    if (w == power::WAKE_LORA) g_pwLoraWakeMs = millis() ? millis() : 1;
    if (g_radio && pins.lora >= 0 && digitalRead(LORA_IRQ) == HIGH) g_radio->packetAvailable = true;
}

void powerStatus() {
    const uint32_t tot = g_pwAwakeMs + g_pwSleepMs;
    char w[16];
    power::fmtWake(w, sizeof(w), g_pwEff.wakeSecs);
    const char* why = powerSleepBlocker();
    Serial.printf("[POWER] mode=%s%s cpu=%u MHz sleep=%s awake=%us lora=%s gps=%s(%s) wifi=%s keylight=%s\n",
                  power::modeName(g_pw.mode), g_pwOnUsb ? " (USB->Full)" : "", (unsigned)g_pwEff.cpuMhz, w,
                  (unsigned)g_pwEff.awakeSecs, g_pwEff.lora ? "on" : "off", power::gpsName(g_pwEff.gps),
                  g_gpsSerial ? "powered" : "resting", g_pwEff.wifi ? "allowed" : "off",
                  g_pwEff.keyLight ? "allowed" : "off");
    Serial.printf("[POWER] asleep %u%% (%lu sleeps; wakes timer %lu key %lu touch %lu lora %lu other %lu) blocker=%s\n",
                  tot ? (unsigned)((uint64_t)g_pwSleepMs * 100 / tot) : 0u, (unsigned long)g_pwSleeps,
                  (unsigned long)g_pwWakes[power::WAKE_TIMER], (unsigned long)g_pwWakes[power::WAKE_KEY],
                  (unsigned long)g_pwWakes[power::WAKE_TOUCH], (unsigned long)g_pwWakes[power::WAKE_LORA],
                  (unsigned long)g_pwWakes[power::WAKE_OTHER], why ? why : "none");
    Serial.printf("[POWER] battery %.3f V %+d mA tte=%u min usb=%d bench=%d\n", g_bat.volts, (int)g_bat.avgMa,
                  (unsigned)g_bat.tteMin, g_bat.usb, g_benchUntil != 0);
    if (g_radio) Serial.printf("[POWER] lora rx ok=%lu crc_fail=%lu  tx started=%lu done=%lu timeout=%lu\n",
                               (unsigned long)g_radio->rxOk, (unsigned long)g_radio->rxCrcFail,
                               (unsigned long)g_radio->txStarted, (unsigned long)g_radio->txDone,
                               (unsigned long)g_radio->txTimeout);
    if (g_radio) Serial.printf("[POWER] lora rx %s (setting %u): listen %lu ms / sleep %lu ms, arms=%lu heals=%lu%s\n",
                               g_radio->rxDutyCycleActive() ? "duty cycle" : "continuous", (unsigned)g_pw.rxDc,
                               (unsigned long)(g_radio->rxDcListenUs() / 1000), (unsigned long)(g_radio->rxDcSleepUs() / 1000),
                               (unsigned long)g_radio->rxDcArms, (unsigned long)g_radio->rxDcHeals,
                               g_radio->rxDutyCycleTcxoFailed() ? " TCXO-FAIL" : "");
    if (g_storeReady) Serial.printf("[POWER] storage writes pending=%d\n", g_msgStore.writeQueue().drainCount());
    Serial.printf("[POWER] messages unread=%d lxmf_queue=%d uptime=%lu s\n",
                  g_storeReady ? g_msgStore.totalUnreadCount() : -1, g_protoReady ? g_proto.lxmfQueuedCount() : -1,
                  (unsigned long)(millis() / 1000));
}

// ---- Serial console (debug) --------------------------------------------------------
// "@..." lines come from the desktop simulator's live-device mode (remote/ScreenMirror.h):
// screen mirror control, raw key events and touches.
void handleRemoteLine(const char* p) {
    if (p[0] == 'M' && p[1] == ' ') {
        mirror::hostCommand(p + 2);
    } else if (p[0] == 'K' && p[1] == ' ') {
        const int raw = atoi(p + 2);
        if (raw > 0 && raw < 256 && g_kbOk) g_kb.injectRaw((uint8_t)raw);
    } else if (p[0] == 'T' && p[1] == ' ') {
        int x = 0, y = 0;
        if (sscanf(p + 2, "%d %d", &x, &y) == 2 && x >= 0 && y >= 0 && x < EPD_WIDTH && y < EPD_HEIGHT)
            handleTouch((int16_t)x, (int16_t)y);
    }
}

void mirrorTick() {
    if (!mirror::active()) return;
    char st[96];
    snprintf(st, sizeof(st), "bat=%d mv=%d ma=%d chg=%d usb=%d full=%d idle=%d", g_bat.pct,
             (int)(g_bat.volts * 1000 + 0.5f), (int)g_bat.avgMa, g_bat.charging, g_bat.usb, g_bat.full,
             g_idleActive);
    mirror::tick(g_display.framebuffer(), g_display.width(), g_display.height(), st);
}

// ---- sdput: file upload to the SD card over USB (tools/maptiles/push_tiles.py) ---------
// "sdput <len> <crc32 hex> <path>" (path under /ratdeckpro, e.g. /maps/9/150/180.rmt).
// The device answers "[SDPUT] ready", then reads exactly <len> raw bytes (no line
// parsing meanwhile), checks the CRC-32 and writes the file: "[SDPUT] ok" or
// "[SDPUT] err <why>". Payload is sent only after "ready", so a refused command (locked,
// bad args) never lets binary bytes reach the line parser.
constexpr size_t kSdPutMax = 16384;
uint8_t* g_putBuf = nullptr;
size_t   g_putLen = 0, g_putGot = 0;
uint32_t g_putCrc = 0, g_putAt = 0;
char     g_putPath[96] = "";
uint32_t g_putOffset = 0;                    // > 0: append at this offset
bool     g_putActive = false;

uint32_t crc32Of(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void sdputStart(const char* args) {
    unsigned long len = 0, crc = 0, off = 0;
    char rel[80] = "";
    // Optional 4th value: append at this byte offset (big files in 16 KB pieces; the
    // file must already be exactly that long). Without it the file is (re)written.
    if (sscanf(args, "%lu %lx %79s %lu", &len, &crc, rel, &off) < 3 || !len || len > kSdPutMax || rel[0] != '/' ||
        strstr(rel, "..")) {
        Serial.println("[SDPUT] err usage: sdput <len 1-16384> <crc32 hex> </path under ratdeckpro> [offset]");
        return;
    }
    g_putOffset = off;
    if (!g_putBuf) {
#if !defined(SIMULATOR)
        g_putBuf = (uint8_t*)heap_caps_malloc(kSdPutMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!g_putBuf) g_putBuf = (uint8_t*)malloc(kSdPutMax);
        if (!g_putBuf) { Serial.println("[SDPUT] err no memory"); return; }
    }
    if (!sdMount()) { Serial.println("[SDPUT] err no SD card"); return; }
    mapReadDone();                              // the file may be one the map holds open
    g_putLen = len; g_putGot = 0; g_putCrc = (uint32_t)crc; g_putAt = millis();
    snprintf(g_putPath, sizeof(g_putPath), "%s%s", SD_PATH_ROOT, rel);
    g_putActive = true;
    Serial.println("[SDPUT] ready");
}

void sdputFinish() {
    g_putActive = false;
    if (crc32Of(g_putBuf, g_putLen) != g_putCrc) { Serial.println("[SDPUT] err crc"); return; }
    static char lastDir[96] = "";
    char dir[96];
    snprintf(dir, sizeof(dir), "%s", g_putPath);
    if (char* slash = strrchr(dir, '/')) *slash = 0;
    if (strcmp(dir, lastDir) != 0) {            // tiles arrive grouped by folder
        if (!g_sdStore.ensureDir(dir)) { lastDir[0] = 0; Serial.println("[SDPUT] err mkdir"); return; }
        snprintf(lastDir, sizeof(lastDir), "%s", dir);
    }
    if (g_putOffset) {                          // append piece: the file must end at the offset
        File af = g_sdStore.openFile(g_putPath, FILE_APPEND);
        if (!af) { Serial.println("[SDPUT] err open"); return; }
        if (af.size() != g_putOffset) { af.close(); Serial.printf("[SDPUT] err offset (file is %u)\n", (unsigned)af.size()); return; }
        const size_t wr = af.write(g_putBuf, g_putLen);
        af.close();
        if (wr != g_putLen) { Serial.println("[SDPUT] err write"); return; }
    } else if (!g_sdStore.writeSimple(g_putPath, g_putBuf, g_putLen)) { lastDir[0] = 0; Serial.println("[SDPUT] err write"); return; }
    Serial.println("[SDPUT] ok");
}

// Returns true while a payload is being received (the line parser must not run).
bool sdputPoll() {
    if (!g_putActive) return false;
    while (g_putGot < g_putLen && Serial.available()) {
#if !defined(SIMULATOR)
        const size_t got = Serial.read(g_putBuf + g_putGot, g_putLen - g_putGot);
        if (!got) break;
        g_putGot += got;
#else
        const int b = Serial.read();
        if (b < 0) break;
        g_putBuf[g_putGot++] = (uint8_t)b;
#endif
        g_putAt = millis();
    }
    if (g_putGot >= g_putLen) { sdputFinish(); return false; }
    if (millis() - g_putAt > 3000) { g_putActive = false; Serial.println("[SDPUT] err timeout"); }
    return g_putActive;
}

void handleSerialLine(char* p) {
    while (*p == ' ') p++;
    if (!*p) return;
    if (p[0] == '@') { handleRemoteLine(p + 1); return; }   // keys still meet the lock prompt
    if (g_locked) {                       // locked: status only, nothing that reads or changes data
        static const char* kOk[] = {"pw", "clk", "id", "lock", "ev", "rl", "sdinfo", "energy"};
        bool ok = false;
        for (const char* k : kOk) ok |= !strcmp(p, k);
        if (!ok) { Serial.println("[CMD] locked: unlock on the device first"); return; }
    }
    if (!strncmp(p, "wiz", 3) && (p[3] == 0 || p[3] == ' ')) {   // wiz [start|skip|listen|step <n>]
        if (!strcmp(p, "wiz start")) wizStart();
        else if (!strcmp(p, "wiz skip")) wizFinish(false);
        else if (!strcmp(p, "wiz listen")) { if (g_wizOpen) { g_wizStep = WZ_IMPORT; wizListen(true); } }
        else if (!strncmp(p, "wiz step ", 9) && g_wizOpen) { g_wizStep = constrain(atoi(p + 9), 0, WZ_COUNT - 1); requestRedraw(); }
        Serial.printf("[WIZ] open=%d step=%d listen=%d got=%d done=%d\n", g_wizOpen, g_wizStep, g_wizListen, g_wizGot,
                      g_cfg.wizDone);
        return;
    }
    if (!strncmp(p, "shareall ", 9)) {                     // shareall <32 hex dest>: every contact's card
        uint8_t to[16];
        if (strlen(p + 9) < 32 || !hexToHash(p + 9, to)) { Serial.println("[CARD] shareall <32 hex>"); return; }
        shareAllTo(to);
        return;
    }
    if (!strncmp(p, "rt", 2) && (p[2] == 0 || p[2] == ' ')) {   // rt | rt start <hex> [secs] | rt stop
        char hx[40] = "";
        int secs = 0;
        uint8_t h[16];
        if (!strcmp(p, "rt stop")) rtStop();
        else if (!strncmp(p, "rt start ", 9) && sscanf(p + 9, "%39s %d", hx, &secs) >= 1 && hexToHash(hx, h)) rtStart(h, secs);
        Serial.printf("[RT] on=%d sent=%lu replies=%lu lost=%lu heard=%lu file=%s\n", g_rtOn, (unsigned long)g_rtSent,
                      (unsigned long)g_rtGot, (unsigned long)g_rtLost, (unsigned long)g_rtHeard, g_rtFile);
        return;
    }
    if (!strncmp(p, "batcap", 6)) {           // batcap | batcap reset | batcap sample <from%> <to%> <mAh>
        int a = 0, b = 0, m = 0;
        if (!strcmp(p, "batcap reset")) {
            g_capMah = g_capW = g_capN = 0;
            capSave();
            snprintf(g_capLast, sizeof(g_capLast), "reset");
        } else if (sscanf(p + 6, " sample %d %d %d", &a, &b, &m) == 3) {
            capSample(a, b, (float)m);
        }
        Serial.printf("[BATCAP] learned %u mAh (w %u, %u discharges), anchor %d %%, %.0f mAh since, last: %s, uses %u\n",
                      (unsigned)g_capMah, (unsigned)g_capW, (unsigned)g_capN, g_capAnchor, g_capMas / 3600.0, g_capLast,
                      (unsigned)batCapacityMah(nullptr));
        return;
    }
    if (!strncmp(p, "batleft", 7)) {          // batleft [feed <mA> <min>]: estimate (bench: feed drain data)
        int ma = 0, mins = 0;
        if (sscanf(p + 7, " feed %d %d", &ma, &mins) == 2 && ma > 0 && mins > 0)
            energy::addMeasured((int16_t)-ma, (uint32_t)mins * 60000UL);
        char t[48];
        batLeftText(t, sizeof(t));
        Serial.printf("[BAT] left: %s\n", t);
        return;
    }
    if (!strncmp(p, "lowbat", 6)) {           // lowbat <pct> <hex> | lowbat (status)
        int pc = -1;
        char hx[40] = "";
        uint8_t h[16];
        if (sscanf(p + 6, " %d %39s", &pc, hx) == 2 && hexToHash(hx, h)) {
            g_cfg.lowBatPct = (uint8_t)pc; memcpy(g_cfg.lowBatTo, h, 16); g_cfg.lowBatSent = false; saveConfig();
        }
        Serial.printf("[LOWBAT] at %u%% sent=%d\n", (unsigned)g_cfg.lowBatPct, g_cfg.lowBatSent);
        return;
    }
    if (!strncmp(p, "geof", 4) && (p[4] == 0 || p[4] == ' ')) {   // geof | geof add <hex> me|w<n> <m> [rep] | geof clear
        char hx[40] = "", at[8] = "";
        int m = 0, rep = 0;
        uint8_t h[16];
        if (!strcmp(p, "geof clear")) { g_geofCount = 0; geofSave(); }
        else if (!strncmp(p, "geof add ", 9) && sscanf(p + 9, "%39s %7s %d %d", hx, at, &m, &rep) >= 3 && hexToHash(hx, h)) {
            const int place = (at[0] == 'w' || at[0] == 'W') ? atoi(at + 1) : 0;
            Serial.printf("[GEOF] add %s\n", geofAdd(h, place, (uint16_t)m, rep != 0) ? "ok" : "failed");
        }
        for (int i = 0; i < g_geofCount; i++) {
            char t[40];
            geofText(g_geof[i], t, sizeof(t));
            Serial.printf("[GEOF] %d: %s%s%s\n", i + 1, t, g_geof[i].repeat ? " repeat" : "", g_geof[i].armed ? "" : " (inside)");
        }
        Serial.printf("[GEOF] %d alerts\n", g_geofCount);
        return;
    }
    if (!strcmp(p, "contacts export")) { Serial.printf("[CONTACTS] exported %d\n", contactsExport()); return; }
    if (!strcmp(p, "contacts import")) { Serial.printf("[CONTACTS] imported %d\n", contactsImport()); return; }
    if (!strcmp(p, "setcheck")) {             // every Settings row whose title or value is wider than a row
        int bad = 0;
        for (int pg = 0; pg <= (int)PG_BACKUPS; pg++) {
            if (pg == PG_ENERGY_GRAPH) continue;
            buildSettingsRows((Page)pg);
            for (int i = 0; i < g_srowCount; i++) {
                const int tw = textW(g_srows[i].title), sw = textW(g_srows[i].sub);
                if (tw > W - 16 || sw > W - 16) {
                    Serial.printf("[SETCHECK] %s row %d: \"%s\" (%d) / \"%s\" (%d)\n", pageTitle((Page)pg), i + 1,
                                  g_srows[i].title, tw, g_srows[i].sub, sw);
                    bad++;
                }
            }
        }
        buildSettingsRows(g_page);
        Serial.printf("[SETCHECK] %d rows too wide\n", bad);
        return;
    }
    if (!strncmp(p, "idlescr", 7)) {                   // idlescr [0-3]: idle screen; "idle now"
        const int v = p[7] ? atoi(p + 8) : -1;
        if (v >= 0 && v < IDLE_COUNT) { g_cfg.idleScreen = (uint8_t)v; saveConfig(); }
        Serial.printf("[IDLE] screen %s\n", kIdleScreenName[g_cfg.idleScreen]);
        return;
    }
    if (!strcmp(p, "idle now")) { enterIdle(); return; }
    if (!strncmp(p, "lock", 4) && (p[4] == 0 || p[4] == ' ')) {
        if (!strcmp(p, "lock now")) lockNow();
        Serial.printf("[LOCK] locked=%d password=%d idleMode=%u fails=%u wait=%us vault=%s\n", g_locked,
                      lockHasPassword(), (unsigned)g_cfg.idleMode, (unsigned)g_lockFails, (unsigned)lockWaitS(),
                      vault::status());
        return;
    }
    if ((p[0] == 't' || p[0] == 'T') && p[1] == ':') {
        if (!g_threadOpen) { Serial.println("[CMD] open a thread first"); return; }
        if (sendOrQueue(g_threadPeer, p + 2) != SendResult::Failed) loadHistory(g_threadPeer);
        Serial.printf("[CMD] send: %s\n", g_txStr);
        requestRedraw();
    } else if (!strcmp(p, "a")) {
        doAnnounce();
        requestRedraw();
    } else if (!strcmp(p, "h") || !strcmp(p, "i") || !strcmp(p, "c") || !strcmp(p, "p") || !strcmp(p, "s")) {
        const ScreenId map[] = {SCR_HOME, SCR_INBOX, SCR_CONTACTS, SCR_PEERS, SCR_SETTINGS};
        const char* keys = "hicps";
        goScreen(map[strchr(keys, p[0]) - keys]);
    } else if (!strcmp(p, "d")) {
        batLogDump();
    } else if (!strcmp(p, "g")) {
        gpsDump();
    } else if (!strcmp(p, "gp")) {
        gpsPowerCycle("console");
    } else if (!strcmp(p, "gr")) {
        gpsHardReset();
    } else if (!strcmp(p, "pw")) {
        powerStatus();
    } else if (!strncmp(p, "epoch", 5)) {
        // epoch            print the clock (Unix seconds)
        // epoch <secs>     set it (bench: e.g. a clock a little off, to test check-in sync)
        long long v = 0;
        if (sscanf(p + 5, "%lld", &v) == 1 && v > 1700000000LL) setEpoch((time_t)v);
        Serial.printf("[CLOCK] epoch %lld (%s)\n", (long long)time(nullptr),
                      handheld::ClockConfidence::synchronized() ? "synchronized" : "not synchronized");
    } else if (!strncmp(p, "checkin", 7)) {
        // checkin [hh:mm | minute-of-hour] [every-min]
        int h = -1, m = -1, e = -1;
        bool set = false;
        if (sscanf(p + 7, " %d:%d %d", &h, &m, &e) >= 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
            g_pw.checkinStart = (uint16_t)(h * 60 + m);
            set = true;
        } else if (sscanf(p + 7, "%d %d", &m, &e) >= 1 && m >= 0 && m <= 59) {
            g_pw.checkinStart = (uint16_t)(g_pw.checkinStart / 60 * 60 + m);   // minute of the hour
            set = true;
        }
        if (set) {
            if (e >= 5 && e <= 1440) g_pw.checkinEvery = (uint16_t)e;   // any >= 5 for bench tests
            power::save(g_pw);
            powerApply(true);
        }
        const power::CheckinPhase ph = checkinNow();
        struct tm nt;
        localNow(nt);
        Serial.printf("[CHECKIN] now %02d:%02d:%02d at %02u:%02u every %u min; %s, change in %lu s, next in %lu s; %d queued; clock %s\n",
                      nt.tm_hour, nt.tm_min, nt.tm_sec, (unsigned)(g_pw.checkinStart / 60), (unsigned)(g_pw.checkinStart % 60),
                      (unsigned)g_pw.checkinEvery,
                      ph.inWindow ? "window open" : "closed",
                      (unsigned long)ph.toChangeS, (unsigned long)ph.toNextS, g_outboxCount,
                      handheld::ClockConfidence::synchronized() ? "synced" : g_clkLastAt ? "from peer" : "own");
    } else if (p[0] == 'p' && p[1] == 'm' && (p[2] == ' ' || isdigit((unsigned char)p[2]))) {
        const int m = atoi(p + 2);
        if (m >= 0 && m < power::MODE_COUNT) {
            if (m == power::MODE_CUSTOM && !g_pw.customSet) { g_pw.custom = power::selected(g_pw); g_pw.customSet = true; }
            g_pw.mode = (uint8_t)m;
            power::save(g_pw);
            powerApply(true);
        }
    } else if (!strncmp(p, "pset ", 5)) {
        // Custom-mode knob from the console: pset cpu|wake|awake|lora|gps|wifi|kbl|touch|mapgps <value>
        char k[8] = "";
        int v = -1;
        if (sscanf(p + 5, "%7s %d", k, &v) == 2 && v >= 0) {
            if (!g_pw.customSet) { g_pw.custom = power::selected(g_pw); g_pw.customSet = true; }
            power::Profile& c = g_pw.custom;
            if (!strcmp(k, "cpu") && (v == 80 || v == 160 || v == 240)) c.cpuMhz = (uint16_t)v;
            else if (!strcmp(k, "wake") && v <= 3600) c.wakeSecs = (uint16_t)v;
            else if (!strcmp(k, "awake") && v >= 1 && v <= 600) c.awakeSecs = (uint16_t)v;
            else if (!strcmp(k, "lora")) { c.lora = v == 1; c.loraOnUse = v == 2; }
            else if (!strcmp(k, "gps") && v < power::GPS_COUNT) c.gps = (uint8_t)v;
            else if (!strcmp(k, "wifi")) c.wifi = v != 0;
            else if (!strcmp(k, "kbl")) c.keyLight = v != 0;
            else if (!strcmp(k, "touch")) c.touchOnKey = v != 0;
            else if (!strcmp(k, "mapgps")) c.mapGps = v != 0;
            else if (!strcmp(k, "trackgps")) c.trackGps = v != 0;
            else { Serial.println("[POWER] pset: bad knob/value"); return; }
            g_pw.mode = power::MODE_CUSTOM;
            power::save(g_pw);
            powerApply(true);
        } else {
            Serial.println("[POWER] pset cpu 80|160|240, wake <s>, awake <s>, lora 0|1|2(on use), gps 0-4 (on,15m,1h,off,on use), wifi 0|1, kbl 0|1, touch 0|1, mapgps 0|1");
        }
    } else if (!strncmp(p, "bench", 5)) {
        const int secs = p[5] ? atoi(p + 5) : 120;
        if (secs <= 0) { if (g_benchUntil) { g_benchUntil = 1; Serial.println("[BENCH] stopping"); } }
        else if (!g_bat.valid || g_bat.volts < 3.7f) Serial.println("[BENCH] refused: battery missing or below 3.70 V");
        else if (!chargerSetHiz(true)) Serial.println("[BENCH] charger not responding");
        else {
            g_benchUntil = millis() + (uint32_t)(secs > 14400 ? 14400 : secs) * 1000UL;
            Serial.printf("[BENCH] charger input off for %d s - running from battery\n", secs > 14400 ? 14400 : secs);
        }
    } else if (!strncmp(p, "sleeptest ", 10)) {
        // Diagnostic: one timer-only light sleep. "sleeptest <ms> 0" skips the watchdog guard.
        int ms = 0, guard = 1;
        // Third argument: what to stop first (bits): 1 audio I2S, 2 GPS UART + receiver, 4 LoRa.
        int stop = 0;
        sscanf(p + 10, "%d %d %d", &ms, &guard, &stop);
        if (ms < 10 || ms > 60000) { Serial.println("[SLEEPTEST] sleeptest <10-60000 ms> [guard 0|1] [stop bits 1 audio 2 gps 4 lora]"); return; }
        Serial.printf("[SLEEPTEST] sleeping %d ms, guard %d, stop %d\n", ms, guard, stop);
        Serial.flush();
        if (stop & 1) g_audio.pause();
        if (stop & 2) { g_gpsAllowed = false; gpsApplyPower(); }
        if ((stop & 4) && g_loraIf && g_radio) { g_loraIf->stop(); g_radio->sleep(); }
        delay(50);
        uint32_t slept = 0;
        const power::Wake w = power::lightSleep((uint32_t)ms, power::WakePins{}, slept, guard != 0);
        if (stop & 1) g_audio.resume();
        if (stop & 2) { g_gpsAllowed = true; gpsApplyPower(); }
        if ((stop & 4) && g_loraIf) g_loraIf->start();
        rtclog::sample(1, (int16_t)stop, false, 1, slept / 1000);   // marks a survived sleep
        Serial.printf("[SLEEPTEST] woke (%s) after %lu ms, uptime %lu s\n", power::wakeName(w),
                      (unsigned long)slept, (unsigned long)(millis() / 1000));
    } else if (!strncmp(p, "msg ", 4) && strlen(p) > 4 + 32 + 1) {
        // Send a message to a destination hash: msg <32 hex> <text>
        uint8_t dest[16];
        bool ok = true;
        for (int i = 0; i < 16 && ok; i++) {
            char h[3] = {p[4 + i * 2], p[5 + i * 2], 0};
            char* end = nullptr;
            dest[i] = (uint8_t)strtoul(h, &end, 16);
            ok = end == h + 2;
        }
        if (!ok) { Serial.println("[CMD] msg <32 hex dest> <text>"); return; }
        const power::CheckinPhase ph = checkinNow();
        if (g_pwEff.checkin && !(ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS)) {
            Serial.printf("[MSG] outbox %s\n", outboxAdd(dest, p + 4 + 32 + 1) ? "queued for check-in" : "full");
            return;
        }
        const SendResult r = sendOrQueue(dest, p + 4 + 32 + 1);
        Serial.printf("[MSG] send %s at uptime %lu ms: %s\n",
                      r == SendResult::Sent ? "queued" : r == SendResult::Queued ? "outboxed" : "failed",
                      (unsigned long)millis(), g_txStr);
    } else if (!strncmp(p, "gpio ", 5)) {
        // Bench: drive an output pin (power-rail experiments): gpio <pin> <0|1>
        int pin = -1, v = -1;
        if (sscanf(p + 5, "%d %d", &pin, &v) == 2 && pin >= 0 && pin <= 48 && (v == 0 || v == 1)) {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, v);
            Serial.printf("[GPIO] %d -> %d\n", pin, v);
        } else {
            Serial.println("[CMD] gpio <pin> <0|1>");
        }
    } else if (!strcmp(p, "epdsleep")) {
        g_display.hibernate();
        Serial.println("[EINK] controller in deep sleep (next refresh re-inits)");
    } else if (!strcmp(p, "map") || !strncmp(p, "map ", 4)) {
        // map | map close | map <lat> <lon> [zoom]: open the offline map (tiles on SD).
        double la = 0, lo = 0; int z = 0;
        const int got = sscanf(p + 3, "%lf %lf %d", &la, &lo, &z);
        if (!strcmp(p, "map close")) closeMap();
        else if (got >= 2) {
            if (got >= 3) mapview::setZoom(z);
            openMapAt(la, lo);
        } else if (!mapview::isOpen()) openMap();   // "map" on an open map: status only
        char st[64]; mapview::status(st, sizeof(st));
        uint32_t ld, ms, hi; mapview::stats(ld, ms, hi);
        Serial.printf("[MAP] %s %s; tiles loaded %lu (packed %lu, %lu ms, decode %lu ms) missing %lu cache hits %lu\n",
                      mapview::isOpen() ? "open" : "closed", st, (unsigned long)ld,
                      (unsigned long)mapview::packedLoaded(), (unsigned long)mapview::packedMillis(),
                      (unsigned long)mapview::decodeMillis(), (unsigned long)ms, (unsigned long)hi);
    } else if (!strncmp(p, "maptest ", 8)) {
        // Bench: write labelled grid test tiles (3x3 at z14 and z15) around <lat> <lon> on the SD card.
        double la = 0, lo = 0;
        if (sscanf(p + 8, "%lf %lf", &la, &lo) != 2 || !sdMount()) { Serial.println("[MAP] maptest <lat> <lon> (needs SD)"); return; }
        static uint8_t buf[mapview::kHeaderBytes + mapview::kTileBytes];
        int written = 0;
        const uint32_t t0 = millis();
        for (int z = 14; z <= 15; z++) {
            const double n = (double)(1u << z), r = la * 3.14159265358979 / 180.0;
            const int cx = (int)((lo + 180.0) / 360.0 * n);
            const int cy = (int)((1.0 - log(tan(r) + 1.0 / cos(r)) / 3.14159265358979) / 2.0 * n);
            for (int ty = cy - 1; ty <= cy + 1; ty++) for (int tx = cx - 1; tx <= cx + 1; tx++) {
                memcpy(buf, "RMT1", 4); buf[4] = 0; buf[5] = 1; buf[6] = 0; buf[7] = 1;
                uint8_t* bits = buf + mapview::kHeaderBytes;
                for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) {
                    const bool on = x < 2 || y < 2 || x > 253 || y > 253 ||          // border
                                    ((x % 32 == 0 || y % 32 == 0) && ((x + y) & 1)) ||   // dotted grid
                                    (((tx * 256 + x) + (ty * 256 + y)) % 97 < 3) ||      // continuous diagonals
                                    (x >= 8 && x < 8 + (tx % 10) * 6 && y >= 8 && y < 14) || // x mark bar
                                    (y >= 18 && y < 18 + (ty % 10) * 6 && x >= 8 && x < 14); // y mark bar
                    if (on) bits[y * 32 + (x >> 3)] |= 0x80 >> (x & 7);
                    else bits[y * 32 + (x >> 3)] &= ~(0x80 >> (x & 7));
                }
                char dir[64], path[80];
                snprintf(dir, sizeof(dir), "%s/maps/%d/%d", SD_PATH_ROOT, z, tx);
                snprintf(path, sizeof(path), "%s/%d.rmt", dir, ty);
                g_sdStore.ensureDir(dir);
                if (g_sdStore.writeSimple(path, buf, sizeof(buf))) written++;
            }
        }
        Serial.printf("[MAP] maptest wrote %d tiles in %lu ms\n", written, (unsigned long)(millis() - t0));
    } else if (!strncmp(p, "sdarc ", 6)) {
        sdArchiveSet(atoi(p + 6) != 0);
        Serial.printf("[SD] message archive setting %s (applies at next boot)\n", sdArchiveEnabled() ? "on" : "off");
    } else if (!strcmp(p, "wpt") || !strcmp(p, "wpt clear") || !strncmp(p, "wpt add ", 8)) {
        // wpt = list waypoints; wpt clear = remove all; wpt add <lat> <lon> <NAME> (bench)
        double la = 0, lo = 0;
        char nm[16] = "";
        if (!strncmp(p, "wpt add ", 8)) {
            if (sscanf(p + 8, "%lf %lf %15s", &la, &lo, nm) == 3 && g_wptCount < kWptMax) {
                Wpt& w = g_wpt[g_wptCount++];
                w.lat = (float)la; w.lon = (float)lo;
                strlcpy(w.name, nm, sizeof(w.name));
                w.at = 0;
                wptSave();
                Serial.printf("[WPT] W%d %s at %.5f,%.5f\n", g_wptCount, w.name, la, lo);
            }
            return;
        }
        if (p[3]) { g_wptCount = 0; g_wptSel = -1; wptSave(); Serial.println("[WPT] cleared"); return; }
        for (int i = 0; i < g_wptCount; i++)
            Serial.printf("[WPT] W%d %s %.5f,%.5f\n", i + 1, g_wpt[i].name, g_wpt[i].lat, g_wpt[i].lon);
        Serial.printf("[WPT] %d waypoints\n", g_wptCount);
    } else if (!strcmp(p, "track") || !strncmp(p, "track ", 6)) {
        // track = status; bench: track add <lat> <lon> | track clear
        double la = 0, lo = 0;
        if (!strcmp(p, "track clear")) { trackClear(); return; }
        if (!strcmp(p, "track gpx")) { Serial.printf("[TRACK] gpx %d points %s\n", trackExportGpx(), g_trackGpx); return; }
        if (!strcmp(p, "track back")) { if (tbStart()) openMap(); return; }
        if (!strcmp(p, "track back stop")) { tbStop("stopped"); return; }
        if (!strncmp(p, "track add ", 10) && sscanf(p + 10, "%lf %lf", &la, &lo) == 2) {
            if (!g_trackLoaded) trackLoad();
            trackAdd(la, lo);
            return;
        }
        Serial.printf("[TRACK] %s, %d points in RAM, waypoints %d\n", g_cfg.trackOn ? "on" : "off", g_trackN, g_wptCount);
    } else if (!strcmp(p, "cpos") || !strncmp(p, "cpos ", 5)) {
        // cpos = contacts' last positions; bench: cpos <hex32> <lat> <lon> [age min] |
        // cpos num <hex32> <n> (map number) | cpos clear
        if (!strcmp(p, "cpos clear")) { g_cposCount = 0; g_mapSel = -1; cposSave(); Serial.println("[LOC] cpos cleared"); return; }
        if (!strncmp(p, "cpos num ", 9)) {
            uint8_t d[16];
            if (strlen(p + 9) < 34 || !hexToHash(p + 9, d) || atoi(p + 9 + 33) < 1 || atoi(p + 9 + 33) > 99) {
                Serial.println("[LOC] cpos num <hex32> <1-99>");
                return;
            }
            cposAssign(d, atoi(p + 9 + 33));
            return;
        }
        if (p[4]) {
            uint8_t d[16];
            double la = 0, lo = 0;
            int ageMin = 0;
            if (strlen(p + 5) < 32 || !hexToHash(p + 5, d) || sscanf(p + 5 + 32, "%lf %lf %d", &la, &lo, &ageMin) < 2) {
                Serial.println("[LOC] cpos <hex32> <lat> <lon> [age min]");
                return;
            }
            cposNote(d, la, lo, (uint32_t)time(nullptr) - (uint32_t)ageMin * 60u);
            return;
        }
        for (int i = 0; i < g_cposCount; i++) {
            char nm[24], hex[33];
            peerName(g_cpos[i].dest, nm, sizeof(nm));
            hashToHex(g_cpos[i].dest, hex);
            Serial.printf("[LOC] cpos #%u %s %s %.5f,%.5f at %lu%s\n", (unsigned)g_cpos[i].num, hex, nm,
                          g_cpos[i].lat, g_cpos[i].lon, (unsigned long)g_cpos[i].at,
                          g_cpos[i].hasPos ? "" : " (no position yet)");
        }
        Serial.printf("[LOC] cpos %d contacts\n", g_cposCount);
    } else if (!strcmp(p, "loc")) {
        char pos[56];
        gpsPosText(pos, sizeof(pos));
        Serial.printf("[LOC] %s (hdop %.1f)\n", pos, g_gpsHdop);
        for (int i = 0; i < g_ashareCount; i++) {
            char hex[33]; hashToHex(g_ashare[i].dest, hex);
            Serial.printf("[LOC] auto %s every %u min until %lu last %lu\n", hex, (unsigned)g_ashare[i].mins,
                          (unsigned long)g_ashare[i].until, (unsigned long)g_ashare[i].lastAt);
        }
    } else if (!strncmp(p, "locshare ", 9) || !strncmp(p, "ashare ", 7)) {
        // Bench: locshare <hex32> = share my location; ashare <hex32> <0|5|15|30|60> | ashare off
        const bool share = p[0] == 'l';
        const char* a = p + (share ? 9 : 7);
        if (!share && !strcmp(a, "off")) { ashareStopAll(); return; }
        uint8_t dest[16];
        if (strlen(a) < 32 || !hexToHash(a, dest)) { Serial.println("[LOC] need <hex32>"); return; }
        if (share) { shareMyLocation(dest); return; }
        const int mins = atoi(a + 32);
        int i = ashareFind(dest);
        if (!mins) { if (i >= 0) { ashareRemove(i); ashareSave(); } Serial.println("[LOC] auto-share off"); return; }
        if (i < 0) {
            if (g_ashareCount >= kAutoShareMax) return;
            i = g_ashareCount++;
            memset(&g_ashare[i], 0, sizeof(AutoShare));
            memcpy(g_ashare[i].dest, dest, 16);
        }
        g_ashare[i].mins = (uint8_t)mins;
        g_ashare[i].until = 0;
        ashareSave();
        Serial.printf("[LOC] auto-share every %d min\n", mins);
        requestRedraw();
    } else if (!strncmp(p, "sdput ", 6)) {
        sdputStart(p + 6);
    } else if (!strncmp(p, "sdget ", 6)) {
        // Read one SD file (absolute card path, <= 16 KB): "[SDGET] <len> <crc32 hex>", then
        // exactly <len> raw bytes (tools/maptiles/copy_tiles.py checks the CRC).
        const char* path = p + 6;
        if (!sdMount()) { Serial.println("[SDGET] err no SD card"); return; }
        if (!g_putBuf) {
#if !defined(SIMULATOR)
            g_putBuf = (uint8_t*)heap_caps_malloc(kSdPutMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
            if (!g_putBuf) g_putBuf = (uint8_t*)malloc(kSdPutMax);
            if (!g_putBuf) { Serial.println("[SDGET] err no memory"); return; }
        }
        size_t n = 0;
        if (!g_sdStore.readFile(path, g_putBuf, kSdPutMax, n)) { Serial.println("[SDGET] err read"); return; }
        Serial.printf("[SDGET] %u %08lx\n", (unsigned)n, (unsigned long)crc32Of(g_putBuf, n));
        Serial.write(g_putBuf, n);
        Serial.flush();
    } else if (!strncmp(p, "sdrm ", 5)) {
        // Delete one SD file, or an EMPTY folder (absolute card path). No recursion.
        const char* path = p + 5;
        if (!sdMount() || !path[0] || !strcmp(path, "/")) { Serial.println("[SDRM] err"); return; }
        bool ok = g_sdStore.remove(path) || g_sdStore.removeDir(path);
        Serial.printf("[SDRM] %s %s\n", ok ? "ok" : "err", path);
    } else if (!strcmp(p, "sdls") || !strncmp(p, "sdls ", 5) || !strncmp(p, "sdlsa ", 6)) {
        // List one SD folder (absolute card path, default "/"): name, size or <dir>.
        // sdls shows the first 80 entries; sdlsa (for tools) shows all of them.
        const bool all = p[4] == 'a';
        const char* dirPath = all ? p + 6 : (p[4] ? p + 5 : "/");
        if (!sdMount()) { Serial.println("[SD] no card"); return; }
        File d = g_sdStore.openDir(dirPath);
        if (!d) { Serial.printf("[SDLS] cannot open %s\n", dirPath); return; }
        int n = 0;
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            if (all || n++ < 80) {
                if (all) n++;
                if (e.isDirectory()) Serial.printf("[SDLS] <dir> %s\n", e.name());
                else Serial.printf("[SDLS] %10lu %s\n", (unsigned long)e.size(), e.name());
            }
            e.close();
            if ((n & 31) == 0) yield();
        }
        d.close();
        Serial.printf("[SDLS] %d entries in %s\n", n, dirPath);
    } else if (!strncmp(p, "sdformat", 8)) {
        // sdformat <cluster bytes 512..32768> ERASE-ALL : new FAT32 on the card with that
        // cluster size, then restart. Map tiles are 8 KB: 4096 wastes nothing (stock cards
        // come with 32 KB clusters, 4x the space per tile). Everything on the card is lost.
#if defined(SIMULATOR)
        Serial.println("[SD] sdformat: not in the simulator");
#else
        unsigned long au = 0;
        char conf[16] = "";
        if (sscanf(p + 8, "%lu %15s", &au, conf) != 2 || strcmp(conf, "ERASE-ALL") || au < 512 || au > 32768 ||
            (au & (au - 1))) {
            Serial.println("[SD] usage: sdformat <512|1024|...|32768> ERASE-ALL");
            return;
        }
        if (!sdMount()) { Serial.println("[SD] no card"); return; }
        SharedSPILock lock;
        FATFS* fs = nullptr;
        DWORD freeClst = 0;
        if (f_getfree("0:", &freeClst, &fs) != FR_OK || !fs) { Serial.println("[SD] drive 0: not found"); return; }
#if FF_MAX_SS != FF_MIN_SS
        const uint32_t ss = fs->ssize;
#else
        const uint32_t ss = FF_MAX_SS;
#endif
        const uint64_t vol = (uint64_t)(fs->n_fatent - 2) * fs->csize * ss;
        const uint64_t card = g_sdStore.totalBytes();
        if (vol > card + (64ULL << 20) || vol + (64ULL << 20) < card) {   // drive 0: must be this card
            Serial.printf("[SD] drive 0: is %llu MB, card %llu MB: not formatting\n", vol >> 20, card >> 20);
            return;
        }
        Serial.printf("[SD] formatting FAT32 with %lu-byte clusters (was %lu); takes a minute...\n", au,
                      (unsigned long)(fs->csize * ss));
        Serial.flush();
        f_mount(nullptr, "0:", 0);
        const UINT workLen = 16384;
        void* work = heap_caps_malloc(workLen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        const FRESULT r = work ? f_mkfs("0:", FM_FAT32, (DWORD)au, work, workLen) : FR_NOT_ENOUGH_CORE;
        if (work) free(work);
        Serial.printf("[SD] format %s (%d); restarting\n", r == FR_OK ? "OK" : "FAILED", (int)r);
        Serial.flush();
        delay(500);
        ESP.restart();
#endif
    } else if (!strcmp(p, "sdinfo")) {
        Serial.printf("[SD] %s (mounting to check)%c", g_sdStatus, 10);
        sdMount();
        Serial.printf("[SD] %s; message archive %s%c", g_sdStatus,
                      g_sdArchiveActive ? "active" : sdArchiveEnabled() ? "on, not active" : "off", 10);
#if !defined(SIMULATOR)
        if (g_sdMounted) {
            SharedSPILock lock;
            FATFS* fs = nullptr;
            DWORD freeClst = 0;
            if (f_getfree("0:", &freeClst, &fs) == FR_OK && fs) {
#if FF_MAX_SS != FF_MIN_SS
                const uint32_t ss = fs->ssize;
#else
                const uint32_t ss = FF_MAX_SS;
#endif
                Serial.printf("[SD] FAT%s cluster %lu bytes, free %llu MB%c", fs->fs_type == FS_FAT32 ? "32" : "",
                              (unsigned long)(fs->csize * ss), ((uint64_t)freeClst * fs->csize * ss) >> 20, 10);
            }
        }
#endif
    } else if (!strncmp(p, "epdoff ", 7)) {
        g_epdOffInSleep = (uint8_t)atoi(p + 7);
        Serial.printf("[EINK] before sleep: %s%c", g_epdOffInSleep == 2 ? "controller deep sleep" : g_epdOffInSleep ? "power off" : "left on", 10);
#if !defined(SIMULATOR)
    } else if (!strcmp(p, "iomux")) {
        // Bench: IO_MUX config of the radio / SPI / rail pins. Bit 1 SLP_SEL = the pin switches
        // to its sleep configuration in light sleep; bits 2/3 = sleep pull-down / pull-up.
        static const int pins[] = {LORA_CS, LORA_RST, LORA_IRQ, LORA_BUSY, BOARD_LORA_EN, SPI_SCK, SPI_MOSI,
                                   SPI_MISO, EPD_CS, SD_CS, BOARD_GPS_EN, KB_INT, TOUCH_INT};
        for (int pin : pins)
            Serial.printf("[IOMUX] gpio %2d = 0x%08lx slp_sel=%lu\n", pin,
                          (unsigned long)REG_READ(GPIO_PIN_MUX_REG[pin]),
                          (unsigned long)((REG_READ(GPIO_PIN_MUX_REG[pin]) >> 1) & 1));
#endif
    } else if (!strncmp(p, "audio ", 6)) {
        // Bench: audio path off (I2S stopped, GPIO 41 rail low) or back on.
        if (atoi(p + 6)) g_audio.resume();
        else             g_audio.pause();
        Serial.printf("[AUDIO] path %s\n", atoi(p + 6) ? "on" : "off (I2S stopped, rail 41 low)");
    } else if (!strncmp(p, "name", 4) && (p[4] == ' ' || p[4] == 0)) {
        saveSelfName(p[4] ? p + 5 : "");
        requestRedraw();
        Serial.printf("[NAME] this device is now %s\n", g_selfName);
    } else if (!strncmp(p, "energy", 6)) {
        // energy | energy period <s> | energy clear
        if (!strncmp(p + 6, " period ", 8)) energy::setPeriod((uint32_t)atoi(p + 14));
        else if (!strncmp(p + 6, " fill ", 6)) {
            const time_t now = time(nullptr);
            energy::benchFill(atoi(p + 12), now > 1600000000 ? (uint32_t)now : 0);
        }
        else if (!strcmp(p + 6, " clear")) energy::clear();
        energyAccountAwake();
        energy::dump();
    } else if (!strcmp(p, "clk")) {
        // Bench: wall clock to the microsecond (RTC drift: compare with host time over hours).
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        Serial.printf("[CLK] %lld.%06ld up %lu ms; clock %s\n", (long long)tv.tv_sec, (long)tv.tv_usec,
                      (unsigned long)millis(),
                      handheld::ClockConfidence::synchronized() ? "synced" : g_clkLastAt ? "from peer" : "own");
    } else if (!strncmp(p, "rmconv", 6) && (p[6] == 0 || p[6] == ' ')) {
        // Bench: "rmconv" lists conversations; "rmconv <peer>" deletes one; "rmconv all" deletes all.
        using namespace handheld::storage;
        constexpr int kRmMax = 10;
        std::vector<std::string> peers;
        if (p[6] == ' ' && strlen(p + 7) == 32) peers.push_back(p + 7);
        else {
            auto sub = g_msgStore.requestConversationPage({}, false, ConversationOrder::Recent,
                                                          ConversationDirection::After, (uint8_t)kRmMax);
            MessageStore::Result res;
            ConversationSelector sel[kRmMax];
            size_t n = 0;
            if (sub.accepted() && waitResult(sub.ticket, res) && res.outcome == Outcome::Committed &&
                res.error == Error::None) {
                n = std::min(res.length / sizeof(ConversationSelector), (size_t)kRmMax);
                if (!g_msgStore.readPayload(sub.ticket, sel, n * sizeof(ConversationSelector))) n = 0;
            }
            if (sub.accepted()) g_msgStore.releaseResult(sub.ticket);
            for (size_t i = 0; i < n; i++) {
                char hex[33];
                hashToHex(sel[i].cursor.peer, hex);
                Serial.printf("[MSG] conversation %s\n", hex);
                peers.push_back(hex);
            }
            if (strcmp(p + 6, " all")) peers.clear();   // plain "rmconv": list only
        }
        for (const auto& peer : peers) {
            auto rq = g_msgStore.requestDelete(peer);
            MessageStore::Result r;
            const bool done = rq.accepted() && waitResult(rq.ticket, r, 6000);   // up to 30 s
            if (rq.accepted()) g_msgStore.releaseResult(rq.ticket);
            Serial.printf("[MSG] rmconv %s: %s\n", peer.c_str(),
                          done && r.outcome == Outcome::Committed ? "deleted" : "failed");
        }
        requestRedraw();
    } else if (!strcmp(p, "chess") || !strncmp(p, "chess ", 6)) {
        const char* a = p[5] ? p + 6 : "";
        if (!g_cMoves && !chessLoad()) { Serial.println("[CHESS] no memory"); return; }
        if (!strcmp(a, "new")) chessNew();
        else if (!strncmp(a, "level ", 6)) { const int l = atoi(a + 6); if (l >= 1 && l <= 4) { g_cLevel = (uint8_t)l; chessSave(); } }
        else if (!strncmp(a, "move ", 5) && strlen(a) >= 9) {
            // "chess move e2e4" (q/r/b/n for a promotion): as if picked with the cursor.
            const int from = (a[6] - '1') * 8 + (a[5] - 'a'), to = (a[8] - '1') * 8 + (a[7] - 'a');
            const char pr = a[9];
            chess::Move legal[chess::kMaxMoves];
            const int n = chess::legalMoves(g_cPos, legal);
            int hit = -1;
            for (int i = 0; i < n; i++)
                if (legal[i].from == from && legal[i].to == to &&
                    (!legal[i].promo || legal[i].promo == (pr == 'n' ? chess::KNIGHT : pr == 'r' ? chess::ROOK : pr == 'b' ? chess::BISHOP : chess::QUEEN)))
                    hit = i;
            if (hit < 0 || g_cThinking || g_cPos.side != g_cUser) Serial.println("[CHESS] move refused");
            else { chessPlay(legal[hit]); chessThink(); }
        }
        char fen[96];
        chess::toFen(g_cPos, fen, sizeof(fen));
        Serial.printf("[CHESS] %s | moves %d, you %s, level %u, %s, result %u\n", fen, g_cN, g_cUser > 0 ? "white" : "black",
                      (unsigned)g_cLevel, g_cThinking ? "thinking" : "idle", (unsigned)g_cResult);
    } else if (!strcmp(p, "boottime")) {
        for (int i = 0; i < g_bootMarkN; i++)
            Serial.printf("[BOOTTIME] %6lu ms %s\n", (unsigned long)g_bootMarks[i].ms, g_bootMarks[i].phase);
    } else if (!strcmp(p, "backup")) {
        backupNow();
    } else if (!strncmp(p, "restore ", 8)) {
        // "restore <folder name in /backups> | newest": as S.10.5 > Restore backup, without
        // asking twice.
        sdMount();
        sdToolsScan(true);
        int i = 0;
        if (strcmp(p + 8, "newest")) while (i < g_bkCount && strcmp(g_bkNames[i], p + 8)) i++;
        if (i == g_bkCount) Serial.printf("[RESTORE] no backup %s\n", p + 8);
        else restoreRequest(i);
    } else if (!strcmp(p, "sdtools")) {
        sdMount();
        sdToolsScan(true);
        Serial.printf("[SDTOOLS] slot %s%s, firmware file %s (%lu bytes)%s, %d backups:", sysimg::runningSlot(),
                      sysimg::pendingVerify() ? " (unconfirmed)" : "", g_fwName[0] ? g_fwName : "-",
                      (unsigned long)g_fwSize, g_fwInstalled ? " installed" : "", g_bkCount);
        for (int i = 0; i < g_bkCount; i++) Serial.printf(" %s", g_bkNames[i]);
        Serial.println();
    } else if (!strcmp(p, "fwupdate")) {
        // As S.10.5 > Update firmware, without asking twice.
        sdMount();
        sdToolsScan(true);
        fwUpdate();
    } else if (!strcmp(p, "usbdrive")) {
        // The SD card becomes a USB drive on the PC (as S.10.5 row 6); this COM port goes away.
        usbDriveStart();
        if (!g_usbDrive) Serial.printf("[USB] drive mode not started: %s\n", usbdrive::message());
#if defined(SIMULATOR)
    } else if (!strcmp(p, "usbeject")) {
        usbdrive::simEject();
#endif
    } else if (!strcmp(p, "hb") || !strncmp(p, "hb ", 3)) {
        // Stress-test heartbeat: "hb" prints one line, "hb <s>" every s seconds, "hb 0" off.
        if (p[2]) g_hbEveryMs = (uint32_t)atol(p + 3) * 1000;
        heartbeatPrint();
    } else if (!strcmp(p, "msgidx") || !strcmp(p, "msgidx check")) {
        // Record index: status; "check" also compares it with every record on flash.
        char status[128];
        g_msgStore.indexStatus(status, sizeof(status));
        if (!strcmp(p, "msgidx")) { Serial.printf("[IDX] %s\n", status); return; }
        uint32_t checked = 0, mismatches = 0;
        bool ok = false;
        for (int i = 0; i < 50 && !ok; i++) {   // the storage task may hold the lease briefly
            ok = g_msgStore.indexVerify(checked, mismatches);
            if (!ok) { pumpOnce(); delay(20); }
        }
        Serial.printf("[IDX] %s check=%s records=%lu mismatch=%lu\n", status, ok ? "ok" : "unavailable",
                      (unsigned long)checked, (unsigned long)mismatches);
    } else if (!strncmp(p, "msgstat ", 8) && strlen(p) >= 8 + 32) {
        // Stress test: "msgstat <peer>" counts that conversation's records by direction and
        // status, and checks numbered test messages ("PP <tag> <n>") received from the peer:
        // how many, duplicates, gaps between the lowest and highest number.
        using namespace handheld::storage;
        const std::string peerHex(p + 8, 32);
        uint8_t peer[16];
        for (int b = 0; b < 16; b++) { unsigned v = 0; sscanf(p + 8 + 2 * b, "%2x", &v); peer[b] = (uint8_t)v; }
        uint32_t in = 0, out = 0, st[8] = {}, bad = 0;   // bad = listed but unreadable
        std::vector<uint32_t> nums;
        HistoryEntry cursor{};
        bool more = true, ok = true;
        while (more && ok) {
            auto sub = g_msgStore.requestHistoryPage(peerHex, cursor, 48, HistoryDirection::Before);
            MessageStore::Result res;
            HistoryEntry entries[48];
            size_t n = 0;
            ok = sub.accepted() && waitResult(sub.ticket, res, 2000) && res.outcome == Outcome::Committed &&
                 res.error == Error::None;
            if (ok) {
                n = std::min(res.length / sizeof(HistoryEntry), (size_t)48);
                if (!g_msgStore.readPayload(sub.ticket, entries, n * sizeof(HistoryEntry))) n = 0;
                more = res.more && n > 0;
            }
            if (sub.accepted()) g_msgStore.releaseResult(sub.ticket);
            if (!ok || !n) break;
            cursor = entries[0];
            for (size_t i = 0; i < n; i++) {
                RecordKey key{};
                memcpy(key.peer, peer, 16); key.counter = entries[i].counter; key.incoming = entries[i].incoming;
                auto rs = g_msgStore.requestRecord(key, 0, 160);
                MessageStore::Result rr;
                uint8_t buf[160];
                size_t got = 0;
                if (rs.accepted() && waitResult(rs.ticket, rr, 2000) && rr.outcome == Outcome::Committed &&
                    rr.error == Error::None) {
                    got = std::min((size_t)rr.length, sizeof(buf));
                    if (!g_msgStore.readPayload(rs.ticket, buf, got)) got = 0;
                }
                if (rs.accepted()) g_msgStore.releaseResult(rs.ticket);
                if (got < sizeof(StoredRecordHeader)) { bad++; continue; }
                StoredRecordHeader hdr;
                memcpy(&hdr, buf, sizeof(hdr));
                if (!hdr.incoming) { out++; st[hdr.status & 7]++; continue; }
                in++;
                size_t off = sizeof(hdr) + hdr.titleLength, len = hdr.contentLength;
                if (off >= got) continue;
                if (off + len > got) len = got - off;
                char text[48] = {};
                memcpy(text, buf + off, std::min(len, sizeof(text) - 1));
                char tag[16]; unsigned num = 0;
                if (sscanf(text, "PP %15s %u", tag, &num) == 2) nums.push_back(num);
            }
        }
        std::sort(nums.begin(), nums.end());
        uint32_t dups = 0, gaps = 0;
        for (size_t i = 1; i < nums.size(); i++) {
            if (nums[i] == nums[i - 1]) dups++;
            else if (nums[i] > nums[i - 1] + 1) gaps += nums[i] - nums[i - 1] - 1;
        }
        Serial.printf("[STAT] %s %s in=%lu out=%lu queued=%lu sending=%lu sent=%lu delivered=%lu failed=%lu "
                      "noproof=%lu bad=%lu pp=%u ppmin=%lu ppmax=%lu dup=%lu gap=%lu\n", peerHex.c_str(), ok ? "ok" : "ERROR",
                      (unsigned long)in, (unsigned long)out, (unsigned long)(st[0] + st[1]), (unsigned long)st[2],
                      (unsigned long)st[3], (unsigned long)st[4], (unsigned long)st[5], (unsigned long)st[6], (unsigned long)bad,
                      (unsigned)nums.size(), (unsigned long)(nums.empty() ? 0 : nums.front()),
                      (unsigned long)(nums.empty() ? 0 : nums.back()), (unsigned long)dups, (unsigned long)gaps);
    } else if (!strcmp(p, "flash")) {
        if (handheld::storage::MessageTransactions::spaceStats().capBytes)
            Serial.printf("[FLASH] stress-test cap %lu KB (flashcap)\n",
                          (unsigned long)(handheld::storage::MessageTransactions::spaceStats().capBytes >> 10));
        Serial.printf("[FLASH] used %u of %u KB (%u%%)\n", (unsigned)(g_flash.usedBytes() >> 10),
                      (unsigned)(g_flash.totalBytes() >> 10),
                      g_flash.totalBytes() ? (unsigned)(100ull * g_flash.usedBytes() / g_flash.totalBytes()) : 0u);
        const auto& sp = handheld::storage::MessageTransactions::spaceStats();
        Serial.printf("[FLASH] messages: %s; %lu oldest removed for space in %lu passes (from %u%% to %u%%, refuse at %u%%)\n",
                      sp.full ? "FULL, new messages refused" : "ok", (unsigned long)sp.removed,
                      (unsigned long)sp.passes, (unsigned)handheld::storage::MessageTransactions::SpaceHighPct,
                      (unsigned)handheld::storage::MessageTransactions::SpaceLowPct,
                      (unsigned)handheld::storage::MessageTransactions::SpaceRefusePct);
    } else if (!strncmp(p, "flashcap", 8) && (p[8] == 0 || p[8] == ' ')) {
        // Bench / stress test (RAM only, cleared by a restart): "flashcap <n>" makes the message
        // upkeep start removing after about <n> more messages (treats the flash as smaller);
        // "flashcap 0" = real size. Other files still use the whole flash.
        auto& sp = handheld::storage::MessageTransactions::spaceStats();
        const long n = p[8] ? atol(p + 9) : -1;
        if (n == 0) sp.capBytes = 0;
        else if (n > 0) {
            const uint64_t used = g_flash.usedBytes();
            sp.capBytes = (used + (uint64_t)n * 4096) * 100 / handheld::storage::MessageTransactions::SpaceHighPct;
        }
        sp.checkEvery = 0; sp.sinceCheck = 0;   // re-read at the next save
        Serial.printf("[FLASH] message cap %s%lu KB (real %u KB)\n", sp.capBytes ? "" : "off, ",
                      (unsigned long)(sp.capBytes >> 10), (unsigned)(g_flash.totalBytes() >> 10));
    } else if (!strncmp(p, "msgfill ", 8)) {
        // Bench (full-flash test): "msgfill <count> [peers] [bytes]" stores <count> fake incoming
        // messages spread over [peers] fake conversations through the normal save path. Stops at
        // the first failed save and prints flash use.
        using namespace handheld::storage;
        int count = 0, peers = 4, bytes = 200;
        sscanf(p + 8, "%d %d %d", &count, &peers, &bytes);
        peers = peers < 1 ? 1 : (peers > 200 ? 200 : peers);
        bytes = bytes < 1 ? 1 : (bytes > 3000 ? 3000 : bytes);
        std::string body(bytes, 'x');
        int saved = 0, failed = 0;
        const unsigned long t0 = millis();
        for (int i = 0; i < count; i++) {
            Request rq; rq.operation = Operation::CreateIncoming; rq.key.incoming = true;
            memset(rq.key.peer, 0xF0, 16); rq.key.peer[15] = (uint8_t)(i % peers);
            memcpy(rq.source, rq.key.peer, 16); for (int b = 0; b < 16; b++) { unsigned v = 0; sscanf(g_destHex + 2 * b, "%2x", &v); rq.destination[b] = (uint8_t)v; }
            for (int b = 0; b < 32; b++) rq.messageId[b] = (uint8_t)esp_random();
            rq.hasMessageId = true; rq.timestamp = (double)time(nullptr) + i * 0.001;
            rq.status = (uint8_t)LXMFStatus::DELIVERED; rq.read = true;
            rq.contentLength = (uint16_t)bytes;
            auto sub = g_msgStore.requestSave(rq, nullptr, body.data());
            MessageStore::Result r;
            const bool done = sub.accepted() && waitResult(sub.ticket, r, 2000);
            if (sub.accepted()) g_msgStore.releaseResult(sub.ticket);
            if (done && r.outcome == Outcome::Committed && r.error == Error::None) { saved++; continue; }
            failed++;
            Serial.printf("[FILL] save %d failed: %s, outcome %d, error %d\n", i + 1,
                          !sub.accepted() ? "not accepted" : !done ? "timeout" : "store",
                          done ? (int)r.outcome : -1, done ? (int)r.error : -1);
            break;
        }
        Serial.printf("[FILL] saved %d, failed %d in %lu ms; flash %u of %u KB\n", saved, failed,
                      millis() - t0, (unsigned)(g_flash.usedBytes() >> 10), (unsigned)(g_flash.totalBytes() >> 10));
        requestRedraw();
    } else if (!strncmp(p, "contact ", 8) && strlen(p) > 8 + 32 + 1) {
        // Bench: contact <32 hex dest> <name>  - save a contact (sim list / paging tests)
        uint8_t h[16];
        bool ok = true;
        for (int i = 0; i < 16 && ok; i++) {
            const int a = hexNib(p[8 + 2 * i]), b = hexNib(p[9 + 2 * i]);
            ok = a >= 0 && b >= 0;
            h[i] = (uint8_t)((a << 4) | b);
        }
        if (ok) { saveContactName(h, p + 8 + 32 + 1); if (isListScreen(g_screen)) loadRows(g_screen); requestRedraw(); }
        else Serial.println("[CMD] contact <32 hex dest> <name>");
    } else if (!strcmp(p, "ids")) {
        const auto& v = g_idMgr.identities();
        for (size_t i = 0; i < v.size(); i++)
            Serial.printf("[ID] slot %u %s %s%s\n", (unsigned)i, v[i].hash.c_str(), v[i].displayName.c_str(),
                          v[i].active ? " (active)" : "");
        Serial.printf("[ID] %u identities, tcp %s\n", (unsigned)v.size(), tcpStatus());
    } else if (!strcmp(p, "id")) {
        Serial.printf("[ID] dest=%s name=%s\n", g_destHex, g_selfName);
    } else if (!strncmp(p, "txp", 3) && (p[3] == ' ' || p[3] == 0)) {
        if (p[3]) {
            int v = atoi(p + 4);
            v = v < -9 ? -9 : (v > 22 ? 22 : v);
            Preferences pr;
            if (pr.begin("rs_radio", false)) { pr.putChar("txp", (int8_t)v); pr.end(); }
            if (g_radio) g_radio->setTxPower(v);
        }
        // "(saved)" only when NVS holds a value: a build default (22, or -9 in a bench
        // build) changes with the firmware that is flashed.
        Preferences pr;
        bool saved = false;
        if (pr.begin("rs_radio", true)) { saved = pr.isKey("txp"); pr.end(); }
        Serial.printf("[LORA] tx power %d dBm (%s)\n", loraTxPower(),
                      saved ? "saved" : "build default - set with txp <dBm>");
    } else if (!strcmp(p, "tcxo") && g_radio) {
        // Bench: TCXO cold-start check at each delay (bit 0x20 = XOSC start error).
        static const uint32_t us[] = {5000, 10000, 20000, 40000, 640000};
        for (uint32_t u : us)
            Serial.printf("[LORA] tcxo %lu ms: errors 0x%04X%c", (unsigned long)(u / 1000), g_radio->tcxoProbe(u), 10);
        if (g_loraIf && g_loraIf->isOnline()) g_radio->receive();
    } else if (!strncmp(p, "dctcxo", 6) && g_radio) {
        // Bench: dctcxo <ms> = lowest TCXO delay the RX duty cycle may use (0 = auto); not saved.
        if (p[6]) {
            g_radio->setDcTcxoMinUs((uint32_t)atoi(p + 6) * 1000UL);
            if (g_loraIf && g_loraIf->isOnline()) g_radio->receive();
        }
        Serial.printf("[LORA] duty-cycle tcxo %lu ms (%s)%c", (unsigned long)(g_radio->rxDcTcxoUs() / 1000),
                      g_radio->rxDutyCycleActive() ? "active" : "not active", 10);
    } else if (!strncmp(p, "fe", 2) && (p[2] == ' ' || p[2] == 0) && g_radio) {
        // Bench: fe 1 = log each received frame's frequency error; not saved.
        if (p[2]) g_radio->logFreqErr = atoi(p + 3) != 0;
        Serial.printf("[LORA] freq error log %s (last %ld Hz)%c", g_radio->logFreqErr ? "on" : "off", (long)g_radio->lastFreqErrHz, 10);
    } else if (!strncmp(p, "rxdc", 4) && (p[4] == ' ' || p[4] == 0)) {
        // rxdc | rxdc 0 (continuous) | rxdc <2..8> (preamble symbols per listen window)
        if (p[4]) {
            int v = atoi(p + 5);
            g_pw.rxDc = (uint8_t)(v <= 0 ? 0 : (v < 2 ? 2 : (v > 8 ? 8 : v)));
            power::save(g_pw);
            powerApply(true);
        }
        Serial.printf("[LORA] rx duty cycle setting %u (%s now)\n", (unsigned)g_pw.rxDc,
                      g_radio && g_radio->rxDutyCycleActive() ? "active" : "not active");
    } else if (!strncmp(p, "usbsleep", 8)) {
        const int secs = p[8] ? atoi(p + 8) : 600;
        g_usbSleepUntil = secs > 0 ? millis() + (uint32_t)(secs > 7200 ? 7200 : secs) * 1000UL : 0;
        Serial.printf("[POWER] light sleep with USB host %s for %d s (console drops while asleep)\n",
                      secs > 0 ? "allowed" : "blocked", secs);
    } else if (!strcmp(p, "ev") || !strcmp(p, "ev 0")) {
        // Bench: RAM event trace (radio rx/tx, wakes, inbound messages); "ev 0" clears.
        if (p[2]) { evtrace::clear(); Serial.println("[EV] cleared"); }
        else {
            const auto& r = evtrace::ring();
            const uint32_t n = r.total < evtrace::Capacity ? r.total : evtrace::Capacity;
            uint32_t prev = 0;
            for (uint32_t i = r.total - n; i < r.total; ++i) {
                const auto& e = r.events[i % evtrace::Capacity];
                Serial.printf("[EV] %8lu ms  +%6lu  %-10s %ld%c", (unsigned long)e.ms,
                              (unsigned long)(prev ? e.ms - prev : 0), e.tag, (long)e.value, 10);
                prev = e.ms;
            }
            Serial.printf("[EV] %lu marks (%lu shown), now %lu ms%c", (unsigned long)r.total, (unsigned long)n,
                          (unsigned long)millis(), 10);
        }
    } else if (!strcmp(p, "rl")) {
        rtclog::dump();
    } else if (!strcmp(p, "rl 0")) {
        rtclog::clear();
        Serial.println("[RTCLOG] cleared");
    } else if (!strncmp(p, "blog", 4)) {
        g_batLogInternal = atoi(p + 4) != 0;
        if (g_batLogInternal) batLogSample();
        Serial.printf("[BATLOG] internal log %s (%u samples)\n", g_batLogInternal ? "on" : "off", (unsigned)g_batLogCount);
    } else if (!strcmp(p, "L")) {
        g_cfg.kbLight = !g_cfg.kbLight;
        applyKeyLight();
        requestRedraw();
    } else if (!strcmp(p, "FORMAT")) {
        Serial.println(recoverCorruptLittleFS() ? "[FLASH] formatted - reboot now" : "[FLASH] format failed");
    } else {
        Serial.println("[CMD] t:<msg> a h i c p s d g gp gr pw pm<0-3> pset bench<s> blog<0|1> usbsleep<s> rxdc<0|2-8> dctcxo<ms> fe<0|1> ev txp<dBm> msg id name energy rl sleeptest L FORMAT");
    }
}

void pollSerial() {
    static char line[180];
    static size_t len = 0;
    if (sdputPoll()) return;
    while (Serial.available()) {
        if (g_putActive) { if (sdputPoll()) return; continue; }
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (!len) continue;
            line[len] = 0;
            len = 0;
            handleSerialLine(line);
        } else if (c >= 32 && c < 127 && len + 1 < sizeof(line)) {
            line[len++] = c;
        }
    }
}

// ---- Background watchers -------------------------------------------------------------
// ---- Battery: measured time left, low-battery notice --------------------------------------
// Time left: the gauge's remaining mAh over the drain the energy monitor measured on battery
// (the period in progress + up to 12 closed ones, normally 1 h each); needs 30 min of data.
int batHoursLeft10(int* dataMin) {
    uint32_t sec = 0, mah10 = 0;
    const energy::Record cur = energy::current(millis(), g_bat.valid ? g_bat.pct : -1);
    sec += cur.measuredS; mah10 += cur.measured10;
    for (int i = 0; i < energy::count() && i < 12; i++) {
        const energy::Record& r = energy::closed(i);
        sec += r.measuredS; mah10 += r.measured10;
    }
    if (dataMin) *dataMin = (int)(sec / 60);
    if (sec < 1800 || !mah10 || !g_bat.valid) return -1;
    const float ma = (mah10 / 10.0f) * 3600.0f / sec;
    if (ma < 0.5f) return -1;
    const float rem = g_capMah ? g_capMah * g_bat.pct / 100.0f
                    : g_bat.remMah ? (float)g_bat.remMah : kNominalMah * g_bat.pct / 100.0f;
    return (int)(rem / ma * 10.0f);
}

void batLeftText(char* out, size_t n) {
    int mins = 0;
    const int h10 = batHoursLeft10(&mins);
    if (g_bat.usb) snprintf(out, n, "on USB power");
    else if (h10 < 0) snprintf(out, n, "learning: %d of 30 min", mins);
    else if (h10 < 480) snprintf(out, n, "about %d h in %s", (h10 + 5) / 10, power::modeName(g_pw.mode));
    else snprintf(out, n, "about %.1f days in %s", h10 / 240.0f, power::modeName(g_pw.mode));
}

// Low-battery notice (S.5 row 12): once per discharge, at the chosen %, one contact gets
// "Battery dying (9%). Last position ..."; check-in mode queues it for the window.
void lowBatTick() {
    if (!g_bat.valid) return;
    if (g_cfg.lowBatSent && (g_bat.usb || g_bat.charging || g_bat.pct >= g_cfg.lowBatPct + 5)) {
        g_cfg.lowBatSent = false;                    // charged: armed again
        saveConfig();
        return;
    }
    if (!g_cfg.lowBatPct || g_cfg.lowBatSent || g_bat.usb || g_bat.charging || g_bat.pct > g_cfg.lowBatPct) return;
    static const uint8_t zero[16] = {0};
    if (!memcmp(g_cfg.lowBatTo, zero, 16)) return;
    char body[120];
    double la, lo;
    if (g_gpsPosValid) {
        const uint32_t age = (millis() - g_gpsPosAt) / 60000;
        snprintf(body, sizeof(body), "Battery dying (%d%%). Last position %.5f, %.5f (%lu min old)", g_bat.pct,
                 g_gpsLat, g_gpsLon, (unsigned long)age);
    } else if (ownPosition(la, lo)) {
        snprintf(body, sizeof(body), "Battery dying (%d%%). Saved position %.5f, %.5f (no GPS fix)", g_bat.pct, la, lo);
    } else {
        snprintf(body, sizeof(body), "Battery dying (%d%%). No position known.", g_bat.pct);
    }
    const power::CheckinPhase ph = checkinNow();
    const bool queue = g_pwEff.checkin && !(ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS);
    bool ok;
    if (queue) ok = outboxAdd(g_cfg.lowBatTo, body);
    else ok = sendOrQueue(g_cfg.lowBatTo, body) != SendResult::Failed;
    char nm[24];
    peerName(g_cfg.lowBatTo, nm, sizeof(nm));
    Serial.printf("[LOWBAT] %s to %s: %s\n", ok ? (queue ? "queued" : "sent") : "FAILED", nm, body);
    if (ok) {
        g_cfg.lowBatSent = true;
        saveConfig();
        toast("Low battery: told %.14s", nm);
    }
}

void watchBattery() {
    static uint32_t last = 0;
    if (millis() - last < 800) return;
    last = millis();
    const bool chg = g_bat.charging, usb = g_bat.usb;
    const int pct = g_bat.pct;
    const bool wasValid = g_bat.valid;
    readBattery();
    capTick();
    lowBatTick();
    if (g_idleActive) return;
    const bool changed = chg != g_bat.charging || usb != g_bat.usb || wasValid != g_bat.valid ||
                         (g_screen == SCR_HOME && pct != g_bat.pct);
    if (changed && browseMode()) requestRedraw();
}

void watchMessages() {
    if (!g_storeReady) return;
    // Alert on newly saved incoming messages (not on a rising unread count: a save that also
    // removed old unread messages for space leaves the count flat and the alert was missed).
    static uint32_t lastIncoming = 0;
    static uint32_t lastRev = 0, lastRedraw = 0;
    const uint32_t incoming = g_msgStore.incomingCommitted();
    if (incoming != lastIncoming) { lastIncoming = incoming; notifyInbound(); }
    static int lastUnread = -1;
    const int unread = g_msgStore.totalUnreadCount();
    if (g_locked && !g_idleActive && unread != lastUnread) requestRedraw();   // lock prompt count
    lastUnread = unread;

    const uint32_t rev = g_msgStore.revision();
    if (rev == lastRev || g_idleActive) return;
    if (millis() - lastRedraw < 2500) return;
    if (g_threadOpen) {
        if (g_bufLen) return;            // never clobber a draft; retry later
        loadHistory(g_threadPeer);
    } else if (g_screen == SCR_INBOX && browseMode()) {
        loadRows(SCR_INBOX);
    } else if (g_screen != SCR_HOME) {
        lastRev = rev;                   // nothing on screen depends on it
        return;
    }
    lastRev = rev;
    lastRedraw = millis();
    requestRedraw();
}

void watchPeers() {
    static size_t lastCount = 0;
    static uint32_t lastMs = 0;
    const size_t n = g_announceMgr.nodes().size();
    if (n == lastCount || g_idleActive) return;
    if (millis() - lastMs < 30000UL && lastMs) return;
    lastCount = n;
    lastMs = millis();
    if (!browseMode()) return;
    if (g_screen == SCR_PEERS || g_screen == SCR_INBOX || g_screen == SCR_CONTACTS) {
        loadRows(g_screen);
        requestRedraw();
    } else if (g_screen == SCR_HOME) {
        requestRedraw();
    }
}

void watchClockAndToast() {
    static uint32_t lastMin = 0;
    const uint32_t m = millis() / 60000UL;
    if (m != lastMin) {
        lastMin = m;
        if (!g_idleActive && !g_bufLen) requestRedraw();   // top-bar clock
    }
    if (g_toast[0] && (int32_t)(millis() - g_toastUntil) >= 0) {
        g_toast[0] = 0;
        if (!g_idleActive) requestRedraw();
    }
}

void watchIdle() {
    if (g_idleActive) { updateIdle(); return; }
    if (g_locked) {                                    // prompt left alone -> splash
        if (millis() - g_lastInputMs >= kLockPromptMs) { lockWipe(); enterIdle(); }
        return;
    }
    if (g_cfg.idleMode == 0) return;
    const bool lockIt = lockOnIdle();                  // the draft stays in its buffer
    if (g_bufLen && !lockIt) return;                   // never hide an unsent draft
    if (millis() - g_lastInputMs >= g_cfg.idleSecs * 1000UL) {
        if (lockIt) lockNow(); else enterIdle();
    }
}

}  // namespace

// =============================================================================
// 8. begin() / loop()
// =============================================================================

// Boot phase timing: "[BOOT] <ms since power-on> <phase>" (unit boot is ~15 s with an empty store).
void bootMark(const char* phase) {
    if (g_bootMarkN < 16) g_bootMarks[g_bootMarkN++] = {millis(), phase};
    Serial.printf("[BOOT] %6lu ms %s\n", (unsigned long)millis(), phase);
}

// The vault key (PBKDF2, ~2 s) is derived on core 0 while core 1 starts the radio, flash
// and protocol; vaultWait() joins it before anything uses the vault.
SemaphoreHandle_t g_vaultDone = nullptr;
void vaultTask(void*) {
    vault::begin();
    xSemaphoreGive(g_vaultDone);
    vTaskDelete(nullptr);
}
void vaultStart() {
    g_vaultDone = xSemaphoreCreateBinary();
    // Priority 0 (idle's): shares core 0 with the idle task, so its watchdog stays fed.
    if (!g_vaultDone || xTaskCreatePinnedToCore(vaultTask, "vault", 8192, nullptr, 0, nullptr, 0) != pdPASS) {
        if (g_vaultDone) vSemaphoreDelete(g_vaultDone);
        g_vaultDone = nullptr;
        vault::begin();
    }
}
void vaultWait() {
    if (!g_vaultDone) return;
    const uint32_t t0 = millis();
    xSemaphoreTake(g_vaultDone, portMAX_DELAY);
    vSemaphoreDelete(g_vaultDone);
    g_vaultDone = nullptr;
    Serial.printf("[VAULT] ready (waited %lu ms)\n", (unsigned long)(millis() - t0));
}

bool begin() {
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RatSpeak T-Deck Pro - standalone");
    Serial.println("========================================");
    bootMark("start");
    sysimg::bootCheck();
    loadConfig();
    outboxLoad();
    power::load(g_pw);
    applyTz();
    ensureClockSeeded();
    loadSelfName();

    handheld::bindDeviceOwner();
    vaultStart();

#if !defined(SIMULATOR)
    // A pad hold left by other firmware (e.g. a deep-sleep test) survives resets on the RTC
    // pins and would pin LoRa CS / RST low: release holds on every pin we drive.
    {
        static const int kPins[] = {LORA_CS, LORA_RST, LORA_IRQ, LORA_BUSY, BOARD_LORA_EN, BOARD_GPS_EN, GPS_TX,
                                    GPS_RX, BOARD_6609_EN, BOARD_1V8_EN, BOARD_EPD_BACKLIGHT, BOARD_KEYBOARD_LED,
                                    BOARD_MOTOR_PIN, EPD_CS, SD_CS};
        for (int p : kPins) gpio_hold_dis((gpio_num_t)p);
        gpio_deep_sleep_hold_dis();
    }
#endif
    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
#if defined(RS_NO_RADIO) && RS_NO_RADIO
    // Bench build (antenna off): the LoRa chip stays unpowered, so it can never transmit.
    digitalWrite(BOARD_LORA_EN, LOW);
#else
    digitalWrite(BOARD_LORA_EN, HIGH);
#endif
    delay(30);

    pinMode(BOARD_EPD_BACKLIGHT, OUTPUT);
    digitalWrite(BOARD_EPD_BACKLIGHT, LOW);   // front light off
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);  delay(10);
    digitalWrite(TOUCH_RST, HIGH); delay(50);
    pinMode(TOUCH_INT, INPUT_PULLUP);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);
    // A reset during a bench run would leave the charger input off (EN_HIZ survives an
    // ESP32 reset, and the charger's watchdog does not clear it): always switch it back on.
    chargerSetHiz(false);
#if !defined(SIMULATOR)
    Serial.printf("[BOOT] reset reason %d\n", (int)esp_reset_reason());
    rtclog::boot((int)esp_reset_reason());
    energy::begin();
#else
    energy::begin();
    rtclog::boot(0);
#endif
    spiBusIdle();
    logPsram();
    bootMark("config, power, I2C");

    // Boot screen first, so the unit shows it is starting (~1 s after power-on). The radio
    // shares this SPI bus; its own SPI.begin() later is then a no-op.
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    if (!g_display.begin()) { Serial.println("[BOOT] display failed"); return false; }
    if (!LvglPort::begin(g_display)) { Serial.println("[BOOT] LVGL failed"); return false; }
    drawBootSplash();
    bootMark("display, splash");

#if defined(RS_NO_RADIO) && RS_NO_RADIO
    Serial.println("[BOOT] radio DISABLED (RS_NO_RADIO build) - LoRa powered off, no transmit");
#else
    if (!initRadio()) Serial.println("[BOOT] radio failed");
#endif
    bootMark("radio");
    restoreAtBoot();                    // a restore asked for before the restart (restarts again)
    initStorageAndProto();
    bootMark("flash, messages, protocol");

    // Finish LXMF outbound recovery while internal heap is still large: pump until the
    // engine has read the persisted outgoing records (was a fixed 3 s on every boot).
    if (g_protoReady) {
        const uint32_t t0 = millis();
        for (int i = 0; i < 300 && (i < 20 || g_proto.lxmfRecovering()); i++) { pumpOnce(); delay(10); }
        Serial.printf("[BOOT] outgoing recovery %s after %lu ms\n", g_proto.lxmfRecovering() ? "still running" : "done",
                      (unsigned long)(millis() - t0));
    }

    spiBusIdle();
    g_display.setBusyCallback(onPanelBusy);

    g_kbOk = g_kb.begin();
    applyKeyLight();
    keyboardSetNumLayer(false);

    g_audio.begin();
    g_audio.setEnabled(g_cfg.beep);
    g_audio.setVolume(g_cfg.volume);
#if defined(BOARD_MOTOR_PIN) && (BOARD_MOTOR_PIN >= 0)
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    digitalWrite(BOARD_MOTOR_PIN, LOW);
#endif

    Wire.beginTransmission(TOUCH_I2C_ADDR);
    g_touchOk = Wire.endTransmission() == 0;
    Serial.printf("[TOUCH] CST328 %s\n", g_touchOk ? "ok" : "absent");

    bootMark("keyboard, audio, touch");
    pinMode(KB_INT, INPUT_PULLUP);
    loadBatteryCal();
    capLoad();
    readBattery();
    powerApply(true);                     // CPU clock, LoRa, WiFi, key light for the mode
    powerGpsTick();                       // decides whether the GPS starts powered
    gpsApplyPower();
    vaultWait();                          // custom password -> stays locked until typed
    lockLoadFails();
    if (lockHasPassword()) { g_locked = true; Serial.println("[LOCK] boot locked (password set)"); }
    if (vault::unlocked()) wifiLoad();    // else after unlock (lockTry)
    tcpLoad();
    ashareLoad();
    cposLoad();
    wptLoad();
    geofLoad();
    bootMark("battery, power mode, GPS, vault, settings");
    mapview::setCoordFormatter(fmtCoord);
    g_proto.setMessageCallback(onCommittedMessage);
    g_wifiNextAuto = millis() + 8000;    // auto-connect after the boot announce
    noteInput();
    goScreen(SCR_HOME);
    render();
    Serial.println("[BOOT] ready");
    restoreReport();
    if (sysimg::pendingVerify()) g_markValidAt = millis() + 10000;   // new firmware: confirm if it keeps running
    if (g_wizNeeded || (g_cfgFresh && !g_hadIdentity)) wizStart();   // a new device (or unfinished)
    return true;
}

void loop() {
    processKeys();          // input first: lowest latency to the next render
    pumpOnce();
    g_announceMgr.loop();
    g_audio.loop();
    gpsPoll();
    gpsWatchLink();

    static uint32_t bootAnnounceAt = millis() + 3000;
    if (bootAnnounceAt && (int32_t)(millis() - bootAnnounceAt) >= 0) {
        bootAnnounceAt = 0;
        doAnnounce();
        if (g_screen == SCR_HOME && !g_idleActive) requestRedraw();
    }

    watchBattery();
    batLogTick();
    saveEpochThrottled();
    watchMessages();
    watchPeers();
    watchClockAndToast();
    watchWifi();
    tcpTick();
    autoShareTick();
    inboundTick();
    trackTick();
    navTick();
    rtTick();

    processKeys();         // again, in case a watcher above took a while
    pollTouch();
    pollSerial();
    watchIdle();
    powerTick();

    if (!g_idleActive) {
        const bool debounced = g_redrawAt && (int32_t)(millis() - g_redrawAt) >= 0;
        if (g_redraw || debounced) {
            processKeys();  // fold any just-arrived keys into this frame
            render();
        }
    }
    mirrorTick();
    powerIdle();
}

}  // namespace standalone
