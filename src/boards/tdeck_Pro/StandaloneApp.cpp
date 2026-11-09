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
#include <string>
#include <sys/time.h>
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
#include "transport/WiFiInterface.h"
#include <WiFi.h>
#include "protocol/ProtocolRuntime.h"
#include "protocol/OutgoingContract.h"
#include "runtime/TaskOwner.h"
#include "ratspeak_protocol.h"
#include "util/Bytes.h"

// hal/Keyboard.cpp — app-driven number layer for numeric fields
extern void keyboardSetNumLayer(bool on);

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
AnnounceManager g_announceMgr;
ProtocolRuntime g_proto;

bool g_kbOk       = false;
bool g_touchOk    = false;
bool g_storeReady = false;
bool g_protoReady = false;

SX1262*        g_radio  = nullptr;
LoRaInterface* g_loraIf = nullptr;

constexpr const char* kDefaultSelfName = "MyRatSpeakPro";

struct Config {
    bool     dark        = false;   // false = white paper, true = black ink
    int      tz          = 0;       // index into kTz
    uint8_t  idleMode    = 1;       // 0 off, 1 on (no password)
    uint32_t idleSecs    = 30;      // 10..99999
    bool     beep        = true;
    bool     vibe        = true;
    uint8_t  volume      = 80;
    bool     kbLight     = false;
    bool     gpsOn       = true;
    bool     gpsAutoSync = true;
    bool     hasPos      = false;
    float    lat         = 0.0f;
    float    lon         = 0.0f;
};
Config g_cfg;

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

void loadConfig() {
    Preferences p;
    if (!p.begin("rs_ui", true)) return;
    g_cfg.dark        = p.getBool("dark", false);
    g_cfg.tz          = p.getInt("tz", 0);
    g_cfg.idleMode    = p.getUChar("idleMode", 1);
    g_cfg.idleSecs    = p.getUInt("idleSecs", 30);
    g_cfg.beep        = p.getBool("beep", true);
    g_cfg.vibe        = p.getBool("vibe", true);
    g_cfg.volume      = p.getUChar("vol", 80);
    g_cfg.kbLight     = p.getBool("kbl", false);
    g_cfg.gpsOn       = p.getBool("gps", true);
    g_cfg.gpsAutoSync = p.getBool("gpsSync", true);
    g_cfg.hasPos      = p.getBool("hasPos", false);
    g_cfg.lat         = p.getFloat("lat", 0.0f);
    g_cfg.lon         = p.getFloat("lon", 0.0f);
    int64_t epoch     = p.getLong64("epoch", 0);
    p.end();

    if (g_cfg.tz < 0 || g_cfg.tz >= kTzCount) g_cfg.tz = 0;
    if (g_cfg.idleMode > 1) g_cfg.idleMode = 1;
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

void loadSelfName() {
    Preferences p;
    if (p.begin("rs_self", true)) {
        String s = p.getString("name", "");
        if (s.length()) snprintf(g_selfName, sizeof(g_selfName), "%.23s", s.c_str());
        p.end();
    }
    if (!g_selfName[0]) {
        snprintf(g_selfName, sizeof(g_selfName), "%s", kDefaultSelfName);
        Preferences w;
        if (w.begin("rs_self", false)) { w.putString("name", g_selfName); w.end(); }
        Serial.printf("[NAME] default -> %s\n", g_selfName);
    }
}

void saveSelfName(const char* name) {
    snprintf(g_selfName, sizeof(g_selfName), "%.23s", (name && name[0]) ? name : kDefaultSelfName);
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
    g_bat.chrgStat = chargerOk ? chrg : 0xFF;
    g_bat.charging = charging;
    g_bat.usb = usb;
    g_bat.full = chargeDone;
    g_bat.valid = true;
    return true;
}

// ---- SD card + battery log --------------------------------------------------
bool g_sdMounted = false;
char g_sdStatus[28] = "not mounted";
constexpr const char* kBatLogPath = "/batlog2.csv";
constexpr const char* kBatLogHeader =
    "ms,mv,avgMa,pct,rawSoc,remMah,fccMah,fullCal,chrgStat,flags,chg,usb";

bool sdMount() {
    if (g_sdMounted) return true;
    spiBusIdle();
    if (!SD.begin(SD_CS, SPI, 20000000)) {
        snprintf(g_sdStatus, sizeof(g_sdStatus), "mount failed");
        Serial.println("[SD] mount failed");
        return false;
    }
    g_sdMounted = true;
    snprintf(g_sdStatus, sizeof(g_sdStatus), "OK %u/%u MB",
             (unsigned)(SD.usedBytes() >> 20), (unsigned)(SD.totalBytes() >> 20));
    if (!SD.exists(kBatLogPath)) {
        File f = SD.open(kBatLogPath, FILE_WRITE);
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

void sdUnmount() {
    g_batLogSd = false;
    if (g_sdMounted) { SD.end(); g_sdMounted = false; }
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
        File f = SD.open(kBatLogPath, FILE_APPEND);
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
    if (localNow(t)) snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
    else snprintf(out, n, "--:--");
}

void fmtDate(char* out, size_t n) {
    struct tm t;
    if (localNow(t)) snprintf(out, n, "%02d/%02d/%02d", t.tm_mon + 1, t.tm_mday, (t.tm_year + 1900) % 100);
    else snprintf(out, n, "--/--/--");
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
char    g_gpsStatus[16] = "off";
int     g_gpsUtc[6] = {0};       // y mo d hh mm ss of last valid RMC
bool    g_gpsUtcValid = false;
bool    g_gpsSyncedOnce = false;
uint32_t g_gpsLastSyncMs = 0;

void gpsApplyPower() {
#if defined(HAS_GPS) && HAS_GPS
    pinMode(GPS_EN, OUTPUT);
    if (!g_cfg.gpsOn) {
        digitalWrite(GPS_EN, LOW);
        g_gpsSerial = nullptr;
        g_gpsFix = false; g_gpsSats = 0; g_gpsUtcValid = false;
        snprintf(g_gpsStatus, sizeof(g_gpsStatus), "off");
        return;
    }
    digitalWrite(GPS_EN, HIGH);
    delay(30);
    static HardwareSerial gpsPort(1);
    gpsPort.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
    g_gpsSerial = &gpsPort;
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "searching");
    Serial.printf("[GPS] on RX=%d TX=%d baud=%d\n", GPS_RX, GPS_TX, GPS_BAUD);
#else
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "n/a");
#endif
}

bool gpsSyncClock() {
    if (!g_gpsFix || !g_gpsUtcValid) return false;
    if (!setClockUtc(g_gpsUtc[0], g_gpsUtc[1], g_gpsUtc[2], g_gpsUtc[3], g_gpsUtc[4], g_gpsUtc[5]))
        return false;
    g_gpsSyncedOnce = true;
    g_gpsLastSyncMs = millis() ? millis() : 1;
    Serial.println("[RTC] synced from GPS");
    return true;
}

int twoDigits(const char* s) { return (s[0] - '0') * 10 + (s[1] - '0'); }

void gpsHandleSentence(char* line) {
    char* fld[16] = {};
    int n = 0;
    fld[n++] = line;
    for (char* p = line; *p && n < 16; p++)
        if (*p == ',') { *p = 0; fld[n++] = p + 1; }

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
        g_gpsFix = valid;
    } else if (gga && n > 7) {
        if (fld[7][0]) g_gpsSats = (uint8_t)atoi(fld[7]);
        if (fld[6][0]) g_gpsFix = fld[6][0] > '0';
    } else {
        return;
    }
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "%s", g_gpsFix ? "fix" : "searching");

    // Auto sync: once after boot, then daily.
    if (g_cfg.gpsAutoSync && g_gpsFix && g_gpsUtcValid) {
        const uint32_t day = 24UL * 3600UL * 1000UL;
        if (!g_gpsSyncedOnce || millis() - g_gpsLastSyncMs >= day) gpsSyncClock();
    }
}

void gpsPoll() {
    if (!g_gpsSerial) return;
    static char line[100];
    static size_t len = 0;
    while (g_gpsSerial->available()) {
        char c = (char)g_gpsSerial->read();
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
    g_radio->setTxPower(LORA_DEFAULT_TX_POWER);
    g_radio->setPreambleLength(LORA_DEFAULT_PREAMBLE);
    g_radio->receive();
    g_loraIf = new LoRaInterface(g_radio, "LoRa");
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
    if (!g_msgStore.begin(&g_flash, nullptr, false)) { Serial.println("[MSG] store failed"); return; }
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
    if (g_cfg.beep) g_audio.requestMessage();
    if (g_cfg.vibe) vibrate(40);
}

// =============================================================================
// 3. Peer rows, history, send
// =============================================================================

constexpr int MAX_ROWS = 10;      // Contacts 0..9; Inbox/Peers 1..9 (+1 spare)
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

PeerRow* addRow(const uint8_t peer[16]) {
    if (g_rowCount >= MAX_ROWS || findRow(peer) >= 0) return nullptr;
    PeerRow& r = g_rows[g_rowCount++];
    memset(&r, 0, sizeof(r));
    memcpy(r.peer, peer, 16);
    r.self = isSelf(peer);
    peerName(peer, r.name, sizeof(r.name));
    const DiscoveredNode* node = g_announceMgr.findNode(rs::Bytes(peer, 16));
    r.saved = node && node->saved;
    char hex[33];
    hashToHex(peer, hex);
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

// Conversations (most recent first) with unread counts.
void appendConversations(int limit) {
    using namespace handheld::storage;
    if (!g_storeReady || limit <= 0) return;
    auto sub = g_msgStore.requestConversationPage({}, false, ConversationOrder::Recent,
                                                  ConversationDirection::After, (uint8_t)limit);
    for (int a = 0; a < 8 && !sub.accepted(); a++) {
        unsigned r = (unsigned)sub.rejection;
        if (r != 1 && r != 8) break;            // only Busy / Recovering are worth retrying
        pumpFor(50, 12);
        sub = g_msgStore.requestConversationPage({}, false, ConversationOrder::Recent,
                                                 ConversationDirection::After, (uint8_t)limit);
    }
    if (!sub.accepted()) { Serial.printf("[MSG] page rejected %u\n", (unsigned)sub.rejection); return; }

    MessageStore::Result res;
    ConversationSelector sel[MAX_ROWS];
    size_t nSel = 0;
    if (waitResult(sub.ticket, res) && res.outcome == Outcome::Committed &&
        res.error == Error::None && res.length >= sizeof(ConversationSelector)) {
        nSel = res.length / sizeof(ConversationSelector);
        if (nSel > (size_t)MAX_ROWS) nSel = MAX_ROWS;
        if (!g_msgStore.readPayload(sub.ticket, sel, nSel * sizeof(ConversationSelector))) nSel = 0;
    }
    g_msgStore.releaseResult(sub.ticket);

    for (size_t i = 0; i < nSel; i++) {
        if (isSelf(sel[i].cursor.peer)) continue;
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
        if (PeerRow* r = addRow(sel[i].cursor.peer)) r->unread = unread;
    }
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

// ---- Thread history ------------------------------------------------------------
constexpr int HIST_ROWS = 6;
struct HistRow { char text[36]; char status[12]; bool incoming; };
HistRow g_hist[HIST_ROWS];
int  g_histCount = 0;
char g_histNote[24] = "";

void loadHistory(const uint8_t peer[16]) {
    using namespace handheld::storage;
    g_histCount = 0;
    g_histNote[0] = 0;
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
        if (clen >= sizeof(h.text)) clen = sizeof(h.text) - 1;
        if (clen) {
            memcpy(h.text, buf + off, clen);
            h.text[clen] = 0;
            for (size_t k = 0; k < clen; k++)
                if ((unsigned char)h.text[k] < 32 || (unsigned char)h.text[k] > 126) h.text[k] = '?';
        }
        if (!h.incoming) {
            switch (hdr.status) {
                case 0:  snprintf(h.status, sizeof(h.status), "queued"); break;
                case 1:  snprintf(h.status, sizeof(h.status), "sending"); break;
                case 2:
                case 4:  snprintf(h.status, sizeof(h.status), "sent"); break;
                case 3:  snprintf(h.status, sizeof(h.status), "delivered"); break;
                default: snprintf(h.status, sizeof(h.status), "s%u", (unsigned)hdr.status); break;
            }
        }
    }
    if (g_histCount == 0) snprintf(g_histNote, sizeof(g_histNote), "(no messages yet)");
}

// ---- Send ------------------------------------------------------------------------
char g_txStr[16] = "";
lv_obj_t* g_root = nullptr;

enum class TxStep { Done, Rejected, Timeout, Invalid };

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
    return TxStep::Timeout;
}

bool sendMessage(const uint8_t dest[16], const char* text) {
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
    if (lastTx && millis() - lastTx < 3000UL) { snprintf(g_txStr, sizeof(g_txStr), "wait 3s"); return false; }

    // Free LVGL widgets so the store can allocate the outgoing record.
    if (g_root) lv_obj_clean(g_root);
    pumpFor(30, 10);

    bool allocRetried = false;
    for (int attempt = 0; attempt < 13; attempt++) {
        handheld::outgoing::InitialResult ir{};
        unsigned rej = 0;
        TxStep st = txOnce(dest, body, n, ir, rej);
        if (st == TxStep::Rejected) {
            if ((rej == 1 || rej == 8) && attempt < 12) { pumpFor(60, 20); continue; }   // Busy / Recovering
            snprintf(g_txStr, sizeof(g_txStr), "rejected %u", rej);
            return false;
        }
        if (st == TxStep::Timeout) { snprintf(g_txStr, sizeof(g_txStr), "timeout"); return false; }
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
const ScreenId kMenuScreens[5] = {SCR_HOME, SCR_SETTINGS, SCR_CONTACTS, SCR_PEERS, SCR_INBOX};
const char*    kMenuLabels[5]  = {"Home", "Settings", "Contacts", "Peers", "Inbox"};
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
enum class Entry : uint8_t { None, Message, Name, RtcField, IdleSecs, Latitude, Longitude, WifiPass };
Entry   g_entry = Entry::None;
int     g_entryArg = 0;              // RtcField index
uint8_t g_nameTarget[16];
constexpr size_t BUF_CAP = 120;
char    g_buf[BUF_CAP + 1] = "";
size_t  g_bufLen = 0;

bool entryActive() { return g_entry != Entry::None; }
bool browseMode()  { return !entryActive(); }
bool entryNumeric() {
    return g_entry == Entry::RtcField || g_entry == Entry::IdleSecs ||
           g_entry == Entry::Latitude || g_entry == Entry::Longitude;
}

// Settings
enum Page : uint8_t {
    PG_ROOT, PG_TIME, PG_RTC, PG_TZ, PG_LOC, PG_WIFI, PG_SCREEN, PG_IDLE,
    PG_BATTERY, PG_BATLOG, PG_IDENTITY, PG_LORA, PG_ALERTS, PG_DIAG, PG_STORAGE, PG_SD,
    PG_WIFI_SCAN, PG_WIFI_SAVED, PG_WIFI_NET, PG_WIFI_JOIN
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

void startEntry(Entry kind, const char* initial = nullptr, int arg = 0) {
    g_entry = kind;
    g_entryArg = arg;
    bufSet(initial);
    syncNumLayer();
    requestRedraw();
}

void endEntry() {
    g_entry = Entry::None;
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

struct WifiCred { char ssid[33]; char pass[65]; bool autoJoin; };
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
           p == PG_WIFI_NET || p == PG_WIFI_JOIN;
}

void wifiUiChanged() {
    if (g_idleActive || g_screen != SCR_SETTINGS) return;
    if (g_page == PG_ROOT || isWifiPage(g_page)) requestRedraw();
}

void wifiLoad() {
    Preferences p;
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
        strlcpy(c.pass, p.getString(k, "").c_str(), sizeof(c.pass));
        snprintf(k, sizeof(k), "a%d", i);
        c.autoJoin = p.getUChar(k, 1) != 0;
        g_wifiSavedCount++;
    }
    p.end();
    Serial.printf("[WIFI] %d saved network(s)\n", g_wifiSavedCount);
}

void wifiSaveAll() {
    Preferences p;
    if (!p.begin("rs_wifi", false)) return;
    p.clear();
    p.putUChar("n", (uint8_t)g_wifiSavedCount);
    for (int i = 0; i < g_wifiSavedCount; i++) {
        char k[4];
        snprintf(k, sizeof(k), "s%d", i); p.putString(k, g_wifiSaved[i].ssid);
        snprintf(k, sizeof(k), "p%d", i); p.putString(k, g_wifiSaved[i].pass);
        snprintf(k, sizeof(k), "a%d", i); p.putUChar(k, g_wifiSaved[i].autoJoin ? 1 : 0);
    }
    p.end();
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
    for (int i = 0; i < g_wifiSavedCount; i++) if (g_wifiSaved[i].autoJoin) return true;
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
            if (k >= 0 && g_wifiSaved[k].autoJoin) best = k;
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
    if (idle && !g_wifiUserOff && !g_wifiScanning && wifiHasAutoCreds() &&
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
constexpr int BOT_C_W     = 80;                      // Contact | Peers | Inbox
constexpr int BOT_P_W     = 64;
constexpr int BOT_I_W     = W - BOT_C_W - BOT_P_W;   // 96
constexpr int MENU_ROW_H  = 36;
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
    H_TILE_AUDIO, H_UP, H_DOWN
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
    }
    return "";
}

// Text shown in a row's subtitle while that row is being typed into.
const char* editingSub(Entry kind, int arg = 0) {
    static char s[34];
    if (g_entry != kind || (kind == Entry::RtcField && g_entryArg != arg)) return nullptr;
    constexpr size_t kShow = 27;                 // keep the cursor end visible
    if (g_bufLen > kShow) snprintf(s, sizeof(s), ">..%s_", g_buf + g_bufLen - (kShow - 2));
    else                  snprintf(s, sizeof(s), ">%s_", g_buf);
    return s;
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
        srow(RK_TOGGLE, "Enable GPS", "%s  %s  %u sats", g_cfg.gpsOn ? "ON" : "OFF", g_gpsStatus, g_gpsSats);
        if (g_cfg.hasPos) srow(RK_MENU, "Manual location", "%.4f, %.4f", g_cfg.lat, g_cfg.lon);
        else              srow(RK_MENU, "Manual location", "not set");
        srow(RK_ACTION, "Sync GPS now", g_gpsFix ? "Enter = sync" : "needs GPS fix");
        srow(RK_TOGGLE, "Auto GPS sync", g_cfg.gpsAutoSync ? "ON (boot + daily)" : "OFF");
        break;

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
        srow(RK_MENU, "Saved networks", "%d saved%s", g_wifiSavedCount,
             wifiHasAutoCreds() ? ", auto-connect" : "");
        srow(RK_ACTION, "Disconnect", g_wifiSt == WifiSt::Off ? "-" : "Enter = off (pause auto)");
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
        else   srow(RK_ENTRY, "Password", c.pass[0] ? "%u chars, Enter = change" : "none (open)",
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

    case PG_SCREEN:
        srow(RK_TOGGLE, "Theme", "%s", g_cfg.dark ? "Black ink" : "White paper");
        srow(RK_MENU, "Idle splash", g_cfg.idleMode ? "On, %u s" : "Off", (unsigned)g_cfg.idleSecs);
        srow(RK_TOGGLE, "Key backlight", "%s", g_cfg.kbLight ? "ON" : "OFF");
        break;

    case PG_IDLE:
        srow(RK_PICK, "Off", "%s", g_cfg.idleMode == 0 ? "(current)" : "");
        srow(RK_PICK, "On, no password", "%s", g_cfg.idleMode == 1 ? "(current)" : "");
        srow(RK_INFO, "On + password", "TBD");
        e = editingSub(Entry::IdleSecs);
        if (e) srow(RK_ENTRY, "Timeout (10-99999 s)", "%s", e);
        else   srow(RK_ENTRY, "Timeout (10-99999 s)", "%u s", (unsigned)g_cfg.idleSecs);
        break;

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
        break;

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
        break;

    case PG_LORA:
        srow(RK_INFO, "Radio", "%s", loraOnline() ? "online" : "offline");
        srow(RK_INFO, "Frequency", "%.3f MHz", LORA_DEFAULT_FREQ / 1e6);
        srow(RK_PICK, "Link preset", "%s%s", kLoraPresets[g_loraPreset].name,
             g_loraPreset ? " (peers need same)" : "");
        srow(RK_INFO, "Last RSSI", "%d dBm", loraRssi());
        srow(RK_INFO, "Last SNR", "%.1f dB", loraSnr());
        break;

    case PG_ALERTS:
        srow(RK_TOGGLE, "Beep on message", "%s", g_cfg.beep ? "ON" : "OFF");
        srow(RK_TOGGLE, "Vibrate on message", "%s", g_cfg.vibe ? "ON" : "OFF");
        srow(RK_PICK, "Volume", "%u%%", (unsigned)g_cfg.volume);
        srow(RK_ACTION, "Test alert", "Enter = play");
        break;

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
        break;

    case PG_SD:
        srow(RK_INFO, "Status", "%s", g_sdStatus);
        srow(RK_ACTION, "Mount", g_sdMounted ? "mounted" : "Enter = mount");
        srow(RK_ACTION, "Unmount", g_sdMounted ? "Enter = eject" : "-");
        srow(RK_ACTION, "Reset log file", g_sdMounted ? "Enter = reset" : "mount first");
        srow(RK_INFO, "Log file", "%s", kBatLogPath);
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

void activateSetting(int idx) {
    switch (g_page) {
    case PG_ROOT: {
        static const Page kSub[9] = {PG_TIME, PG_WIFI, PG_SCREEN, PG_BATTERY, PG_IDENTITY,
                                     PG_LORA, PG_ALERTS, PG_DIAG, PG_STORAGE};
        if (idx >= 0 && idx < 9) pushPage(kSub[idx]);
        break;
    }
    case PG_TIME:
        if (idx == 0) { loadRtcStaging(); pushPage(PG_RTC); }
        else if (idx == 1) pushPage(PG_TZ, g_cfg.tz);
        else if (idx == 2) { g_cfg.gpsOn = !g_cfg.gpsOn; saveConfig(); gpsApplyPower(); requestRedraw(); }
        else if (idx == 3) pushPage(PG_LOC);
        else if (idx == 4) { if (gpsSyncClock()) toast("Clock synced from GPS"); else toast("No GPS fix yet"); }
        else if (idx == 5) { g_cfg.gpsAutoSync = !g_cfg.gpsAutoSync; saveConfig(); requestRedraw(); }
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
        break;

    case PG_WIFI_SCAN: {
        if (idx == 0) { wifiStartScan(false); break; }
        if (idx < 1 || idx > g_wifiSeenCount) break;
        const WifiSeen& s = g_wifiSeen[idx - 1];
        const int k = wifiFindSaved(s.ssid);
        if (wifiConnected() && strcmp(s.ssid, g_wifiTarget) == 0) { toast("Already connected"); break; }
        if (k >= 0) {                            // known: join with the stored password
            wifiConnect(g_wifiSaved[k].ssid, g_wifiSaved[k].pass, false, g_wifiSaved[k].autoJoin);
            popPage();
            toast("Joining %.16s", s.ssid);
            break;
        }
        strlcpy(g_joinSsid, s.ssid, sizeof(g_joinSsid));
        g_joinPass[0] = 0;
        g_joinSecure = s.secure;
        g_joinAuto = true;                       // default: auto-connect once credentials exist
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
            if (g_cfg.kbLight) g_kb.backlightOn(); else g_kb.backlightOff();
            saveConfig();
            requestRedraw();
        }
        break;

    case PG_IDLE:
        if (idx == 0 || idx == 1) { g_cfg.idleMode = (uint8_t)idx; saveConfig(); requestRedraw(); }
        else if (idx == 2) toast("Password lock: TBD");
        else if (idx == 3) { char s[8]; snprintf(s, sizeof(s), "%u", (unsigned)g_cfg.idleSecs); startEntry(Entry::IdleSecs, s); }
        break;

    case PG_BATTERY:
        if (idx == 7) pushPage(PG_BATLOG);
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
        break;

    case PG_LORA:
        if (idx == 2) { applyLoraPreset(g_loraPreset + 1); requestRedraw(); }
        break;

    case PG_ALERTS:
        if (idx == 0) { g_cfg.beep = !g_cfg.beep; g_audio.setEnabled(g_cfg.beep); saveConfig(); requestRedraw(); }
        else if (idx == 1) { g_cfg.vibe = !g_cfg.vibe; saveConfig(); requestRedraw(); }
        else if (idx == 2) {
            g_cfg.volume = g_cfg.volume >= 100 ? 20 : g_cfg.volume + 20;
            g_audio.setVolume(g_cfg.volume);
            saveConfig();
            requestRedraw();
        } else if (idx == 3) {
            if (g_cfg.beep) g_audio.playMessage();
            if (g_cfg.vibe) vibrate(60);
            toast("Test alert");
        }
        break;

    case PG_STORAGE:
        if (idx == 4) pushPage(PG_SD);
        break;

    case PG_SD:
        if (idx == 1) { if (sdMount()) toast("SD mounted"); else toast("SD mount failed"); }
        else if (idx == 2) { sdUnmount(); toast("SD ejected"); }
        else if (idx == 3) {
            if (!g_sdMounted) { toast("Mount SD first"); break; }
            SD.remove(kBatLogPath);
            File f = SD.open(kBatLogPath, FILE_WRITE);
            if (f) { f.println(kBatLogHeader); f.close(); }
            toast("Log file reset");
        }
        break;

    default:
        break;
    }
}

// ---- List screens (Inbox / Contacts / Peers) ------------------------------------
int rowNumber(ScreenId s, int idx) { return s == SCR_CONTACTS ? idx : idx + 1; }

void rememberSelection() {
    if (g_listSel >= 0 && g_listSel < g_rowCount) {
        memcpy(g_rememberHash[g_screen], g_rows[g_listSel].peer, 16);
        g_rememberValid[g_screen] = true;
    }
}

void loadRows(ScreenId s) {
    g_rowCount = 0;
    if (s == SCR_INBOX) {
        appendConversations(9);
        appendAnnounced(false, false);     // so a new peer can be messaged
        if (g_rowCount > 9) g_rowCount = 9;
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
        if (g_rowCount > 9) g_rowCount = 9;
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
    box(BOT_C_W, FOOT_Y, BOT_P_W, FOOT_H, 1);
    label(BOT_C_W, FOOT_Y - 1, BOT_P_W, FOOT_H, "G=up", false, 'G');
    addHit(BOT_C_W, FOOT_Y, BOT_P_W, FOOT_H, H_UP);
    box(BOT_C_W + BOT_P_W, FOOT_Y, BOT_I_W, FOOT_H, 1);
    if (moreDn) {
        label(BOT_C_W + BOT_P_W, FOOT_Y - 1, BOT_I_W, FOOT_H, "v more");
        addHit(BOT_C_W + BOT_P_W, FOOT_Y, BOT_I_W, FOOT_H, H_DOWN);
    }
}

void drawHeader(const char* left, bool moreUp) {
    text(4, HEADER_Y, left, true, moreUp ? MENU_X - 8 : W - 8);
    if (moreUp) {
        box(MENU_X, TOP_H - 1, TAB_W, 18, 1);
        label(MENU_X, TOP_H - 2, TAB_W, 18, "^ more");
        addHit(MENU_X, TOP_H - 1, TAB_W, 18, H_UP);
    }
}

void renderList() {
    char hdr[32];
    if (g_screen == SCR_INBOX) {
        int unread = g_storeReady ? g_msgStore.totalUnreadCount() : 0;
        snprintf(hdr, sizeof(hdr), "Inbox  unread %02d", unread > 99 ? 99 : unread);
    } else {
        snprintf(hdr, sizeof(hdr), "%s (%d)", screenName(g_screen), g_rowCount);
    }
    drawHeader(hdr, g_listScroll > 0);

    if (g_rowCount == 0) {
        const char* empty = g_screen == SCR_CONTACTS ? "No contacts - see Peers"
                          : g_screen == SCR_PEERS    ? "No peers - Announce on Home"
                                                     : "No chats yet";
        text(8, LIST_TOP + 8, empty);
    }
    for (int i = g_listScroll; i < g_rowCount && i < g_listScroll + VIS_ROWS; i++) {
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

void renderThread() {
    char name[24], hdr[40];
    peerName(g_threadPeer, name, sizeof(name));
    snprintf(hdr, sizeof(hdr), ">>> %s", name);
    text(4, HEADER_Y, hdr, true, W - 8);
    int y = HEADER_Y + 20;
    if (g_txStr[0]) {
        char st[32];
        snprintf(st, sizeof(st), "last send: %s", g_txStr);
        text(4, y, st);
        y += 18;
    }
    const int yMax = STRIP_Y - 2;
    if (g_histCount == 0) {
        text(4, y, g_histNote[0] ? g_histNote : "(no messages yet)");
    } else {
        // Newest messages win the space; older ones drop off the top.
        auto hOf = [](int i) { return (!g_hist[i].incoming && g_hist[i].status[0]) ? 42 : 28; };
        int start = g_histCount, used = 0;
        while (start > 0 && y + used + hOf(start - 1) <= yMax) { used += hOf(start - 1); start--; }
        for (int i = start; i < g_histCount; i++) {
            drawBubble(y, g_hist[i].text, g_hist[i].incoming, g_hist[i].incoming ? nullptr : g_hist[i].status);
            y += hOf(i);
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
    text(6, STRIP_Y + STRIP_LINE * 2, g_toast[0] ? g_toast : "[Enter] send  [Alt] case", false, W - 12);
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
    label(0, ay, W, ANN_H, "Announce ID", true, browseMode() ? 'A' : 0);
    addHit(0, ay, W, ANN_H, H_ANNOUNCE);
    int ny = ay + ANN_H + 4;
    if (ny > BOT_TOP - 18) ny = BOT_TOP - 18;
    label(0, ny, W, 16, g_selfName, true);
}

// ---- Settings ---------------------------------------------------------------------
void renderSettings() {
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
    char d[12], t[8], md[8];
    fmtDate(d, sizeof(d));
    fmtTime(t, sizeof(t));
    snprintf(md, sizeof(md), "%.5s", d);
    label(CLOCK_X, 1, TAB_W, 16, md);
    label(CLOCK_X, 17, TAB_W, 16, t);

    navBox(MENU_X, 0, TAB_W, TOP_H, "MENU", g_menuOpen, browseMode() ? 'M' : 0);
    if (browseMode()) addHit(MENU_X, 0, TAB_W, TOP_H, H_MENU);
}

void drawBottomBar() {
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
    navBox(BOT_C_W, BOT_TOP, BOT_P_W, BOT_H, "Peers", g_screen == SCR_PEERS, hk ? 'P' : 0);
    navBox(BOT_C_W + BOT_P_W, BOT_TOP, BOT_I_W, BOT_H, "Inbox", g_screen == SCR_INBOX, hk ? 'I' : 0);
    if (hk) {
        addHit(0, BOT_TOP, BOT_C_W, BOT_H, H_TAB, SCR_CONTACTS);
        addHit(BOT_C_W, BOT_TOP, BOT_P_W, BOT_H, H_TAB, SCR_PEERS);
        addHit(BOT_C_W + BOT_P_W, BOT_TOP, BOT_I_W, BOT_H, H_TAB, SCR_INBOX);
    }
}

void drawMenuOverlay() {
    if (!g_menuOpen) return;
    for (int i = 0; i < 5; i++) {
        const int y = TOP_H + i * MENU_ROW_H;
        navBox(MENU_X, y, TAB_W, MENU_ROW_H, kMenuLabels[i], i == g_menuSel, kMenuLabels[i][0]);
        addHit(MENU_X, y, TAB_W, MENU_ROW_H, H_MENU_GO, i);
    }
}

void render() {
    const uint32_t t0 = millis();
    clearRoot();
    if (g_threadOpen)                  renderThread();
    else if (g_entry == Entry::Name)   renderRename();
    else switch (g_screen) {
        case SCR_HOME:     renderHome(); break;
        case SCR_SETTINGS: renderSettings(); break;
        default:           renderList(); break;
    }
    drawTopBar();
    drawBottomBar();
    drawMenuOverlay();
    lv_refr_now(nullptr);
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

void paintIdle() {
    clearRoot();
    const int y0 = 76, step = 20;
    label(0, y0, W, 20, "RatSpeak", true);
    char d[12], t[8], tmp[24], line[40];
    fmtDate(d, sizeof(d));
    fmtTime(t, sizeof(t));
    spaced(d, line, sizeof(line));
    label(0, y0 + step, W, 20, line, true);
    snprintf(tmp, sizeof(tmp), "%s %s", t, tzCur().name);
    spaced(tmp, line, sizeof(line));
    label(0, y0 + 2 * step, W, 20, line, true);

    const int pct = g_bat.valid ? g_bat.pct : -1;
    const int bx = W / 2 - 23, by = y0 + 3 * step + 2;
    drawBatteryIcon(bx, by, 18, 28, pct);
    snprintf(tmp, sizeof(tmp), pct >= 0 ? "%d%%" : "--%%", pct);
    label(bx + 20, by + 6, 40, 16, tmp);
    label(0, y0 + 5 * step, W, 18,
          g_bat.charging ? (g_bat.usb ? "charging | plugged in" : "charging")
                         : (g_bat.usb ? "plugged in" : "unplugged"));

    const int unread = g_storeReady ? g_msgStore.totalUnreadCount() : 0;
    if (unread > 0) snprintf(line, sizeof(line), "NEW messages: %d unread", unread);
    else            snprintf(line, sizeof(line), "No new messages");
    label(0, y0 + 6 * step, W, 18, line, unread > 0);

    uint32_t s = millis() / 1000UL;
    label(0, y0 + 7 * step, W, 16, "uptime");
    snprintf(line, sizeof(line), "%05luD : %02luH : %02luM", (unsigned long)(s / 86400UL),
             (unsigned long)((s / 3600UL) % 24UL), (unsigned long)((s / 60UL) % 60UL));
    label(0, y0 + 8 * step, W, 16, line);
    label(0, y0 + 9 * step, W, 18, g_selfName, true);
    label(0, 292, W, 22, "press a KEY to wake", true);

    lv_refr_now(nullptr);
    drawLogo(W / 2, 36);     // stamped after LVGL so the clear does not wipe it
    g_idleShownUnread = unread;
    g_idleShownPct = pct;
    g_idleShownMin = millis() / 60000UL;
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
    if (g_threadOpen) { g_threadOpen = false; }
    if (entryActive()) endEntry();
    if (s == SCR_SETTINGS) {          // doc: M -> S always lands on the Settings root
        g_navDepth = 0;
        g_page = PG_ROOT;
        g_sel = 0;
        g_scroll = 0;
    }
    if (g_screen != s) g_listScroll = 0;
    g_screen = s;
    if (s == SCR_INBOX || s == SCR_CONTACTS || s == SCR_PEERS) loadRows(s);
    requestRedraw();
    Serial.printf("[UI] -> %s\n", screenName(s));
}

void openMenu() {
    g_menuOpen = true;
    g_menuSel = 0;
    for (int i = 0; i < 5; i++) if (kMenuScreens[i] == g_screen) g_menuSel = i;
    requestRedraw();
}

void closeMenu() { g_menuOpen = false; requestRedraw(); }

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
    else startNameEntry(g_rows[idx].peer);    // Contacts + Peers: Enter names the device
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
        int idx = d - 1;
        int n = settingsRowCount();
        if (idx < 0 || idx >= n) return;           // no clamping to "nearest" row
        g_sel = idx;
        clampScroll(g_sel, g_scroll, n);
        activateSetting(idx);
        requestRedraw();
        return;
    }
    if (g_screen == SCR_HOME) return;
    int idx = (g_screen == SCR_CONTACTS) ? d : d - 1;
    if (idx < 0 || idx >= g_rowCount) return;
    g_listSel = idx;
    clampScroll(g_listSel, g_listScroll, g_rowCount);
    rememberSelection();
    requestRedraw();
}

void cancelEntry() {
    if (g_entry == Entry::Message) { closeThread(); return; }
    endEntry();
}

void goBack() {
    if (g_menuOpen)              { closeMenu(); return; }
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
        if (!g_bufLen) return;
        char body[BUF_CAP + 1];
        memcpy(body, g_buf, g_bufLen + 1);
        if (sendMessage(g_threadPeer, body)) {
            bufClear();
            if (g_cfg.beep) g_audio.playMessage();
        } else {
            toast("Not sent: %s", g_txStr);   // keep the draft for retry
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
    case Entry::WifiPass: {
        if (g_bufLen > 0 && g_bufLen < 8) { toast("Password: 8+ chars"); return; }
        if (g_bufLen > WIFI_PASS_MAX) { toast("Max 63 chars"); return; }
        if (g_page == PG_WIFI_NET && g_wifiNetIdx >= 0 && g_wifiNetIdx < g_wifiSavedCount) {
            if (!g_bufLen) { endEntry(); toast("Password unchanged"); return; }
            WifiCred& c = g_wifiSaved[g_wifiNetIdx];
            strlcpy(c.pass, g_buf, sizeof(c.pass));
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

void entryKey(const KeyEvent& ev) {
    if (ev.enter) { commitEntry(); return; }
    if (ev.del) {
        if (g_bufLen) g_buf[--g_bufLen] = 0;
        requestRedraw();
        return;
    }
    char c = 0;
    if (ev.space) {
        if (g_entry == Entry::Latitude || g_entry == Entry::Longitude) c = '.';
        else if (g_entry == Entry::Message || g_entry == Entry::Name || g_entry == Entry::WifiPass) c = ' ';
    } else if (ev.fn) {
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
    bufAppend(c);
    // Show it on the next frame. Keys typed while that frame is on the panel are
    // queued by onPanelBusy() and land together in the frame after.
    requestRedraw();
}

void menuKey(const KeyEvent& ev) {
    if (ev.enter) { goScreen(kMenuScreens[g_menuSel]); return; }
    const char c = (char)toupper((unsigned char)ev.character);
    if (c == 'M') { closeMenu(); return; }
    if (c == 'G') { if (g_menuSel > 0) g_menuSel--; requestRedraw(); return; }
    if (c == 'V') { if (g_menuSel < 4) g_menuSel++; requestRedraw(); return; }
    for (int i = 0; i < 5; i++)
        if (c == kMenuLabels[i][0]) { goScreen(kMenuScreens[i]); return; }
}

void browseKey(const KeyEvent& ev) {
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
        case 'A':
            if (g_screen == SCR_HOME) activateCurrent();
            break;
        default: break;
    }
}

void handleKey(const KeyEvent& ev) {
    if (g_idleActive) {                 // any key wakes; the key itself is consumed
        g_idleActive = false;
        noteInput();
        g_fullRefreshReq = true;
        requestRedraw();
        Serial.println("[IDLE] wake");
        return;
    }
    noteInput();
    if (ev.up) { goBack(); return; }                          // lone Shift = ^
    if (g_menuOpen) { menuKey(ev); return; }
    if (entryActive()) { entryKey(ev); return; }
    if (ev.fn) { g_fullRefreshReq = true; requestRedraw(); return; }   // MIC = full refresh
    browseKey(ev);
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
    if (g_idleActive) return;                 // doc: key-only wake
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
        case H_MENU_GO:    goScreen(kMenuScreens[hit->arg]); break;
        case H_TAB:        goScreen((ScreenId)hit->arg); break;
        case H_BACK:       goBack(); break;
        case H_ANNOUNCE:   activateCurrent(); break;
        case H_UP:         moveSelection(-1); break;
        case H_DOWN:       moveSelection(+1); break;
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

// ---- Serial console (debug) --------------------------------------------------------
void handleSerialLine(char* p) {
    while (*p == ' ') p++;
    if (!*p) return;
    if ((p[0] == 't' || p[0] == 'T') && p[1] == ':') {
        if (!g_threadOpen) { Serial.println("[CMD] open a thread first"); return; }
        if (sendMessage(g_threadPeer, p + 2)) loadHistory(g_threadPeer);
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
    } else if (!strcmp(p, "L")) {
        g_cfg.kbLight = !g_cfg.kbLight;
        if (g_cfg.kbLight) g_kb.backlightOn(); else g_kb.backlightOff();
        requestRedraw();
    } else if (!strcmp(p, "FORMAT")) {
        Serial.println(recoverCorruptLittleFS() ? "[FLASH] formatted - reboot now" : "[FLASH] format failed");
    } else {
        Serial.println("[CMD] t:<msg> a h i c p s d L FORMAT");
    }
}

void pollSerial() {
    static char line[180];
    static size_t len = 0;
    while (Serial.available()) {
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
void watchBattery() {
    static uint32_t last = 0;
    if (millis() - last < 800) return;
    last = millis();
    const bool chg = g_bat.charging, usb = g_bat.usb;
    const int pct = g_bat.pct;
    const bool wasValid = g_bat.valid;
    readBattery();
    if (g_idleActive) return;
    const bool changed = chg != g_bat.charging || usb != g_bat.usb || wasValid != g_bat.valid ||
                         (g_screen == SCR_HOME && pct != g_bat.pct);
    if (changed && browseMode()) requestRedraw();
}

void watchMessages() {
    if (!g_storeReady) return;
    static int lastUnread = -1;
    static uint32_t lastRev = 0, lastRedraw = 0;
    const int unread = g_msgStore.totalUnreadCount();
    if (lastUnread >= 0 && unread > lastUnread) notifyInbound();
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
    if (g_cfg.idleMode == 0 || g_bufLen) return;      // never hide an unsent draft
    if (millis() - g_lastInputMs >= g_cfg.idleSecs * 1000UL) enterIdle();
}

}  // namespace

// =============================================================================
// 8. begin() / loop()
// =============================================================================

bool begin() {
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RatSpeak T-Deck Pro - standalone");
    Serial.println("========================================");
    loadConfig();
    applyTz();
    ensureClockSeeded();
    loadSelfName();

    handheld::bindDeviceOwner();

    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(30);

    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);  delay(10);
    digitalWrite(TOUCH_RST, HIGH); delay(50);
    pinMode(TOUCH_INT, INPUT_PULLUP);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);
    spiBusIdle();
    logPsram();

    if (!initRadio()) Serial.println("[BOOT] radio failed");
    initStorageAndProto();

    // Finish LXMF outbound recovery while internal heap is still large.
    if (g_protoReady) pumpFor(300, 10);

    spiBusIdle();
    if (!g_display.begin()) { Serial.println("[BOOT] display failed"); return false; }
    if (!LvglPort::begin(g_display)) { Serial.println("[BOOT] LVGL failed"); return false; }
    g_display.setBusyCallback(onPanelBusy);
    drawBootSplash();

    g_kbOk = g_kb.begin();
    if (g_kbOk && g_cfg.kbLight) g_kb.backlightOn();
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

    gpsApplyPower();
    loadBatteryCal();
    readBattery();
    wifiLoad();
    g_wifiNextAuto = millis() + 8000;    // auto-connect after the boot announce
    noteInput();
    goScreen(SCR_HOME);
    render();
    Serial.println("[BOOT] ready");
    return true;
}

void loop() {
    processKeys();          // input first: lowest latency to the next render
    pumpOnce();
    g_announceMgr.loop();
    g_audio.loop();
    gpsPoll();

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

    processKeys();         // again, in case a watcher above took a while
    pollTouch();
    pollSerial();
    watchIdle();

    if (!g_idleActive) {
        const bool debounced = g_redrawAt && (int32_t)(millis() - g_redrawAt) >= 0;
        if (g_redraw || debounced) {
            processKeys();  // fold any just-arrived keys into this frame
            render();
        }
    }
    if (!g_redraw) delay(2);
}

}  // namespace standalone
