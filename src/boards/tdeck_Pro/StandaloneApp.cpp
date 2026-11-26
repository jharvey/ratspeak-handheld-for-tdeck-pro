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
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#endif
#include <LittleFS.h>

#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include "Eink/RatSpeakLogo.h"
#include "MapView.h"
#include "CotFeed.h"
#include "CotBle.h"
#include "Chess.h"
#include "audio/AudioNotify.h"
#include "voice/HaVoice.h"
#include "voice/VoiceCodec.h"
#include "voice/Walkie.h"
#include "protocol/LxmfFields.h"

// Shown on the boot screen and in Settings (bump with each release to the units).
#define RATSPEAK_FW_VERSION "v0.1"
#include "hal/Keyboard.h"
#include "hal/UsbDrive.h"
#include "hal/SysImage.h"
#include "radio/SX1262.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "storage/StorageLease.h"
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
#if !defined(SIMULATOR)
#include "transport/RnsAutoInterface.h"  // Reticulum over WiFi (LAN peers)
#include <esp_netif.h>
#endif
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
#include "PsramBuf.h"

// hal/Keyboard.cpp — app-driven number layer for numeric fields
extern void keyboardSetNumLayer(bool on);

#if defined(SIMULATOR)
namespace sim { void setPowerInfo(const char* line); }   // side panel (sim/shim)
namespace sim { size_t processBytes(); void wearDump(); }   // sim/shim: process memory, flash wear counters
namespace sim { uint32_t uptimeOffsetS(); }                 // sim.ini "uptime = 1d2h": where the shown uptime starts
#endif

namespace standalone {
namespace {
// Soak test (see "Soak test" below): hooks called from earlier code.
void soakLog(const char* fmt, ...);
void soakIncoming(const uint8_t peer[16], const char* text);
void soakSent(const char* body);
void soakNoteSlow(const char* name, uint32_t ms);
uint32_t soakGoingDark();
bool soakIsOn();
uint8_t soakPeerCount();
void soakToggle();
// Hardware self-test (Settings > Diagnostics > Check this unit, console "selftest").
void selftestStart(bool console);
void selftestRows();
bool selftestRan();
uint32_t g_i2cSlowCount = 0;              // slow keyboard / gauge reads (the 1 s I2C waits)

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

// Idle screens (S.4.2 "Idle screen"); add new ones at the end.
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
    char     found[10][41] = {};    // if-found screen: name, phone, address 1-2, town, state, zip,
                                    // country, misc 1-2 (S.4.2.9); NVS fnd0..fnd9
    bool     touchOff    = false;   // S.1.4: touch screen off, keys only
    bool     haVoice     = false;   // S.3.7: Home Assistant voice satellite (WiFi, ESPHome API)
    uint8_t  haWake      = 0;       // S.3.7 row 7: wake word 0 off, 1 on USB power only, 2 always
    uint8_t  haSensors   = 0;       // S.3.7 row 8: sensors for HA 0 off, 1 battery, 2 battery + position
    uint8_t  quiet       = 0;       // S.9 row 6: quiet hours preset (kQuiet), 0 = off
    uint8_t  relay       = 0;       // S.8 row 7: relay for other units 0 off, 1 on USB power, 2 always
    char     haKey[45]   = "";      // its ESPHome API encryption key (base64); "" = none
    bool     meshWifi    = true;    // S.3.8: Reticulum over WiFi (AutoInterface: units on the LAN)
    uint8_t  micGain     = 4;       // microphone gain for HA voice (console micgain)
    bool     foundAddr   = true;    // if-found screen shows this radio's LXMF address
    char     libLast[40] = "";      // library file last read (idle screen "Library")
    uint32_t libLastOff  = 0;       // and the page it was on
    bool     libLastWrap = true;
    bool     clock12     = false;   // 12 h clock (1:45 PM) instead of 24 h
    bool     imperial    = false;   // distances in ft / mi instead of m / km
    uint8_t  lowBatPct   = 0;       // low-battery notice at this % (0 = off)
    uint8_t  lowBatTo[16] = {};     // ... sent to this contact
    bool     lowBatSent  = false;   // sent in this discharge (reset when charging)
    uint8_t  opMode      = 2;       // S.1 operator mode: 0 Minimal, 1 Typical, 2 Everything (default)
    uint16_t typFeat     = 0x79;    // S.1.5 features Typical shows (Feature bits; kTypicalDefault)
    bool     wizDone     = true;    // setup wizard finished or skipped
    bool     trackOn     = false;   // breadcrumb track recording (S.2.10)
    uint8_t  coordFmt    = 0;       // 0 decimal degrees, 1 deg-min-sec, 2 MGRS (S.2.12)
    bool     kbLight     = false;
    bool     gpsOn       = true;
    bool     gpsAutoSync = true;
    bool     hasPos      = false;
    float    lat         = 0.0f;
    float    lon         = 0.0f;
};
Config g_cfg;
const char* const kHaWakeName[3] = {"Off (MIC to talk)", "On USB power only", "Always (more battery)"};
const char* const kHaSensName[3] = {"Off", "Battery", "Battery + position"};
// Quiet hours presets: from, to (local hours); index 0 = off.
const uint8_t kQuiet[5][2] = {{0, 0}, {22, 7}, {23, 6}, {21, 7}, {20, 8}};
constexpr int kQuietN = 5;
// True during the quiet hours: no message sound or vibration (the message still arrives and
// shows); also no chime for Home Assistant announcements.
int g_quietTestHour = -1;                    // bench "quiet hour <h>": pretend it is that hour
bool quietNow() {
    if (!g_cfg.quiet || g_cfg.quiet >= kQuietN) return false;
    int h = g_quietTestHour;
    if (h < 0) {
        const time_t now = time(nullptr);
        if (now < 1600000000) return false;
        struct tm t;
        localtime_r(&now, &t);
        h = t.tm_hour;
    }
    const int from = kQuiet[g_cfg.quiet][0], to = kQuiet[g_cfg.quiet][1];
    return from > to ? (h >= from || h < to) : (h >= from && h < to);
}
const char* const kRelayName[3] = {"Off", "On USB power", "Always"};
// Operator modes (S.1). Minimal: Inbox, Contacts and Settings S.1 only (no Home, Peers, Atlas,
// Library); waking goes to the Inbox. Normal is Advanced for now.
// Operator modes (S.1): Minimal (Inbox, Contacts, S.1, walkie-talkie: kept minimal on purpose,
// owner 2026-11-25: Home, Switch off, the trip check and the Atlas are NOT in Minimal), Typical
// (the features picked on S.1.5, most on by default; the Power page shows only Full, Balanced,
// Eco and Saver unless "Advanced" is on), Everything (all). Same NVS values as before.
constexpr uint8_t OP_MINIMAL = 0, OP_NORMAL = 1, OP_ADVANCED = 2;   // NORMAL = Typical, ADVANCED = Everything
const char* const kOpModeName[3] = {"Minimal", "Typical", "Everything"};
bool minimalMode() { return g_cfg.opMode == OP_MINIMAL; }

// Features Typical mode can show or hide (S.1.5). Everything shows all; Minimal only F_WALKIE.
enum Feature : uint16_t {
    F_WALKIE = 1 << 0,     // walkie-talkie (contact card W)
    F_VOICEMSG = 1 << 1,   // voice messages and other voice options
    F_HAVOICE = 1 << 2,    // Home Assistant voice (S.3.7, MIC)
    F_WIFI = 1 << 3,       // WiFi settings (S.3)
    F_ATLAS = 1 << 4,      // Atlas (maps)
    F_LIBRARY = 1 << 5,    // Library and games
    F_PEERS = 1 << 6,      // Peers list and announce
    F_ADVANCED = 1 << 7,   // LoRa page, Diagnostics, TCP hub
};
constexpr int kFeatureCount = 8;
const char* const kFeatureName[kFeatureCount] = {"Walkie-talkie", "Voice messages", "Home Assistant voice",
                                                 "WiFi settings", "Atlas (maps)", "Library & games",
                                                 "Peers & announce", "Advanced: LoRa, diag."};
constexpr uint16_t kTypicalDefault = F_WALKIE | F_WIFI | F_ATLAS | F_LIBRARY | F_PEERS;   // 0x79
bool feature(uint16_t f) {
    if (g_cfg.opMode == OP_ADVANCED) return true;
    if (g_cfg.opMode == OP_MINIMAL) return (f & F_WALKIE) != 0;
    return (g_cfg.typFeat & f) != 0;
}
bool g_haveLib = false;           // the card's library folder has files (else the menu says Games)

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
    bool anyFound = false;
    for (int i = 0; i < 10; i++) {
        char k[6]; snprintf(k, sizeof(k), "fnd%d", i);
        p.getString(k, g_cfg.found[i], sizeof(g_cfg.found[i]));
        anyFound |= g_cfg.found[i][0] != 0;
    }
    if (!anyFound && p.isKey("foundTxt")) {          // older single text: line 1 -> Name, the rest -> Misc
        char old[121] = "";
        p.getString("foundTxt", old, sizeof(old));
        int line = 0;
        for (char* tok = strtok(old, ";|"); tok; tok = strtok(nullptr, ";|"), line++) {
            while (*tok == ' ') tok++;
            char* dst = line == 0 ? g_cfg.found[0] : line == 1 ? g_cfg.found[8] : g_cfg.found[9];
            if (line >= 2 && dst[0]) strlcat(dst, ", ", sizeof(g_cfg.found[9]));
            strlcat(dst, tok, sizeof(g_cfg.found[0]));
        }
    }
    g_cfg.touchOff    = p.getBool("touchOff", false);
    g_cfg.haVoice     = p.getBool("haVoice", false);
    g_cfg.haWake      = p.getUChar("haWake", 0);
    g_cfg.haSensors   = p.getUChar("haSens", 0);
    if (g_cfg.haSensors > 2) g_cfg.haSensors = 0;
    g_cfg.quiet       = p.getUChar("quiet", 0);
    g_cfg.relay       = p.getUChar("relay", 0);
    if (g_cfg.relay > 2) g_cfg.relay = 0;
    if (g_cfg.haWake > 2) g_cfg.haWake = 0;
    p.getString("haKey", g_cfg.haKey, sizeof(g_cfg.haKey));
    g_cfg.meshWifi    = p.getBool("meshWifi", true);
    g_cfg.micGain     = p.getUChar("micGain", 4);
    g_cfg.foundAddr   = p.getBool("foundAddr", true);
    p.getString("libLast", g_cfg.libLast, sizeof(g_cfg.libLast));
    g_cfg.libLastOff  = p.getUInt("libOff", 0);
    g_cfg.libLastWrap = p.getBool("libWrap", true);
    g_cfg.clock12     = p.getBool("clk12", false);
    g_cfg.imperial    = p.getBool("imperial", false);
    g_cfg.lowBatPct   = p.getUChar("lbPct", 0);
    p.getBytes("lbTo", g_cfg.lowBatTo, 16);
    g_cfg.lowBatSent  = p.getBool("lbSent", false);
    g_cfg.opMode      = p.getUChar("opMode", 2);
    if (g_cfg.opMode > 2) g_cfg.opMode = 2;
    g_cfg.typFeat     = p.getUShort("typFeat", 0x79);
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
    for (int i = 0; i < 10; i++) {
        char k[6]; snprintf(k, sizeof(k), "fnd%d", i);
        p.putString(k, g_cfg.found[i]);
    }
    if (p.isKey("foundTxt")) p.remove("foundTxt");   // moved to fnd0..9
    p.putBool("touchOff", g_cfg.touchOff);
    p.putBool("haVoice", g_cfg.haVoice);
    p.putUChar("haWake", g_cfg.haWake);
    p.putUChar("haSens", g_cfg.haSensors);
    p.putUChar("quiet", g_cfg.quiet);
    p.putUChar("relay", g_cfg.relay);
    p.putString("haKey", g_cfg.haKey);
    p.putBool("meshWifi", g_cfg.meshWifi);
    p.putUChar("micGain", g_cfg.micGain);
    p.putBool("foundAddr", g_cfg.foundAddr);
    p.putString("libLast", g_cfg.libLast);
    p.putUInt("libOff", g_cfg.libLastOff);
    p.putBool("libWrap", g_cfg.libLastWrap);
    p.putBool("clk12", g_cfg.clock12);
    p.putBool("imperial", g_cfg.imperial);
    p.putUChar("lbPct", g_cfg.lowBatPct);
    p.putBytes("lbTo", g_cfg.lowBatTo, 16);
    p.putBool("lbSent", g_cfg.lowBatSent);
    p.putUChar("opMode", g_cfg.opMode);
    p.putUShort("typFeat", g_cfg.typFeat);
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

void bootCacheDest();
void setSelfHash(const uint8_t h[16]) {
    memcpy(g_selfHash, h, 16);
    g_haveSelf = true;
    hashToHex(h, g_destHex);
    bootCacheDest();
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
#if defined(SIMULATOR)
// Test hooks (simulator only): console "fake bat <pct|off>" and "fake move <metres north>".
int g_fakeBat = -1;
double g_fakeNorthM = 0;
#endif

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
    const uint32_t t0 = millis();
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(reg);
    const uint8_t e = Wire.endTransmission(false);
    const int got = e == 0 ? Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) : 0;
    if (millis() - t0 > 100) g_i2cSlowCount++;
    if (millis() - t0 > 100)                 // see readKeyboardIntoQueue: the 1 s I2C waits
        Serial.printf("[I2C] gauge reg 0x%02x %lu ms, end %u got %d\n", reg, (unsigned long)(millis() - t0), e, got);
    if (e != 0 || got != 2) return false;
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
// carries it. Runtimes (S.5.12), "time left" and the battery page use it once there is one.
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

#if defined(SIMULATOR)
    if (g_fakeBat >= 0) pct = g_fakeBat;      // console "fake bat <pct>" (tests)
#endif
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
// SD tools (S.11.5 rows 8-10): back up / restore, firmware update. Defined after the USB block.
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
    g_haveMaps = g_sdStore.exists(SD_PATH_ROOT "/maps") || g_sdStore.exists(SD_PATH_ROOT "/packs");
    g_haveLib = false;
    if (File d = g_sdStore.openDir(SD_PATH_ROOT "/library")) {
        for (File e = d.openNextFile(); e && !g_haveLib; e = d.openNextFile()) {
            const char* nm = strrchr(e.name(), '/');
            nm = nm ? nm + 1 : e.name();
            g_haveLib = !e.isDirectory() && nm[0] != '.';
            e.close();
        }
        d.close();
    }
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
    mapview::forget();                      // another card may hold other maps
    if (g_sdMounted) { g_sdStore.end(); g_sdMounted = false; }
    g_haveMaps = false;
    g_haveLib = false;
    snprintf(g_sdStatus, sizeof(g_sdStatus), "not mounted");
    Serial.println("[SD] unmounted");
}

// Is the mounted card still there? sdMount() trusts an earlier mount, so a card pulled while
// the radio runs looked present until a restart. One stat over SPI; used where a person asks
// (trip check, console sdinfo), not on every card access.
bool sdProbe() {
    if (!g_sdMounted) return sdMount();
    if (g_sdStore.exists(SD_PATH_ROOT)) return true;
    Serial.println("[SD] card removed");
    sdUnmount();
    snprintf(g_sdStatus, sizeof(g_sdStatus), "no card");
    return sdMount();                        // another card may be in by now
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
#if defined(SIMULATOR)
    la += g_fakeNorthM / 111320.0;            // console "fake move <metres north>" (tests)
#endif
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
int8_t g_gpsPsmSent = -1;          // operate mode last sent (-1: send again)
uint32_t g_gpsRmcAt = 0;           // millis() of the last good RMC sentence
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
    g_gpsPsmSent = -1;                 // power-save setting goes out once it talks
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "searching");
    Serial.printf("[GPS] on RX=%d TX=%d baud=%d\n", GPS_RX, GPS_TX, GPS_BAUD);
#else
    snprintf(g_gpsStatus, sizeof(g_gpsStatus), "n/a");
#endif
}

void ubxSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len);

// GPS on while the CPU sleeps (Balanced): the receiver may use its own power-save mode
// (u-blox M10 PSMCT: about half the current once it has a fix). Full keeps full power.
bool gpsSleepsWithCpu() { return g_pwEff.gps == power::GPS_ON && g_pwEff.wakeSecs != 0; }

void gpsSetPowerSave(bool on) {
    if (!g_gpsSerial) return;
    const uint8_t pl[9] = {0, 0x01, 0, 0, 0x01, 0x00, 0xd0, 0x20, (uint8_t)(on ? 2 : 0)};   // RAM, CFG-PM-OPERATEMODE
    ubxSend(0x06, 0x8A, pl, sizeof(pl));
    g_gpsPsmSent = on ? 1 : 0;
    Serial.printf("[GPS] receiver power save %s\n", on ? "on (PSMCT)" : "off (full power)");
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
        {0x20d00001, 0},   // CFG-PM-OPERATEMODE = FULL (gpsSetPowerSave sets PSMCT later)
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

// Time without a position fix: the receiver knows UTC once it hears a satellite (or from its
// backup clock), well before it can compute a position. The ESP32's own clock drifts a lot in
// sleep (tens of minutes a day), so set it from this when a satellite is heard and the clock
// at most every 10 min. Not marked "synchronized" (only a fix does that).
void gpsTimeNoFix(const char* hms, const char* dmy) {
    static uint32_t last = 0;
    if (!g_cfg.gpsAutoSync || (last && millis() - last < 600000UL)) return;
    int inView = 0, heard = 0, best = 0;
    gpsSkySummary(inView, heard, best);
    if (heard == 0) return;
    const int yr = 2000 + twoDigits(dmy + 4);
    if (yr < 2026 || yr > 2079) return;
    const int mo = twoDigits(dmy + 2), d = twoDigits(dmy), hh = twoDigits(hms), mi = twoDigits(hms + 2),
              ss = twoDigits(hms + 4);
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mi > 59 || ss > 60) return;
    const long long now = (long long)time(nullptr);
    if (!setClockUtc(yr, mo, d, hh, mi, ss)) return;
    const long long gps = (long long)time(nullptr);
    last = millis() | 1;
    if (llabs(gps - now) > 5) Serial.printf("[RTC] set from GPS time (no fix yet), was %+llds off\n", now - gps);
}

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
        g_gpsRmcAt = millis() | 1;
        if (g_gpsPsmSent != (gpsSleepsWithCpu() ? 1 : 0)) gpsSetPowerSave(gpsSleepsWithCpu());
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
        else if (!valid && fld[1][0] && strlen(fld[1]) >= 6 && strlen(fld[9]) >= 6) gpsTimeNoFix(fld[1], fld[9]);
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

extern uint32_t g_lastInputMs;
void pumpOnce() {
    handheld::storage::MessageTransactions::userActiveAtMs() = g_lastInputMs;   // index save waits for a pause in use
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
    const bool quiet = quietNow();
    if (g_cfg.beep && !quiet) g_audio.requestMessage();
    if (g_cfg.vibe && !quiet) vibrate(40);
    if (quiet) Serial.println("[ALERT] quiet hours: no sound");
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

bool grpName(const uint8_t h[16], char* out, size_t n);   // a group's row / thread address ("Groups: data")

void peerName(const uint8_t peer[16], char* out, size_t n) {
    if (isSelf(peer)) { snprintf(out, n, "%s", g_selfName); return; }
    if (grpName(peer, out, n)) return;
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

// maxSpin: in 5 ms units, as before (600 = 3 s). Polls each 1 ms (was a 5 ms sleep per try:
// a list of conversations waits once per row, so the Inbox opened up to 5x slower).
bool waitResult(MessageStore::Ticket t, MessageStore::Result& res, int maxSpin = 600) {
    const uint32_t until = millis() + (uint32_t)maxSpin * 5;
    do {
        g_msgStore.poll();
        if (g_msgStore.peekResult(t, res)) return true;
        delay(1);
    } while ((int32_t)(millis() - until) < 0);
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
    int pages = 0, selfConvs = 0;
    long unreadSum = 0;
    while (g_convCount < limit && pages < 8) {
        const int n = fetchConversationPage(sel, limit - g_convCount, haveLast ? &last : nullptr);
        if (n < 0) return;                      // keep "not valid": retried next time
        pages++;
        for (int i = 0; i < n && g_convCount < limit; i++) {
            if (isSelf(sel[i].cursor.peer)) { selfConvs++; continue; }
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
            unreadSum += unread;
            g_convCount++;
        }
        if (n < kConvPage || n == 0) break;   // last page
        last = sel[n - 1].cursor;
        haveLast = true;
    }
    g_convRev = rev;
    g_convValid = true;
    Serial.printf("[LIST] %d conversations in %lu ms (%d pages), unread %ld listed, store total %d, self %d\n",
                  g_convCount, (unsigned long)(millis() - t0), pages, unreadSum, g_msgStore.totalUnreadCount(), selfConvs);
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
            snprintf(r->sub, sizeof(r->sub), "%.8s.. %ddBm %um", hex, node.rssi, ageM);
        else
            snprintf(r->sub, sizeof(r->sub), "%.12s.. %um", hex, ageM);
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
bool    g_shareAll = false;        // ... and gets every contact's card (Settings S.7)
bool    g_pickRt = false;          // ... or is the range-test partner (Settings S.8)
bool    g_pickLowbat = false;      // ... or gets the low-battery notice (Settings S.6)
uint8_t g_sharePeer[16];
// Groups screens, open over Contacts (see "Groups: screens").
enum GrpView : uint8_t { GV_NONE, GV_LIST, GV_PAGE, GV_MEMBERS, GV_SHARE };
uint8_t  g_gv = GV_NONE;
int      g_gvSel = 0, g_gvScroll = 0, g_gvGroup = 0;
uint32_t g_gvDelAt = 0;                // Delete pressed once (again within 3 s deletes)

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

// ---- Groups: data (Contacts > G; docs/HowTo.md "Groups"; docs/groups_report.md) -----------
// A group is a name, a random 4-byte id and up to kGrpMembers contacts, kept on this radio
// (NVS "rs_groups"). A message to a group is one ordinary direct message per member through
// the usual send path / outbox, tagged with LXMF FIELD_GROUP (protocol/LxmfFields.h) so a
// radio that has a group with the same id shows it in that group's thread. Nothing new goes
// on the air and there is no server: every radio keeps its own copy of the member list.
// kGrpMembers = kOutboxCap: one message is 1 send now + the rest in the outbox, 3.5 s apart.
constexpr int kGrpMax = 8, kGrpMembers = 8;
struct Group { uint8_t id[4]; char name[17]; uint8_t n; uint8_t m[kGrpMembers][16]; };
Group g_grp[kGrpMax];
int g_grpCount = 0;

// A group stands in list rows and as the open thread's "peer" with this made-up address:
// 12 fixed bytes, then the group id. It is never sent to (sendMessage refuses it).
const uint8_t kGrpHashMark[12] = {'R', 'a', 't', 'S', 'p', 'e', 'a', 'k', 'G', 'r', 'p', 0};
bool isGrpHash(const uint8_t h[16]) { return h && !memcmp(h, kGrpHashMark, 12); }
void grpHash(const uint8_t id[4], uint8_t out[16]) { memcpy(out, kGrpHashMark, 12); memcpy(out + 12, id, 4); }
int grpFind(const uint8_t id[4]) {
    for (int i = 0; i < g_grpCount; i++) if (!memcmp(g_grp[i].id, id, 4)) return i;
    return -1;
}
int grpOfHash(const uint8_t h[16]) { return isGrpHash(h) ? grpFind(h + 12) : -1; }
int grpMember(const Group& g, const uint8_t h[16]) {
    for (int i = 0; i < g.n; i++) if (!memcmp(g.m[i], h, 16)) return i;
    return -1;
}
bool grpName(const uint8_t h[16], char* out, size_t n) {
    if (!isGrpHash(h)) return false;
    const int gi = grpFind(h + 12);
    snprintf(out, n, "%s", gi >= 0 ? g_grp[gi].name : "(group)");
    return true;
}

uint8_t g_grpBlob[2 + sizeof(g_grp)];                 // save / load buffer (main loop only)

void grpSave() {
    uint8_t* const buf = g_grpBlob;
    Preferences p;
    if (!p.begin("rs_groups", false)) { Serial.println("[GRP] save failed (nvs)"); return; }
    if (g_grpCount) {
        buf[0] = 1;                                      // format
        buf[1] = (uint8_t)g_grpCount;
        memcpy(buf + 2, g_grp, sizeof(Group) * (size_t)g_grpCount);
        p.putBytes("g1", buf, 2 + sizeof(Group) * (size_t)g_grpCount);
    } else {
        p.remove("g1");
    }
    p.end();
}

void grpLoad() {
    g_grpCount = 0;
    Preferences p;
    if (!p.begin("rs_groups", true)) return;
    uint8_t* const buf = g_grpBlob;
    const size_t n = p.getBytesLength("g1");
    if (n >= 2 && n <= sizeof(g_grpBlob) && p.getBytes("g1", buf, n) == n && buf[0] == 1 &&
        buf[1] <= kGrpMax && n == 2 + sizeof(Group) * (size_t)buf[1]) {
        g_grpCount = buf[1];
        memcpy(g_grp, buf + 2, sizeof(Group) * (size_t)g_grpCount);
        for (int i = 0; i < g_grpCount; i++) {           // never trust stored lengths
            g_grp[i].name[sizeof(g_grp[i].name) - 1] = 0;
            if (g_grp[i].n > kGrpMembers) g_grp[i].n = kGrpMembers;
        }
    }
    p.end();
    if (g_grpCount) Serial.printf("[GRP] %d groups loaded\n", g_grpCount);
}

// Group card: a group is shared as one plain-text message per member (the sharer included),
//   "Group card: <group name> #<8 hex id> <part>/<parts>\n<member name>\n<32 hex address>"
// readable on any LXMF client. This firmware shows "[group] name 2/4 member" and Enter on an
// empty draft joins the group once every part is in the chat (grpJoinFromThread).
constexpr const char* kGrpCardPrefix = "Group card: ";
int grpCardFormat(const Group& g, int part, int parts, const uint8_t hash[16], const char* mname, char* out, size_t n) {
    char hex[33];
    hashToHex(hash, hex);
    return snprintf(out, n, "%s%s #%02x%02x%02x%02x %d/%d\n%.23s\n%s", kGrpCardPrefix, g.name, g.id[0], g.id[1],
                    g.id[2], g.id[3], part, parts, mname, hex);
}

bool grpCardParse(const char* s, size_t len, uint8_t id[4], char gname[17], int& part, int& parts,
                  uint8_t hash[16], char mname[24]) {
    const size_t pl = strlen(kGrpCardPrefix);
    if (len < pl + 1 + 2 + 8 + 4 + 1 + 1 + 1 + 32 || strncmp(s, kGrpCardPrefix, pl) != 0) return false;
    size_t eol = pl;
    while (eol < len && s[eol] != '\n' && s[eol] != '\r') eol++;
    size_t hashAt = eol;                                 // the last '#' on the first line
    for (size_t i = pl; i < eol; i++) if (s[i] == '#') hashAt = i;
    if (hashAt >= eol || hashAt < pl + 2 || s[hashAt - 1] != ' ' || hashAt + 9 + 3 > eol) return false;
    size_t gn = 0;
    for (size_t i = pl; i + 1 < hashAt && gn < 16; i++)
        if ((uint8_t)s[i] >= 32 && (uint8_t)s[i] < 127) gname[gn++] = s[i];
    gname[gn] = 0;
    for (int k = 0; k < 4; k++) {
        const int h = hexNib(s[hashAt + 1 + 2 * k]), l = hexNib(s[hashAt + 2 + 2 * k]);
        if (h < 0 || l < 0) return false;
        id[k] = (uint8_t)((h << 4) | l);
    }
    size_t i = hashAt + 9;
    if (s[i++] != ' ') return false;
    part = parts = 0;
    while (i < eol && s[i] >= '0' && s[i] <= '9') part = part * 10 + (s[i++] - '0');
    if (i >= eol || s[i++] != '/') return false;
    while (i < eol && s[i] >= '0' && s[i] <= '9') parts = parts * 10 + (s[i++] - '0');
    if (part < 1 || parts < 2 || part > parts || parts > kGrpMembers + 1 || !gn) return false;
    i = eol;
    while (i < len && (s[i] == '\n' || s[i] == '\r')) i++;
    size_t mn = 0;
    while (i < len && s[i] != '\n' && s[i] != '\r') {
        if (mn < 23 && (uint8_t)s[i] >= 32 && (uint8_t)s[i] < 127) mname[mn++] = s[i];
        i++;
    }
    mname[mn] = 0;
    while (i < len && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) i++;
    if (i + 32 > len || !hexToHash(s + i, hash)) return false;
    return mn > 0;
}

// The open chat's newest incoming group item: a message of a group this radio has (Enter on
// an empty draft opens the group), or a group card (Enter joins / updates the group).
struct InGrp { bool valid; bool join; uint8_t id[4]; char name[17]; uint8_t parts; uint32_t counter; };
InGrp g_threadGrp = {};

extern char g_placeLast[28];                // place search (map F): last place jumped to
// Waypoints + track data (functions in "Waypoints + breadcrumb track" below).
struct Wpt { float lat, lon; char name[16]; uint32_t at; };
constexpr int kWptMax = 32;
Wpt g_wpt[kWptMax];
int g_wptCount = 0;
int g_wptSel = -1;                           // selected on the map (N), -1 = none
// Home point (S.2 rows 13-15, map G with nothing selected, Alt+H): one place the arrow can
// always point to. NVS rs_wpt "hset" / "hlat" / "hlon".
struct HomePt { float lat = 0, lon = 0; bool set = false; } g_home;
void homeSet(double lat, double lon, const char* how);
void homeGo();
bool homeText(char* out, size_t n);
bool sunText(char* out, size_t n);
extern bool g_heldOn;
extern int g_heldCount;
void heldSave();
void heldPrint();
// Trip check (S.10 Diagnostics > Ready for a trip?, console "trip"): what a person should
// have set up before the radio goes away for months. OK / TO DO per line.
struct TripItem { bool ok; char name[18]; char note[44]; };
TripItem g_trip[12];
int g_tripN = 0, g_tripTodo = 0;
void tripRun();
uint32_t g_wptDelAt = 0;                     // Del pressed once (confirm within 3 s)
constexpr int kTrackRam = 1000;
float* g_track = nullptr;                    // kTrackRam * 2 floats, in PSRAM (psramGlobals)
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
int  gpxImport();
enum MarkView : uint8_t { MK_NONE, MK_LIST, MK_ONE, MK_NEW, MK_SEND, MK_NEAR };
MarkView g_mk = MK_NONE;            // marker pages (J on the Atlas)
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
bool batParse(const char* s, size_t len, int& pct, bool* autoUpdate = nullptr);
void batNote(const uint8_t dest[16], int pct, uint32_t at);
bool markRowText(const char* s, size_t len, char* out, size_t n, double* lat = nullptr, double* lon = nullptr);
void markIncoming(const uint8_t from[16], const char* text, uint32_t at);
void sosIncoming(const uint8_t from[16], const char* text);
void sosTargetText(char* out, size_t n);
bool sosActive();
void sosNextTarget();
bool sosBtnOn();
void sosToggleBtn();
bool markInfo(char* out, size_t n);
extern int g_markSel;
extern bool g_layOpen;
void renderLayers();
void layClose();
void layKey(const KeyEvent& ev);
void layOpen();
// ATAK feed (see "ATAK feed" below)
// link: kCotLinkWifi / kCotLinkBle (CotBle.h). Byte 1: the boot check (btInUse) reads it.
struct CotCfg { uint8_t on; uint8_t link; uint16_t port; uint32_t ip; uint16_t every; uint16_t pad2; };
CotCfg g_cot = {0, 0, cot::kSaPort, cot::kSaGroup, 30, 0};
uint32_t g_cotLast = 0, g_cotSent = 0;
bool g_cotListen = false;
void cotSave();
void cotIp(uint32_t ip, char* out, size_t n);
bool cotUseBle();
const char* powerSleepBlocker();
void cotBleName(char* out, size_t n);
void cotSetLink(uint8_t link);
void cposNote(const uint8_t dest[16], double lat, double lon, uint32_t at);
uint32_t msgEpoch(double ts);
// Enter on an empty draft: the newest of an unsaved card / a location wins.
void gpsPosText(char* out, size_t n);
int ashareActive();
void ashareStopAll();
bool locOffered() { return g_threadLoc.valid && (!cardOffered() || g_threadLoc.counter > g_threadCard.counter); }
// ... and a group item newer than both wins over them.
bool grpOffered() {
    if (!g_threadGrp.valid) return false;
    if (locOffered()) return g_threadGrp.counter > g_threadLoc.counter;
    if (cardOffered()) return g_threadGrp.counter > g_threadCard.counter;
    return true;
}

// ---- Thread history ------------------------------------------------------------
constexpr int HIST_ROWS = 6;
struct HistRow { char text[96]; char status[36]; bool incoming; bool autoLoc; bool voice; uint32_t counter; };   // text wraps (bubbleWrap)
HistRow g_hist[HIST_ROWS];
int  g_histCount = 0;
char g_histNote[24] = "";

MessageStore::Ticket g_markReadTicket{};
bool g_markReadPending = false;
char g_markReadNext[33] = "";       // a thread opened while another was still being marked
// Marked read 2 records per store request: one request for a whole backlog (100+ unread, each a
// ~1 s flash rewrite) held the store for minutes - every send froze ~8-12 s waiting to be saved,
// threads opened empty and delivery-status updates were refused (stress test 2026-11-15).
constexpr uint32_t kMarkReadBatch = 2;
char g_markReadHex[33] = "", g_markReadRetry[33] = "";
void markReadStart(const char* hex) {
    auto mr = g_msgStore.requestMarkRead(std::string(hex), 0, 0, kMarkReadBatch);
    if (mr.accepted()) { g_markReadTicket = mr.ticket; g_markReadPending = true; strlcpy(g_markReadHex, hex, 33); }
    else strlcpy(g_markReadRetry, hex, sizeof(g_markReadRetry));   // slot busy (a status update): next loop
}

// markRead false: a refresh (the store changed) with no new incoming message - marking read again
// finished with a store revision bump, which reloaded the thread, which marked read again: a
// 330 ms loop every 2.5 s for as long as a thread was open (stress test 2026-11-16).
uint32_t g_histMarkedAt = UINT32_MAX;   // incomingCommitted() when the open thread was last marked read
void loadGroupHistory(const uint8_t peer[16], bool markRead);

// The group tag of a stored record, if it has one (a second small read: the fields follow
// title and content). A fields map longer than a group tag is a voice message.
bool recordGroupTag(const handheld::storage::RecordKey& key, const handheld::storage::StoredRecordHeader& hdr,
                    handheld::lxmf::GroupTag& tag) {
    using namespace handheld::storage;
    if (!hdr.fieldsLength || hdr.fieldsLength > handheld::lxmf::GroupFieldMax) return false;
    uint8_t buf[sizeof(StoredRecordHeader) + 64];
    auto rs = g_msgStore.requestRecord(key, hdr.titleLength + hdr.contentLength, sizeof(buf), true);
    if (!rs.accepted()) return false;
    MessageStore::Result rr;
    size_t got = 0;
    if (waitResult(rs.ticket, rr) && rr.outcome == Outcome::Committed && rr.error == Error::None) {
        got = rr.length < sizeof(buf) ? rr.length : sizeof(buf);
        if (!g_msgStore.readPayload(rs.ticket, buf, got)) got = 0;
    }
    g_msgStore.releaseResult(rs.ticket);
    return got > sizeof(StoredRecordHeader) &&
           handheld::lxmf::findGroup(buf + sizeof(StoredRecordHeader), got - sizeof(StoredRecordHeader), tag);
}

void loadHistory(const uint8_t peer[16], bool markRead = true) {
    using namespace handheld::storage;
    if (isGrpHash(peer)) { loadGroupHistory(peer, markRead); return; }
    g_histCount = 0;
    g_histNote[0] = 0;
    g_threadCard.valid = false;
    g_threadLoc.valid = false;
    g_threadGrp.valid = false;
    if (!g_storeReady) { snprintf(g_histNote, sizeof(g_histNote), "(store off)"); return; }

    char hex[33];
    hashToHex(peer, hex);
    const std::string peerHex(hex);

    const uint32_t t0 = millis();
    auto sub = g_msgStore.requestHistoryPage(peerHex, HistoryEntry{}, (uint8_t)HIST_ROWS,
                                             HistoryDirection::Before);
    for (int a = 0; a < 40 && !sub.accepted(); a++) {   // store busy (a write): up to 2 s
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
        h.counter = entries[i].counter;
        handheld::lxmf::GroupTag gtag;
        const bool tagged = recordGroupTag(key, hdr, gtag);
        h.voice = hdr.fieldsLength > 0 && !tagged; // kept fields: the audio (a voice message) or a group tag
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
        int batPct = 0;
        bool autoBat = false;
        const bool isBat = !isCard && batParse((const char*)buf + off, clen, batPct, &autoBat);
        if (isBat) {                             // a status share: battery alone, or after the position
            const size_t at = isLoc ? strlen(h.text) : 0;
            if (isLoc) snprintf(h.text + at, sizeof(h.text) - at, " bat %d%%", batPct);
            else snprintf(h.text, sizeof(h.text), "[bat%s] %d%%", autoBat ? " auto" : "", batPct);
            if (autoBat) h.autoLoc = true;
            if (h.incoming) batNote(peer, batPct, msgEpoch(hdr.timestamp));
        }
        double mkLa = 0, mkLo = 0;               // a shared marker: "[marker] hazard 640 m NE of you: note"
        const bool isMark = !isCard && !isLoc && markRowText((const char*)buf + off, clen, h.text, sizeof(h.text), &mkLa, &mkLo);
        if (isMark && h.incoming && (!g_threadLoc.valid || entries[i].counter >= g_threadLoc.counter))
            g_threadLoc = {true, mkLa, mkLo, entries[i].counter};
        if (clen >= sizeof(h.text)) clen = sizeof(h.text) - 1;
        const size_t wtLen = strlen(walkie::kInvitePrefix);   // walkie-talkie invite: not its key
        const bool isWt = !isCard && !isLoc && clen >= wtLen && !memcmp(buf + off, walkie::kInvitePrefix, wtLen);
        if (isWt) snprintf(h.text, sizeof(h.text), "[walkie-talkie call]");
        if (h.voice && !isWt) {                  // ~87.5 B per second of Codec2 700C
            snprintf(h.text, sizeof(h.text), "[voice ~%u s] %.*s", (unsigned)((hdr.fieldsLength + 44) / 88),
                     (int)(clen < 60 ? clen : 60), (const char*)buf + off);
        }
        uint8_t gcId[4], gcHash[16];
        char gcName[17], gcMember[24];
        int gcPart = 0, gcParts = 0;
        const bool isGrpCard = !isCard && !isLoc && !isWt && !h.voice &&
                               grpCardParse((const char*)buf + off, hdr.contentLength < got - off ? hdr.contentLength : got - off,
                                            gcId, gcName, gcPart, gcParts, gcHash, gcMember);
        if (isGrpCard) {
            snprintf(h.text, sizeof(h.text), "[group] %s %d/%d %s", gcName, gcPart, gcParts, gcMember);
            // Offer to join unless this radio already has the group with that many members.
            const int have = grpFind(gcId);
            const bool same = have >= 0 && g_grp[have].n == gcParts - 1;
            if (h.incoming && !same && (!g_threadGrp.valid || entries[i].counter >= g_threadGrp.counter)) {
                g_threadGrp = {};
                g_threadGrp.valid = true; g_threadGrp.join = true; g_threadGrp.counter = entries[i].counter;
                g_threadGrp.parts = (uint8_t)gcParts;
                memcpy(g_threadGrp.id, gcId, 4);
                strlcpy(g_threadGrp.name, gcName, sizeof(g_threadGrp.name));
            }
        }
        if (clen && !isCard && !isLoc && !isBat && !isMark && !isWt && !h.voice && !isGrpCard) {
            size_t pre = 0;
            if (tagged) {                            // a group message: "[Family] text"
                pre = (size_t)snprintf(h.text, sizeof(h.text), "[%s] ", gtag.name);
                const int have = grpFind(gtag.id);
                if (h.incoming && have >= 0 && (!g_threadGrp.valid || entries[i].counter >= g_threadGrp.counter)) {
                    g_threadGrp = {};
                    g_threadGrp.valid = true; g_threadGrp.counter = entries[i].counter;
                    memcpy(g_threadGrp.id, gtag.id, 4);
                    strlcpy(g_threadGrp.name, g_grp[have].name, sizeof(g_threadGrp.name));
                }
            }
            if (clen > sizeof(h.text) - 1 - pre) clen = sizeof(h.text) - 1 - pre;
            memcpy(h.text + pre, buf + off, clen);
            h.text[pre + clen] = 0;
            for (size_t k = pre; k < pre + clen; k++)
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
    const uint32_t tHist = millis() - t0;

    // Mark read after the page is on hand, without waiting: with 100+ unread the store took
    // over 2 s, the wait gave up while it was still busy, the history request was turned
    // away and the thread showed nothing. markReadTick() collects the result.
    if (markRead) {
        g_histMarkedAt = g_msgStore.incomingCommitted();
        if (g_markReadPending) strlcpy(g_markReadNext, hex, sizeof(g_markReadNext));   // after the one running
        else markReadStart(hex);
    }
    int ri = findRow(peer);
    if (ri >= 0) g_rows[ri].unread = 0;
    g_convValid = false;
    Serial.printf("[HIST] %d messages in %lu ms\n", g_histCount, (unsigned long)tHist);
}

// A group thread marks its members' chats read one after another (loadGroupHistory).
char g_grpMarkQ[kGrpMembers][33];
int  g_grpMarkN = 0;

void markReadTick() {
    if (!g_markReadPending) {
        if (g_markReadRetry[0]) {
            char h[33];
            strlcpy(h, g_markReadRetry, sizeof(h));
            g_markReadRetry[0] = 0;
            markReadStart(h);
        } else if (g_grpMarkN && !g_markReadNext[0]) {
            markReadStart(g_grpMarkQ[--g_grpMarkN]);
        }
        return;
    }
    MessageStore::Result r;
    g_msgStore.poll();
    if (!g_msgStore.peekResult(g_markReadTicket, r)) return;
    g_msgStore.releaseResult(g_markReadTicket);
    g_markReadPending = false;
    static uint32_t batchTotal = 0;
    batchTotal += r.total;
    if (r.total >= kMarkReadBatch && r.outcome == handheld::storage::Outcome::Committed) {
        // More to mark: the next batch (other store work goes in between). Another thread opened
        // meanwhile goes first; this one carries on after it.
        if (g_markReadNext[0]) {
            char rest[33];
            strlcpy(rest, g_markReadHex, sizeof(rest));
            markReadStart(g_markReadNext);
            strlcpy(g_markReadNext, rest, sizeof(g_markReadNext));
        } else {
            markReadStart(g_markReadHex);
        }
        return;
    }
    g_convValid = false;
    Serial.printf("[HIST] marked read: outcome %u error %u, %lu records\n", (unsigned)r.outcome, (unsigned)r.error,
                  (unsigned long)batchTotal);
    batchTotal = 0;
    if (g_markReadNext[0]) { markReadStart(g_markReadNext); g_markReadNext[0] = 0; }
}

// ---- Send ------------------------------------------------------------------------
char g_txStr[16] = "";
bool g_txTransient = false;   // last sendMessage failure will pass by itself (busy, spacing)
lv_obj_t* g_root = nullptr;

enum class TxStep { Done, Rejected, Timeout, Late, Invalid };

// Submissions the engine accepted but had not saved within txOnce's wait (kTxWaitMs; a slow
// flash write: 55 s once in a simulator run). The engine still saves and sends them, so they
// count as sent - reporting "failed" made the outbox / the user send them again (a duplicate)
// and left the ticket unacknowledged, holding one of the engine's 20 slots for good. The main
// loop finishes them (txLateTick). 20 entries = the engine's rows, so one is always free.
handheld::outgoing::Ticket g_txLate[20];
// How long a send keeps the screen and keys waiting for the store. It was 800 loops (8 s by
// the count, 12-15 s measured): the unit did not answer for that long whenever the store was
// slow. Most saves take under a second; the rest finish in the background.
constexpr uint32_t kTxWaitMs = 1500;
void toast(const char* fmt, ...);

// Why sends are refused as "busy": the engine's outgoing rows and the store's write slots.
void txDescribe() {
    static char (&buf)[2200] = psram<char, 2200>();
    g_proto.lxmfDescribeRows(buf, sizeof(buf));
    char slots[16];
    g_msgStore.writeQueue().describe(slots, sizeof(slots));
    int late = 0;
    for (auto& t : g_txLate) late += t.valid() ? 1 : 0;
    Serial.printf("[TX] store slots %s (F free, Q queued, R running, D done not taken), %d late, %d queued\n",
                  slots, late, g_proto.lxmfQueuedCount());
    Serial.print(buf[0] ? buf : "[TX] no outgoing rows in use\n");
}

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
              handheld::outgoing::InitialResult& ir, unsigned& rejection,
              const uint8_t* fields = nullptr, size_t fieldsLen = 0) {
    auto sub = fieldsLen ? g_proto.lxmfSubmitFields(dest, nullptr, 0, (const uint8_t*)body, n, fields, fieldsLen, nullptr, 0, false)
                         : g_proto.lxmfSubmit(dest, nullptr, 0, (const uint8_t*)body, n, false);
    if (!sub.accepted()) { rejection = (unsigned)sub.rejection; return TxStep::Rejected; }
    for (const uint32_t t0 = millis(); millis() - t0 < kTxWaitMs;) {
        pumpOnce();
        auto p = g_proto.lxmfPoll(sub.ticket, ir);
        if (p == handheld::outgoing::Poll::Pending) { delay(10); continue; }
        if (p == handheld::outgoing::Poll::Ready) { g_proto.lxmfAcknowledge(sub.ticket); return TxStep::Done; }
        return TxStep::Invalid;
    }
    for (auto& t : g_txLate)
        if (!t.valid()) { t = sub.ticket; return TxStep::Late; }
    return TxStep::Timeout;                      // cannot happen: the list is as long as the rows
}

// gid / gseq: the message is one member's copy of a group message (see "Groups: data"): it
// carries the group tag while this radio still has that group, else it goes as plain text.
// g_txExtra: one more fields-map entry for the sends made while it is set (the Sideband
// telemetry of a one-time status share, see "Location sharing"). A copy that has to wait in
// the outbox goes without it (the text says the same).
uint8_t g_txExtra[handheld::lxmf::TelemetryEntryMax];
size_t  g_txExtraLen = 0;

bool sendMessage(const uint8_t dest[16], const char* text, const uint8_t* gid = nullptr, uint32_t gseq = 0) {
    g_txTransient = false;
    if (!g_protoReady) { snprintf(g_txStr, sizeof(g_txStr), "proto off"); return false; }
    if (isSelf(dest))  { snprintf(g_txStr, sizeof(g_txStr), "pick remote"); return false; }
    if (isGrpHash(dest)) { snprintf(g_txStr, sizeof(g_txStr), "group?"); return false; }   // a group's row address

    uint8_t fields[handheld::lxmf::FieldHeadMax + handheld::lxmf::TelemetryEntryMax];
    size_t fieldsLen = 0;
    const int gi = gid ? grpFind(gid) : -1;
    if (gi >= 0 && handheld::lxmf::available()) {
        handheld::lxmf::GroupTag tag;
        memcpy(tag.id, gid, 4);
        tag.seq = gseq;
        strlcpy(tag.name, g_grp[gi].name, sizeof(tag.name));
        fieldsLen = handheld::lxmf::makeGroupField(tag, fields, sizeof(fields));
    }
    if (g_txExtraLen && handheld::lxmf::available())
        fieldsLen = handheld::lxmf::appendFieldEntry(fields, fieldsLen, sizeof(fields), g_txExtra, g_txExtraLen);

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
        TxStep st = txOnce(dest, body, n, ir, rej, fields, fieldsLen);
        if (st == TxStep::Rejected) {
            g_txTransient = rej == 1 || rej == 8;   // Busy / Recovering
            static uint32_t busySince = 0, toldAt = 0, lastBusy = 0;
            static bool busyRun = false, told = false;
            if (rej == 1) {
                const uint32_t now = millis();
                if (!busyRun || now - lastBusy > 30000) { busyRun = true; busySince = now; }
                lastBusy = now;
                if (now - busySince > 60000 && (!told || now - toldAt > 600000)) {
                    told = true; toldAt = now;
                    Serial.printf("[TX] busy for %lu s\n", (unsigned long)((now - busySince) / 1000));
                    txDescribe();
                }
            }
            snprintf(g_txStr, sizeof(g_txStr), g_txTransient ? "busy" : "rejected %u", rej);
            return false;
        }
        if (st == TxStep::Timeout) { snprintf(g_txStr, sizeof(g_txStr), "timeout"); return false; }
        if (st == TxStep::Late) {                // accepted, still saving: it will go out
            lastTx = millis();
            snprintf(g_txStr, sizeof(g_txStr), "saving");
            Serial.printf("[TX] saving (slow store) \"%s\"\n", body);
            soakSent(body);
            return true;
        }
        if (st == TxStep::Invalid) { snprintf(g_txStr, sizeof(g_txStr), "invalid"); return false; }
        if (ir.outcome == handheld::storage::Outcome::Committed) {
            lastTx = millis();
            snprintf(g_txStr, sizeof(g_txStr), "%s", ir.txSuppressed ? "saved" : "sent");
            Serial.printf("[TX] %s \"%s\"\n", g_txStr, body);
            soakSent(body);
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
// gid / gseq: a group message's copy (all zero = an ordinary message); kept in blob "g2",
// 8 bytes per item in the order of "q2" (absent or another length: no item is a group copy).
struct OutboxItem { uint8_t dest[16]; char text[161]; uint8_t gid[4]; uint32_t gseq; };
struct OutboxItemV1 { uint8_t dest[16]; char text[161]; };   // blob "q" of old firmware
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
        uint8_t g[kOutboxCap * 8];
        bool any = false;
        for (int i = 0; i < g_outboxCount; i++) {
            memcpy(g + i * 8, g_outbox[i].gid, 4);
            memcpy(g + i * 8 + 4, &g_outbox[i].gseq, 4);
            any = any || g_outbox[i].gseq || memcmp(g_outbox[i].gid, "\0\0\0", 4);
        }
        if (any) p.putBytes("g2", g, (size_t)g_outboxCount * 8);
        else if (p.isKey("g2")) p.remove("g2");
    } else {
        p.remove("q2");
        if (p.isKey("g2")) p.remove("g2");
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
            uint8_t g[kOutboxCap * 8];
            if (p.getBytesLength("g2") == (size_t)g_outboxCount * 8 && p.getBytes("g2", g, (size_t)g_outboxCount * 8)) {
                for (int i = 0; i < g_outboxCount; i++) {
                    memcpy(g_outbox[i].gid, g + i * 8, 4);
                    memcpy(&g_outbox[i].gseq, g + i * 8 + 4, 4);
                }
            }
        }
    } else if ((n = p.getBytesLength("q")) && n % sizeof(OutboxItemV1) == 0 && n <= sizeof(OutboxItemV1) * kOutboxCap) {
        OutboxItemV1* old = (OutboxItemV1*)malloc(sizeof(OutboxItemV1) * kOutboxCap);
        if (old) {
            g_outboxCount = (int)(p.getBytes("q", old, n) / sizeof(OutboxItemV1));
            for (int i = 0; i < g_outboxCount; i++) {
                memset(&g_outbox[i], 0, sizeof(g_outbox[i]));
                memcpy(g_outbox[i].dest, old[i].dest, 16);
                old[i].text[sizeof(old[i].text) - 1] = 0;
                strlcpy(g_outbox[i].text, old[i].text, sizeof(g_outbox[i].text));
            }
            free(old);
            migrate = true;
        }
    }
    p.end();
    if (migrate) outboxSave();                  // write "q2", remove "q"
    if (g_outboxCount) Serial.printf("[CHECKIN] outbox restored: %d waiting\n", g_outboxCount);
}

// save false: the caller adds several and saves once (a group message: one NVS write, not 8).
bool outboxAdd(const uint8_t dest[16], const char* text, const uint8_t* gid = nullptr, uint32_t gseq = 0,
               bool save = true) {
    if (g_outboxCount >= kOutboxCap) return false;
    OutboxItem& o = g_outbox[g_outboxCount++];
    memset(&o, 0, sizeof(o));
    memcpy(o.dest, dest, 16);
    strlcpy(o.text, text, sizeof(o.text));
    if (gid) { memcpy(o.gid, gid, 4); o.gseq = gseq; }
    if (save) outboxSave();
    Serial.printf("[CHECKIN] queued \"%s\" (%d waiting)\n", o.text, g_outboxCount);
    return true;
}

// Send now, or queue in the outbox when the radio is busy / the 3 s spacing applies (the
// outbox retries in the background, see checkinTick). While anything waits in the outbox,
// new messages queue behind it so they keep their order.
enum class SendResult { Sent, Queued, Failed };
SendResult sendOrQueue(const uint8_t dest[16], const char* text, const uint8_t* gid = nullptr, uint32_t gseq = 0,
                       bool save = true) {
    if (g_outboxCount == 0) {
        if (sendMessage(dest, text, gid, gseq)) return SendResult::Sent;
        if (!g_txTransient) return SendResult::Failed;
    }
    if (!outboxAdd(dest, text, gid, gseq, save)) { snprintf(g_txStr, sizeof(g_txStr), "outbox full"); return SendResult::Failed; }
    return SendResult::Queued;
}

// ---- Groups: send, share, join, the group thread ------------------------------------------
// Messages wait for the check-in window (check-in mode, outside the window)?
bool sendHeld() {
    if (!g_pwEff.checkin) return false;
    const power::CheckinPhase ph = checkinNow();
    return !(ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS);
}

// The same number on every member's copy of one group message. Clock seconds, never going
// back or repeating in one session, so nothing has to be saved per message.
uint32_t grpNextSeq() {
    static uint32_t last = 0;
    uint32_t s = (uint32_t)time(nullptr);
    if (s <= last) s = last + 1;
    last = s;
    return s;
}

// Outbox slots a send of `count` messages needs now (the first goes at once when nothing waits).
bool outboxRoomFor(int count) {
    const int need = (sendHeld() || g_outboxCount) ? count : count - 1;
    return g_outboxCount + need <= kOutboxCap;
}

// One text to every member of a group: the first copy goes now, the others through the outbox
// (one send every ~3.5 s, retried while the radio is busy; check-in mode: all wait for the
// window). All or nothing: with too little room in the outbox nothing is sent. Returns the
// number of members addressed (0: see g_txStr). Other features that share something with a
// group (locations, markers) can call this with their own text.
int groupSend(int gi, const char* text, int* queuedOut = nullptr) {
    if (queuedOut) *queuedOut = 0;
    if (gi < 0 || gi >= g_grpCount) { snprintf(g_txStr, sizeof(g_txStr), "no group"); return 0; }
    const Group g = g_grp[gi];
    if (!g.n) { snprintf(g_txStr, sizeof(g_txStr), "no members"); return 0; }
    if (!g_protoReady) { snprintf(g_txStr, sizeof(g_txStr), "proto off"); return 0; }
    if (!outboxRoomFor(g.n)) { snprintf(g_txStr, sizeof(g_txStr), "radio busy"); return 0; }
    const bool hold = sendHeld();
    const uint32_t seq = grpNextSeq();
    int now = 0, queued = 0, failed = 0;
    for (int i = 0; i < g.n; i++) {
        if (hold) { if (outboxAdd(g.m[i], text, g.id, seq, false)) queued++; else failed++; continue; }
        const SendResult r = sendOrQueue(g.m[i], text, g.id, seq, false);
        if (r == SendResult::Sent) now++;
        else if (r == SendResult::Queued) queued++;
        else failed++;
    }
    if (queued) outboxSave();
    Serial.printf("[GRP] send \"%s\" #%02x%02x%02x%02x seq %08lx \"%s\": %d now, %d queued, %d failed\n", g.name,
                  g.id[0], g.id[1], g.id[2], g.id[3], (unsigned long)seq, text, now, queued, failed);
    if (queuedOut) *queuedOut = queued;
    if (now + queued) snprintf(g_txStr, sizeof(g_txStr), "to %d", now + queued);
    return now + queued;
}

// A group to one of its members as group cards: this radio first, then every member.
bool grpShareTo(int gi, const uint8_t to[16]) {
    if (gi < 0 || gi >= g_grpCount) return false;
    const Group g = g_grp[gi];
    char toName[24];
    peerName(to, toName, sizeof(toName));
    if (isSelf(to) || grpMember(g, to) < 0) { toast("Pick a member"); return false; }
    const int parts = g.n + 1;
    if (!outboxRoomFor(parts)) { toast("Radio busy: try again soon"); return false; }
    const bool hold = sendHeld();
    int ok = 0;
    for (int i = 0; i < parts; i++) {
        char body[128], nm[24];
        const uint8_t* h = i ? g.m[i - 1] : g_selfHash;
        peerName(h, nm, sizeof(nm));
        grpCardFormat(g, i + 1, parts, h, nm, body, sizeof(body));
        if (hold) { if (outboxAdd(to, body, nullptr, 0, false)) ok++; continue; }
        if (sendOrQueue(to, body, nullptr, 0, false) != SendResult::Failed) ok++;
    }
    outboxSave();
    Serial.printf("[GRP] share \"%s\" to %s: %d of %d cards\n", g.name, toName, ok, parts);
    if (ok == parts) toast("Group sent to %.12s", toName); else toast("Not sent: %s", g_txStr);
    return ok == parts;
}

// One page of a chat's history, oldest first; before `cursor` (counter 0 = the newest page).
int histPage(const uint8_t peer[16], handheld::storage::HistoryEntry cursor, handheld::storage::HistoryEntry* out, int cap) {
    using namespace handheld::storage;
    char hex[33];
    hashToHex(peer, hex);
    const std::string peerHex(hex);
    auto sub = g_msgStore.requestHistoryPage(peerHex, cursor, (uint8_t)cap, HistoryDirection::Before);
    for (int a = 0; a < 40 && !sub.accepted(); a++) {    // store busy (a write): up to 2 s
        const unsigned r = (unsigned)sub.rejection;
        if (r != 1 && r != 8) break;
        pumpFor(50, 12);
        sub = g_msgStore.requestHistoryPage(peerHex, cursor, (uint8_t)cap, HistoryDirection::Before);
    }
    if (!sub.accepted()) return -1;
    MessageStore::Result res;
    size_t n = 0;
    if (waitResult(sub.ticket, res) && res.outcome == Outcome::Committed && res.error == Error::None &&
        res.length >= sizeof(HistoryEntry)) {
        n = res.length / sizeof(HistoryEntry);
        if (n > (size_t)cap) n = (size_t)cap;
        if (!g_msgStore.readPayload(sub.ticket, out, n * sizeof(HistoryEntry))) n = 0;
    }
    g_msgStore.releaseResult(sub.ticket);
    return (int)n;
}

// A stored record's header and the start of its content (NUL-terminated in `text`).
bool readRecordText(const handheld::storage::RecordKey& key, handheld::storage::StoredRecordHeader& hdr,
                    char* text, size_t cap, size_t& len) {
    using namespace handheld::storage;
    uint8_t buf[256];
    auto rs = g_msgStore.requestRecord(key, 0, sizeof(buf));
    if (!rs.accepted()) return false;
    MessageStore::Result rr;
    size_t got = 0;
    if (waitResult(rs.ticket, rr) && rr.outcome == Outcome::Committed && rr.error == Error::None) {
        got = rr.length < sizeof(buf) ? rr.length : sizeof(buf);
        if (!g_msgStore.readPayload(rs.ticket, buf, got)) got = 0;
    }
    g_msgStore.releaseResult(rs.ticket);
    if (got < sizeof(hdr)) return false;
    memcpy(&hdr, buf, sizeof(hdr));
    size_t off = sizeof(hdr) + hdr.titleLength;
    if (off > got) off = got;
    len = hdr.contentLength;
    if (off + len > got) len = got - off;
    if (len > cap - 1) len = cap - 1;
    memcpy(text, buf + off, len);
    text[len] = 0;
    return true;
}

void requestRedraw();
bool cardIsSaved(const uint8_t hash[16]);
void saveContactName(const uint8_t peer[16], const char* name);

// Enter on a group card in a chat: collect every part of the newest share of that group from
// the chat (the newest 24 messages), save members that are not contacts yet, then add the
// group, or replace this radio's member list when it has the group already.
bool grpJoinFromThread(const uint8_t peer[16]) {
    using namespace handheld::storage;
    if (!g_threadGrp.valid || !g_threadGrp.join) return false;
    static HistoryEntry e[24];
    const int n = histPage(peer, HistoryEntry{}, e, 24);
    if (n <= 0) { toast("Chat busy: try again"); return false; }
    static uint8_t hashes[kGrpMembers + 1][16];
    static char names[kGrpMembers + 1][24];
    bool have[kGrpMembers + 1] = {};
    int parts = 0, got = 0;
    char gname[17] = "";
    for (int i = n - 1; i >= 0 && (!parts || got < parts); i--) {        // newest first
        if (!e[i].incoming) continue;
        RecordKey key{};
        memcpy(key.peer, peer, 16);
        key.counter = e[i].counter;
        key.incoming = true;
        StoredRecordHeader hdr;
        char text[160];
        size_t len = 0;
        if (!readRecordText(key, hdr, text, sizeof(text), len)) continue;
        uint8_t id[4], h[16];
        char gn[17], mn[24];
        int part = 0, total = 0;
        if (!grpCardParse(text, len, id, gn, part, total, h, mn) || memcmp(id, g_threadGrp.id, 4)) continue;
        if (!parts) { parts = total; strlcpy(gname, gn, sizeof(gname)); }
        if (total != parts || have[part - 1]) continue;                  // an older share, or a repeat
        have[part - 1] = true;
        memcpy(hashes[part - 1], h, 16);
        strlcpy(names[part - 1], mn, sizeof(names[0]));
        got++;
    }
    if (!parts || got < parts) {
        toast("Group: %d of %d parts here yet", got, parts ? parts : (int)g_threadGrp.parts);
        Serial.printf("[GRP] join waits: %d of %d parts\n", got, parts);
        return false;
    }
    Group g = {};
    memcpy(g.id, g_threadGrp.id, 4);
    strlcpy(g.name, gname, sizeof(g.name));
    for (int i = 0; i < parts; i++) {
        if (isSelf(hashes[i]) || grpMember(g, hashes[i]) >= 0) continue;
        if (g.n >= kGrpMembers) { toast("Group too large (max %d)", kGrpMembers); return false; }
        memcpy(g.m[g.n++], hashes[i], 16);
    }
    int gi = grpFind(g.id);
    const bool update = gi >= 0;
    if (!update) {
        if (g_grpCount >= kGrpMax) { toast("Group list full (%d)", kGrpMax); return false; }
        gi = g_grpCount++;
    } else {
        strlcpy(g.name, g_grp[gi].name, sizeof(g.name));                 // this radio's name for it stays
    }
    int saved = 0;
    for (int i = 0; i < g.n; i++)
        for (int k = 0; k < parts; k++)
            if (!memcmp(hashes[k], g.m[i], 16) && !cardIsSaved(g.m[i])) { saveContactName(g.m[i], names[k]); saved++; }
    g_grp[gi] = g;
    grpSave();
    g_threadGrp.valid = false;
    Serial.printf("[GRP] %s \"%s\" #%02x%02x%02x%02x: %d members, %d new contacts\n", update ? "updated" : "joined",
                  g.name, g.id[0], g.id[1], g.id[2], g.id[3], (int)g.n, saved);
    toast(update ? "Group updated: %.12s" : "Joined group %.14s", g.name);
    return true;
}

// The group thread: every member's chat is searched (its newest HIST_ROWS messages) for
// messages tagged with this group; the newest HIST_ROWS of them are shown. This radio's own
// message is one row for all its copies ("2/3 delivered"); g_grpLast keeps who got the newest
// one (MIC in the thread). Nothing extra is stored: the tagged records are the thread.
enum : uint8_t { GS_NONE, GS_WAIT, GS_SENT, GS_DELIVERED, GS_FAILED, GS_NOPROOF };
struct GrpLast { bool valid; uint8_t gid[4]; uint8_t st[kGrpMembers]; char text[40]; };
GrpLast g_grpLast = {};

void loadGroupHistory(const uint8_t peer[16], bool markRead) {
    using namespace handheld::storage;
    g_histCount = 0;
    g_histNote[0] = 0;
    g_threadCard.valid = false;
    g_threadLoc.valid = false;
    g_threadGrp.valid = false;
    g_grpLast = {};
    const int gi = grpOfHash(peer);
    if (gi < 0) { snprintf(g_histNote, sizeof(g_histNote), "(group deleted)"); return; }
    if (!g_storeReady) { snprintf(g_histNote, sizeof(g_histNote), "(store off)"); return; }
    const Group g = g_grp[gi];
    const uint32_t t0 = millis();

    struct Cand { double ts; uint32_t seq; char text[96]; uint8_t st[kGrpMembers]; uint8_t member; bool incoming, dead; };
    constexpr int kCand = kGrpMembers * HIST_ROWS + kOutboxCap;
    Cand* const cand = (Cand*)malloc(sizeof(Cand) * kCand);              // ~7 KB, only while the chat is built
    if (!cand) { snprintf(g_histNote, sizeof(g_histNote), "(low memory)"); return; }
    int nc = 0, reads = 0;
    bool busy = false;
    for (int m = 0; m < g.n; m++) {
        HistoryEntry e[HIST_ROWS];
        const int n = histPage(g.m[m], HistoryEntry{}, e, HIST_ROWS);
        if (n < 0) { busy = true; continue; }
        int unread = 0, unreadMine = 0;
        bool olderUnread = false;
        for (int i = 0; i < n; i++) {
            RecordKey key{};
            memcpy(key.peer, g.m[m], 16);
            key.counter = e[i].counter;
            key.incoming = e[i].incoming;
            StoredRecordHeader hdr;
            Cand& c = cand[nc];
            size_t len = 0;
            if (!readRecordText(key, hdr, c.text, sizeof(c.text), len)) continue;
            reads++;
            handheld::lxmf::GroupTag tag;
            const bool mine = recordGroupTag(key, hdr, tag) && !memcmp(tag.id, g.id, 4);
            if (hdr.incoming && !hdr.read) {
                unread++;
                if (mine) unreadMine++;
                if (i == 0 && n == HIST_ROWS) olderUnread = true;       // more unread may lie further back
            }
            if (!mine) continue;
            {   // automatic status updates go to the map and the contact card, not into the chat
                double la = 0, lo = 0;
                int bp = 0;
                bool autoL = false, autoB = false;
                locParse(c.text, len, la, lo, &autoL);
                batParse(c.text, len, bp, &autoB);
                if (autoL || autoB) continue;
                char row[96];
                if (markRowText(c.text, len, row, sizeof(row))) { strlcpy(c.text, row, sizeof(c.text)); len = strlen(c.text); }
            }
            for (size_t k = 0; k < len; k++)
                if ((unsigned char)c.text[k] < 32 || (unsigned char)c.text[k] > 126) c.text[k] = '?';
            c.ts = hdr.timestamp;
            c.seq = tag.seq;
            c.member = (uint8_t)m;
            c.incoming = hdr.incoming;
            c.dead = false;
            memset(c.st, GS_NONE, sizeof(c.st));
            if (!hdr.incoming)                       // LXMFStatus, as in loadHistory
                c.st[m] = hdr.status == 4 ? GS_DELIVERED : hdr.status == 5 ? GS_FAILED : hdr.status == 6 ? GS_NOPROOF
                        : hdr.status <= 2 ? GS_WAIT : GS_SENT;
            nc++;
        }
        // Their chat is marked read only when everything unread in it was this group's.
        if (markRead && unread && unread == unreadMine && !olderUnread && g_grpMarkN < kGrpMembers)
            hashToHex(g.m[m], g_grpMarkQ[g_grpMarkN++]);
    }
    for (int i = 0; i < g_outboxCount; i++) {        // copies still waiting in the outbox
        const int m = grpMember(g, g_outbox[i].dest);
        if (m < 0 || memcmp(g_outbox[i].gid, g.id, 4)) continue;
        Cand& c = cand[nc++];
        c.ts = 4e9 + i;                              // newest
        c.seq = g_outbox[i].gseq;
        strlcpy(c.text, g_outbox[i].text, sizeof(c.text));
        c.member = (uint8_t)m;
        c.incoming = false;
        c.dead = false;
        memset(c.st, GS_NONE, sizeof(c.st));
        c.st[m] = GS_WAIT;
    }
    // This radio's copies of one message (same send number) become one row, at its first copy's time.
    for (int i = 0; i < nc; i++) {
        if (cand[i].incoming || cand[i].dead) continue;
        for (int k = i + 1; k < nc; k++) {
            if (cand[k].incoming || cand[k].dead || cand[k].seq != cand[i].seq) continue;
            cand[i].st[cand[k].member] = cand[k].st[cand[k].member];
            if (cand[k].ts < cand[i].ts) cand[i].ts = cand[k].ts;
            cand[k].dead = true;
        }
    }
    int pick[HIST_ROWS], np = 0;                     // the newest HIST_ROWS, then oldest first
    for (; np < HIST_ROWS; np++) {
        int best = -1;
        for (int i = 0; i < nc; i++) {
            if (cand[i].dead) continue;
            bool taken = false;
            for (int k = 0; k < np; k++) taken = taken || pick[k] == i;
            if (!taken && (best < 0 || cand[i].ts > cand[best].ts)) best = i;
        }
        if (best < 0) break;
        pick[np] = best;
    }
    for (int k = np - 1; k >= 0; k--) {
        const Cand& c = cand[pick[k]];
        HistRow& h = g_hist[g_histCount++];
        memset(&h, 0, sizeof(h));
        h.incoming = c.incoming;
        if (c.incoming) {
            char nm[24];
            peerName(g.m[c.member], nm, sizeof(nm));
            snprintf(h.text, sizeof(h.text), "%.10s: %s", nm, c.text);
            continue;
        }
        strlcpy(h.text, c.text, sizeof(h.text));
        int total = 0, d = 0, f = 0, w = 0, nr = 0;
        for (int m = 0; m < g.n; m++) {
            if (c.st[m] == GS_NONE) continue;
            total++;
            d += c.st[m] == GS_DELIVERED;
            f += c.st[m] == GS_FAILED;
            w += c.st[m] == GS_WAIT;
            nr += c.st[m] == GS_NOPROOF;
        }
        if (w)       snprintf(h.status, sizeof(h.status), "%d/%d delivered, %d to send", d, total, w);
        else if (f)  snprintf(h.status, sizeof(h.status), "%d/%d delivered, %d failed", d, total, f);
        else if (nr) snprintf(h.status, sizeof(h.status), "%d/%d delivered, %d no reply", d, total, nr);
        else if (d == total) snprintf(h.status, sizeof(h.status), "delivered %d/%d", d, total);
        else        snprintf(h.status, sizeof(h.status), "%d/%d delivered", d, total);
        g_grpLast.valid = true;                      // rows go oldest first: the last one set is the newest
        memcpy(g_grpLast.gid, g.id, 4);
        memcpy(g_grpLast.st, c.st, sizeof(g_grpLast.st));
        strlcpy(g_grpLast.text, c.text, sizeof(g_grpLast.text));
    }
    free(cand);
    if (g_histCount == 0) snprintf(g_histNote, sizeof(g_histNote), busy ? "(history busy)" : "(no messages yet)");
    if (markRead) g_histMarkedAt = g_msgStore.incomingCommitted();
    g_convValid = false;
    Serial.printf("[HIST] group \"%s\": %d messages (%d records read) in %lu ms\n", g.name, g_histCount, reads,
                  (unsigned long)(millis() - t0));
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
// A new random ESPHome API encryption key (32 bytes, base64), as "esphome" would make one.
void haNewKey() {
    uint8_t k[32];
    for (int i = 0; i < 32; i += 4) { const uint32_t r = esp_random(); memcpy(k + i, &r, 4); }
    havoice::keyEncode(k, g_cfg.haKey);
    Serial.printf("[HAV] new key %s\n", g_cfg.haKey);
}

enum class Entry : uint8_t { None, Message, Name, RtcField, IdleSecs, Latitude, Longitude, WifiPass,
                             PassOld, PassNew, PassConfirm,     // Pass*: lock password (arg = purpose)
                             Search,                            // list filter (Inbox / Contacts / Peers)
                             TcpHost, TcpPort,
                             MapNum,                            // contact page: map number
                             MarkDist, MarkNote,                // new marker: distance, note
                             Note,                              // Library: a new note
                             WptName,                           // map P: waypoint name
                             LibFind,                           // library: find text
                             PlaceFind,                         // map R: place name
                             FoundText,                         // idle "If found": owner text
                             CheckinTime,                       // S.5.8: hhmm
                             HaKey,                             // S.3.7: HA encryption key
                             GroupName };                       // Groups: new group / rename (arg = group, -1 new)
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
           g_entry == Entry::MapNum || g_entry == Entry::MarkDist ||
           g_entry == Entry::Latitude || g_entry == Entry::Longitude;
}

// Settings
const char* const kFoundLabel[10] = {"Name", "Phone", "Address 1", "Address 2", "Town", "State",
                                      "ZIP code", "Country", "Misc 1", "Misc 2"};
int g_foundField = 0;                            // which If-found line Entry::FoundText edits
int foundCount() { int n = 0; for (auto& s : g_cfg.found) n += s[0] != 0; return n; }

// Loop steps over 0.2 s (see SLOW_STEP at loop()).
struct SlowRec { const char* name; uint16_t ms; uint32_t atS; };
SlowRec  g_slow[12] = {};          // newest slow loop steps (render excluded), Settings S.10
uint8_t  g_slowHead = 0;
uint32_t g_slowCount = 0;
enum Page : uint8_t {
    PG_ROOT, PG_TIME, PG_RTC, PG_TZ, PG_LOC, PG_WIFI, PG_SCREEN, PG_IDLE,
    PG_BATTERY, PG_BATLOG, PG_IDENTITY, PG_LORA, PG_ALERTS, PG_DIAG, PG_STORAGE, PG_SD,
    PG_WIFI_SCAN, PG_WIFI_SAVED, PG_WIFI_NET, PG_WIFI_JOIN, PG_POWER, PG_POWER_CUSTOM, PG_ENERGY, PG_ENERGY_GRAPH,
    PG_CHECKIN, PG_TCP, PG_IDS, PG_ID_ONE, PG_WPTS, PG_TRACK, PG_GEOFS, PG_GEOF_NEW, PG_LOWBAT, PG_OFF,
    PG_BACKUPS, PG_FOUND, PG_MODE, PG_HAVOICE, PG_FEATURES, PG_SELFTEST, PG_TRIP
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
// Standby when idle (S.5.11): g_lastUseMs is the last real key or touch (console keys, the
// soak test and incoming messages do not count); g_standby = the standby profile is in force.
uint32_t g_lastUseMs = 0;
bool     g_standby = false;
uint32_t g_standbySince = 0;
uint32_t g_standbyTestS = 0;       // console "standby test <s>": timeout in seconds, not saved
volatile bool g_useSeen = false;   // a key came off the keyboard chip (set in the busy callback too)
void standbyNoteUse(const char* why);

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
           p == PG_WIFI_NET || p == PG_WIFI_JOIN || p == PG_TCP || p == PG_HAVOICE;
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
bool wifiHeldForBle();
void wifiConnect(const char* ssid, const char* pass, bool save, bool autoJoin) {
    if (!g_pwEff.wifi) { toast("WiFi off in %s mode", power::modeName(g_pw.mode)); return; }
    if (wifiHeldForBle()) { if (!autoJoin) toast("WiFi off: ATAK on Bluetooth"); return; }
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
    if (wifiHeldForBle()) { if (!forAuto) toast("WiFi off: ATAK on Bluetooth"); return; }
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

// The ATAK feed on Bluetooth: WiFi stays off. Both stacks together leave the firmware almost
// no internal RAM (COM9: 288 bytes when WiFi started next to Bluetooth), so WiFi waits until
// the feed is off or back on the WiFi link; auto-join is not paused for good.
bool wifiHeldForBle() { return g_cot.on && cotUseBle(); }

void wifiOffForBle() {
    if (WiFi.getMode() == WIFI_OFF && g_wifiSt == WifiSt::Off) return;
    g_wifiJoinAfterScan = false;
    wifiRadioOff();
    g_wifiScanning = false;
    g_wifiSt = WifiSt::Off;
    strlcpy(g_wifiMsg, "off (ATAK on Bluetooth)", sizeof(g_wifiMsg));
    Serial.println("[WIFI] off: the ATAK feed uses Bluetooth");
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

bool g_wtWifiOwned = false;   // a walkie-talkie call runs its own hotspot / join (watchWifi stands aside)

void watchWifi() {
    static uint32_t last = 0;
    if (millis() - last < 250) return;
    last = millis();
    if (g_wtWifiOwned) return;

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
    H_TILE_AUDIO, H_UP, H_DOWN, H_CARD, H_TMENU, H_NAVKEYS, H_GRP, H_MARK
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

// The Inbox button names this unit: "Inbox of" and the name below it in the small font, so
// several radios on one desk can be told apart from any screen with the bottom bar.
void smallText(int x, int y, const char* s, bool bold);
void inboxNavBox(int x, int y, int w, int h, bool active, char hotkey) {
    box(x, y, w, h, active ? 3 : 1);
    label(x, y - 9, w, h, "Inbox of", active, hotkey);          // 16 px line at y+3, underline y+20
    char nm[sizeof(g_selfName)];
    strlcpy(nm, g_selfName[0] ? g_selfName : "no name", sizeof(nm));
    const lv_font_t* f = &lv_font_montserrat_12;
    size_t n = strlen(nm);                                     // cut to fit the button
    while (n > 1 && lv_txt_get_width(nm, n, f, 0, LV_TEXT_FLAG_NONE) > w - 3) n--;
    nm[n] = 0;
    const int nw = lv_txt_get_width(nm, n, f, 0, LV_TEXT_FLAG_NONE);
    smallText(x + (w - nw) / 2, y + h - 17, nm, active);
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

// ---- Reticulum over WiFi (Settings > WiFi row 8) -----------------------------------------
// The core's RnsAutoInterface (as on the T-Pager): IPv6 link-local discovery + UDP, group
// "reticulum", so units (and Reticulum on PCs / phones) on the same WiFi exchange packets
// without LoRa. Started once WiFi is up and the link-local address exists; on the protocol
// pump like the TCP hub. Device only (the simulator has no lwIP).
#if !defined(SIMULATOR)
RnsAutoInterface g_auto;
#endif
bool g_autoUp = false;

int meshPeers() {
#if !defined(SIMULATOR)
    return g_autoUp ? (int)g_auto.peerCount() : -1;
#else
    return -1;
#endif
}

void meshTick() {
#if !defined(SIMULATOR)
    if (!g_protoReady) return;
    static uint32_t checkAt = 0, lastLink = 0;
    const bool want = g_cfg.meshWifi && wifiConnected();
    if (!want) {
        if (g_autoUp) {
            g_proto.pump().attachAuto(nullptr);
            g_auto.stop();
            g_autoUp = false;
            Serial.println("[MESH] WiFi interface stopped");
        }
        return;
    }
    auto linkLocal = [](const IPv6Address& a) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; };
    auto scope = []() {
        auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        return sta ? (uint32_t)esp_netif_get_netif_impl_index(sta) : 1u;
    };
    const uint32_t now = millis();
    if (!g_autoUp) {
        if (now - checkAt < 1000) return;
        checkAt = now;
        // (Not WiFi.enableIpV6() here: that creates the address again each call, which restarts
        // its duplicate-address check, so it never became usable.)
        const IPv6Address a = WiFi.localIPv6();
        if (!linkLocal(a)) {
            // No address yet: ask for one once, then give it 15 s to pass the check.
            static uint32_t askedAt = 0;
            if (!askedAt || now - askedAt > 15000) {
                askedAt = now;
                auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (sta) esp_netif_create_ip6_linklocal(sta);
                Serial.println("[MESH] waiting for an IPv6 link-local address");
            }
            return;
        }
        if (g_auto.start("reticulum", 8, (const uint8_t*)a, scope())) {
            g_proto.pump().attachAuto(&g_auto);
            g_autoUp = true;
            lastLink = now;
            Serial.printf("[MESH] WiFi interface up (group reticulum, %s)\n", g_auto.multicastAddress().c_str());
        } else {
            g_auto.stop();
            Serial.println("[MESH] WiFi interface failed to start");
            checkAt = now + 9000;                // retry in ~10 s
        }
        return;
    }
    if (now - lastLink >= 5000) {                // the address can change after a rejoin
        lastLink = now;
        const IPv6Address a = WiFi.localIPv6();
        if (linkLocal(a)) g_auto.notifyLinkChange((const uint8_t*)a, scope());
    }
    g_auto.loop();
    static int lastPeers = -2;
    const int peers = (int)g_auto.peerCount();
    if (peers != lastPeers) {
        lastPeers = peers;
        Serial.printf("[MESH] %d unit%s on WiFi\n", peers, peers == 1 ? "" : "s");
        if (g_screen == SCR_SETTINGS && g_page == PG_WIFI) requestRedraw();
    }
#endif
}

// ---- Identities (Settings > Identity > Identities) ------------------------------------
// The core's IdentityManager slots (as on the Plus build): list, create, switch, erase.
// The protocol loads the active identity at boot, so a switch restarts the device.
// Destructive actions need Enter twice within 5 s.
int      g_idSel = 0;                 // slot shown on PG_ID_ONE
void popPage();
[[noreturn]] void powerDown(const char* why, uint16_t lookMin, bool resume);
// Daily contact while switched off (S.5 > Switch off): at the set hour the radio starts by
// itself, sends its position and battery to the contacts that have location auto-share,
// listens for the set minutes (messages held for it arrive now), and switches off again.
// A key during the window keeps it on. NVS rs_boot dcOn / dcMin / dcStay / dcDay.
struct OffContact { bool on = false; uint16_t atMin = 20 * 60; uint8_t stayMin = 5; } g_offc;
bool g_contactWin = false;            // this start is a contact window
void offcSave();
void offcPrint();
extern char g_offReport[72];
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
    void minimalToInbox();
    const bool ok = vault::unlock(g_lockBuf);   // derives the key, or checks it when unlocked
    lockWipe();
    if (!ok) { lockNoteWrong(); requestRedraw(); return; }
    lockNoteRight();
    g_locked = false;
    if (vaultWasLocked) wifiLoad();            // saved WiFi passwords readable now
    minimalToInbox();
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

struct SetRow { char title[40]; char sub[34]; uint8_t kind; uint8_t num; };   // num: 0-based, as in Everything
constexpr int MAX_SET_ROWS = kTzCount > 12 ? kTzCount : 12;
SetRow g_srows[MAX_SET_ROWS];
int g_srowCount = 0;
int g_srowNext = 0;                  // the next row number (hidden rows skip theirs)
void srowSkip() { g_srowNext++; }      // a row this mode does not show

void srow(uint8_t kind, const char* name, const char* subFmt, ...) {
    if (g_srowCount >= MAX_SET_ROWS) return;
    SetRow& r = g_srows[g_srowCount];
    r.num = (uint8_t)g_srowNext++;
    snprintf(r.title, sizeof(r.title), "%d : %s : %s", r.num + 1, kKindName[kind], name);
    if (textW(r.title) > W - 16)              // too long for one line: the kind word goes
        snprintf(r.title, sizeof(r.title), "%d : %s", r.num + 1, name);
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
        case PG_OFF:          return "Switch off";
        case PG_GEOF_NEW:     return "New arrival alert";
        case PG_BACKUPS:      return "Restore backup";
        case PG_FOUND:        return "If found";
        case PG_HAVOICE:      return "Home Assistant voice";
        case PG_FEATURES:     return "Typical features";
        case PG_MODE:         return "Mode";
        case PG_SELFTEST:     return "Check this unit";
        case PG_TRIP:         return "Ready for a trip?";
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
    g_srowNext = 0;
    char a[34], b[16];
    const char* e;
    switch (p) {
    case PG_ROOT:
        srow(RK_MENU, "Mode", "%s, touch %s", kOpModeName[g_cfg.opMode], g_cfg.touchOff ? "off" : "on");
        if (minimalMode()) break;                  // Minimal: only the mode (S.1)
        fmtTime(a, sizeof(a));
        srow(RK_MENU, "Time & Location", "%s %s", a, tzCur().name);
        wifiStatusLine(a, sizeof(a));
        if (feature(F_WIFI)) srow(RK_MENU, "WiFi", "%s", a); else srowSkip();
        srow(RK_MENU, "Screen & I/O", "%s, keys %s", g_cfg.dark ? "Black ink" : "White paper",
             g_cfg.kbLight ? "lit" : "dark");
        srow(RK_MENU, "Power", "%s%s", power::modeName(g_pw.mode), g_pwOnUsb ? " (USB: Full)" : "");
        if (g_bat.valid) srow(RK_MENU, "Battery", "%.2f V  %d%%", g_bat.volts, g_bat.pct);
        else             srow(RK_MENU, "Battery", "gauge n/a");
        srow(RK_MENU, "Identity", "%s", g_selfName);
        if (feature(F_ADVANCED)) srow(RK_MENU, "LoRa", "%.0f MHz %s", LORA_DEFAULT_FREQ / 1e6, loraOnline() ? "online" : "offline");
        else srowSkip();
        srow(RK_MENU, "Alerts", "beep %s  vibe %s", g_cfg.beep ? "on" : "off", g_cfg.vibe ? "on" : "off");
        if (feature(F_ADVANCED))
            srow(RK_MENU, "Diagnostics", "paths %u  links %u",
                 g_protoReady ? (unsigned)g_proto.pathCount() : 0u,
                 g_protoReady ? (unsigned)g_proto.linkCount() : 0u);
        else srowSkip();
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
            char hd[24];
            if (!g_home.set) srow(RK_ACTION, "Go Home", "not set (rows 14, 15)");
            else if (homeText(hd, sizeof(hd))) srow(RK_ACTION, "Go Home", "%s; Enter = arrow", hd);
            else srow(RK_ACTION, "Go Home", "%.4f, %.4f", g_home.lat, g_home.lon);
            srow(RK_ACTION, "Set Home here", "my position; Enter twice");
            srow(RK_ACTION, "Set Home = map centre", "Enter twice");
            char sun[40];
            if (sunText(sun, sizeof(sun))) srow(RK_INFO, "Sun today", "%s", sun);
            else srow(RK_INFO, "Sun today", "needs the clock and a position");
        }
        break;

    case PG_TRACK:
        srow(RK_TOGGLE, "Record track", "%s, %d points", g_cfg.trackOn ? "ON" : "OFF", g_trackN);
        if (g_tbOn) srow(RK_ACTION, "Track back", "ON: to point %d; Enter = stop", g_tbIdx + 1);
        else srow(RK_ACTION, "Track back", "guide me back along it");
        srow(RK_ACTION, "Export as GPX", "%s", g_trackGpx[0] ? g_trackGpx : "<SD>/ratdeckpro/tracks/");
        srow(RK_ACTION, "Clear track", "Enter twice");
        srow(RK_ACTION, "Import route points", "GPX files in <SD>/ratdeckpro/routes/");
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
        if (feature(F_ADVANCED)) srow(RK_MENU, "TCP hub", "%s", tcpStatus()); else srowSkip();
        if (feature(F_HAVOICE)) srow(RK_MENU, "Home Assistant voice", "%s", g_cfg.haVoice ? havoice::status() : "OFF");
        else srowSkip();
        if (!g_cfg.meshWifi) srow(RK_TOGGLE, "Mesh over WiFi", "OFF");
        else if (meshPeers() < 0) srow(RK_TOGGLE, "Mesh over WiFi", "ON (starts with WiFi)");
        else srow(RK_TOGGLE, "Mesh over WiFi", "ON, %d unit%s seen", meshPeers(), meshPeers() == 1 ? "" : "s");
        if (!g_cot.on) srow(RK_TOGGLE, "ATAK feed (CoT)", "OFF");
        else if (cotUseBle()) srow(RK_TOGGLE, "ATAK feed (CoT)", "ON, %lu sent", (unsigned long)g_cotSent);
        else if (g_cot.ip == cot::kSaGroup) srow(RK_TOGGLE, "ATAK feed (CoT)", "ON, every %us, %lu sent", (unsigned)g_cot.every, (unsigned long)g_cotSent);
        else { char ip[16]; cotIp(g_cot.ip, ip, sizeof(ip)); srow(RK_TOGGLE, "ATAK feed (CoT)", "ON, to %s", ip); }
        srow(RK_PICK, "ATAK link to phone", "%s", cotUseBle() ? "Bluetooth (bridge app)" : "WiFi (same network)");
        if (!cotUseBle()) srow(RK_INFO, "Bluetooth", "off (link is WiFi)");
        else if (!g_cot.on) srow(RK_INFO, "Bluetooth", "off (feed off)");
        else if (!g_pwEff.wifi) srow(RK_INFO, "Bluetooth", "off in this energy mode");
        else if (cotble::connected()) srow(RK_ACTION, "Bluetooth", "phone connected; Enter x2 = forget");
        else {
            char nm[24];
            cotBleName(nm, sizeof(nm));
            srow(RK_ACTION, "Bluetooth", "%s PIN %06lu", nm, (unsigned long)cotble::passkey());
        }
        break;

    case PG_HAVOICE: {                               // S.3.7: on / off, the encryption key
        srow(RK_TOGGLE, "Voice for Home Assistant", "%s", g_cfg.haVoice ? havoice::status() : "OFF");
        const bool hasKey = g_cfg.haKey[0] != 0;
        e = editingSub(Entry::HaKey);
        if (e) srow(RK_ENTRY, "Encryption key (44)", "%s", e);
        else   srow(RK_ENTRY, "Encryption key 1-22", "%.22s", hasKey ? g_cfg.haKey : "none: Enter to type");
        srow(RK_INFO, "Encryption key 23-44", "%s", hasKey ? g_cfg.haKey + 22 : "-");
        srow(RK_ACTION, "New random key", "%s", hasKey ? "Enter twice" : "Enter");
        srow(RK_ACTION, "No encryption", "%s", hasKey ? "Enter twice" : "(none set)");
        srow(RK_INFO, "Name in Home Assistant", "%s, port %u", havoice::nodeName(), (unsigned)havoice::kPort);
        srow(RK_TOGGLE, "Wake word", "%s", kHaWakeName[g_cfg.haWake > 2 ? 0 : g_cfg.haWake]);
        srow(RK_TOGGLE, "Sensors for HA", "%s", kHaSensName[g_cfg.haSensors > 2 ? 0 : g_cfg.haSensors]);
        break;
    }

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
        // WiFi joined or Bluetooth on keeps the CPU awake: the current mode's figure counts it.
        // (Checked directly: while this page is open the blocker reads "recent input".)
        const char* why = powerSleepBlocker();
        const bool wifiAwake = g_wifiSt == WifiSt::Connecting || g_wifiSt == WifiSt::Connected ||
                               WiFi.getMode() != WIFI_OFF;
        const bool bleAwake = !wifiAwake && cotble::active();
        const int extra = wifiAwake ? power::kWifiAwakeMa : bleAwake ? power::kBleAwakeMa : -1;
        bool capLearned = false;
        const uint16_t cap = batCapacityMah(&capLearned);
        static const char* kNote[power::MODE_CUSTOM] = {"always awake", "wake 1 s", "no WiFi",
                                                        "wake 60 s", "LoRa only", "LoRa on key", ""};
        char rt[16], ci[24], at[12];
        fmtClockMin(g_pw.checkinStart, at, sizeof(at));
        checkinEveryText(ci, sizeof(ci));
        const bool pwAll = feature(F_ADVANCED);    // Typical without Advanced: the short list
        for (uint8_t m = power::MODE_FULL; m < power::MODE_CUSTOM; m++) {
            if (!pwAll && g_pw.mode != m && m + 3 >= power::MODE_CUSTOM) { srowSkip(); continue; }   // Ultra-low, On demand, Check-in
            const bool cur = g_pw.mode == m && extra >= 0;
            power::fmtRuntime(rt, sizeof(rt), cap, power::estimateMa(power::preset(m), cur ? extra : -1));
            if (m == power::MODE_CHECKIN)        // a menu: the Check-in page (time, every ...)
                srow(RK_MENU, "Check-in", "%s%s, every %s%s", g_pw.mode == m ? "ON " : "", at, ci,
                     g_pw.mode == m && g_outboxCount ? ", msgs wait" : "");
            else
                srow(RK_PICK, power::modeName(m), "%s%s, %s", g_pw.mode == m ? "ON " : "", rt,
                     cur ? (wifiAwake ? "awake: WiFi" : "awake: Bluetooth") : kNote[m]);
        }
        if (!pwAll && g_pw.mode != power::MODE_CUSTOM) {
            srowSkip();
        } else if (g_pw.customSet || g_pw.mode == power::MODE_CUSTOM) {
            power::fmtRuntime(rt, sizeof(rt), cap, power::estimateMa(g_pw.custom));
            srow(RK_MENU, "Custom", "%s%s (your settings)", g_pw.mode == power::MODE_CUSTOM ? "ON " : "", rt);
        } else {
            srow(RK_MENU, "Custom", "set your own");
        }
        if (pwAll) srow(RK_MENU, "Energy monitor", "what uses the battery"); else srowSkip();
        srow(RK_TOGGLE, "Full power on USB", "%s", g_pw.fullOnUsb ? "ON" : "OFF");
        if (pwAll || g_pw.standby) {
            srow(RK_TOGGLE, "Standby when idle", g_pw.standby ? "ON: GPS off after %u h idle" : "OFF",
                 (unsigned)g_pw.standbyHours);
            srow(RK_PICK, "Standby after", "%u h without a key", (unsigned)g_pw.standbyHours);
        } else {
            srowSkip();
            srowSkip();
        }
        if (g_offc.on) srow(RK_MENU, "Switch off", "storage; contact daily %02u:%02u", (unsigned)(g_offc.atMin / 60), (unsigned)(g_offc.atMin % 60));
        else srow(RK_MENU, "Switch off", "for storage; daily contact");
        const uint32_t tot = g_pwAwakeMs + g_pwSleepMs;
        if (g_standby)               srow(RK_INFO, "Now", "STANDBY for %lu min, GPS off",
                                          (unsigned long)((millis() - g_standbySince) / 60000UL));
        else if (g_pwOnUsb)          srow(RK_INFO, "Now", "Full (USB power)");
        else if (!g_pwEff.wakeSecs)  srow(RK_INFO, "Now", "%u MHz, never sleeps", (unsigned)g_pwEff.cpuMhz);
        else if (wifiAwake || bleAwake)
            srow(RK_INFO, "Now", "%u MHz, kept awake by %s", (unsigned)g_pwEff.cpuMhz, wifiAwake ? "WiFi" : "Bluetooth");
        else if (why && strcmp(why, "recent input") && strcmp(why, "redraw") && strcmp(why, "keys"))
            srow(RK_INFO, "Now", "%u MHz, awake: %s", (unsigned)g_pwEff.cpuMhz, why);
        else srow(RK_INFO, "Now", "%u MHz, asleep %u%%", (unsigned)g_pwEff.cpuMhz,
                  tot ? (unsigned)((uint64_t)g_pwSleepMs * 100 / tot) : 0u);
        if (!g_bat.valid)                          srow(RK_INFO, "Battery draw", "gauge n/a");
        else if (g_bat.usb && !g_benchUntil)       srow(RK_INFO, "Battery draw", "on USB (%+d mA)", (int)g_bat.avgMa);
        else if (g_bat.tteMin && g_bat.tteMin != 0xFFFF)
            srow(RK_INFO, "Battery draw", "%d mA, %u h %02u m left", (int)-g_bat.avgMa,
                 (unsigned)(g_bat.tteMin / 60), (unsigned)(g_bat.tteMin % 60));
        else                                       srow(RK_INFO, "Battery draw", "%d mA", (int)-g_bat.avgMa);
        srow(RK_INFO, "Times use", capLearned ? "%u mAh (learned)" : "%u mAh (nominal)", (unsigned)cap);
        if (g_offReport[0]) srow(RK_INFO, "Last time", "%s", g_offReport);
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
        srow(RK_ACTION, "Lock now", "%s", pw ? "Enter, or the top side button" : "set a password first");
        srow(RK_PICK, "Idle screen", "%s", kIdleScreenName[g_cfg.idleScreen]);
        srow(RK_MENU, "If-found details", "%d of 10 lines set", foundCount());
        srow(RK_TOGGLE, "If found: my address", "%s", g_cfg.foundAddr ? "shown" : "hidden");
        break;
    }

    case PG_MODE:
        srow(RK_PICK, "Minimal", "%sInbox, Contacts, talk", g_cfg.opMode == OP_MINIMAL ? "(current) " : "");
        srow(RK_PICK, "Typical", "%sthe features on S.1.5", g_cfg.opMode == OP_NORMAL ? "(current) " : "");
        srow(RK_PICK, "Everything", "%sall features", g_cfg.opMode == OP_ADVANCED ? "(current) " : "");
        srow(RK_TOGGLE, "Touch screen", "%s", g_cfg.touchOff ? "OFF (keys only)" : "ON");
        if (!minimalMode()) {
            int on = 0;
            for (int i = 0; i < kFeatureCount; i++) on += (g_cfg.typFeat >> i) & 1;
            srow(RK_MENU, "Typical features", "%d of %d on", on, kFeatureCount);
        }
        break;

    case PG_FEATURES:                                // S.1.5: what Typical shows
        for (int i = 0; i < kFeatureCount; i++)
            srow(RK_TOGGLE, kFeatureName[i], "%s", (g_cfg.typFeat >> i) & 1 ? "ON" : "off");
        srow(RK_ACTION, "Back to the defaults", "Enter");
        break;

    case PG_FOUND:                                   // S.4.2.9: what the If found idle screen shows
        for (int i = 0; i < 10; i++) {
            const char* e = g_foundField == i ? editingSub(Entry::FoundText) : nullptr;
            if (e) srow(RK_ENTRY, kFoundLabel[i], "%s", e);
            else   srow(RK_ENTRY, kFoundLabel[i], "%s", g_cfg.found[i][0] ? g_cfg.found[i] : "-");
        }
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

    case PG_OFF: {
        srow(RK_ACTION, "Switch off now", "top button 2 s = on again");
        srow(RK_TOGGLE, "Daily contact", g_offc.on ? "ON: starts daily, then off" : "OFF: off until the button");
        srow(RK_PICK, "Contact time", "%02u:00", (unsigned)(g_offc.atMin / 60));
        srow(RK_PICK, "Stay on for", "%u min", (unsigned)g_offc.stayMin);
        srow(RK_MENU, "Ready for a trip?", "what is set, what is to do");
        if (ashareActive()) srow(RK_INFO, "Sends to", "%d with location auto-share", ashareActive());
        else srow(RK_INFO, "Sends to", "nobody yet: chat, MIC, row 2");
        {
            const time_t now = time(nullptr);
            if (now < 1600000000) srow(RK_INFO, "Clock", "not set: no daily contact");
            else srow(RK_INFO, "Each contact uses", "~%u mAh (estimate)", (unsigned)(g_offc.stayMin * 70 / 60 + 2));
        }
        if (g_offReport[0]) srow(RK_INFO, "Last time", "%s", g_offReport);
        break;
    }

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
        // The 32-digit LXMF address on two rows: digits 1-16, then 17-32 (a peer's card on
        // another radio shows all 32 on one line).
        srow(RK_INFO, "LXMF address 1-16", "%.16s", g_destHex[0] ? g_destHex : "unknown");
        srow(RK_ENTRY, "Display name", "%s", g_selfName);
        srow(RK_INFO, "LXMF address 17-32", "%.16s", g_destHex[0] ? g_destHex + 16 : "");
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
        srow(RK_TOGGLE, "Relay for others", "%s%s", kRelayName[g_cfg.relay > 2 ? 0 : g_cfg.relay],
             g_cfg.relay ? (g_proto.relay() ? ": relaying now" : ": not now") : "");
        break;

    case PG_ALERTS:
        srow(RK_TOGGLE, "Beep on message", "%s", g_cfg.beep ? "ON" : "OFF");
        srow(RK_TOGGLE, "Vibrate on message", "%s", g_cfg.vibe ? "ON" : "OFF");
        srow(RK_PICK, "Volume", "%u%%%s", (unsigned)g_cfg.volume,
             g_cfg.volume == 0 ? " (silent)" : g_cfg.volume == 80 ? " (default)" : g_cfg.volume == 100 ? " (max)" : "");
        srow(RK_ACTION, "Test alert", "Enter = play");
        srow(RK_MENU, "Arrival alerts", "%d set (contact near a place)", g_geofCount);
        if (!g_cfg.quiet || g_cfg.quiet >= kQuietN) srow(RK_TOGGLE, "Quiet hours", "Off");
        else srow(RK_TOGGLE, "Quiet hours", "%02u:00-%02u:00 no sound%s", (unsigned)kQuiet[g_cfg.quiet][0],
                  (unsigned)kQuiet[g_cfg.quiet][1], quietNow() ? " (now)" : "");
        {
            char t[40];
            sosTargetText(t, sizeof(t));
            srow(RK_PICK, "SOS sends to", "%s%s", t, sosActive() ? " (ACTIVE)" : "");
            srow(RK_TOGGLE, "SOS button (hold 3 s)", "%s", sosBtnOn() ? "ON" : "OFF");
        }
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
        {
            const SlowRec& r = g_slow[(g_slowHead + 11) % 12];
            if (g_slowCount && r.name)
                srow(RK_INFO, "Pauses over 0.2 s", "%lu, last %.12s %ums", (unsigned long)g_slowCount, r.name, (unsigned)r.ms);
            else
                srow(RK_INFO, "Pauses over 0.2 s", "none");
        }
        srow(RK_INFO, "Uptime", "%lud %02lu:%02lu", (unsigned long)(s / 86400),
             (unsigned long)((s / 3600) % 24), (unsigned long)((s / 60) % 60));
        if (soakIsOn()) srow(RK_TOGGLE, "Stress test", "ON  %u peers, Enter = off", (unsigned)soakPeerCount());
        else            srow(RK_TOGGLE, "Stress test", "OFF");
        srow(RK_MENU, "Check this unit", "hardware self-test");
        srow(RK_MENU, "Ready for a trip?", "battery, maps, Home, contacts");
        break;
    }

    case PG_TRIP:
        srow(RK_ACTION, "Check again", g_tripTodo ? "%d to do" : "all OK", g_tripTodo);
        for (int i = 0; i < g_tripN; i++) srow(RK_INFO, g_trip[i].name, "%s%s", g_trip[i].ok ? "OK: " : "TO DO: ", g_trip[i].note);
        break;

    case PG_SELFTEST:
        selftestRows();
        break;

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
    Serial.printf("[UI] page %s (depth %d)\n", pageTitle(p), g_navDepth);   // for tests (timed tasks)
}

void popPage() {
    if (g_navDepth == 0) return;
    const NavFrame& f = g_navStack[--g_navDepth];
    g_page = f.page;
    g_sel = f.sel;
    g_scroll = f.scroll;
    clampScroll(g_sel, g_scroll, settingsRowCount());
    requestRedraw();
    Serial.printf("[UI] page %s (depth %d)\n", pageTitle(g_page), g_navDepth);
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
        static const Page kSub[11] = {PG_MODE, PG_TIME, PG_WIFI, PG_SCREEN, PG_POWER, PG_BATTERY, PG_IDENTITY,
                                      PG_LORA, PG_ALERTS, PG_DIAG, PG_STORAGE};   // S.11 Storage: by scrolling
        if (minimalMode() && idx != 0) break;
        if (idx >= 0 && idx < 11) pushPage(kSub[idx], idx == 0 ? g_cfg.opMode : idx == 4 ? g_pw.mode : 0);
        break;
    }
    case PG_MODE:
        if (idx >= 0 && idx <= 2) {
            g_cfg.opMode = (uint8_t)idx;
            saveConfig();
            Serial.printf("[MODE] %s\n", kOpModeName[idx]);
            toast("Mode: %s", kOpModeName[idx]);
            requestRedraw();
        } else if (idx == 3) {
            g_cfg.touchOff = !g_cfg.touchOff;
            saveConfig();
            Serial.printf("[TOUCH] screen %s\n", g_cfg.touchOff ? "off (keys only)" : "on");
            requestRedraw();
        } else if (idx == 4 && !minimalMode()) {
            pushPage(PG_FEATURES);
        }
        break;
    case PG_FEATURES:
        if (idx >= 0 && idx < kFeatureCount) g_cfg.typFeat ^= (uint16_t)(1u << idx);
        else if (idx == kFeatureCount) g_cfg.typFeat = kTypicalDefault;
        else break;
        saveConfig();
        Serial.printf("[MODE] Typical features 0x%02x\n", (unsigned)g_cfg.typFeat);
        if (g_cfg.opMode != OP_NORMAL) toast("Used in Typical mode (S.1.2)");
        requestRedraw();
        break;
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
        } else if (idx == 12) {
            homeGo();
        } else if (idx == 13) {
            double la, lo;
            if (!ownPosition(la, lo)) toast("No position yet (GPS or S.2 row 4)");
            else if (confirmTwice(1213, "Home = here")) { homeSet(la, lo, "my position"); toast("Home set here"); }
            requestRedraw();
        } else if (idx == 14) {
            if (confirmTwice(1214, "Home = map centre")) {
                double la, lo;
                mapview::getCentre(la, lo);
                if (la == 0 && lo == 0) toast("Open the map first");
                else { homeSet(la, lo, "map centre"); toast("Home set to the map centre"); }
            }
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
        } else if (idx == 4) {
            const int n = gpxImport();
            if (n > 0) toast("%d waypoints added (P on the map)", n);
            else if (n == 0) toast("Nothing new in routes/*.gpx");
            requestRedraw();
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
        else if (idx == 6) pushPage(PG_HAVOICE);
        else if (idx == 7) {
            g_cfg.meshWifi = !g_cfg.meshWifi;
            saveConfig();
            toast(g_cfg.meshWifi ? "Mesh over WiFi on" : "Mesh over WiFi off");
            requestRedraw();
        } else if (idx == 8) {
            g_cot.on = !g_cot.on;
            g_cotLast = 0;
            cotSave();
            Serial.printf("[COT] feed %s\n", g_cot.on ? "on" : "off");
            if (!g_cot.on) toast("ATAK feed off");
            else if (cotUseBle()) toast("ATAK on: pair the phone");
            else toast(wifiConnected() ? "ATAK feed on" : "ATAK feed on with WiFi");
            requestRedraw();
        } else if (idx == 9) {
            cotSetLink(cotUseBle() ? kCotLinkWifi : kCotLinkBle);
            requestRedraw();
        } else if (idx == 10 && cotUseBle() && cotble::active()) {
            if (confirmTwice(1311, "forget paired phones")) {
                cotble::forget();
                toast("Paired phones forgotten");
            }
            requestRedraw();
        }
        break;

    case PG_HAVOICE: {
        static uint32_t armed = 0;               // Enter twice within 3 s for the key actions
        static int armedRow = -1;
        const bool twice = armedRow == idx && armed && millis() - armed < 3000;
        if (idx == 0) {
            g_cfg.haVoice = !g_cfg.haVoice;
            if (g_cfg.haVoice && !g_cfg.haKey[0]) haNewKey();   // first time: make a key
            saveConfig();
            Serial.printf("[HAV] voice %s\n", g_cfg.haVoice ? "on" : "off");
            toast(g_cfg.haVoice ? (wifiConnected() ? "On: add it in HA, give it the key" : "On when WiFi is up")
                                : "HA voice off");
        } else if (idx == 1) {
            startEntry(Entry::HaKey, g_cfg.haKey);
            g_entryReplace = true;               // typing starts a new key
        } else if (idx == 3 || idx == 4) {
            if (g_cfg.haKey[0] && !twice) {
                armed = millis(); armedRow = idx;
                toast(idx == 3 ? "Enter again: new key" : "Enter again: no encryption");
                break;
            }
            armed = 0; armedRow = -1;
            if (idx == 3) { haNewKey(); toast("New key: give it to HA"); }
            else { g_cfg.haKey[0] = 0; toast("No encryption: HA connects without a key"); Serial.println("[HAV] key cleared"); }
            saveConfig();
        } else if (idx == 6) {                   // wake word: off -> on USB power only -> always
            g_cfg.haWake = (uint8_t)((g_cfg.haWake + 1) % 3);
            saveConfig();
            Serial.printf("[HAV] wake word setting: %s\n", kHaWakeName[g_cfg.haWake]);
            toast(g_cfg.haWake == 0 ? "Wake word off: MIC to talk"
                  : g_cfg.haWake == 1 ? "Wake word on USB power (HA needs a wake word engine)"
                                      : "Wake word always: uses much more battery");
        } else if (idx == 7) {                   // sensors for HA: off -> battery -> battery + position
            g_cfg.haSensors = (uint8_t)((g_cfg.haSensors + 1) % 3);
            saveConfig();
            Serial.printf("[HAV] sensors setting: %s\n", kHaSensName[g_cfg.haSensors]);
            toast(g_cfg.haSensors == 0 ? "HA gets no sensors" : g_cfg.haSensors == 1 ? "HA gets the battery %"
                                                                               : "HA gets battery and position");
        }
        requestRedraw();
        break;
    }

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
        } else if (idx == power::MODE_CUSTOM + 1) {   // (row 7 Check-in opens the schedule)
            g_energyShowCurrent = false;
            pushPage(PG_ENERGY);
        } else if (idx == power::MODE_CUSTOM + 2) {
            g_pw.fullOnUsb = !g_pw.fullOnUsb;
            power::save(g_pw);
            powerApply(true);
            requestRedraw();
        } else if (idx == power::MODE_CUSTOM + 3) {
            g_pw.standby = !g_pw.standby;
            power::save(g_pw);
            toast(g_pw.standby ? "Standby after %u h idle" : "Standby when idle: off", (unsigned)g_pw.standbyHours);
            Serial.printf("[STANDBY] option %s, after %u h\n", g_pw.standby ? "on" : "off", (unsigned)g_pw.standbyHours);
            requestRedraw();
        } else if (idx == power::MODE_CUSTOM + 4) {
            g_pw.standbyHours = power::nextStandbyHours(g_pw.standbyHours);
            power::save(g_pw);
            Serial.printf("[STANDBY] option %s, after %u h\n", g_pw.standby ? "on" : "off", (unsigned)g_pw.standbyHours);
            requestRedraw();
        } else if (idx == power::MODE_CUSTOM + 5) {
            pushPage(PG_OFF);
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
            if (g_cfg.idleScreen == IDLE_FOUND && !foundCount()) toast("Fill in row 9: If-found details");
            requestRedraw();
        } else if (idx == 8) {
            pushPage(PG_FOUND);
        } else if (idx == 9) {
            g_cfg.foundAddr = !g_cfg.foundAddr;
            saveConfig();
            requestRedraw();
        }
        break;

    case PG_FOUND:
        if (idx >= 0 && idx < 10) {
            g_foundField = idx;
            startEntry(Entry::FoundText, g_cfg.found[idx]);
            g_entryReplace = true;            // typing starts over, Backspace edits the old text
        }
        break;

    case PG_OFF:
        if (idx == 0) {
            if (confirmTwice(0x0FF, "switch off")) powerDown("menu", 12 * 60, false);
        } else if (idx == 1) {
            g_offc.on = !g_offc.on;
            offcSave();
            toast(g_offc.on ? "Daily contact on" : "Daily contact off");
        } else if (idx == 2) {
            g_offc.atMin = (uint16_t)((g_offc.atMin / 60 + 1) % 24 * 60);
            offcSave();
        } else if (idx == 3) {
            g_offc.stayMin = g_offc.stayMin < 10 ? 10 : g_offc.stayMin < 15 ? 15 : 5;
            offcSave();
        } else if (idx == 4) {
            tripRun();
            pushPage(PG_TRIP);
            break;
        }
        requestRedraw();
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
        } else if (idx == 6) {                       // relay: off -> on USB power -> always -> off
            g_cfg.relay = (uint8_t)((g_cfg.relay + 1) % 3);
            saveConfig();
            Serial.printf("[RELAY] setting: %s\n", kRelayName[g_cfg.relay]);
            toast(g_cfg.relay == 0 ? "Relay off" : g_cfg.relay == 1 ? "Relays others while on USB power"
                                                                  : "Relays others always (more battery)");
            requestRedraw();
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
        else if (idx == 5) {
            g_cfg.quiet = (uint8_t)((g_cfg.quiet + 1) % kQuietN);
            saveConfig();
            Serial.printf("[ALERT] quiet hours: %u-%u%s\n", (unsigned)kQuiet[g_cfg.quiet][0], (unsigned)kQuiet[g_cfg.quiet][1],
                          g_cfg.quiet ? "" : " (off)");
            requestRedraw();
        } else if (idx == 6) {
            sosNextTarget();
            requestRedraw();
        } else if (idx == 7) {
            sosToggleBtn();
            requestRedraw();
        }
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

    case PG_DIAG:
        if (idx == 6) soakToggle();
        else if (idx == 7) { pushPage(PG_SELFTEST); if (!selftestRan()) selftestStart(false); }
        else if (idx == 8) { tripRun(); pushPage(PG_TRIP); }
        break;

    case PG_TRIP:
        if (idx == 0) { tripRun(); toast(g_tripTodo ? "%d to do" : "All OK", g_tripTodo); requestRedraw(); }
        break;

    case PG_SELFTEST:
        if (idx == 0) selftestStart(false);
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
        for (int i = 0; i < g_grpCount; i++) {       // group chats first
            uint8_t h[16];
            grpHash(g_grp[i].id, h);
            if (PeerRow* r = addRow(h))
                snprintf(r->sub, sizeof(r->sub), g_grp[i].n == 1 ? "group, %d member" : "group, %d members", (int)g_grp[i].n);
        }
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
    // T (up) first, then V (down), as on the keyboard (T sits above V).
    box(0, FOOT_Y, BOT_C_W, FOOT_H, 1);
    label(0, FOOT_Y - 1, BOT_C_W, FOOT_H, "T=up", false, 'T');
    addHit(0, FOOT_Y, BOT_C_W, FOOT_H, H_UP);
    box(BOT_C_W, FOOT_Y, BOT_A_W, FOOT_H, 1);
    label(BOT_C_W, FOOT_Y - 1, BOT_A_W, FOOT_H, "V=dn", false, 'V');
    addHit(BOT_C_W, FOOT_Y, BOT_A_W, FOOT_H, H_DOWN);
    box(BOT_C_W + BOT_A_W, FOOT_Y, BOT_I_W, FOOT_H, 1);
    if (moreDn) {
        label(BOT_C_W + BOT_A_W, FOOT_Y - 1, BOT_I_W, FOOT_H, "more below");
        addHit(BOT_C_W + BOT_A_W, FOOT_Y, BOT_I_W, FOOT_H, H_DOWN);
    }
}

void drawHeader(const char* left, bool moreUp) {
    text(4, HEADER_Y, left, true, moreUp ? MENU_X - 8 : W - 8);
    if (moreUp) {
        // Hangs off MENU: same left/right edges, bottom edge = top line of
        // the first list row, so it reads like the footer's "more below" cell.
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
        if (g_screen == SCR_PEERS && strlen(hdr) + 13 < sizeof(hdr)) strlcat(hdr, "  R=announce", sizeof(hdr));
        if (g_screen == SCR_CONTACTS && pages <= 1) strlcat(hdr, "  G=groups", sizeof(hdr));
    }
    drawHeader(hdr, g_listScroll > 0);

    if (g_rowCount == 0) {
        const char* empty = g_filter[0] || g_entry == Entry::Search ? "No match"
                          : g_screen == SCR_CONTACTS ? "No contacts - see Peers"
                          : g_screen == SCR_PEERS    ? "No peers yet: R = announce"
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
        if (g_screen == SCR_INBOX && isGrpHash(r.peer))
            snprintf(right, sizeof(right), "group");
        else if (g_screen == SCR_INBOX && !r.self)   // every chat: "0 new" .. "9 new", "+9 new"
            snprintf(right, sizeof(right), r.unread > 9 ? "+9 new" : "%u new", (unsigned)r.unread);
        listRow(y, title, r.sub, i == g_listSel, right);
        addHit(2, y, W - 4, ROW_H, H_ROW, i);
    }
    drawFooter();
}

// ---- Thread (entry mode: typing goes to compose) ---------------------------------
// A message in up to 6 lines (word wrap; the last line ends ".." only if still longer), each <= maxW px.
constexpr int kBubbleLines = 6, kBubbleLineH = 18;   // 6: a 95-character message never needs ".."
int bubbleWrap(const char* s, int maxW, char out[kBubbleLines][48]) {
    int n = 0;
    const char* p = s;
    while (*p && n < kBubbleLines) {
        while (*p == ' ') p++;
        size_t best = 0, i = 0;
        char tmp[48];
        while (p[i] && i < sizeof(tmp) - 1) {     // longest prefix that fits, ending at a word
            memcpy(tmp, p, i + 1); tmp[i + 1] = 0;
            if (textW(tmp) > maxW) break;
            i++;
            if (!p[i] || p[i] == ' ') best = i;
        }
        if (!best) best = i ? i : 1;              // one long word: cut it
        if (n == kBubbleLines - 1 && p[best]) {   // last line and more text: end with ".."
            size_t k = best;
            while (k > 0) { memcpy(tmp, p, k); memcpy(tmp + k, "..", 3); if (textW(tmp) <= maxW) break; k--; }
            strlcpy(out[n++], tmp, 48);
            return n;
        }
        memcpy(out[n], p, best); out[n][best] = 0;
        n++;
        p += best;
    }
    return n ? n : (strlcpy(out[0], "", 48), 1);
}
int bubbleLines(const char* s) { char l[kBubbleLines][48]; return bubbleWrap(s, W - 28 - 16, l); }

void drawBubble(int y, const char* s, bool incoming, const char* status) {
    char l[kBubbleLines][48];
    const int n = bubbleWrap(s, W - 28 - 16, l);
    int tw = 40;
    for (int i = 0; i < n; i++) if (textW(l[i]) + 16 > tw) tw = textW(l[i]) + 16;
    if (tw > W - 28) tw = W - 28;
    const int x = incoming ? (W - 4 - tw) : 4;   // sent left, received right
    const int h = 6 + n * kBubbleLineH;
    lv_obj_t* b = box(x, y, tw, h, 2);
    lv_obj_set_style_radius(b, 4, 0);
    for (int i = 0; i < n; i++) text(x + 6, y + 4 + i * kBubbleLineH, l[i], false, tw - 10);
    if (status && status[0]) text(x + 2, y + h + 1, status);
}

extern bool g_tmenuOpen;
void renderThreadMenu(int y);
void renderQuickReplies(int y);
extern bool g_qrOpen;
void renderGrpWho(int y);

// The message being typed, wrapped at words to the typing strip's width, cursor "_" at the
// end. The strip shows 1 to kDraftLines lines as the text needs: out gets the last ones (so
// the cursor is always in view); returns how many (1..kDraftLines).
constexpr int kDraftLines = 3;
int draftWrap(char out[kDraftLines][64], int maxW) {
    char all[BUF_CAP + 2];
    snprintf(all, sizeof(all), "%s_", g_buf);
    const char* start[kDraftLines];
    size_t len[kDraftLines];
    int n = 0;
    const char* p = all;
    while (*p) {
        size_t fit = 0, lastSpace = 0, i = 0;
        while (p[i] && i < 63) {                  // longest prefix that fits
            if (textW(p, i + 1) > maxW) break;
            i++;
            fit = i;
            if (p[i - 1] == ' ') lastSpace = i;   // a line may end after a space
        }
        size_t take = fit ? fit : 1;
        if (p[take] && lastSpace) take = lastSpace;   // more text follows: break at a word
        if (n == kDraftLines) {                   // keep the newest lines
            for (int k = 1; k < kDraftLines; k++) { start[k - 1] = start[k]; len[k - 1] = len[k]; }
            n--;
        }
        start[n] = p;
        len[n] = take;
        n++;
        p += take;
    }
    if (!n) { start[0] = all; len[0] = strlen(all); n = 1; }
    for (int k = 0; k < n; k++) { memcpy(out[k], start[k], len[k]); out[k][len[k]] = 0; }
    return n;
}
uint8_t ashareMinsFor(const uint8_t dest[16]);
const char* ashareMarkFor(const uint8_t dest[16]);
extern bool g_tmenuGrpShare;
int ashareActive();

void renderThread() {
    char name[24], hdr[48];
    peerName(g_threadPeer, name, sizeof(name));
    const uint8_t am = ashareMinsFor(g_threadPeer);
    const int grp = grpOfHash(g_threadPeer);         // a group chat (-1: a contact's)
    if (g_tmenuOpen && g_toast[0]) snprintf(hdr, sizeof(hdr), "%s", g_toast);   // menu: no strip
    else if (g_tmenuOpen && g_qrOpen) snprintf(hdr, sizeof(hdr), "Quick reply: 1-8 sends, ^ back");
    else if (g_tmenuOpen && isGrpHash(g_threadPeer) && !g_tmenuGrpShare) snprintf(hdr, sizeof(hdr), "Who got it  L share  Q reply");
    else if (g_tmenuOpen && isGrpHash(g_threadPeer)) snprintf(hdr, sizeof(hdr), "1-3 Enter, B what, ^ back");
    else if (g_tmenuOpen) snprintf(hdr, sizeof(hdr), "1-5, B what, Q reply, ^ back");
    else if (am) snprintf(hdr, sizeof(hdr), ">>> %.12s [%s %um]", name, ashareMarkFor(g_threadPeer), (unsigned)am);   // sharing on
    else if (grp >= 0) snprintf(hdr, sizeof(hdr), g_grp[grp].n == 1 ? ">>> %s (%d member)" : ">>> %s (%d members)", name, (int)g_grp[grp].n);
    else snprintf(hdr, sizeof(hdr), ">>> %s", name);
    text(4, HEADER_Y, hdr, true, W - 8);
    int y = HEADER_Y + 20;
    if (g_tmenuOpen && g_qrOpen) { renderQuickReplies(y + 4); return; }                          // Q: quick replies
    if (g_tmenuOpen && isGrpHash(g_threadPeer) && !g_tmenuGrpShare) { renderGrpWho(y); return; }   // MIC in a group chat
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
    // Typing strip: 1 to 3 lines of the draft and the hint line; the history gets the rest.
    char draft[kDraftLines][64];
    const int nDraft = draftWrap(draft, W - 14);
    const int stripY = BOT_TOP - STRIP_LINE * (nDraft + 1);
    int yMax = stripY - 2;
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
        auto hOf = [](int i) {                 // 28 per one-line bubble, +18 a line, +14 with a status
            return 28 + (bubbleLines(g_hist[i].text) - 1) * kBubbleLineH + ((!g_hist[i].incoming && g_hist[i].status[0]) ? 14 : 0);
        };
        int start = g_histCount, used = 0;
        while (start > 0 && y + used + hOf(start - 1) <= yMax) { used += hOf(start - 1); start--; }
        if (start == g_histCount) start = g_histCount - 1;   // the newest always shows (taller: clipped)
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

    // Compose strip: the draft (word-wrapped, its last lines, cursor at the end), then the hint.
    box(2, stripY, W - 4, BOT_TOP - stripY, 2);
    for (int k = 0; k < nDraft; k++) text(6, stripY + 1 + k * STRIP_LINE - (k ? 1 : 0), draft[k], false, W - 12);
    char hint[40] = "[Enter] send  [MIC] location";
    if (isGrpHash(g_threadPeer)) snprintf(hint, sizeof(hint), "[Enter] send [MIC] who got it");
    const bool offer = !g_bufLen && (locOffered() || cardOffered() || grpOffered());
    if (!g_bufLen && grpOffered())
        snprintf(hint, sizeof(hint), g_threadGrp.join ? "[Enter] join group: %.14s" : "[Enter] open group: %.14s", g_threadGrp.name);
    else if (!g_bufLen && locOffered()) snprintf(hint, sizeof(hint), "[Enter] map %.3f,%.3f", g_threadLoc.lat, g_threadLoc.lon);
    else if (!g_bufLen && cardOffered()) snprintf(hint, sizeof(hint), "[Enter] save card: %.14s", g_threadCard.name);
    text(6, stripY + STRIP_LINE * nDraft, g_toast[0] ? g_toast : hint, !g_toast[0] && offer, W - 12);
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
    g_srowNext = 0;
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

// ---- SD tools: back up / restore, firmware update (S.11.5 rows 8-10) ------------------------
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

// ---- USB drive mode (S.11.5 row 6) -------------------------------------------------------
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
constexpr char kKeyMark = 'J';                 // markers: J list, Alt+J new at the cross
constexpr char kKeyGo = 'G', kKeyBack = 'B';   // bearing + distance screen, track back
bool isAltSym(const KeyEvent& ev) {
    return ev.alt && ev.opt && !ev.character && !ev.enter && !ev.del && !ev.space && !ev.fn;
}
struct NavKeyRow { const char* keys; const char* what; };
// A key card: a bold title line, a line of the move keys in words, then two columns of 6.
struct NavCard { const char* title; const char* moves; NavKeyRow rows[12]; };
constexpr const char* kMoves4 = "T up   F left   V down   H right";
const NavCard kAtlasKeys = {"Atlas keys", kMoves4, {
    {"D", "centre on me"},   {"Z", "zoom in"},       {"W", "zoom out"},
    {"L", "locate a place"}, {"N", "next contact"},  {"G", "go: arrow"},
    {"P Alt+P", "wpt, add"},  {"J Alt+J", "marker, add"}, {"Alt+SYM", "del wpt"},
    {"B  O", "back, layers"}, {"12 Ent", "chat #12"}, {"M  ^", "menu, close"},
}};
// Library reader and list (K there too).
const NavCard kReaderKeys = {"Reader keys", "T V half page   F H pan", {
    {"Space N", "next page"}, {"B", "prev page"},   {"W", "wrap on/off"},
    {"D", "left edge"},       {"L", "find"},        {"", ""},
    {"M", "menu"},            {"^", "list"},        {"", ""},
}};
const NavCard kLibListKeys = {"Library keys", "T up   V down", {
    {"1-4", "open row"},      {"Enter", "open"},    {"", ""}, {"", ""}, {"", ""}, {"", ""},
    {"M", "menu"},            {"^", "close"},       {"", ""},
}};
const NavCard kGameKeys = {"Rat Trap keys", kMoves4, {
    {"D Enter", "open square"}, {"Alt+P", "mark a trap"}, {"Q", "new game"},
    {"", ""},                 {"", ""},             {"", ""},
    {"M", "menu"},            {"^", "Library"},     {"", ""},
}};
constexpr int kNavRowH = 14, kNavRows = 6;      // rows per column
constexpr int kNavH = (kNavRows + 2) * kNavRowH + 6;   // title + moves + 6 rows: 118 px
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
void drawNavKeys(int y0, const NavCard& card = kAtlasKeys) {
    fillRect(0, y0, W, kNavH, false);
    box(0, y0, W, kNavH, 1);
    auto w12 = [](const char* s) {
        return (int)lv_txt_get_width(s, strlen(s), &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE);
    };
    smallText(4, y0 + 3, card.title, false);
    smallText(W - 4 - w12("K hide") - 1, y0 + 3, "K hide", false);
    smallText(4, y0 + 3 + kNavRowH, card.moves, false);
    const NavKeyRow* rows = card.rows;
    const int top = y0 + 3 + 2 * kNavRowH;
    int keyW[2] = {16, 16}, colEnd = 0;
    for (int i = 0; i < 2 * kNavRows; i++)          // widest key per column
        if (rows[i].keys && rows[i].keys[0] && w12(rows[i].keys) + 1 > keyW[i / kNavRows])
            keyW[i / kNavRows] = w12(rows[i].keys) + 1;           // +1: the bold offset
    for (int i = 0; i < kNavRows; i++) {             // where the left column ends
        if (!rows[i].keys || !rows[i].keys[0]) continue;
        const int e = keyW[0] + 6 + w12(rows[i].what);
        if (e > colEnd) colEnd = e;
    }
    const int x1 = std::max(4 + colEnd + 10, W / 3);
    // The right column lines up after its widest key only if every row still fits; else each
    // text follows its own key.
    bool aligned = true;
    for (int i = kNavRows; i < 2 * kNavRows; i++)
        if (rows[i].keys && rows[i].keys[0] && x1 + keyW[1] + 6 + w12(rows[i].what) > W - 2) aligned = false;
    for (int i = 0; i < 2 * kNavRows; i++) {
        if (!rows[i].keys || !rows[i].keys[0]) continue;
        const int c = i / kNavRows, x = c ? x1 : 4, y = top + (i % kNavRows) * kNavRowH;
        smallText(x, y, rows[i].keys, false);
        const int dx = c == 0 || aligned ? keyW[c] + 6 : std::max(w12(rows[i].keys) + 7, 22);
        smallText(x + dx, y, rows[i].what, false);
        if (x + dx + w12(rows[i].what) > W - 2)
            Serial.printf("[NAV] key card row \"%s\" runs past the edge\n", rows[i].keys);
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
    // Over the Atlas / Library / a game, Contacts and Inbox are not where you are: no highlight.
    const bool over = mapview::isOpen() || g_libOpen || g_gameOpen || g_chessOpen;
    navBox(0, BOT_TOP, BOT_C_W, BOT_H, "Contact", g_screen == SCR_CONTACTS && !over, hk ? 'C' : 0);
    // Middle tab: where you are + "K keys" over the Atlas / Library / games (K shows the key
    // card); Menu in Minimal; else Atlas when the card has maps, else Peers.
    if (over) {
        const char* where = mapview::isOpen() ? "Atlas" : g_chessOpen ? "Chess" : g_gameOpen ? "Trap"
                          : g_haveLib ? "Library" : "Games";
        box(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, 3);
        label(BOT_C_W, BOT_TOP + 2, BOT_A_W, 16, where, true);
        label(BOT_C_W, BOT_TOP + 19, BOT_A_W, 16, g_navKeysOpen ? "K hide" : "K keys", g_navKeysOpen, kKeyNav);
        addHit(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, H_NAVKEYS);
    } else if (minimalMode() || (!(g_haveMaps && feature(F_ATLAS)) && !feature(F_PEERS))) {
        navBox(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, "Menu", g_menuOpen, hk ? 'M' : 0);
        if (hk) addHit(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, H_MENU);
    } else if (g_haveMaps && feature(F_ATLAS)) {
        navBox(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, "Atlas", false, hk ? 'A' : 0);
        if (hk) addHit(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, H_MENU_GO, kMenuMap);
    } else {
        navBox(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, "Peers", g_screen == SCR_PEERS && !over, hk ? 'P' : 0);
        if (hk) addHit(BOT_C_W, BOT_TOP, BOT_A_W, BOT_H, H_TAB, SCR_PEERS);
    }
    inboxNavBox(BOT_C_W + BOT_A_W, BOT_TOP, BOT_I_W, BOT_H, g_screen == SCR_INBOX && !over, hk ? 'I' : 0);
    if (hk) {
        addHit(0, BOT_TOP, BOT_C_W, BOT_H, H_TAB, SCR_CONTACTS);
        addHit(BOT_C_W + BOT_A_W, BOT_TOP, BOT_I_W, BOT_H, H_TAB, SCR_INBOX);
    }
}

// Menu rows shown: Minimal = Settings, Contacts, Inbox; else all, Atlas only with maps on the
// card. Library reads "Games" when the card has no library files (Rat Trap, Chess live there).
bool menuShown(int i) {
    if (minimalMode()) return kMenuKeys[i] == 'S' || kMenuKeys[i] == 'C' || kMenuKeys[i] == 'I';
    if (i == kMenuMap) return g_haveMaps && feature(F_ATLAS);
    if (i == kMenuLib) return feature(F_LIBRARY);
    if (kMenuKeys[i] == 'P') return feature(F_PEERS);
    return true;
}
const char* menuLabel(int i) { return i == kMenuLib && !g_haveLib ? "Games" : kMenuLabels[i]; }

void drawMenuOverlay() {
    if (!g_menuOpen) return;
    int row = 0;
    for (int i = 0; i < kMenuCount; i++) {
        if (!menuShown(i)) continue;
        const int y = TOP_H + row++ * MENU_ROW_H;
        navBox(MENU_X, y, TAB_W, MENU_ROW_H, menuLabel(i), i == g_menuSel, kMenuKeys[i]);
        addHit(MENU_X, y, TAB_W, MENU_ROW_H, H_MENU_GO, i);
    }
}

// ---- Offline map (MapView.cpp; tiles from the SD card) ----------------------------------
void gpsRequestVisit(const char* why = "location share");
void mapGpsAsk(bool opening);
bool gpsCurrent();
bool gpsFresh();
uint32_t g_cardWaitFails = 0, g_cardWaitMaxMs = 0;   // waits for the storage lease: timed out / longest
uint32_t g_cardWaitGaveUpAt = 0;                     // millis() of the last wait that timed out
bool g_mapSdTried = false;
File g_mapFile;                  // pack file kept open between reads
char g_mapFilePath[80] = "";
uint32_t g_looseChecked = 0, g_looseHave = 0, g_looseAt = 0;   // per zoom: /maps/<z> looked up / exists

// Card calls from the main loop fail at once while the storage worker holds the storage
// lease (it is saving a message: up to ~1 s on a busy unit). For the map and for uploads a
// short wait is better than a file reported missing, so hold the lease across the call.
// After one wait has timed out the worker is in a long job (a big index save): the next
// calls do not wait again for 4 s, or a map view of many tiles would freeze the unit for
// a multiple of the wait. Those tiles show as missing and are looked up again later.
struct CardWait {
    handheld::storage::StorageLease lease{false};
    bool ok = false;
    explicit CardWait(uint32_t maxMs = 1200) {
        const uint32_t t0 = millis();
        if (g_cardWaitGaveUpAt && t0 - g_cardWaitGaveUpAt < 4000) maxMs = 0;
        while (!(ok = lease.tryAcquire()) && millis() - t0 < maxMs) delay(2);
        if (!ok) { g_cardWaitFails++; if (maxMs) g_cardWaitGaveUpAt = millis() | 1; }
        else if (millis() - t0 > g_cardWaitMaxMs) g_cardWaitMaxMs = millis() - t0;
    }
};

size_t mapReadTile(const char* rel, uint32_t offset, uint8_t* buf, size_t len) {
    CardWait card;
    if (!g_sdMounted) {
        if (g_mapSdTried) return 0;          // one mount attempt per map visit
        g_mapSdTried = true;
        if (!sdMount()) return 0;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s%s", SD_PATH_ROOT, rel);
    // Pack files are read many times in a row (header, index, then one piece per tile):
    // keep the last one open, since a fresh exists() + open costs ~45 ms on the card.
    const bool pack = strncmp(rel, "/maps/packs/", 12) == 0 || strncmp(rel, "/packs/", 7) == 0;
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
    g_gv = GV_NONE;
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
    g_gv = GV_NONE;
    g_mk = MK_NONE;
    g_layOpen = false;
    g_markSel = -1;
    g_mapSel = -1;                           // callers that show a contact select it after
    mapview::setReader(mapReadTile);
    g_mapSdTried = false;
    mapFeedPosition();
    mapGpsAsk(true);
    // Centre on the newest position: a fresh fix, the last fix, the saved position (S.2).
    double lat = 0, lon = 0; bool have = false;
    const char* how = "no position";
    if (g_gpsPosValid) { lat = g_gpsLat; lon = g_gpsLon; have = true; how = gpsCurrent() ? "GPS" : "last fix"; }
    else if (g_cfg.hasPos) { lat = g_cfg.lat; lon = g_cfg.lon; have = true; how = "saved position"; }
    if (g_menuOpen) g_menuOpen = false;
    // No position: the card's own start view, maps/home.txt = "lat lon zoom"
    // (written by tools/maptiles/make_tiles.py), so the first view is not empty sea.
    bool cardHome = false;
    int homeZoom = 0;
    if (!have) {
        char txt[48] = "";
        size_t n = mapReadTile("/maps/home.txt", 0, (uint8_t*)txt, sizeof(txt) - 1);
        if (!n) n = mapReadTile("/packs/home.txt", 0, (uint8_t*)txt, sizeof(txt) - 1);
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
                sp.full = false; sp.usedPct = (uint8_t)((uint64_t)used * 100 / total); sp.checkEvery = 0;
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
bool gpsCurrent();

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
    else if (g_markSel >= 0) snprintf(st, sizeof(st), "J list  Alt+J new  D me");
    else {
        mapview::status(st, sizeof(st));
        for (int s = 1; s <= 2 && textW(st) > W - 8; s++) mapview::status(st, sizeof(st), s);
    }
    drawHeader(st, false);
    char pos[56];
    if (g_placeOpen) renderPlaces();
    if (g_entry == Entry::WptName) snprintf(pos, sizeof(pos), "Name: %s_", g_buf);
    else if (g_entry == Entry::PlaceFind) snprintf(pos, sizeof(pos), "Find place: %s_", g_buf);
    else if (g_toast[0]) snprintf(pos, sizeof(pos), "%s", g_toast);               // key hints
    else if (markInfo(pos, sizeof(pos))) {}
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
// map. Auto-share: per contact or group, OFF by default, NVS rs_ashare; Check-in mode queues
// to the window. What it sends and when: "Status sharing" below.
constexpr const char* kLocPrefix = "My location: ";
constexpr const char* kLocLastPrefix = "My last location: ";
constexpr const char* kLocAutoPrefix = "Loc: ";
constexpr uint32_t kFreshFixMs = 120000;
constexpr int kAutoShareMax = 8;
const uint8_t kAutoMins[] = {0, 5, 15, 30, 60};           // 0 = off
const uint16_t kAutoForMin[] = {60, 240, 0};               // 0 = until turned off
const char* const kAutoForName[] = {"1 h", "4 h", "until off"};

// Status sharing (2026-11-22): a share carries the position, the battery %, or both.
//   manual  "My location: 43.64012, -70.92011 (+-10 m, 14:32). Battery 84%"  /  "My battery: 84% (14:32)"
//   auto    "Loc: 43.64012, -70.92011 14:32 Bat 84%"                         /  "Bat: 84% 14:32"
// A one-time share also carries Sideband's telemetry field (LxmfFields.h). The receiver keeps
// the newest position and battery per contact (rs_cpos): Atlas marker, contact card. An
// automatic update makes no alert and shows as one "[loc auto]" line in the chat (the newest).
// Automatic: per contact or group, OFF by default, starts with position + battery. An update
// is due every interval and goes out when the radio moved > 50 m or the battery changed by
// >= 5 points, else at every second interval. No fix within 3 minutes (GPS off, standby, a
// power mode without GPS, no sky): the battery goes alone and the receiver keeps showing the
// last position with its age.
enum : uint8_t { SH_POS = 1, SH_BAT = 2 };
constexpr const char* kBatPrefix = "My battery: ";
constexpr const char* kBatAutoPrefix = "Bat: ";
constexpr float kAutoMoveM = 50.0f;
constexpr int kAutoBatStep = 5;
constexpr uint32_t kAutoFixWaitMs = 180000;
uint8_t g_shareWhat = SH_POS | SH_BAT;   // what "share now" sends and a new auto-share starts with

struct AutoShare {
    uint8_t dest[16];        // a contact, or a group's row address (isGrpHash)
    uint8_t mins;            // interval
    uint8_t what;            // SH_POS | SH_BAT (0 = saved before 2026-11-22: position)
    uint8_t lastBat;         // battery % + 1 last sent (0 = none yet)
    uint8_t pad;
    uint32_t until;          // epoch s, 0 = until turned off
    uint32_t lastAt;         // epoch s of the last update sent
    float lastLat, lastLon;
};
AutoShare g_ashare[kAutoShareMax];
int g_ashareCount = 0;
uint8_t g_autoForSel = 1;    // duration picked in the thread menu (4 h)

bool gpsFresh() { return g_gpsFix && g_gpsPosValid && millis() - g_gpsPosAt < kFreshFixMs; }

// "Current" for what the screens show: in a mode where the GPS rests between visits, the last
// fix stays good until two visits in a row have failed (2.5 x the interval + a visit), so the
// Atlas does not say "no fix" just because the receiver is waiting for its next visit.
uint32_t gpsCurrentMs() {
    const uint32_t ivl = power::gpsIntervalMs(g_pwEff.gps);
    return ivl ? ivl * 5 / 2 + power::kGpsWindowMs : kFreshFixMs;
}
bool gpsCurrent() { return g_gpsPosValid && (gpsFresh() || millis() - g_gpsPosAt < gpsCurrentMs()); }

void fmtAge(uint32_t s, char* out, size_t n) {
    if (s < 90) snprintf(out, n, "%u s", (unsigned)s);
    else if (s < 5400) snprintf(out, n, "%u min", (unsigned)((s + 30) / 60));
    else snprintf(out, n, "%u h", (unsigned)((s + 1800) / 3600));
}

// ---- Coordinate formats (S.2.12) ---------------------------------------------------
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
    if (g_pwEff.gps == power::GPS_OFF) { snprintf(out, n, g_standby ? "GPS off (standby)" : "GPS off (power)"); return; }
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
    char st[28];
    if (gpsCurrent()) {                      // between visits: the last fix with its age
        char c[40], age[8];
        fmtCoord(g_gpsLat, g_gpsLon, c, sizeof(c));
        const uint32_t a = (millis() - g_gpsPosAt) / 1000;
        if (a < 90) snprintf(age, sizeof(age), "%us", (unsigned)a);
        else snprintf(age, sizeof(age), "%um", (unsigned)((a + 30) / 60));
        snprintf(out, n, "%s %s ago", c, age);
        return;
    }
    // No current fix: what the receiver is doing, after the age of the last fix (or "set" =
    // the saved position S.2 is what the map shows). Fits the 240 px footer.
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

// The battery % of a status share (alone, or after a position).
bool batParse(const char* s, size_t len, int& pct, bool* autoUpdate) {
    char t[120];
    if (len >= sizeof(t)) len = sizeof(t) - 1;
    memcpy(t, s, len);
    t[len] = 0;
    const char* p = nullptr;
    bool au = false;
    if (!strncmp(t, kBatPrefix, strlen(kBatPrefix))) p = t + strlen(kBatPrefix);
    else if (!strncmp(t, kBatAutoPrefix, strlen(kBatAutoPrefix))) { p = t + strlen(kBatAutoPrefix); au = true; }
    else if (!strncmp(t, kLocAutoPrefix, strlen(kLocAutoPrefix))) { p = strstr(t, " Bat "); if (p) p += 5; au = true; }
    else if (!strncmp(t, kLocPrefix, strlen(kLocPrefix)) || !strncmp(t, kLocLastPrefix, strlen(kLocLastPrefix))) {
        p = strstr(t, "Battery ");
        if (p) p += 8;
    }
    if (!p) return false;
    char* e = nullptr;
    const long v = strtol(p, &e, 10);
    if (e == p || *e != '%' || v < 0 || v > 100) return false;
    pct = (int)v;
    if (autoUpdate) *autoUpdate = au;
    return true;
}

const char* shareWhatName(uint8_t what) {
    return what == SH_BAT ? "battery" : what == SH_POS ? "position" : "position + battery";
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

// A status text to a contact, or to every member of a group (dest = the group's row address).
SendResult statusSend(const uint8_t dest[16], const char* body) {
    if (!isGrpHash(dest)) return locSendOrQueue(dest, body);
    int queued = 0;
    const int n = groupSend(grpOfHash(dest), body, &queued);
    requestRedraw();
    return !n ? SendResult::Failed : queued == n ? SendResult::Queued : SendResult::Sent;
}

// One-time share: position, battery % or both (what = SH_POS | SH_BAT). With no position to
// send, "both" sends the battery alone.
// Incoming telemetry (Sideband's FIELD_TELEMETRY, also sent by RatSpeak radios with a one-time
// share): the sender's position and battery go where a shared position goes (Atlas, contact
// page), so Sideband users show on the map without any text to read.
void onTelemetry(const uint8_t src[16], const handheld::lxmf::Telemetry& t) {
    if (isSelf(src)) return;
    const time_t now = time(nullptr);
    const uint32_t at = t.positionTime ? t.positionTime : t.time ? t.time : (now > 1600000000 ? (uint32_t)now : 0);
    char nm[24];
    peerName(src, nm, sizeof(nm));
    Serial.printf("[LOC] telemetry from %s:%s%s\n", nm, t.hasPosition ? " position" : "", t.hasBattery ? " battery" : "");
    if (t.hasPosition) cposNote(src, t.latitude, t.longitude, at);
    if (t.hasBattery) batNote(src, t.percent, t.time ? t.time : at);
}

void shareMyStatus(const uint8_t dest[16], uint8_t what) {
    char body[128] = "", hhmm[8], age[16];
    if (!(what & (SH_POS | SH_BAT))) what = SH_POS;
    const bool haveBat = (what & SH_BAT) && g_bat.valid;
    const time_t now = time(nullptr);
    handheld::lxmf::Telemetry tm;
    tm.time = now > 1600000000 ? (uint32_t)now : 0;
    localHHMM(hhmm, sizeof(hhmm));
    bool fresh = false;
    if (what & SH_POS) {
        tm.hasPosition = true;
        tm.latitude = g_gpsLat; tm.longitude = g_gpsLon; tm.positionTime = tm.time;
        if (gpsFresh()) {
            fresh = true;
            const int acc = g_gpsHdop > 0 ? (int)(g_gpsHdop * 5.0f + 0.5f) : 0;
            tm.accuracyM = (float)acc;
            char grid[28] = "";
            if (g_cfg.coordFmt == 2) { mgrsText(g_gpsLat, g_gpsLon, grid, sizeof(grid)); strlcat(grid, ", ", sizeof(grid)); }
            if (acc > 0) snprintf(body, sizeof(body), "%s%.5f, %.5f (%s+-%d m, %s)", kLocPrefix, g_gpsLat, g_gpsLon, grid, acc < 3 ? 3 : acc, hhmm);
            else snprintf(body, sizeof(body), "%s%.5f, %.5f (%s%s)", kLocPrefix, g_gpsLat, g_gpsLon, grid, hhmm);
        } else if (g_gpsPosValid) {
            const uint32_t old = (millis() - g_gpsPosAt) / 1000;
            fmtAge(old, age, sizeof(age));
            if (tm.time > old) tm.positionTime = tm.time - old;
            snprintf(body, sizeof(body), "%s%.5f, %.5f (%s old)", kLocLastPrefix, g_gpsLat, g_gpsLon, age);
        } else if (g_cfg.hasPos) {
            tm.latitude = g_cfg.lat; tm.longitude = g_cfg.lon;
            snprintf(body, sizeof(body), "%s%.5f, %.5f (set by hand)", kLocLastPrefix, g_cfg.lat, g_cfg.lon);
        } else {
            tm.hasPosition = false;
            if (!haveBat) { toast(g_cfg.gpsOn ? "No GPS position yet" : "GPS is off (S.2)"); return; }
        }
    }
    if (!body[0] && !haveBat) { toast("No battery reading yet"); return; }
    if (haveBat) {
        tm.hasBattery = true;
        tm.percent = (uint8_t)g_bat.pct;
        tm.charging = g_bat.charging;
        const size_t at = strlen(body);
        if (at) snprintf(body + at, sizeof(body) - at, ". Battery %d%%", g_bat.pct);
        else snprintf(body, sizeof(body), "%s%d%% (%s)", kBatPrefix, g_bat.pct, hhmm);
    }
    g_txExtraLen = tm.time ? handheld::lxmf::makeTelemetryEntry(tm, g_txExtra, sizeof(g_txExtra)) : 0;
    const SendResult r = statusSend(dest, body);
    g_txExtraLen = 0;
    Serial.printf("[LOC] share %s: %s\n", r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "queued" : "failed", body);
    if ((what & SH_POS) && !gpsFresh() && g_cfg.gpsOn) gpsRequestVisit();       // the next share can be fresh
    const char* const nm = !tm.hasPosition ? "Battery" : !haveBat ? (fresh ? "Location" : "Last location")
                                                                  : (fresh ? "Location + battery" : "Last loc. + battery");
    if (r == SendResult::Failed) toast("Not sent: %s", g_txStr);
    else toast("%s %s", nm, r == SendResult::Sent ? "sent" : "queued");
}

void shareMyLocation(const uint8_t dest[16]) { shareMyStatus(dest, SH_POS); }

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
uint8_t ashareWhat(const AutoShare& a) { return (a.what & (SH_POS | SH_BAT)) ? (a.what & (SH_POS | SH_BAT)) : SH_POS; }
// "loc+bat" / "loc" / "bat" for the chat header mark; "" when this chat has no automatic share.
const char* ashareMarkFor(const uint8_t dest[16]) {
    const int i = ashareFind(dest);
    if (i < 0) return "";
    const uint8_t w = ashareWhat(g_ashare[i]);
    return w == SH_BAT ? "bat" : w == SH_POS ? "loc" : "loc+bat";
}
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
            g_ashare[i].what = g_shareWhat;
        }
        g_ashare[i].mins = next;
        const uint16_t forMin = kAutoForMin[g_autoForSel];
        const time_t now = time(nullptr);
        g_ashare[i].until = forMin ? (uint32_t)now + forMin * 60UL : 0;
        if (!g_cfg.gpsOn && (ashareWhat(g_ashare[i]) & SH_POS)) toast("Every %u min; GPS is OFF (S.2)", (unsigned)next);
        else toast("Every %u min, %s", (unsigned)next, kAutoForName[g_autoForSel]);
    }
    ashareSave();
    Serial.printf("[LOC] auto-share %u min (%d on)\n", (unsigned)next, g_ashareCount);
    requestRedraw();
}

// What a chat's shares carry: position + battery -> position -> battery. Applies to "share
// now" and to this chat's automatic share when it is on.
uint8_t shareWhatFor(const uint8_t dest[16]) {
    const int i = ashareFind(dest);
    return i >= 0 ? ashareWhat(g_ashare[i]) : g_shareWhat;
}

void shareWhatSet(const uint8_t dest[16], uint8_t what) {
    g_shareWhat = what;
    const int i = ashareFind(dest);
    if (i >= 0) { g_ashare[i].what = what; g_ashare[i].lastAt = 0; ashareSave(); }   // lastAt 0: the next one goes now
    Serial.printf("[LOC] sharing %s\n", shareWhatName(what));
    requestRedraw();
}

void shareWhatCycle(const uint8_t dest[16]) {
    const uint8_t cur = shareWhatFor(dest);
    shareWhatSet(dest, cur == (SH_POS | SH_BAT) ? SH_POS : cur == SH_POS ? SH_BAT : (SH_POS | SH_BAT));
}

float metersBetween(double la1, double lo1, double la2, double lo2) {
    const double k = 3.14159265358979 / 180.0;
    const double x = (lo2 - lo1) * k * cos((la1 + la2) * 0.5 * k), y = (la2 - la1) * k;
    return (float)(sqrt(x * x + y * y) * 6371000.0);
}

void autoShareTick() {
    static uint32_t last = 0;
    static uint32_t waitSince[kAutoShareMax] = {};   // millis when an update first waited for a fix
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
        if (isGrpHash(a.dest) && grpOfHash(a.dest) < 0) {       // the group was deleted
            Serial.println("[LOC] auto-share to a deleted group ended");
            ashareRemove(i--);
            ashareSave();
            requestRedraw();
            continue;
        }
        const uint32_t iv = a.mins * 60UL;
        if (a.lastAt && (uint32_t)now - a.lastAt < iv) { waitSince[i] = 0; continue; }
        const uint8_t what = ashareWhat(a);
        // The receiver is off in standby, in a power mode without GPS and with GPS off (S.2).
        bool pos = (what & SH_POS) && g_cfg.gpsOn && g_pwEff.gps != power::GPS_OFF;
        if (pos && !gpsFresh()) {            // no fresh fix: power the GPS for a visit
            static uint32_t asked = 0;
            if (!asked || millis() - asked > 180000) { asked = millis(); gpsRequestVisit(); }
            if (!waitSince[i]) waitSince[i] = millis() | 1;
            if (!(what & SH_BAT) || millis() - waitSince[i] < kAutoFixWaitMs) continue;
            pos = false;                     // no fix in 3 minutes: the battery goes alone
        }
        const bool bat = (what & SH_BAT) && g_bat.valid;
        if (!pos && !bat) continue;
        const bool moved = pos && (!a.lastAt || metersBetween(a.lastLat, a.lastLon, g_gpsLat, g_gpsLon) > kAutoMoveM);
        const bool batChanged = bat && (!a.lastBat || abs(g_bat.pct - (int)(a.lastBat - 1)) >= kAutoBatStep);
        if (a.lastAt && !moved && !batChanged && (uint32_t)now - a.lastAt < 2 * iv) continue;
        char body[80], hhmm[8];
        localHHMM(hhmm, sizeof(hhmm));
        if (pos && bat) snprintf(body, sizeof(body), "%s%.5f, %.5f %s Bat %d%%", kLocAutoPrefix, g_gpsLat, g_gpsLon, hhmm, g_bat.pct);
        else if (pos) snprintf(body, sizeof(body), "%s%.5f, %.5f %s", kLocAutoPrefix, g_gpsLat, g_gpsLon, hhmm);
        else snprintf(body, sizeof(body), "%s%d%% %s", kBatAutoPrefix, g_bat.pct, hhmm);
        const SendResult r = statusSend(a.dest, body);
        Serial.printf("[LOC] auto %s: %s\n", r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "queued" : "failed", body);
        if (r != SendResult::Failed) {
            waitSince[i] = 0;
            a.lastAt = (uint32_t)now;
            if (pos) { a.lastLat = (float)g_gpsLat; a.lastLon = (float)g_gpsLon; }
            if (bat) a.lastBat = (uint8_t)(g_bat.pct + 1);
            ashareSave();
            if (g_threadOpen && !memcmp(g_threadPeer, a.dest, 16)) loadHistory(g_threadPeer);
        }
        // No break: every share that is due goes in this pass (the second and later ones wait
        // in the outbox for the 3 s spacing), so a radio that wakes for a few seconds between
        // sleeps (standby, Saver) serves all of them in one wake.
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
// bat = their battery % + 1 from a status share (0 = none yet), batAt = when.
struct CPos { uint8_t dest[16]; float lat, lon; uint32_t at; uint8_t num, hasPos, bat, pad; uint32_t batAt; };
struct CPosV2 { uint8_t dest[16]; float lat, lon; uint32_t at; uint8_t num, hasPos, pad[2]; };   // NVS before 2026-11-22
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
    } else if (n > 0 && n <= kCPosMax && len == sizeof(CPosV2) * (size_t)n) {   // migrate: no battery yet
        CPosV2 old[kCPosMax];
        if (p.getBytes("list", old, len) == len) {
            for (int i = 0; i < n; i++) {
                memset(&g_cpos[i], 0, sizeof(CPos));
                memcpy(&g_cpos[i], &old[i], offsetof(CPosV2, pad));
            }
            g_cposCount = n;
        }
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

// ---- Arrival alerts (Settings S.9 Alerts > Arrival alerts) ---------------------------------
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
// A contact's battery % from a status share (contact card, thread menu).
void batNote(const uint8_t dest[16], int pct, uint32_t at) {
    if (isSelf(dest) || pct < 0 || pct > 100) return;
    const int found = cposFind(dest);
    if (found >= 0 && g_cpos[found].bat && g_cpos[found].batAt >= at) return;   // have this one or a newer
    const int i = cposSlot(dest);
    g_cpos[i].bat = (uint8_t)(pct + 1);
    g_cpos[i].batAt = at;
    cposSave();
    char nm[24];
    peerName(dest, nm, sizeof(nm));
    Serial.printf("[LOC] %s battery %d%%\n", nm, pct);
    requestRedraw();
}

// "84%, 12 min old" for a contact; false when no battery report came from them.
bool batText(const uint8_t dest[16], char* out, size_t n) {
    const int i = cposFind(dest);
    if (i < 0 || !g_cpos[i].bat) return false;
    const time_t now = time(nullptr);
    char age[12] = "?";
    if (g_cpos[i].batAt) fmtAge(now > (time_t)g_cpos[i].batAt ? (uint32_t)(now - g_cpos[i].batAt) : 0, age, sizeof(age));
    snprintf(out, n, "%u%%, %s old", (unsigned)(g_cpos[i].bat - 1), age);
    return true;
}

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
void noteInput();

// ---- Walkie-talkie (contact card W; voice/Walkie) ----------------------------------------
bool     g_wtOpen = false;            // the walkie-talkie screen is up
uint8_t  g_wtPeer[16];
bool     g_wtIncoming = false;        // an invite waits for Enter / ^
uint32_t g_wtIp = 0;
uint16_t g_wtPort = 0;
uint8_t  g_wtKey[32];
uint32_t g_wtInviteAt = 0;
char     g_wtSsid[33] = "";           // the caller's hotspot for this call ("" = shared WiFi)
char     g_wtPass[33] = "";
bool     g_wtJoining = false;         // callee: joining the caller's hotspot
uint32_t g_wtJoinAt = 0;

// After a hotspot call: hotspot off / leave it, and let the normal WiFi logic take over.
void wtReleaseWifi() {
    if (!g_wtWifiOwned) return;
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    g_wtWifiOwned = false;
    g_wtJoining = false;
    g_wifiSt = WifiSt::Off;
    g_wifiNextAuto = millis() + 2000;           // back on the usual network soon
    Serial.println("[WT] hotspot / call WiFi released");
}

void wtClose() {
    if (walkie::state() != walkie::State::Idle) walkie::hangUp("hung up");
    wtReleaseWifi();
    g_wtOpen = false;
    g_wtIncoming = false;
    g_fullRefreshReq = true;
    requestRedraw();
}

void wtCall(const uint8_t peer[16]) {
    if (walkie::state() != walkie::State::Idle) { toast("A call is already open"); return; }
    char invite[160];
    uint32_t myIp;
    const char* apSsid = nullptr;
    if (wifiConnected()) {                       // both on the same WiFi: use it
        myIp = (uint32_t)WiFi.localIP();
        myIp = __builtin_bswap32(myIp);          // IPAddress -> host order (a.b.c.d = a<<24 ...)
    } else {                                     // no WiFi: this unit becomes a hotspot for the call
        static const char* kAlnum = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        snprintf(g_wtSsid, sizeof(g_wtSsid), "RatSpeak-%04X", (unsigned)(esp_random() & 0xFFFF));
        for (int i = 0; i < 16; i++) g_wtPass[i] = kAlnum[esp_random() % strlen(kAlnum)];
        g_wtPass[16] = 0;
        g_wtWifiOwned = true;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP(g_wtSsid, g_wtPass, 6, 0, 1)) { wtReleaseWifi(); toast("Hotspot failed"); return; }
        delay(100);
        myIp = __builtin_bswap32((uint32_t)WiFi.softAPIP());
        apSsid = g_wtSsid;
        Serial.printf("[WT] hotspot %s up at %s\n", g_wtSsid, WiFi.softAPIP().toString().c_str());
    }
    if (!walkie::startCall(invite, sizeof(invite), myIp, apSsid, g_wtPass)) {
        wtReleaseWifi();
        toast("Walkie-talkie: %s", walkie::status());
        return;
    }
    if (!sendMessage(peer, invite)) { walkie::hangUp("invite not sent"); wtReleaseWifi(); toast("Not sent: %s", g_txStr); return; }
    memcpy(g_wtPeer, peer, 16);
    g_wtOpen = true;
    g_wtIncoming = false;
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.println("[WT] invite sent");
}

// An incoming "[wt] call ..." message (inboundTick).
void wtInvite(const uint8_t peer[16], const char* text) {
    uint32_t ip; uint16_t port; uint8_t key[32];
    if (!walkie::parseInvite(text, ip, port, key, g_wtSsid, sizeof(g_wtSsid), g_wtPass, sizeof(g_wtPass))) return;
    if (walkie::state() != walkie::State::Idle) return;     // busy: ignore
    memcpy(g_wtPeer, peer, 16);
    g_wtIp = ip; g_wtPort = port; memcpy(g_wtKey, key, 32);
    g_wtInviteAt = millis();
    g_wtIncoming = true;
    g_wtOpen = true;
    if (g_idleActive) { g_idleActive = false; noteInput(); }
    if (g_cfg.beep) g_audio.playAnnounce();
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.println("[WT] incoming call");
}

void wtKey(const KeyEvent& ev) {
    if (g_wtIncoming) {
        if (ev.enter) {
            g_wtIncoming = false;
            if (g_wtSsid[0]) {                       // the caller's hotspot: join it, answer when on
                g_wtWifiOwned = true;
                WiFi.disconnect(true);
                WiFi.mode(WIFI_STA);
                WiFi.begin(g_wtSsid, g_wtPass);
                g_wtJoining = true;
                g_wtJoinAt = millis();
                Serial.printf("[WT] joining hotspot %s\n", g_wtSsid);
                requestRedraw();
                return;
            }
            if (!wifiConnected()) { toast("Join WiFi first (S.3)"); g_wtOpen = false; requestRedraw(); return; }
            if (!walkie::answer(g_wtIp, g_wtPort, g_wtKey)) toast("Walkie-talkie: %s", walkie::status());
            requestRedraw();
        } else if (ev.up) {
            Serial.println("[WT] declined");
            wtClose();
        }
        return;
    }
    if (ev.up) { wtClose(); return; }
    if (ev.space || ev.character == ' ' || ev.enter) {
        walkie::setTalking(!walkie::talking());
        requestRedraw();
    }
}

void renderWalkie() {
    char nm[24], line[48];
    peerName(g_wtPeer, nm, sizeof(nm));
    drawHeader("Walkie-talkie", false);
    int y = LIST_TOP + 6;
    label(0, y, W, 22, nm, true); y += 34;
    const walkie::State st = walkie::state();
    if (g_wtIncoming) {
        label(0, y, W, 22, "is calling you", true); y += 40;
        label(0, y, W, 20, "Enter = answer"); y += 24;
        label(0, y, W, 20, "^ = decline");
    } else if (g_wtJoining) {
        label(0, y, W, 22, "joining the hotspot...", true); y += 40;
        label(0, y, W, 20, "^ = cancel");
    } else if (st == walkie::State::Connected) {
        const char* big = walkie::talking() ? "TALKING" : walkie::peerTalking() ? "THEY TALK" : "LISTENING";
        label(0, y, W, 30, big, true); y += 50;
        label(0, y, W, 20, walkie::talking() ? "Space = stop, listen" : "Space = talk"); y += 24;
        label(0, y, W, 20, "^ = hang up");
    } else {
        snprintf(line, sizeof(line), "%s", walkie::status());
        label(0, y, W, 22, line, true); y += 40;
        label(0, y, W, 20, st == walkie::State::Calling ? "^ = cancel" : "^ = close");
    }
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : g_wtSsid[0] ? "Codec2 3200, encrypted, hotspot"
                                                                       : "Codec2 3200, encrypted, WiFi");
}

// The new-message alert (beep / vibration) is raised here, per message, once its text is
// known: an automatic status update (position / battery) from a contact makes none, and is
// marked read at once when nothing else is unread, so the new-message count stays honest.
bool g_inMissed = false;                     // the queue was full: alert without reading

void onCommittedMessage(const LXMFManager::CommittedMessage& m) {
    if (!m.key.incoming) return;
    if (g_inQn >= (int)(sizeof(g_inQ) / sizeof(g_inQ[0]))) { g_inMissed = true; return; }
    g_inQ[g_inQn++] = m.key;
}

void inboundTick() {
    if (g_inMissed) { g_inMissed = false; notifyInbound(); }
    if (!g_inQn || !g_storeReady) return;
    const handheld::storage::RecordKey key = g_inQ[0];
    char text[176];                          // a hotspot walkie-talkie invite is ~110 characters
    double ts = 0;
    if (!readRecordText(key, text, sizeof(text), &ts)) {
        static uint8_t tries = 0;
        if (++tries < 20) return;            // store busy: try again next loop
        tries = 0;
        notifyInbound();
    } else {
        char from[33];
        hashToHex(key.peer, from);
        Serial.printf("[MSG] in from %s: \"%.80s\"\n", from, text);   // stress tests match on this
        soakIncoming(key.peer, text);
        double la, lo;
        int bp = 0;
        bool autoL = false, autoB = false;
        if (locParse(text, strlen(text), la, lo, &autoL)) cposNote(key.peer, la, lo, msgEpoch(ts));
        if (batParse(text, strlen(text), bp, &autoB)) batNote(key.peer, bp, msgEpoch(ts));
        markIncoming(key.peer, text, msgEpoch(ts));
        sosIncoming(key.peer, text);
        if (!autoL && !autoB) notifyInbound();
        else if (g_inQn == 1 && g_msgStore.totalUnreadCount() == 1) {   // the only unread message is this one
            if (g_markReadPending) { if (!g_markReadNext[0]) strlcpy(g_markReadNext, from, sizeof(g_markReadNext)); }
            else markReadStart(from);
            Serial.println("[LOC] status update: no alert, marked read");
        }
        wizCardIn(text);
        if (!strncmp(text, walkie::kInvitePrefix, strlen(walkie::kInvitePrefix))) wtInvite(key.peer, text);
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
int markFeed(mapview::Marker* m, int n);
int takFeed(mapview::Marker* m, int n);

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
    n = markFeed(m, n);                      // items of interest (triangles)
    n = takFeed(m, n);                       // ATAK units (squares)
    mapview::setMarkers(m, n);
}

// ---- Waypoints + breadcrumb track ------------------------------------------------------
// Waypoints: named places (up to 32, NVS rs_wpt), drawn as diamonds. Map keys: P = mark a
// waypoint at the map centre (name it, Enter), N = next waypoint (centred, selected, a
// dashed guide line from your position, footer "W2 Ballfield 2.1km SW"), Del twice =
// remove the selected one. Settings S.2.9 lists them (Enter = show on the map).
// Track: Settings S.2.10 on/off. Records a point when the GPS fix moved > 25 m (fresh fix
// only; in Balanced / Saver it asks the GPS for a visit every ~2 min, so points are
// sparser and the battery cost stays low). Last 1000 points in RAM, all of them appended
// to <SD>/ratdeckpro/tracks/track.bin (uint32 epoch, float lat, float lon); dotted line on
// the map. S.2.11 clears it.

void wptSave() {
    Preferences p;
    if (!p.begin("rs_wpt", false)) return;
    p.putBytes("list", g_wpt, sizeof(Wpt) * g_wptCount);
    p.putUChar("n", (uint8_t)g_wptCount);
    p.end();
}

void homeSet(double lat, double lon, const char* how) {
    g_home.lat = (float)lat; g_home.lon = (float)lon; g_home.set = true;
    Preferences p;
    if (p.begin("rs_wpt", false)) {
        p.putBool("hset", true); p.putFloat("hlat", g_home.lat); p.putFloat("hlon", g_home.lon);
        p.end();
    }
    Serial.printf("[HOME] set (%s): %.5f, %.5f%c", how, lat, lon, 10);
}

void homeClear() {
    g_home = HomePt{};
    Preferences p;
    if (p.begin("rs_wpt", false)) { p.putBool("hset", false); p.end(); }
    Serial.println("[HOME] cleared");
}

// "212 mi NE" from here to Home; false = no Home or no own position.
bool homeText(char* out, size_t n) {
    double la, lo;
    if (!g_home.set || !ownPosition(la, lo)) return false;
    distText(la, lo, g_home.lat, g_home.lon, out, n);
    return true;
}

// Sunrise and sunset today at the own position, local time "rise 07:02 set 16:31" (S.2 row
// 16, console "sun"). The usual almanac method (zenith 90.833 deg), good to a minute or two;
// "no sunrise today" / "no sunset today" inside the polar circles. false = no position or clock.
bool sunUt(double lat, double lon, int dayOfYear, bool rise, double& utHours) {
    const double r = 3.14159265358979 / 180.0, lngHour = lon / 15.0;
    const double t = dayOfYear + ((rise ? 6.0 : 18.0) - lngHour) / 24.0;
    const double M = 0.9856 * t - 3.289;
    double L = fmod(M + 1.916 * sin(M * r) + 0.020 * sin(2 * M * r) + 282.634 + 720.0, 360.0);
    double RA = fmod(atan(0.91764 * tan(L * r)) / r + 360.0, 360.0);
    RA = (RA + (floor(L / 90.0) * 90.0 - floor(RA / 90.0) * 90.0)) / 15.0;
    const double sinDec = 0.39782 * sin(L * r), cosDec = cos(asin(sinDec));
    const double cosH = (cos(90.833 * r) - sinDec * sin(lat * r)) / (cosDec * cos(lat * r));
    if (cosH > 1 || cosH < -1) return false;
    const double H = (rise ? 360.0 - acos(cosH) / r : acos(cosH) / r) / 15.0;
    utHours = fmod(H + RA - 0.06571 * t - 6.622 - lngHour + 48.0, 24.0);
    return true;
}

bool sunText(char* out, size_t n) {
    double la, lo;
    const time_t now = time(nullptr);
    if (now < 1600000000 || !ownPosition(la, lo)) return false;
    struct tm g, l;
    localtime_r(&now, &l);
    // The local calendar day decides which sunrise: take noon local time, in UTC.
    const time_t noon = now - (l.tm_hour * 3600 + l.tm_min * 60 + l.tm_sec) + 12 * 3600;
    gmtime_r(&noon, &g);
    const time_t utcMidnight = noon - (g.tm_hour * 3600 + g.tm_min * 60 + g.tm_sec);
    char part[2][12];
    for (int k = 0; k < 2; k++) {
        double ut;
        if (!sunUt(la, lo, g.tm_yday + 1, k == 0, ut)) { snprintf(out, n, k == 0 ? "no sunrise today" : "no sunset today"); return true; }
        time_t at = utcMidnight + (time_t)(ut * 3600.0 + 0.5);
        if (at < noon - 12 * 3600) at += 86400;          // keep it on the local day
        if (at >= noon + 12 * 3600) at -= 86400;
        struct tm e;
        localtime_r(&at, &e);
        snprintf(part[k], sizeof(part[k]), "%02d:%02d", e.tm_hour, e.tm_min);
    }
    snprintf(out, n, "rise %s  set %s", part[0], part[1]);
    return true;
}

void wptLoad() {
    Preferences p;
    g_wptCount = 0;
    if (!p.begin("rs_wpt", true)) return;
    g_home.set = p.getBool("hset", false);
    g_home.lat = p.getFloat("hlat", 0); g_home.lon = p.getFloat("hlon", 0);
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
    static uint8_t (&rb)[2048] = psram<uint8_t, 2048>();                 // buffered line reader (works on the simulator too)
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

void render();

// ---- Voice messages (chat menu 5; LXMF FIELD_AUDIO, Sideband compatible) -------------------
// Codec2 700C (~88 bytes per second: fine for LoRa), up to 20 s. Recorded on the main loop
// from the mic ring (an e-ink refresh does not lose audio); sent with lxmfSubmitAudio.
// P plays the newest voice message in the chat.
constexpr int kVmMaxSec = 20;
bool     g_vmOpen = false;
bool     g_vmRec = false;
uint8_t* g_vmBits = nullptr;            // PSRAM: encoded frames
size_t   g_vmLen = 0;
int16_t* g_vmPcm = nullptr;             // one frame of 16 kHz samples being filled
int      g_vmHave = 0;
uint32_t g_vmStart = 0;
char     g_vmStatus[40] = "";

void vmStatus(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_vmStatus, sizeof(g_vmStatus), fmt, ap); va_end(ap);
    Serial.printf("[VM] %s\n", g_vmStatus);
    requestRedraw();
}

void vmOpen() {
    g_vmOpen = true;
    g_vmRec = false;
    vmStatus(handheld::lxmf::available() ? "Space = record" : "engine without voice support");
    g_fullRefreshReq = true;
}

void vmClose() {
    if (g_vmRec) { g_audio.micStop(); g_vmRec = false; }
    voicecodec::end();
    g_vmOpen = false;
    g_fullRefreshReq = true;
    requestRedraw();
}

void vmStartRec() {
    if (!handheld::lxmf::available()) { vmStatus("engine without voice support"); return; }
    if (walkie::state() != walkie::State::Idle) { vmStatus("a walkie-talkie call is open"); return; }
    if (!voicecodec::begin(700)) { vmStatus("codec failed"); return; }
    const size_t cap = (size_t)kVmMaxSec * 1000 / voicecodec::frameMs() * voicecodec::frameBytes();
    if (!g_vmBits) g_vmBits = (uint8_t*)ps_malloc(handheld::lxmf::MaxAudioBytes);
    if (!g_vmPcm) g_vmPcm = (int16_t*)ps_malloc(sizeof(int16_t) * 640);
    if (!g_vmBits || !g_vmPcm || cap > handheld::lxmf::MaxAudioBytes) { vmStatus("no memory"); return; }
    if (!g_audio.micStart()) { vmStatus("microphone failed"); return; }
    g_vmLen = 0; g_vmHave = 0; g_vmRec = true; g_vmStart = millis();
    vmStatus("RECORDING  Space = send");
}

bool vmSend() {
    if (!g_vmLen) { vmStatus("nothing recorded"); return false; }
    if (g_root) lv_obj_clean(g_root);
    auto sub = g_proto.lxmfSubmitAudio(g_threadPeer, handheld::lxmf::AudioCodec2_700C, g_vmBits, g_vmLen);
    if (!sub.accepted()) { vmStatus("not sent (%u)", (unsigned)sub.rejection); return false; }
    handheld::outgoing::InitialResult ir{};
    for (const uint32_t t0 = millis(); millis() - t0 < 4000;) {   // a voice body is larger: 4 s
        pumpOnce();
        auto p = g_proto.lxmfPoll(sub.ticket, ir);
        if (p == handheld::outgoing::Poll::Pending) { delay(10); continue; }
        if (p == handheld::outgoing::Poll::Ready) {
            g_proto.lxmfAcknowledge(sub.ticket);
            vmStatus("sent: %u bytes, %u s", (unsigned)g_vmLen, (unsigned)(g_vmLen / 88));
            loadHistory(g_threadPeer);
            return true;
        }
        vmStatus("not sent");
        return false;
    }
    for (auto& t : g_txLate)
        if (!t.valid()) { t = sub.ticket; vmStatus("saving, then sent"); return true; }
    vmStatus("send timed out");
    return false;
}

void vmStopRec(bool send) {
    if (!g_vmRec) return;
    g_audio.micStop();
    g_vmRec = false;
    if (send) vmSend(); else vmStatus("cancelled");
}

// Main loop: encode what the mic has (one 40 ms frame per pass at most two).
void vmTick() {
    if (!g_vmRec) return;
    const int fs = voicecodec::frameSamples16k(), fb = voicecodec::frameBytes();
    for (int k = 0; k < 2; k++) {
        g_vmHave += (int)g_audio.micRead(g_vmPcm + g_vmHave, fs - g_vmHave, g_cfg.micGain);
        if (g_vmHave < fs) break;
        g_vmHave = 0;
        if (g_vmLen + fb > (size_t)kVmMaxSec * 1000 / voicecodec::frameMs() * fb) { vmStopRec(true); return; }
        voicecodec::encode(g_vmPcm, g_vmBits + g_vmLen);
        g_vmLen += fb;
    }
    static uint32_t shown = 0;
    if (millis() - shown > 1000) { shown = millis(); vmStatus("RECORDING %u s  Space = send", (unsigned)((millis() - g_vmStart) / 1000)); }
}

// Play the newest voice message in this chat (blocking: up to ~40 s of audio).
void vmPlayLast() {
    int idx = -1;
    for (int i = g_histCount - 1; i >= 0; i--) if (g_hist[i].voice) { idx = i; break; }
    if (idx < 0) { vmStatus("no voice message in this chat"); return; }
    handheld::storage::RecordKey key{};
    memcpy(key.peer, g_threadPeer, 16);
    key.counter = g_hist[idx].counter;
    key.incoming = g_hist[idx].incoming;
    auto rs = g_msgStore.requestRecord(key, 0, 4096, true);
    if (!rs.accepted()) { vmStatus("store busy"); return; }
    MessageStore::Result rr;
    uint8_t* buf = (uint8_t*)ps_malloc(4096);
    size_t got = 0;
    if (buf && waitResult(rs.ticket, rr) && rr.outcome == handheld::storage::Outcome::Committed &&
        rr.error == handheld::storage::Error::None) {
        got = rr.length < 4096 ? rr.length : 4096;
        if (!g_msgStore.readPayload(rs.ticket, buf, got)) got = 0;
    }
    g_msgStore.releaseResult(rs.ticket);
    handheld::storage::StoredRecordHeader hdr;
    uint8_t mode = 0; const uint8_t* audio = nullptr; size_t alen = 0;
    bool ok = got >= sizeof(hdr);
    if (ok) {
        memcpy(&hdr, buf, sizeof(hdr));
        const size_t off = sizeof(hdr) + hdr.titleLength + hdr.contentLength;
        ok = off + hdr.fieldsLength <= got && handheld::lxmf::findAudio(buf + off, hdr.fieldsLength, mode, audio, alen);
    }
    const int bps = mode == handheld::lxmf::AudioCodec2_700C ? 700 : mode == handheld::lxmf::AudioCodec2_1600 ? 1600
                  : mode == handheld::lxmf::AudioCodec2_3200 ? 3200 : 0;
    if (!ok || !bps || !voicecodec::begin(bps)) { free(buf); vmStatus("cannot play (mode %u)", (unsigned)mode); return; }
    vmStatus("PLAYING %u s", (unsigned)(alen / voicecodec::frameBytes() * voicecodec::frameMs() / 1000));
    render();
    static int16_t pcm[640];
    const int fb = voicecodec::frameBytes();
    for (size_t i = 0; i + fb <= alen; i += fb) {
        voicecodec::decode(audio + i, pcm);
        g_audio.writePcm(pcm, voicecodec::frameSamples16k());
        pumpOnce();
    }
    free(buf);
    vmStatus("played  P = again");
}

void vmKey(const KeyEvent& ev) {
    if (ev.up) { if (g_vmRec) vmStopRec(false); else vmClose(); return; }
    const char c = (char)toupper((unsigned char)ev.character);
    if (ev.enter || ev.space || ev.character == ' ') { if (g_vmRec) vmStopRec(true); else vmStartRec(); return; }
    if (c == 'P' && !g_vmRec) vmPlayLast();
}

void renderVoiceMsg() {
    char nm[24];
    peerName(g_threadPeer, nm, sizeof(nm));
    drawHeader("Voice message", false);
    int y = LIST_TOP + 6;
    label(0, y, W, 22, nm, true); y += 34;
    label(0, y, W, 24, g_vmStatus, true); y += 44;
    if (g_vmRec) {
        label(0, y, W, 20, "Space / Enter = stop and send"); y += 24;
        label(0, y, W, 20, "^ = cancel");
    } else {
        label(0, y, W, 20, "Space / Enter = record"); y += 24;
        label(0, y, W, 20, "P = play the newest"); y += 24;
        label(0, y, W, 20, "^ = back to the chat");
    }
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : "Codec2 700C, max 20 s, LoRa ok");
}

void openMenu();
bool parseLong(const char* s, long& out);
void laySave();

// ---- Markers: items of interest (ATAK-style) -------------------------------------------
// A marker is a place with a type and a short note. Made on the Atlas (Alt+J: at the map
// cross, or by distance and direction from the own position), kept in NVS rs_mark (24; a
// new one replaces the oldest received one when full), drawn on the Atlas as a triangle and
// listed nearest first (Atlas J) with distance and compass direction from this radio. Sent
// to a contact or a group as text any LXMF client can read:
//   "Marker: hazard at 43.64012, -70.92011 (bridge out)"
// The receiving radio stores it (from = the sender) and shows it in the chat as
// "[marker] hazard 640 m NE of you: bridge out"; Enter on the empty draft opens the map there.
// J on the marker list is "Nearby": the contacts with a shared position, nearest first.
constexpr const char* kMarkPrefix = "Marker: ";
constexpr int kMarkMax = 24;
constexpr int kMarkTypes = 7;
const char* const kMarkType[kMarkTypes] = {"hazard", "person", "vehicle", "camp", "water", "meet-up", "other"};
struct Mark { float lat, lon; uint32_t at; uint8_t type, mine, pad[2]; char note[24]; uint8_t from[16]; };
Mark g_mark[kMarkMax];
int g_markCount = 0;
int g_markSel = -1;                          // selected on the map (bottom line, guide line)
// Markers from ATAK (WiFi, anyone on the network): at most half the store, so a busy or
// hostile network cannot push out the ones contacts sent; saved at most every 30 s.
constexpr int kMarkAtakMax = kMarkMax / 2;
uint32_t g_markDirtyAt = 0;

// A marker that came from ATAK (the feed's receive side) has from = A7 AC + a hash of its uid.
bool markFromAtak(const uint8_t from[16]) { return from[0] == 0xA7 && from[1] == 0xAC; }
void markWho(const Mark& m, char* out, size_t n) {
    if (m.mine) strlcpy(out, "mine", n);
    else if (markFromAtak(m.from)) strlcpy(out, "ATAK", n);
    else peerName(m.from, out, n);
}

void markSave() {
    g_markDirtyAt = 0;
    Preferences p;
    if (!p.begin("rs_mark", false)) return;
    p.putBytes("list", g_mark, sizeof(Mark) * g_markCount);
    p.putUChar("n", (uint8_t)g_markCount);
    p.end();
}

void markLoad() {
    Preferences p;
    g_markCount = 0;
    if (!p.begin("rs_mark", true)) return;
    const int n = p.getUChar("n", 0);
    if (n > 0 && n <= kMarkMax && p.getBytes("list", g_mark, sizeof(Mark) * n) == sizeof(Mark) * (size_t)n)
        g_markCount = n;
    p.end();
    for (int i = 0; i < g_markCount; i++) {
        g_mark[i].note[sizeof(g_mark[i].note) - 1] = 0;
        if (g_mark[i].type >= kMarkTypes) g_mark[i].type = kMarkTypes - 1;
    }
}

bool markParse(const char* s, size_t len, int& type, double& lat, double& lon, char* note, size_t noteCap) {
    char t[120];
    if (len >= sizeof(t)) len = sizeof(t) - 1;
    memcpy(t, s, len);
    t[len] = 0;
    if (strncmp(t, kMarkPrefix, strlen(kMarkPrefix))) return false;
    const char* p = t + strlen(kMarkPrefix);
    const char* at = strstr(p, " at ");
    if (!at) return false;
    type = kMarkTypes - 1;                   // a type this firmware does not know: "other"
    for (int i = 0; i < kMarkTypes; i++)
        if ((size_t)(at - p) == strlen(kMarkType[i]) && !strncasecmp(p, kMarkType[i], (size_t)(at - p))) type = i;
    if (sscanf(at + 4, "%lf, %lf", &lat, &lon) != 2) return false;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    if (note && noteCap) {
        note[0] = 0;
        const char* o = strchr(at, '(');
        const char* c = o ? strrchr(o, ')') : nullptr;
        if (o && c && c > o + 1) {
            size_t n = (size_t)(c - o - 1);
            if (n >= noteCap) n = noteCap - 1;
            memcpy(note, o + 1, n);
            note[n] = 0;
            for (size_t k = 0; k < n; k++) if ((unsigned char)note[k] < 32 || (unsigned char)note[k] > 126) note[k] = '?';
        }
    }
    return true;
}

void markFormat(const Mark& m, char* out, size_t n) {
    if (m.note[0]) snprintf(out, n, "%s%s at %.5f, %.5f (%s)", kMarkPrefix, kMarkType[m.type], m.lat, m.lon, m.note);
    else snprintf(out, n, "%s%s at %.5f, %.5f", kMarkPrefix, kMarkType[m.type], m.lat, m.lon);
}

// "640 m NE of you" from this radio's position; the coordinates when it has none.
void markWhere(double lat, double lon, char* out, size_t n, bool ofYou = true) {
    double la, lo;
    if (!ownPosition(la, lo)) { snprintf(out, n, "%.4f,%.4f", lat, lon); return; }
    char d[16];
    fmtDist(metersBetween(la, lo, lat, lon), d, sizeof(d));
    snprintf(out, n, "%s %s%s", d, compass8(bearingDeg(la, lo, lat, lon)), ofYou ? " of you" : "");
}

// A chat row for a marker message: "[marker] hazard 640 m NE of you: bridge out".
bool markRowText(const char* s, size_t len, char* out, size_t n, double* lat, double* lon) {
    int type = 0;
    double la = 0, lo = 0;
    char note[24], where[40];
    if (!markParse(s, len, type, la, lo, note, sizeof(note))) return false;
    markWhere(la, lo, where, sizeof(where));
    if (note[0]) snprintf(out, n, "[marker] %s %s: %s", kMarkType[type], where, note);
    else snprintf(out, n, "[marker] %s %s", kMarkType[type], where);
    if (lat) *lat = la;
    if (lon) *lon = lo;
    return true;
}

// from = nullptr: made on this radio. The same marker again (same maker, same type, within
// 15 m) updates the note instead of adding a second one. Returns its index.
int markAdd(int type, double lat, double lon, const char* note, const uint8_t* from, uint32_t at) {
    if (type < 0 || type >= kMarkTypes) type = kMarkTypes - 1;
    int i = -1;
    for (int k = 0; k < g_markCount && i < 0; k++) {
        const Mark& m = g_mark[k];
        if (m.type != type || (m.mine != 0) != (from == nullptr)) continue;
        if (from && memcmp(m.from, from, 16)) continue;
        if (metersBetween(m.lat, m.lon, lat, lon) < 15.0f) i = k;
    }
    const bool atak = from && markFromAtak(from);
    if (i < 0 && atak) {                     // ATAK's share full: its oldest one makes room
        int n = 0, old = -1;
        for (int k = 0; k < g_markCount; k++)
            if (!g_mark[k].mine && markFromAtak(g_mark[k].from)) { n++; if (old < 0 || g_mark[k].at < g_mark[old].at) old = k; }
        if (n >= kMarkAtakMax) i = old;
    }
    if (i < 0) {
        if (g_markCount < kMarkMax) i = g_markCount++;
        else {                               // full: the oldest received one makes room, else the oldest
            for (int k = 0; k < kMarkMax; k++)
                if (!g_mark[k].mine && (i < 0 || g_mark[k].at < g_mark[i].at)) i = k;
            if (i < 0) { i = 0; for (int k = 1; k < kMarkMax; k++) if (g_mark[k].at < g_mark[i].at) i = k; }
        }
    }
    Mark& m = g_mark[i];
    memset(&m, 0, sizeof(m));
    m.lat = (float)lat; m.lon = (float)lon; m.type = (uint8_t)type; m.at = at;
    m.mine = from ? 0 : 1;
    if (from) memcpy(m.from, from, 16);
    strlcpy(m.note, note ? note : "", sizeof(m.note));
    if (atak) { if (!g_markDirtyAt) g_markDirtyAt = millis() ? millis() : 1; }   // cotTick saves it
    else markSave();
    char who[24] = "this radio", where[40];
    if (from && markFromAtak(from)) strlcpy(who, "ATAK", sizeof(who));
    else if (from) peerName(from, who, sizeof(who));
    markWhere(lat, lon, where, sizeof(where));
    Serial.printf("[MARK] #%d %s at %.5f,%.5f \"%s\" from %s: %s\n", i + 1, kMarkType[type], lat, lon, m.note, who, where);
    requestRedraw();
    return i;
}

void markRemove(int i) {
    if (i < 0 || i >= g_markCount) return;
    Serial.printf("[MARK] removed #%d %s\n", i + 1, kMarkType[g_mark[i].type]);
    for (int k = i; k < g_markCount - 1; k++) g_mark[k] = g_mark[k + 1];
    g_markCount--;
    g_markSel = -1;
    markSave();
    requestRedraw();
}

// A point `metres` away from the own position in compass direction `deg` (0 = north).
bool markPointFromMe(double metres, double deg, double& lat, double& lon) {
    double la, lo;
    if (!ownPosition(la, lo)) return false;
    const double r = 3.14159265358979 / 180.0;
    lat = la + metres * cos(deg * r) / 111320.0;
    const double c = cos(la * r);
    lon = lo + metres * sin(deg * r) / (111320.0 * (c < 0.01 ? 0.01 : c));
    return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

// To a contact, or to every member of a group (dest = the group's row address).
bool markSend(int i, const uint8_t dest[16]) {
    if (i < 0 || i >= g_markCount) return false;
    char body[120], nm[24];
    markFormat(g_mark[i], body, sizeof(body));
    peerName(dest, nm, sizeof(nm));
    const SendResult r = statusSend(dest, body);
    Serial.printf("[MARK] send #%d to %s: %s\n", i + 1, nm, r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "queued" : "failed");
    if (r == SendResult::Failed) toast("Not sent: %s", g_txStr);
    else toast("Marker %s: %.12s", r == SendResult::Sent ? "sent" : "queued", nm);
    return r != SendResult::Failed;
}

// An incoming message (inboundTick): a marker from a contact is stored and drawn.
void markIncoming(const uint8_t from[16], const char* text, uint32_t at) {
    int type = 0;
    double la = 0, lo = 0;
    char note[24];
    if (!markParse(text, strlen(text), type, la, lo, note, sizeof(note))) return;
    markAdd(type, la, lo, note, from, at);
    if (mapview::isOpen()) mapContactsFeed();
}

int markFeed(mapview::Marker* m, int n) {
    for (int i = 0; i < g_markCount && n < mapview::kMaxMarkers; i++)
        m[n++] = {g_mark[i].lat, g_mark[i].lon, i == g_markSel, (uint8_t)(i + 1), true, false, 2};
    if (g_markSel >= 0 && g_markSel < g_markCount && !g_tbOn) mapview::setGuide(true, g_mark[g_markSel].lat, g_mark[g_markSel].lon);
    return n;
}

// The map's bottom line for the selected marker.
bool markInfo(char* out, size_t n) {
    if (g_markSel < 0 || g_markSel >= g_markCount) return false;
    const Mark& m = g_mark[g_markSel];
    char where[32];
    markWhere(m.lat, m.lon, where, sizeof(where), false);
    snprintf(out, n, "%s %s %.12s", kMarkType[m.type], where, m.note);
    return true;
}

// ---- Markers: screens -------------------------------------------------------------------
// MK_LIST nearest first, last row "New marker"; MK_ONE one marker (show / send / delete);
// MK_NEW the form; MK_SEND who gets it (groups, then saved contacts); MK_NEAR contacts by distance.
int g_mkSel = 0, g_mkScroll = 0;
int g_mkIdx = -1;                            // the marker MK_ONE / MK_SEND is about
int g_mkOrder[kMarkMax];                     // MK_LIST rows -> g_mark index
uint8_t g_mkCand[kGrpMax + MAX_ROWS][16];    // MK_SEND rows
int g_mkCandN = 0;
int g_mkNear[kCPosMax];                      // MK_NEAR rows -> g_cpos index
int g_mkNearN = 0;
uint32_t g_mkDelAt = 0;
struct MarkDraft { uint8_t type; bool fromMe; uint16_t dir; float distM; double crossLat, crossLon; bool haveCross; char note[24]; };
MarkDraft g_mkNew = {};
const char* const kMkDir[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

void markSort() {
    double la = 0, lo = 0;
    const bool own = ownPosition(la, lo);
    for (int i = 0; i < g_markCount; i++) g_mkOrder[i] = i;
    for (int a = 1; a < g_markCount; a++) {
        for (int b = a; b > 0; b--) {
            const Mark& x = g_mark[g_mkOrder[b]];
            const Mark& y = g_mark[g_mkOrder[b - 1]];
            const bool before = own ? metersBetween(la, lo, x.lat, x.lon) < metersBetween(la, lo, y.lat, y.lon) : x.at > y.at;
            if (!before) break;
            std::swap(g_mkOrder[b], g_mkOrder[b - 1]);
        }
    }
}

void markNearLoad() {
    double la = 0, lo = 0;
    const bool own = ownPosition(la, lo);
    g_mkNearN = 0;
    for (int i = 0; i < g_cposCount; i++) if (g_cpos[i].hasPos) g_mkNear[g_mkNearN++] = i;
    for (int a = 1; a < g_mkNearN; a++) {
        for (int b = a; b > 0; b--) {
            const CPos& x = g_cpos[g_mkNear[b]];
            const CPos& y = g_cpos[g_mkNear[b - 1]];
            const bool before = own ? metersBetween(la, lo, x.lat, x.lon) < metersBetween(la, lo, y.lat, y.lon) : x.at > y.at;
            if (!before) break;
            std::swap(g_mkNear[b], g_mkNear[b - 1]);
        }
    }
}

int markRowCount() {
    switch (g_mk) {
        case MK_LIST: return g_markCount + 1;
        case MK_ONE:  return 3;
        case MK_NEW:  return 6;
        case MK_SEND: return g_mkCandN;
        case MK_NEAR: return g_mkNearN;
        default:      return 0;
    }
}

void markOpenList() {
    g_mk = MK_LIST;
    g_mkSel = 0;
    g_mkScroll = 0;
    markSort();
    Serial.printf("[MARK] list (%d markers)\n", g_markCount);
    requestRedraw();
}

void markOpenOne(int i) {
    g_mkIdx = i;
    g_mk = MK_ONE;
    g_mkSel = 0;
    g_mkScroll = 0;
    g_mkDelAt = 0;
    requestRedraw();
}

// atCross: the form starts at the map cross (Alt+J on the Atlas), else from the own position.
void markOpenNew(bool atCross) {
    g_mkNew = {};
    g_mkNew.distM = 200.0f;
    g_mkNew.haveCross = mapview::isOpen();
    if (g_mkNew.haveCross) mapview::getCentre(g_mkNew.crossLat, g_mkNew.crossLon);
    double la, lo;
    g_mkNew.fromMe = !(atCross && g_mkNew.haveCross) && ownPosition(la, lo);
    if (!g_mkNew.fromMe && !g_mkNew.haveCross) { toast("No position yet"); return; }
    g_mk = MK_NEW;
    g_mkSel = 0;
    g_mkScroll = 0;
    Serial.printf("[MARK] new (%s)\n", g_mkNew.fromMe ? "from my position" : "at the map cross");
    requestRedraw();
}

void markClose() {
    g_mk = MK_NONE;
    if (mapview::isOpen()) mapContactsFeed();   // partial refresh back to the map (MIC = full refresh)
    requestRedraw();
}

void markBack() {
    if (g_mk == MK_SEND) markOpenOne(g_mkIdx);
    else if (g_mk == MK_ONE || g_mk == MK_NEW || g_mk == MK_NEAR) markOpenList();
    else markClose();
}

void markSaveNew() {
    double lat = g_mkNew.crossLat, lon = g_mkNew.crossLon;
    if (g_mkNew.fromMe && !markPointFromMe(g_mkNew.distM, g_mkNew.dir * 45.0, lat, lon)) { toast("No position yet"); return; }
    const time_t now = time(nullptr);
    const int i = markAdd(g_mkNew.type, lat, lon, g_mkNew.note, nullptr, now > 1600000000 ? (uint32_t)now : 0);
    toast("Marker saved: 2 sends it");
    markOpenOne(i);
}

void markActivate(int row) {
    const int n = markRowCount();
    if (row < 0 || row >= n) return;
    g_mkSel = row;
    switch (g_mk) {
        case MK_LIST:
            if (row == g_markCount) markOpenNew(false);
            else markOpenOne(g_mkOrder[row]);
            break;
        case MK_ONE:
            if (g_mkIdx < 0 || g_mkIdx >= g_markCount) { markOpenList(); break; }
            if (row == 0) {                  // show it on the map
                const int i = g_mkIdx;
                g_mk = MK_NONE;
                if (!mapview::isOpen()) openMapAt(g_mark[i].lat, g_mark[i].lon);
                else { mapview::centreOn(g_mark[i].lat, g_mark[i].lon); mapview::fitZoom(); }
                g_markSel = i; g_wptSel = -1; g_mapSel = -1;
                mapContactsFeed();
                Serial.printf("[MARK] show #%d on the map\n", i + 1);
                // openMapAt() asks for a full refresh itself; back to an open map: partial (MIC = full)
            } else if (row == 1) {           // who gets it
                g_mkCandN = 0;
                for (int g = 0; g < g_grpCount; g++) grpHash(g_grp[g].id, g_mkCand[g_mkCandN++]);
                for (const auto& node : g_announceMgr.nodes()) {
                    if (node.hash.size() != 16 || !node.saved || isSelf(node.hash.data())) continue;
                    if (g_mkCandN >= kGrpMax + MAX_ROWS) break;
                    memcpy(g_mkCand[g_mkCandN++], node.hash.data(), 16);
                }
                if (!g_mkCandN) { toast("No contacts yet - see Peers"); break; }
                g_mk = MK_SEND;
                g_mkSel = 0;
                g_mkScroll = 0;
            } else {                         // delete: Enter twice within 3 s
                if (g_mkDelAt && millis() - g_mkDelAt < 3000) { markRemove(g_mkIdx); toast("Marker removed"); markOpenList(); }
                else { g_mkDelAt = millis(); toast("Enter again: remove it"); }
            }
            break;
        case MK_NEW:
            if (row == 0) g_mkNew.type = (uint8_t)((g_mkNew.type + 1) % kMarkTypes);
            else if (row == 1) {
                double la, lo;
                if (g_mkNew.fromMe && !g_mkNew.haveCross) toast("Open from the Atlas for the cross");
                else if (!g_mkNew.fromMe && !ownPosition(la, lo)) toast("No position yet");
                else g_mkNew.fromMe = !g_mkNew.fromMe;
            } else if (row == 2) {
                if (!g_mkNew.fromMe) { toast("Row 2: from my position first"); break; }
                char cur[12];
                snprintf(cur, sizeof(cur), "%d", (int)(g_mkNew.distM * (g_cfg.imperial ? 3.28084f : 1.0f) + 0.5f));
                startEntry(Entry::MarkDist, cur);
                g_entryReplace = true;
            } else if (row == 3) {
                if (!g_mkNew.fromMe) { toast("Row 2: from my position first"); break; }
                g_mkNew.dir = (uint16_t)((g_mkNew.dir + 1) % 8);
            } else if (row == 4) startEntry(Entry::MarkNote, g_mkNew.note);
            else markSaveNew();
            break;
        case MK_SEND:
            if (markSend(g_mkIdx, g_mkCand[row])) markOpenOne(g_mkIdx);
            break;
        case MK_NEAR: {
            const int ci = g_mkNear[row];
            uint8_t peer[16];
            memcpy(peer, g_cpos[ci].dest, 16);
            g_mk = MK_NONE;
            mapShowContact(peer);
            break;
        }
        default: break;
    }
    requestRedraw();
}

void markKey(const KeyEvent& ev) {
    const int n = markRowCount();
    if (ev.enter) { markActivate(g_mkSel); return; }
    if (ev.del || ev.space) return;
    const char raw = ev.character;
    if (raw >= '1' && raw <= '9') {
        const int idx = (g_mkSel / 9) * 9 + (raw - '1');
        if (idx >= n) return;
        g_mkSel = idx;
        clampScroll(g_mkSel, g_mkScroll, n);
        if (g_mk == MK_ONE || g_mk == MK_NEW) markActivate(idx);   // a page of actions: the digit acts
        requestRedraw();
        return;
    }
    switch (toupper((unsigned char)raw)) {
        case 'T': if (g_mkSel > 0) g_mkSel--; clampScroll(g_mkSel, g_mkScroll, n); requestRedraw(); break;
        case 'V': if (g_mkSel < n - 1) g_mkSel++; clampScroll(g_mkSel, g_mkScroll, n); requestRedraw(); break;
        case 'N': if (g_mk == MK_LIST) markOpenNew(false); break;
        case 'J':                            // list <-> contacts by distance
            if (g_mk == MK_LIST) { markNearLoad(); g_mk = MK_NEAR; g_mkSel = 0; g_mkScroll = 0; Serial.printf("[MARK] nearby (%d contacts)\n", g_mkNearN); requestRedraw(); }
            else if (g_mk == MK_NEAR) markOpenList();
            break;
        case 'M': markClose(); openMenu(); break;
        default: break;
    }
}

void markCommitEntry() {                     // Enter in the distance / note field of the form
    if (g_entry == Entry::MarkDist) {
        long v = 0;
        if (!parseLong(g_buf, v) || v < 1 || v > 99999) { toast("Distance 1 - 99999"); return; }
        g_mkNew.distM = (float)v / (g_cfg.imperial ? 3.28084f : 1.0f);
    } else {
        strlcpy(g_mkNew.note, g_buf, sizeof(g_mkNew.note));
    }
    endEntry();
    requestRedraw();
}

void renderMarks() {
    char hdr[48], title[44], sub[48];
    const int n = markRowCount();
    clampScroll(g_mkSel, g_mkScroll, n);
    const Mark* one = (g_mkIdx >= 0 && g_mkIdx < g_markCount) ? &g_mark[g_mkIdx] : nullptr;
    if (g_mk == MK_LIST) snprintf(hdr, sizeof(hdr), "Markers (%d)", g_markCount);
    else if (g_mk == MK_NEW) snprintf(hdr, sizeof(hdr), "New marker");
    else if (g_mk == MK_NEAR) snprintf(hdr, sizeof(hdr), "Nearby contacts (%d)", g_mkNearN);
    else if (g_mk == MK_SEND && one) snprintf(hdr, sizeof(hdr), "Send %s to:", kMarkType[one->type]);
    else if (one) { char w[32]; markWhere(one->lat, one->lon, w, sizeof(w), false); snprintf(hdr, sizeof(hdr), "%s %s", kMarkType[one->type], w); }
    else snprintf(hdr, sizeof(hdr), "Marker");
    drawHeader(hdr, g_mkScroll > 0);
    if (!n) text(8, LIST_TOP + 8, g_mk == MK_NEAR ? "No contact has shared a position" : "Nothing here");
    const time_t now = time(nullptr);
    for (int i = g_mkScroll; i < n && i < g_mkScroll + VIS_ROWS; i++) {
        const int y = LIST_TOP + (i - g_mkScroll) * ROW_H;
        sub[0] = 0;
        if (g_mk == MK_LIST) {
            if (i < g_markCount) {
                const Mark& m = g_mark[g_mkOrder[i]];
                char w[32], who[24] = "mine", age[12] = "?";
                markWhere(m.lat, m.lon, w, sizeof(w), false);
                markWho(m, who, sizeof(who));
                if (m.at) fmtAge(now > (time_t)m.at ? (uint32_t)(now - m.at) : 0, age, sizeof(age));
                snprintf(title, sizeof(title), "%d : %s %s", i % 9 + 1, kMarkType[m.type], w);
                if (m.note[0]) snprintf(sub, sizeof(sub), "%.14s (%.8s, %s)", m.note, who, age);
                else snprintf(sub, sizeof(sub), "%.12s, %s old", who, age);
            } else {
                snprintf(title, sizeof(title), "%d : New marker", i % 9 + 1);
                snprintf(sub, sizeof(sub), "from me or the map cross");
            }
        } else if (g_mk == MK_ONE) {
            static const char* const kAct[3] = {"Show on the map", "Send", "Delete"};
            snprintf(title, sizeof(title), "%d : %s", i + 1, kAct[i]);
            if (i == 0 && one) {
                char who[24] = "mine";
                markWho(*one, who, sizeof(who));
                if (one->note[0]) snprintf(sub, sizeof(sub), "%.20s (%.10s)", one->note, who);
                else snprintf(sub, sizeof(sub), "from: %s", who);
            } else if (i == 1) snprintf(sub, sizeof(sub), "to a contact or a group");
            else snprintf(sub, sizeof(sub), "remove from this radio");
        } else if (g_mk == MK_NEW) {
            static const char* const kRow[6] = {"Type", "Where", "Distance", "Direction", "Note", "Save"};
            snprintf(title, sizeof(title), "%d : %s", i + 1, kRow[i]);
            if (i == 0) snprintf(sub, sizeof(sub), "%s  (Enter: next)", kMarkType[g_mkNew.type]);
            else if (i == 1) {
                if (g_mkNew.fromMe) snprintf(sub, sizeof(sub), "from my position");
                else snprintf(sub, sizeof(sub), "map cross %.4f,%.4f", g_mkNew.crossLat, g_mkNew.crossLon);
            } else if (i == 2) {
                if (g_entry == Entry::MarkDist) snprintf(sub, sizeof(sub), ">%s_ %s, Enter", g_buf, g_cfg.imperial ? "ft" : "m");
                else if (g_mkNew.fromMe) fmtDist(g_mkNew.distM, sub, sizeof(sub));
                else snprintf(sub, sizeof(sub), "-");
            } else if (i == 3) snprintf(sub, sizeof(sub), "%s", g_mkNew.fromMe ? kMkDir[g_mkNew.dir % 8] : "-");
            else if (i == 4) {
                if (g_entry == Entry::MarkNote) snprintf(sub, sizeof(sub), ">%s_", g_buf);
                else snprintf(sub, sizeof(sub), "%s", g_mkNew.note[0] ? g_mkNew.note : "(none; Enter: type it)");
            } else snprintf(sub, sizeof(sub), "keep it; then 2 sends it");
        } else if (g_mk == MK_SEND) {
            char nm[24];
            peerName(g_mkCand[i], nm, sizeof(nm));
            const int gi = grpOfHash(g_mkCand[i]);
            snprintf(title, sizeof(title), "%d : %s", i % 9 + 1, nm);
            if (gi >= 0) snprintf(sub, sizeof(sub), g_grp[gi].n == 1 ? "group, %d member" : "group, %d members", (int)g_grp[gi].n);
            else { char hex[33]; hashToHex(g_mkCand[i], hex); snprintf(sub, sizeof(sub), "%.24s", hex); }
        } else {                             // MK_NEAR
            const CPos& c = g_cpos[g_mkNear[i]];
            char nm[24], w[32], age[12] = "?";
            peerName(c.dest, nm, sizeof(nm));
            markWhere(c.lat, c.lon, w, sizeof(w), false);
            if (c.at) fmtAge(now > (time_t)c.at ? (uint32_t)(now - c.at) : 0, age, sizeof(age));
            snprintf(title, sizeof(title), "%d : %.10s %s", i % 9 + 1, nm, w);
            if (c.bat) snprintf(sub, sizeof(sub), "#%u, %s old, battery %u%%", (unsigned)c.num, age, (unsigned)(c.bat - 1));
            else snprintf(sub, sizeof(sub), "#%u on the map, %s old", (unsigned)c.num, age);
        }
        listRow(y, title, sub, i == g_mkSel);
        addHit(2, y, W - 4, ROW_H, H_MARK, i);
    }
    const bool more = g_mkScroll + VIS_ROWS < n;
    const char* hint = g_mk == MK_LIST ? (more ? "V more  N new  J nearby" : "Ent  N new  J nearby  ^")
                     : g_mk == MK_ONE  ? "1-3  T/V  Ent  ^ back"
                     : g_mk == MK_NEW  ? (more ? "1-6  V more  Ent  ^ back" : "1-6  T/V  Ent  ^ back")
                     : g_mk == MK_SEND ? (more ? "V more  Ent send  ^" : "Ent = send  ^ cancel")
                                       : (more ? "V more  Ent map  J markers" : "Ent = map  J markers  ^");
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : hint);
}

// Atlas keys: J = the marker list, Alt+J = a new marker at the map cross.
bool markMapKey(const KeyEvent& ev) {
    const char c = (char)toupper((unsigned char)ev.character);
    if (c != kKeyMark) {
        if (c == 'D' || ev.enter || c == kKeyWpt || c == kKeyContact) g_markSel = -1;
        return false;
    }
    if (ev.alt) markOpenNew(true); else markOpenList();
    return true;
}

// Console: mark | mark new <type> <lat> <lon> [note] | mark rel <type> <metres> <degrees> [note]
//          mark send <n> <hex32 | g<group n>> | mark del <n | all> | mark list | mark near
void markConsole(const char* p) {
    char ty[12] = "", rest[64] = "";
    double a = 0, b = 0;
    int n = 0;
    auto typeOf = [](const char* s) { for (int i = 0; i < kMarkTypes; i++) if (!strcasecmp(s, kMarkType[i])) return i; return kMarkTypes - 1; };
    const time_t now = time(nullptr);
    const uint32_t at = now > 1600000000 ? (uint32_t)now : 0;
    if (sscanf(p, "new %11s %lf %lf %63[^\n]", ty, &a, &b, rest) >= 3) {
        markAdd(typeOf(ty), a, b, rest, nullptr, at);
    } else if (sscanf(p, "rel %11s %lf %lf %63[^\n]", ty, &a, &b, rest) >= 3) {
        double la, lo;
        if (!markPointFromMe(a, b, la, lo)) { Serial.println("[MARK] no own position"); return; }
        markAdd(typeOf(ty), la, lo, rest, nullptr, at);
    } else if (sscanf(p, "send %d %63s", &n, rest) == 2 && n >= 1 && n <= g_markCount) {
        uint8_t dest[16];
        if (rest[0] == 'g' && atoi(rest + 1) >= 1 && atoi(rest + 1) <= g_grpCount) grpHash(g_grp[atoi(rest + 1) - 1].id, dest);
        else if (strlen(rest) < 32 || !hexToHash(rest, dest)) { Serial.println("[MARK] need <hex32> or g<n>"); return; }
        markSend(n - 1, dest);
    } else if (!strcmp(p, "del all")) {
        g_markCount = 0; g_markSel = -1; markSave();
    } else if (sscanf(p, "del %d", &n) == 1) {
        markRemove(n - 1);
    } else if (!strcmp(p, "list")) {
        if (g_threadOpen) closeThread();   // a chat's draft would take the keys
        markOpenList();
        return;
    } else if (!strcmp(p, "near")) {
        markNearLoad();
        for (int i = 0; i < g_mkNearN; i++) {
            const CPos& c = g_cpos[g_mkNear[i]];
            char nm[24], w[32];
            peerName(c.dest, nm, sizeof(nm));
            markWhere(c.lat, c.lon, w, sizeof(w), false);
            Serial.printf("[MARK] near %d: %s %s\n", i + 1, nm, w);
        }
        Serial.printf("[MARK] near: %d contacts\n", g_mkNearN);
        return;
    }
    markSort();
    for (int k = 0; k < g_markCount; k++) {
        const Mark& m = g_mark[g_mkOrder[k]];
        char who[24] = "mine", w[40];
        markWho(m, who, sizeof(who));
        markWhere(m.lat, m.lon, w, sizeof(w));
        Serial.printf("[MARK] %d: #%d %s %.5f,%.5f \"%s\" %s, %s\n", k + 1, g_mkOrder[k] + 1, kMarkType[m.type], m.lat, m.lon, m.note, who, w);
    }
    Serial.printf("[MARK] %d markers\n", g_markCount);
}

// ---- Map layers (Atlas O) ------------------------------------------------------------------
// The layered map packs carry overlays (names, contours, woods, marsh marks, hill shading,
// other); the coverage layer comes from tools/maptiles/rf_coverage.py. O on the Atlas lists
// them: a row's number (or Enter) turns it on / off, ^ goes back to the map. Kept in NVS
// rs_map "layers" (default: all pack layers on, coverage off).
bool g_layOpen = false;
int g_laySel = 0;
struct LayRow { uint32_t bit; const char* name; const char* sub; };
const LayRow kLayRows[7] = {
    {mapview::kLayerLabels, "Place names", "towns, peaks, water"},
    {mapview::kLayerContours, "Contour lines", "height lines"},
    {mapview::kLayerWoods, "Woods", "forest shading"},
    {mapview::kLayerVeg, "Marsh marks", "wetland, scrub"},
    {mapview::kLayerHill, "Hill shading", "relief"},
    {mapview::kLayerOther, "Other", "anything else"},
    {mapview::kLayerCoverage, "Radio coverage", "line of sight dots"},
};

void laySave() {
    Preferences p;
    if (!p.begin("rs_map", false)) return;
    p.putUInt("layers", mapview::layers());
    p.end();
}

void layLoad() {
    Preferences p;
    if (!p.begin("rs_map", true)) return;
    const uint32_t m = p.getUInt("layers", mapview::kLayerAll);
    p.end();
    mapview::setLayers(m & (mapview::kLayerAll | mapview::kLayerCoverage));
}

void layToggle(int row) {
    if (row < 0 || row >= 7) return;
    const uint32_t m = mapview::layers() ^ kLayRows[row].bit;
    mapview::setLayers(m);
    laySave();
    Serial.printf("[MAP] layer %s %s (layers %lu)\n", kLayRows[row].name, (m & kLayRows[row].bit) ? "on" : "off",
                  (unsigned long)m);
    requestRedraw();
}

void layOpen() {
    g_layOpen = true;
    g_laySel = 0;
    Serial.printf("[MAP] layers page (layers %lu)\n", (unsigned long)mapview::layers());
    requestRedraw();
}

void layClose() {
    g_layOpen = false;
    if (mapview::isOpen()) mapContactsFeed();   // partial refresh back to the map (MIC = full refresh)
    requestRedraw();
}

void layKey(const KeyEvent& ev) {
    if (ev.enter) { layToggle(g_laySel); return; }
    const char raw = ev.character;
    if (raw >= '1' && raw <= '7') { g_laySel = raw - '1'; layToggle(g_laySel); return; }
    switch (toupper((unsigned char)raw)) {
        case 'T': if (g_laySel > 0) g_laySel--; requestRedraw(); break;
        case 'V': if (g_laySel < 6) g_laySel++; requestRedraw(); break;
        case 'O': layClose(); break;
        default: break;
    }
}

void renderLayers() {
    drawHeader("Map layers", false);
    const uint32_t m = mapview::layers();
    char title[44], sub[48];
    int scroll = g_laySel >= VIS_ROWS ? g_laySel - VIS_ROWS + 1 : 0;
    for (int i = scroll; i < 7 && i < scroll + VIS_ROWS; i++) {
        const int y = LIST_TOP + (i - scroll) * ROW_H;
        snprintf(title, sizeof(title), "%d : %s", i + 1, kLayRows[i].name);
        snprintf(sub, sizeof(sub), "%s  %s", (m & kLayRows[i].bit) ? "ON " : "off", kLayRows[i].sub);
        listRow(y, title, sub, i == g_laySel);
    }
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : (scroll + VIS_ROWS < 7 ? "1-7 on/off  V more  ^ map" : "1-7 on/off  T/V Ent  ^ map"));
}

// ---- ATAK feed: Cursor-on-Target over WiFi (CotFeed.cpp) ---------------------------------
// When on and WiFi is up, every g_cot.every seconds this unit sends CoT events over UDP:
// itself (friendly ground unit, with battery), each contact's last shared position (until
// an hour old) and each marker (a spot on ATAK's map, coloured by type, note in the name).
// Default target: ATAK's SA multicast 239.2.3.1:6969, so ATAK phones on the same WiFi show
// them with no TAK server. Off by default (S.3 WiFi, last row; console "cot").

void cotSave() {
    Preferences p;
    if (!p.begin("rs_cot", false)) return;
    p.putBytes("cfg", &g_cot, sizeof(g_cot));
    p.end();
}

void cotLoad() {
    Preferences p;
    if (!p.begin("rs_cot", true)) return;
    CotCfg c;
    if (p.getBytes("cfg", &c, sizeof(c)) == sizeof(c)) g_cot = c;
    p.end();
    if (g_cot.every < 5) g_cot.every = 30;
    if (!g_cot.port) g_cot.port = cot::kSaPort;
    if (g_cot.link > kCotLinkBle) g_cot.link = kCotLinkWifi;
}

// The feed's link to the phone: WiFi (UDP, CotFeed) or Bluetooth (CotBle). Bluetooth needs the
// controller RAM kept at boot (cotble::reserved), so switching to Bluetooth restarts the radio once.
bool cotUseBle() { return g_cot.link == kCotLinkBle; }

// Phone radio allowed by the energy mode: the modes that turn WiFi off (Saver, Check-in ...)
// keep Bluetooth off too (it keeps the CPU awake).
bool cotLinkUp() {
    if (!g_pwEff.wifi) return false;
    return cotUseBle() ? cotble::connected() : wifiConnected();
}

void cotBleName(char* out, size_t n) {
    char hex[33];
    hashToHex(g_selfHash, hex);
    snprintf(out, n, "RatSpeak-%.4s", g_haveSelf ? hex : "0000");
}

// Start / stop Bluetooth with the feed, and tell the screen about pairing.
void cotBleTick() {
    const bool want = g_cot.on && cotUseBle() && g_pwEff.wifi && cotble::reserved();
    if (want) wifiOffForBle();                   // WiFi and Bluetooth do not both fit in RAM
    if (want && !cotble::active()) {
        char nm[24];
        cotBleName(nm, sizeof(nm));
        const bool ok = cotble::begin(nm);
        Serial.printf("[BLE] %s as \"%s\", code %06lu\n", ok ? "advertising" : "start failed", nm,
                      (unsigned long)cotble::passkey());
    } else if (!want && cotble::active()) {
        cotble::end();
        Serial.println("[BLE] off");
    }
    for (cotble::Event e; (e = cotble::takeEvent()) != cotble::EV_NONE;) {
        switch (e) {
        case cotble::EV_PAIR_CODE:
            Serial.printf("[BLE] pairing: code %06lu\n", (unsigned long)cotble::passkey());
            if (g_idleActive) { g_idleActive = false; g_fullRefreshReq = true; }
            noteInput();
            toast("Bluetooth code %06lu", (unsigned long)cotble::passkey());
            g_toastUntil = millis() + 30000;     // long enough to type it on the phone
            break;
        case cotble::EV_CONNECTED:
            Serial.println("[BLE] phone connected");
            g_cotLast = 0;                       // send a full round now
            toast("Phone connected (ATAK)");
            break;
        case cotble::EV_DISCONNECTED: Serial.println("[BLE] phone disconnected"); toast("Phone disconnected"); break;
        case cotble::EV_PAIR_FAILED: Serial.println("[BLE] pairing failed"); toast("Bluetooth pairing failed"); break;
        default: break;
        }
        requestRedraw();
    }
}

// The link setting changed: save, and restart when Bluetooth's memory must be kept (back to
// WiFi needs no restart: the kept controller RAM is only ~1.3 KB, freed at the next boot).
void cotSetLink(uint8_t link) {
    if (link == g_cot.link) return;
    g_cot.link = link;
    cotSave();
    Serial.printf("[COT] link %s\n", link == kCotLinkBle ? "Bluetooth" : "WiFi");
    if (link == kCotLinkBle && !cotble::reserved()) {
        toast("Restarting for Bluetooth");
        g_toastUntil = millis() + 4000;
        requestRedraw();
        Serial.println("[COT] restart to apply the link");
        Serial.flush();
#ifndef SIMULATOR
        render();
        delay(1500);
        ESP.restart();
#endif
    }
}

void cotIp(uint32_t ip, char* out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255), (unsigned)(ip & 255));
}

// Marker colours on ATAK (ARGB): hazard red, person cyan, vehicle blue, camp green,
// water light blue, meet-up yellow, other white.
const int32_t kCotMarkArgb[kMarkTypes] = {(int32_t)0xFFFF0000, (int32_t)0xFF00FFFF, (int32_t)0xFF0000FF,
    (int32_t)0xFF00FF00, (int32_t)0xFF7FBFFF, (int32_t)0xFFFFFF00, (int32_t)0xFFFFFFFF};

// Each send takes a WiFi buffer in internal RAM until the radio has sent it (multicast goes
// at the lowest rate), so a burst of one event per marker and contact every interval took
// internal RAM down to ~6 KB on COM8. The feed now sends one event at a time (cotTick paces
// them) and skips one while internal RAM is low.
constexpr size_t kCotMinInternal = 24 * 1024;
uint32_t g_cotSkipped = 0;

// Event k of the feed: 0 = this unit, then each contact with a position, then each marker.
// Returns 1 sent, 0 nothing to send for k (skip), 2 skipped (low memory / send failed),
// -1 past the end.
int cotSendItem(int k, uint32_t t) {
    static char xml[900];
    char uid[48], cs[48], hex[33], rem[64];
    cot::Item it;
    if (k == 0) {                                // this unit
        double la, lo;
        if (!g_haveSelf || !ownPosition(la, lo)) return 0;
        hashToHex(g_selfHash, hex);
        snprintf(uid, sizeof(uid), "RatSpeak-%.16s", hex);
        it.lat = la; it.lon = lo; it.at = t; it.stale = t + 3u * g_cot.every + 60u;
        it.uid = uid; it.type = "a-f-G-U-C"; it.callsign = g_selfName; it.battery = g_bat.pct;
    } else if (k - 1 < g_cposCount) {            // contacts' last shared positions
        const CPos& c = g_cpos[k - 1];
        if (!c.hasPos || !c.at || c.at + 3600u < t) return 0;
        hashToHex(c.dest, hex);
        snprintf(uid, sizeof(uid), "RatSpeak-%.16s", hex);
        peerName(c.dest, cs, sizeof(cs));
        it.lat = c.lat; it.lon = c.lon; it.at = c.at; it.stale = c.at + 3600u;
        it.uid = uid; it.type = "a-f-G-U-C"; it.callsign = cs; it.battery = c.bat ? c.bat - 1 : -1;
    } else if (k - 1 - g_cposCount < g_markCount) {   // markers
        const Mark& m = g_mark[k - 1 - g_cposCount];
        if (markFromAtak(m.from)) return 0;      // came from ATAK: do not send it back
        uint32_t h = 2166136261u;                // uid from what the marker is, not its slot
        auto mix = [&](const void* d, size_t n) { for (size_t q = 0; q < n; q++) h = (h ^ ((const uint8_t*)d)[q]) * 16777619u; };
        mix(&m.lat, sizeof(m.lat)); mix(&m.lon, sizeof(m.lon)); mix(&m.type, 1); mix(m.from, 16);
        snprintf(uid, sizeof(uid), "RatSpeak-mk-%08lx", (unsigned long)h);
        if (m.note[0]) snprintf(cs, sizeof(cs), "%s: %s", kMarkType[m.type], m.note);
        else snprintf(cs, sizeof(cs), "%s", kMarkType[m.type]);
        char who[24] = "this unit";
        if (!m.mine) peerName(m.from, who, sizeof(who));
        snprintf(rem, sizeof(rem), "RatSpeak marker from %s", who);
        it.lat = m.lat; it.lon = m.lon; it.at = m.at ? m.at : t; it.stale = t + 86400u;
        it.uid = uid; it.type = "b-m-p-s-m"; it.callsign = cs; it.argb = kCotMarkArgb[m.type]; it.remarks = rem;
    } else {
        return -1;
    }
    if (!cotUseBle() && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kCotMinInternal) { g_cotSkipped++; return 2; }
    const size_t n = cot::format(it, xml, sizeof(xml));
    if (!n) return 2;
    if (cotUseBle() ? !cotble::send(xml, n) : !cot::send(xml, n, g_cot.ip, g_cot.port)) return 2;
    g_cotSent++;
    return 1;
}

// All events now (bench "cot now").
int cotSendAll(bool log) {
    const time_t now = time(nullptr);
    if (now < 1600000000) { if (log) Serial.println("[COT] no clock yet"); return 0; }
    int sent = 0, skipped = 0, r;
    for (int k = 0; (r = cotSendItem(k, (uint32_t)now)) >= 0; k++) { if (r == 1) sent++; if (r == 2) skipped++; }
    if (log) {
        char ip[16]; cotIp(g_cot.ip, ip, sizeof(ip));
        Serial.printf("[COT] sent %d events to %s:%u%s\n", sent, ip, (unsigned)g_cot.port, skipped ? " (some skipped: low memory)" : "");
    }
    return sent;
}

// ---- ATAK feed, receive side: phones running ATAK (and TAK devices) on the same WiFi ----------
// Their own position ("a-..." events) is kept per uid (16 at most, gone when stale or an hour
// old) and drawn on the Atlas as a square; their markers (b-m-p..., u-d-p, hostile a-h-...)
// become markers from "ATAK". Events from RatSpeak units (uid RatSpeak-...) are skipped: those
// are this radio's own or come over LoRa anyway.
struct TakUnit { char uid[40]; char cs[24]; float lat, lon; uint32_t at, stale; };
constexpr int kTakMax = 16;
TakUnit g_tak[kTakMax];
int g_takCount = 0;
uint32_t g_takRx = 0;                        // CoT events taken in
bool g_cotRxOpen = false;
uint16_t g_cotListenPort = 0;                // bench "cot listen <port>": print what arrives

bool cotAttr(const char* xml, const char* name, char* out, size_t n) {
    char key[24];
    snprintf(key, sizeof(key), " %s=\"", name);
    const char* q = strstr(xml, key);
    if (!q) return false;
    q += strlen(key);
    size_t k = 0;
    for (; *q && *q != '"' && k + 1 < n; q++) out[k++] = *q;
    out[k] = 0;
    return true;
}

// "2026-11-23T14:32:00Z" (or with .sss) -> UTC seconds, 0 when it does not parse.
uint32_t cotTime(const char* t) {
    int y, mo, d, h, mi, se;
    if (sscanf(t, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    y -= mo <= 2;                                // days from civil (H. Hinnant)
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468;
    return days < 0 ? 0 : (uint32_t)(days * 86400L + h * 3600 + mi * 60 + se);
}

// XML entities back to characters (in place), the few CoT uses.
void cotUnescape(char* s) {
    static const char* const kEnt[5][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    char* w = s;
    for (const char* r = s; *r;) {
        bool hit = false;
        for (auto& e : kEnt)
            if (!strncmp(r, e[0], strlen(e[0]))) { *w++ = e[1][0]; r += strlen(e[0]); hit = true; break; }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
}

void cotReceive(const char* xml) {
    char uid[48] = "", ty[24] = "", cs[48] = "", la[24] = "", lo[24] = "", ts[32] = "", st[32] = "";
    if (!cotAttr(xml, "uid", uid, sizeof(uid)) || !cotAttr(xml, "type", ty, sizeof(ty))) return;
    if (!strncmp(uid, "RatSpeak-", 9)) return;
    const char* pt = strstr(xml, "<point ");
    if (!pt || !cotAttr(pt, "lat", la, sizeof(la)) || !cotAttr(pt, "lon", lo, sizeof(lo))) return;
    const double lat = atof(la), lon = atof(lo);
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) return;
    const char* c = strstr(xml, "<contact ");
    if (c) cotAttr(c, "callsign", cs, sizeof(cs));
    cotUnescape(cs);
    if (!cs[0]) strlcpy(cs, uid, sizeof(cs));
    cotAttr(xml, "time", ts, sizeof(ts));
    cotAttr(xml, "stale", st, sizeof(st));
    const time_t now = time(nullptr);
    uint32_t at = cotTime(ts);
    if (!at) at = now > 1600000000 ? (uint32_t)now : 0;
    g_takRx++;
    const bool hostile = !strncmp(ty, "a-h", 3);
    if (ty[0] == 'a' && ty[1] == '-' && !hostile) {           // a unit: its position
        int i = -1;
        for (int k = 0; k < g_takCount && i < 0; k++) if (!strcmp(g_tak[k].uid, uid)) i = k;
        if (i < 0) {
            if (g_takCount < kTakMax) i = g_takCount++;
            else { i = 0; for (int k = 1; k < kTakMax; k++) if (g_tak[k].at < g_tak[i].at) i = k; }
            Serial.printf("[COT] ATAK unit \"%s\" %.5f,%.5f\n", cs, lat, lon);
        }
        TakUnit& u = g_tak[i];
        strlcpy(u.uid, uid, sizeof(u.uid));
        strlcpy(u.cs, cs, sizeof(u.cs));
        u.lat = (float)lat; u.lon = (float)lon; u.at = at; u.stale = cotTime(st);
        if (mapview::isOpen()) mapContactsFeed();
        return;
    }
    if (ty[0] != 'b' && ty[0] != 'u' && !hostile) return;      // tasks, sensors ...: not shown
    int type = hostile ? 0 : kMarkTypes - 1;                    // hazard / other
    char note[24] = "";
    for (int k = 0; k < kMarkTypes; k++) {                      // "camp: tents" -> camp, tents
        const size_t l = strlen(kMarkType[k]);
        if (!strncasecmp(cs, kMarkType[k], l) && (cs[l] == ':' || cs[l] == 0)) {
            type = k;
            strlcpy(note, cs[l] == ':' ? cs + l + 1 + (cs[l + 1] == ' ') : "", sizeof(note));
        }
    }
    if (!note[0] && strcmp(cs, uid)) strlcpy(note, cs, sizeof(note));
    for (char* q = note; *q; q++) if ((unsigned char)*q < 32 || (unsigned char)*q > 126) *q = '?';
    uint8_t from[16] = {0xA7, 0xAC};
    uint32_t h = 2166136261u;
    for (const char* q = uid; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
    for (int k = 2; k < 16; k++) { h = h * 1103515245u + 12345u; from[k] = (uint8_t)(h >> 16); }
    for (int k = 0; k < g_markCount; k++) {                     // already have it, unchanged: nothing
        const Mark& m = g_mark[k];
        if (!memcmp(m.from, from, 16) && m.type == type && !strcmp(m.note, note) &&
            metersBetween(m.lat, m.lon, lat, lon) < 15.0f) return;
    }
    markAdd(type, lat, lon, note, from, at);
    if (mapview::isOpen()) mapContactsFeed();
}

// The Atlas: ATAK units as squares (kind 3).
int takFeed(mapview::Marker* m, int n) {
    for (int i = 0; i < g_takCount && n < mapview::kMaxMarkers; i++)
        m[n++] = {g_tak[i].lat, g_tak[i].lon, false, (uint8_t)(i + 1), true, false, 3};
    return n;
}

void takPrune() {
    const time_t now = time(nullptr);
    if (now < 1600000000) return;
    for (int i = 0; i < g_takCount;) {
        const TakUnit& u = g_tak[i];
        const bool old = (u.stale && u.stale < (uint32_t)now) || (u.at && u.at + 3600u < (uint32_t)now);
        if (old) {
            Serial.printf("[COT] ATAK unit \"%s\" gone (stale)\n", u.cs);
            g_tak[i] = g_tak[--g_takCount];
        } else i++;
    }
}

void cotTick() {
    cotBleTick();
    if (cotble::active()) {                      // Bluetooth: events from ATAK on the phone
        static char bbuf[1024];
        for (int k = 0; k < 8 && cotble::poll(bbuf, sizeof(bbuf)) > 0; k++) {
            cotReceive(bbuf);
            if (g_cotListen) Serial.printf("[COT] ble xml %s\n", bbuf);
        }
    }
    // Receive while the feed is on WiFi and WiFi is up, or while the bench listener is on.
    const bool want = g_cotListenPort || (g_cot.on && !cotUseBle() && wifiConnected() && g_pwEff.wifi);
    if (want && !g_cotRxOpen) g_cotRxOpen = cot::listen(g_cotListenPort ? g_cotListenPort : cot::kSaPort, cot::kSaGroup);
    if (!want && g_cotRxOpen) { cot::listen(0); g_cotRxOpen = false; }
    static uint32_t pruned = 0;
    if (millis() - pruned > 30000) { pruned = millis(); takPrune(); }
    if (g_markDirtyAt && millis() - g_markDirtyAt > 30000) markSave();     // ATAK markers, batched
    if (g_cotRxOpen) {
        static char buf[1024];
        uint32_t from = 0;
        for (int k = 0; k < 8 && cot::poll(buf, sizeof(buf), &from) > 0; k++) {
            cotReceive(buf);
            if (!g_cotListen) continue;              // bench: also print what arrives
            char ip[16], cs[40] = "?", uid[48] = "?", ty[24] = "?";
            double la = 0, lo = 0;
            cotIp(from, ip, sizeof(ip));
            const char* q;
            if ((q = strstr(buf, "callsign=\""))) sscanf(q + 10, "%39[^\"]", cs);
            if ((q = strstr(buf, " uid=\""))) sscanf(q + 6, "%47[^\"]", uid);
            if ((q = strstr(buf, " type=\""))) sscanf(q + 7, "%23[^\"]", ty);
            if ((q = strstr(buf, "<point lat=\""))) sscanf(q + 12, "%lf\" lon=\"%lf", &la, &lo);
            const char* b = strstr(buf, "battery=\"");
            Serial.printf("[COT] rx from %s %s %s \"%s\" %.5f,%.5f%s%.3s\n", ip, uid, ty, cs, la, lo,
                          b ? " bat " : "", b ? b + 9 : "");
            Serial.printf("[COT] xml %s\n", buf);
        }
    }
    static int next = -1;                        // the event a running round sends next (-1: none)
    static uint32_t roundT = 0, stepAt = 0, lowNoted = 0;
    if (!g_cot.on || !cotLinkUp()) { next = -1; return; }
    if (next < 0) {                              // start a round every interval
        if (g_cotLast && millis() - g_cotLast < (uint32_t)g_cot.every * 1000u) return;
        const time_t now = time(nullptr);
        if (now < 1600000000) return;
        g_cotLast = millis() | 1;
        roundT = (uint32_t)now;
        next = 0;
    }
    if (millis() - stepAt < 100) return;         // one event per 100 ms: no burst of WiFi buffers
    stepAt = millis();
    for (int tries = 0; tries < 64; tries++) {   // skip the slots with nothing to send
        const int r = cotSendItem(next, roundT);
        if (r < 0) { next = -1; break; }
        next++;
        if (r == 2 && (!lowNoted || millis() - lowNoted > 60000)) {
            lowNoted = millis() | 1;
            Serial.printf("[COT] low memory (%u B internal): events skipped (%lu so far)\n",
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)g_cotSkipped);
        }
        if (r != 0) break;
    }
}

// Console: cot | cot on|off | cot to <a.b.c.d> [port] | cot to default | cot every <s> |
//          cot link wifi|ble (restarts when needed) | cot forget (paired phones) |
//          cot now | cot listen <port|0> (bench: print the CoT events that arrive)
void cotConsole(const char* p) {
    char ip[16];
    unsigned a, b, c, d, port = cot::kSaPort, s = 0;
    if (!strcmp(p, "on") || !strcmp(p, "off")) {
        g_cot.on = p[1] == 'n';
        g_cotLast = 0;
        cotSave();
    } else if (sscanf(p, "to %u.%u.%u.%u %u", &a, &b, &c, &d, &port) >= 4 && a < 256 && b < 256 && c < 256 && d < 256) {
        g_cot.ip = a << 24 | b << 16 | c << 8 | d;
        g_cot.port = (uint16_t)(port ? port : cot::kSaPort);
        cotSave();
    } else if (!strcmp(p, "to default")) {
        g_cot.ip = cot::kSaGroup; g_cot.port = cot::kSaPort;
        cotSave();
    } else if (sscanf(p, "every %u", &s) == 1 && s >= 5 && s <= 3600) {
        g_cot.every = (uint16_t)s;
        cotSave();
    } else if (!strcmp(p, "link wifi") || !strcmp(p, "link ble")) {
        cotSetLink(p[5] == 'b' ? kCotLinkBle : kCotLinkWifi);
    } else if (!strcmp(p, "forget")) {           // Bluetooth: forget the paired phones
        cotble::forget();
        Serial.println("[BLE] paired phones forgotten");
        return;
    } else if (!strcmp(p, "now")) {
        if (!cotLinkUp()) { Serial.printf("[COT] %s is not connected\n", cotUseBle() ? "no phone" : "WiFi"); return; }
        cotSendAll(true);
        return;
    } else if (sscanf(p, "listen %u", &port) == 1) {
        if (g_cotRxOpen) { cot::listen(0); g_cotRxOpen = false; }
        g_cotListenPort = (uint16_t)port;
        g_cotListen = port != 0;
        if (port) g_cotRxOpen = cot::listen((uint16_t)port, cot::kSaGroup);
        Serial.printf("[COT] listen %s\n", g_cotListen && g_cotRxOpen ? "on" : "off");
        return;
    } else if (!strcmp(p, "atak")) {             // the ATAK units heard
        takPrune();
        const time_t now = time(nullptr);
        for (int i = 0; i < g_takCount; i++)
            Serial.printf("[COT] atak %d: \"%s\" %.5f,%.5f %lus old\n", i + 1, g_tak[i].cs, g_tak[i].lat, g_tak[i].lon,
                          (unsigned long)(now > (time_t)g_tak[i].at ? now - g_tak[i].at : 0));
        Serial.printf("[COT] atak: %d units, %lu events taken in\n", g_takCount, (unsigned long)g_takRx);
        return;
    } else if (!strncmp(p, "fake ", 5)) {        // bench: cot fake <a.b.c.d|local> <u|m> <callsign> <lat> <lon>
        char where[16] = "", kind[4] = "", cs[40] = "";   // an event as a phone running ATAK sends it
        double la = 0, lo = 0;
        if (sscanf(p + 5, "%15s %3s %39s %lf %lf", where, kind, cs, &la, &lo) != 5) { Serial.println("[COT] fake <ip|local> <u|m> <callsign> <lat> <lon>"); return; }
        for (char* q = cs; *q; q++) if (*q == '_') *q = ' ';
        const time_t now = time(nullptr);
        char uid[48];
        snprintf(uid, sizeof(uid), "ANDROID-%s", cs);
        for (char* q = uid; *q; q++) if (*q == ' ') *q = '-';
        cot::Item it;
        it.lat = la; it.lon = lo; it.at = (uint32_t)now; it.stale = (uint32_t)now + 300;
        it.uid = uid; it.callsign = cs;
        it.type = kind[0] == 'u' ? "a-f-G-U-C" : "b-m-p-s-m";
        static char xml[900];
        const size_t n = cot::format(it, xml, sizeof(xml));
        if (!strcmp(where, "local")) { cotReceive(xml); return; }
        if (sscanf(where, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
            Serial.printf("[COT] fake sent: %s\n", cot::send(xml, n, a << 24 | b << 16 | c << 8 | d, cot::kSaPort) ? "yes" : "no");
        return;
    } else if (!strncmp(p, "junk ", 5)) {        // bench: cot junk <n> [seed]: broken events into the parser
        int cnt = 0;
        unsigned seed = 1;
        if (sscanf(p + 5, "%d %u", &cnt, &seed) < 1 || cnt < 1) { Serial.println("[COT] junk <n> [seed]"); return; }
        uint32_t r = seed ? seed : 1;
        auto rnd = [&r]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; };
        static char xml[900], bad[1100];
        const uint32_t t0 = millis();
        const uint32_t rx0 = g_takRx;
        for (int k = 0; k < cnt; k++) {
            char cs[40], uid[48];
            snprintf(cs, sizeof(cs), "JUNK %u", (unsigned)(rnd() % 50));
            snprintf(uid, sizeof(uid), "ANDROID-junk-%u", (unsigned)(rnd() % 50));
            const time_t now = time(nullptr);
            cot::Item it;
            it.lat = (double)(rnd() % 18000) / 100.0 - 90.0; it.lon = (double)(rnd() % 36000) / 100.0 - 180.0;
            it.at = (uint32_t)now; it.stale = (uint32_t)now + 300;
            it.uid = uid; it.callsign = cs;
            it.type = (rnd() & 1) ? "a-f-G-U-C" : "b-m-p-s-m";
            size_t n = cot::format(it, xml, sizeof(xml));
            memcpy(bad, xml, n + 1);
            switch (rnd() % 6) {
            case 0: bad[rnd() % (n ? n : 1)] = 0; break;                                  // cut short
            case 1: for (int j = 0; j < 12; j++) bad[rnd() % n] = (char)(rnd() % 255 + 1); break;   // noise
            case 2: { char* q = strstr(bad, "lat=\""); if (q) memcpy(q + 5, "9e999\"", 6); } break;   // bad number
            case 3: { char* q = strstr(bad, "callsign=\""); if (q) { size_t o = q - bad + 10;          // huge callsign
                          for (size_t j = o; j < sizeof(bad) - 1; j++) bad[j] = 'X';
                          bad[sizeof(bad) - 1] = 0; } } break;
            case 4: for (char* q = bad; *q; q++) if (*q == '"') *q = '\''; break;           // other quotes
            default: { char* q = strstr(bad, "uid=\""); if (q) q[1] = 'x'; } break;        // no uid
            }
            cotReceive(bad);
            if ((k & 63) == 63) delay(1);
        }
        Serial.printf("[COT] junk: %d events in %lu ms, %lu taken in, %d ATAK units, heap %u\n", cnt,
                      (unsigned long)(millis() - t0), (unsigned long)(g_takRx - rx0), g_takCount, (unsigned)ESP.getFreeHeap());
        return;
    }
    cotIp(g_cot.ip, ip, sizeof(ip));
    if (cotUseBle())
        Serial.printf("[COT] feed %s, every %u s, link Bluetooth (%s, code %06lu, memory %s), %lu events sent, %lu in, int %u min %u\n",
                      g_cot.on ? "on" : "off", (unsigned)g_cot.every, cotble::state(), (unsigned long)cotble::passkey(),
                      cotble::reserved() ? "kept" : "freed: restart", (unsigned long)g_cotSent, (unsigned long)cotble::rxEvents(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    else
        Serial.printf("[COT] feed %s, every %u s, to %s:%u, %lu events sent, WiFi %s\n", g_cot.on ? "on" : "off",
                      (unsigned)g_cot.every, ip, (unsigned)g_cot.port, (unsigned long)g_cotSent, wifiConnected() ? "up" : "down");
}

// ---- Thread menu (MIC in a conversation) -----------------------------------------------
bool g_tmenuOpen = false;
int  g_tmenuSel = 0;
constexpr int kTmenuRows = 5;
// Quick replies (Q in a chat's MIC menu, contact or group): one digit sends one.
const char* const kQuick[8] = {"OK", "Yes", "No", "On my way", "Where are you?", "Call me", "I'm safe", "Wait for me"};
bool g_qrOpen = false;

void commitEntry();
void quickSend(int i) {
    if (i < 0 || i >= 8 || !g_threadOpen) return;
    g_qrOpen = false;
    g_tmenuOpen = false;
    g_tmenuGrpShare = false;
    if (g_entry != Entry::Message) { toast("Open the chat first"); return; }
    bufSet(kQuick[i]);
    Serial.printf("[MSG] quick reply %d: %s\n", i + 1, kQuick[i]);
    commitEntry();                               // the same path as typing it and Enter
}

// A group chat: MIC shows "who got it"; L there opens rows 1-3 of this menu for the group.
bool g_tmenuGrpShare = false;

void tmenuAct(int i) {
    if (isGrpHash(g_threadPeer) && i > 2) return;
    if (i == 0) { g_tmenuOpen = false; shareMyStatus(g_threadPeer, shareWhatFor(g_threadPeer)); loadHistory(g_threadPeer); }
    else if (i == 1) ashareCycle(g_threadPeer);
    else if (i == 2) {
        g_autoForSel = (uint8_t)((g_autoForSel + 1) % 3);
        const int k = ashareFind(g_threadPeer);
        if (k >= 0) {                        // applies to a running share too
            const uint16_t m = kAutoForMin[g_autoForSel];
            g_ashare[k].until = m ? (uint32_t)time(nullptr) + m * 60UL : 0;
            ashareSave();
        }
    } else if (i == 5) {
        g_qrOpen = true;
    } else if (i == 4) {
        if (!feature(F_VOICEMSG)) { toast("Voice messages are off (S.1.5)"); return; }
        g_tmenuOpen = false;
        vmOpen();
        return;
    } else if (i == 3) {
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
    const bool grp = isGrpHash(g_threadPeer);
    const char c = (char)toupper((unsigned char)ev.character);
    if (g_qrOpen) {                              // quick replies: a digit sends, anything else = back
        if (c >= '1' && c <= '8') { quickSend(c - '1'); return; }
        g_qrOpen = false;
        if (ev.up) { requestRedraw(); return; }
        requestRedraw();
        return;
    }
    if (c == 'Q' && !ev.up && !ev.fn && !ev.enter) { g_qrOpen = true; requestRedraw(); return; }
    if (grp && !g_tmenuGrpShare) {           // "who got it": L = share with the group, any other key = back
        if (c == 'L' && !ev.up && !ev.fn && !ev.enter) { g_tmenuGrpShare = true; g_tmenuSel = 0; }
        else g_tmenuOpen = false;
        requestRedraw();
        return;
    }
    if (ev.up || ev.fn) { g_tmenuOpen = false; g_tmenuGrpShare = false; requestRedraw(); return; }
    if (ev.enter) { tmenuAct(g_tmenuSel); return; }
    const int rows = grp ? 3 : kTmenuRows;
    if (c >= '1' && c <= '0' + rows) { g_tmenuSel = c - '1'; tmenuAct(g_tmenuSel); return; }
    if (c == 'B') { shareWhatCycle(g_threadPeer); return; }   // position + battery / position / battery
    if (c == 'T' && g_tmenuSel > 0) g_tmenuSel--;
    if (c == 'V' && g_tmenuSel < rows - 1) g_tmenuSel++;
    requestRedraw();
}

void renderThreadMenu(int y) {
    char sub[40], a[40];
    const int k = ashareFind(g_threadPeer);
    const uint8_t what = shareWhatFor(g_threadPeer);         // B changes it
    if (what == SH_BAT) {
        if (g_bat.valid) snprintf(sub, sizeof(sub), "now %d%%  (B: what to share)", g_bat.pct);
        else snprintf(sub, sizeof(sub), "no reading yet");
    } else {
        gpsPosText(a, sizeof(a));
        snprintf(sub, sizeof(sub), "%s", a);
    }
    listRow(y, what == SH_BAT ? "1 Share my battery %" : what == SH_POS ? "1 Share my location" : "1 Share location + battery",
            sub, g_tmenuSel == 0);
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
    listRow(y, what == SH_BAT ? "2 Auto-share battery %" : what == SH_POS ? "2 Auto-share location" : "2 Auto-share loc. + battery",
            sub, g_tmenuSel == 1);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 1); y += ROW_H;
    listRow(y, "3 Auto-share for", kAutoForName[g_autoForSel], g_tmenuSel == 2);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 2); y += ROW_H;
    if (isGrpHash(g_threadPeer)) {           // a group: rows 1-3 only
        snprintf(sub, sizeof(sub), "B: share %s", shareWhatName(what));
        text(8, y + 8, sub, false, W - 16);
        text(8, y + 30, "Each member gets a copy.", false, W - 16);
        return;
    }
    const int ci = cposFind(g_threadPeer);
    char bt[28];
    const bool haveBat = batText(g_threadPeer, bt, sizeof(bt));
    if (ci >= 0 && g_cpos[ci].hasPos) {
        const time_t now = time(nullptr);
        const uint32_t a = (g_cpos[ci].at && now > (time_t)g_cpos[ci].at) ? (uint32_t)(now - g_cpos[ci].at) : 0;
        char age[12];
        fmtAge(a, age, sizeof(age));
        if (haveBat) snprintf(sub, sizeof(sub), "#%u %s old, bat %u%%", (unsigned)g_cpos[ci].num, g_cpos[ci].at ? age : "?",
                              (unsigned)(g_cpos[ci].bat - 1));
        else snprintf(sub, sizeof(sub), "#%u, %s old", (unsigned)g_cpos[ci].num, g_cpos[ci].at ? age : "?");
    } else if (haveBat) {
        snprintf(sub, sizeof(sub), "no position; battery %s", bt);
    } else {
        snprintf(sub, sizeof(sub), "no position from them yet");
    }
    listRow(y, "4 Show them on the map", sub, g_tmenuSel == 3);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 3); y += ROW_H;
    listRow(y, "5 Voice message", feature(F_VOICEMSG) ? "record, send, play" : "off (S.1.5)", g_tmenuSel == 4);
    addHit(2, y, W - 4, ROW_H, H_TMENU, 4);
}

// The quick replies list (over the chat, like the MIC menu).
void renderQuickReplies(int y) {
    char t[40];
    for (int i = 0; i < 8; i++) {
        snprintf(t, sizeof(t), "%d %s", i + 1, kQuick[i]);
        text(10, y + i * 22, t, false, W - 20);
    }
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
char (*g_libText)[kLibLineMax] = nullptr;    // the current page: kLibLines lines, in PSRAM (psramGlobals)
uint32_t g_libLineOff[kLibLines];            // file offset where each shown line starts
int g_libTextN = 0;
uint32_t g_libNext = 0;                      // offset after the current page
bool g_libWrap = true;                       // false: one file line per screen line, panned
int g_libCol = 0;                            // wrap off: first character shown
constexpr int kLibPanCols = 12;              // S / F: about half a screen

// NOAA Weather Radio near me: when the station list (Weather-radio-stations.csv, made from
// weather.gov/nwr by hand, see tools/sdcard/library) is in the Library, opening the Library
// writes Weather-radio-near-me.txt there: the 15 nearest transmitters from this radio's
// position, with frequency, distance and direction.
void nwrNearMe() {
    char path[96];
    snprintf(path, sizeof(path), "%s/Weather-radio-stations.csv", kLibDir);
    File f = g_sdStore.openFile(path, FILE_READ);
    if (!f) return;
    double la = 0, lo = 0;
    const bool have = ownPosition(la, lo);
    struct Near { float km; char line[88]; };
    static Near best[15];
    int n = 0;
    if (have) {
        char ln[160];
        size_t k = 0;
        bool first = true;
        while (true) {
            const int c = f.read();
            if (c < 0 || c == '\n') {
                ln[k] = 0;
                k = 0;
                if (first) { first = false; if (c < 0) break; continue; }   // the header
                char st[4] = "", call[8] = "", mhz[10] = "", site[32] = "", status[20] = "";
                double sla = 0, slo = 0;
                if (sscanf(ln, "%3[^,],%7[^,],%9[^,],%31[^,],%lf,%lf,%19[^\r]", st, call, mhz, site, &sla, &slo, status) >= 6) {
                    const float km = metersBetween(la, lo, sla, slo) / 1000.0f;
                    if (n < 15 || km < best[n - 1].km) {
                        int i = n < 15 ? n++ : 14;
                        while (i > 0 && best[i - 1].km > km) { best[i] = best[i - 1]; i--; }
                        char d[16];
                        fmtDist(km * 1000.0, d, sizeof(d));
                        best[i].km = km;
                        snprintf(best[i].line, sizeof(best[i].line), "%s %s %s\n  %s %s, %s%s", mhz, d,
                                 compass8(bearingDeg(la, lo, sla, slo)), call, site, st,
                                 strcmp(status, "NORMAL") && status[0] ? " (OUT OF SERVICE)" : "");
                    }
                }
                if (c < 0) break;
            } else if (c != '\r' && k + 1 < sizeof(ln)) {
                ln[k++] = (char)c;
            }
        }
    }
    f.close();
    snprintf(path, sizeof(path), "%s/Weather-radio-near-me.txt", kLibDir);
    File o = g_sdStore.openFile(path, FILE_WRITE);
    if (!o) return;
    o.print("NOAA WEATHER RADIO NEAR YOU\n\n");
    if (!have) {
        o.print("No position yet: get a GPS fix (or set a manual location, S.2) and open the\n"
                "Library again. The full list is Weather-radio-stations.csv.\n");
    } else {
        char hdr[160];
        snprintf(hdr, sizeof(hdr), "The %d nearest to %.4f, %.4f (made when the Library opened): MHz, distance, "
                 "direction; call sign, site.\n\n", n, la, lo);
        o.print(hdr);
        for (int i = 0; i < n; i++) { o.print(best[i].line); o.print("\n"); }
        o.print("\nWeather radio channels: 162.400 162.425 162.450 162.475 162.500 162.525\n"
                "162.550 MHz (WX1-WX7, numbering differs by radio). Receive only.\n"
                "Source: weather.gov/nwr, public domain. Full list: Weather-radio-stations.csv.\n");
    }
    o.close();
    Serial.printf("[LIB] weather radio near me: %d stations%s\n", n, have ? "" : " (no position)");
}

// A new note from the Library: saved as Note-YYYYMMDD-HHMMSS.txt in the Library folder, with
// the time on its first line; it then opens like any other file.
void libScan();
void noteCommit() {
    if (!g_bufLen) { endEntry(); requestRedraw(); return; }
    if (!sdMount()) { toast("No SD card: note not saved"); return; }
    g_sdStore.ensureDir(kLibDir);
    const time_t now = time(nullptr);
    struct tm lt;
    applyTz();
    localtime_r(&now, &lt);
    char path[96], head[48];
    snprintf(path, sizeof(path), "%s/Note-%04d%02d%02d-%02d%02d%02d.txt", kLibDir, lt.tm_year + 1900, lt.tm_mon + 1,
             lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    snprintf(head, sizeof(head), "Note %04d-%02d-%02d %02d:%02d\n\n", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min);
    File f = g_sdStore.openFile(path, FILE_WRITE);
    if (!f) { toast("Note not saved (card)"); return; }
    f.print(head);
    f.print(g_buf);
    f.print("\n");
    f.close();
    Serial.printf("[LIB] note saved: %s (%u chars)\n", strrchr(path, '/') + 1, (unsigned)g_bufLen);
    endEntry();
    libScan();
    toast("Note saved");
    requestRedraw();
}

void libScan() {
    g_libCount = 0;
    if (!sdMount()) return;
    g_sdStore.ensureDir(kLibDir);
    nwrNearMe();
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
    static uint8_t (&buf)[2048] = psram<uint8_t, 2048>();
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
    static uint8_t (&buf)[2048 + 64] = psram<uint8_t, 2048 + 64>();
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
        if (c == 'N' || ev.space) libPage(+1);
        else if (c == 'B') libPage(-1);
        else if (c == 'V') libHalf(+1);
        else if (c == 'T') libHalf(-1);
        else if (c == 'L') startEntry(Entry::LibFind);
        else if (c == 'W') {
            g_libWrap = !g_libWrap;
            g_libCol = 0;
            libLoadPage(g_libPages.back());
            toast(g_libWrap ? "Wrap on" : "Wrap off: F / H pan");
            Serial.printf("[LIB] wrap %s\n", g_libWrap ? "on" : "off");
            requestRedraw();
        } else if ((c == 'F' || c == 'H') && !g_libWrap) {
            g_libCol += c == 'H' ? kLibPanCols : -kLibPanCols;
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
        else if (k == g_libCount + 2) { startEntry(Entry::Note, ""); toast("Type the note, Enter saves"); requestRedraw(); }
        else libOpenFile(k);
    };
    if (ev.enter) { pick(g_libSel); return; }
    if (c >= '1' && c <= '9') { const int k = g_libScroll + (c - '1'); if (k <= g_libCount + 2) { g_libSel = k; pick(k); } return; }
    if (c == 'V' && g_libSel < g_libCount + 2) g_libSel++;
    if (c == 'T' && g_libSel > 0) g_libSel--;
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
        for (int r = 0; r < 4 && g_libScroll + r <= g_libCount + 2; r++) {
            const int k = g_libScroll + r;
            char t[48], sub[24];
            if (k == g_libCount + 2) {
                snprintf(t, sizeof(t), "%d New note", r + 1);
                listRow(LIST_TOP + r * ROW_H, t, "type it; saved here as a file", k == g_libSel);
                continue;
            }
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
        if (g_entry == Entry::Note) {
            char ln[48];
            snprintf(ln, sizeof(ln), ">%.38s_", g_bufLen > 38 ? g_buf + g_bufLen - 38 : g_buf);
            label(0, FOOT_Y - 1, W, FOOT_H, ln);
        } else label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : "1-4 / Enter  T/V  K keys");
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
    if (c == 'T' && g_gameCy > 0) g_gameCy--;
    else if (c == 'V' && g_gameCy < kGameRows - 1) g_gameCy++;
    else if (c == 'F' && g_gameCx > 0) g_gameCx--;
    else if (c == 'H' && g_gameCx < kGameCols - 1) g_gameCx++;
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
const NavCard kChessKeys = {"Chess keys", kMoves4, {
    {"D Enter", "pick / drop"}, {"U", "take back"}, {"L", "level 1-4"},
    {"C", "swap sides"},      {"Q Q", "new game"},  {"", ""},
    {"M", "menu"},            {"^", "Library"},     {"", ""},
}};

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
    if (c == 'T' || ev.up) r += fl;
    else if (c == 'V' || ev.down) r -= fl;
    else if (c == 'F' || ev.left) f -= fl;
    else if (c == 'H' || ev.right) f += fl;
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
    // The whole 32-digit LXMF address in the small font (same digits as Settings S.7 shows
    // on that radio, which splits it into two lines of 16).
    smallText(6, HEADER_Y + 21, hex, false);
    int y = HEADER_Y + 39;
    for (int i = 0; i < 4; i++) {
        char sub[40];
        snprintf(sub, sizeof(sub), "%s", kCardSubs[i]);
        if (i == 0) {                            // their battery, from a status share
            char bt[28];
            if (batText(g_cardPeer, bt, sizeof(bt))) snprintf(sub, sizeof(sub), "battery %s", bt);
        }
        if (i == 3) {
            const int ci = cposFind(g_cardPeer);
            if (g_entry == Entry::MapNum) snprintf(sub, sizeof(sub), ">%s_  (1-99, Enter)", g_buf);
            else if (ci >= 0 && g_cpos[ci].hasPos && g_cpos[ci].at) {   // the position's age
                const time_t now = time(nullptr);
                char age[12];
                fmtAge(now > (time_t)g_cpos[ci].at ? (uint32_t)(now - g_cpos[ci].at) : 0, age, sizeof(age));
                snprintf(sub, sizeof(sub), "#%u on the map, %s old", (unsigned)g_cpos[ci].num, age);
            }
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
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : "1-4  T/V  Ent  W talk  ^ back");
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
        case 'T': if (g_cardSel > 0) g_cardSel--; requestRedraw(); break;
        case 'V': if (g_cardSel < 3) g_cardSel++; requestRedraw(); break;
        case 'M': closeCard(); openMenu(); break;
        case 'W': {                               // walkie-talkie call (WiFi)
            uint8_t peer[16];
            memcpy(peer, g_cardPeer, 16);
            if (isSelf(peer)) { toast("That is this radio"); break; }
            if (!feature(F_WALKIE)) { toast("Walkie-talkie is off (S.1.5)"); break; }
            closeCard();
            wtCall(peer);
            break;
        }
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

// ---- Contacts: share all, export / import (Settings S.7, setup wizard) -----------------
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

// ---- Groups: screens (Contacts > G) -----------------------------------------------------
// Groups list -> a group's page (Message / Members / Rename / Share / Delete) -> members
// (every saved contact, Enter puts it in or takes it out) or the share picker (a member).
// Keys as on the other lists: T / V move, digits pick a row, Enter opens, ^ goes back.
const char* const kGrpActs[5] = {"Message", "Members", "Rename", "Share", "Delete"};
const char* const kGrpSubs[5] = {"open the group chat", "", "name it on this radio", "send it to a member",
                                 "remove from this radio"};
uint8_t g_gvCand[MAX_ROWS + kGrpMembers][16];     // members view: the group's members, then other contacts
int g_gvCandN = 0;

void grpCandLoad(bool membersOnly) {
    g_gvCandN = 0;
    const Group& g = g_grp[g_gvGroup];
    for (int i = 0; i < g.n; i++) memcpy(g_gvCand[g_gvCandN++], g.m[i], 16);
    if (membersOnly) return;
    for (const auto& node : g_announceMgr.nodes()) {
        if (node.hash.size() != 16 || !node.saved || isSelf(node.hash.data())) continue;
        if (grpMember(g, node.hash.data()) >= 0 || g_gvCandN >= MAX_ROWS + kGrpMembers) continue;
        memcpy(g_gvCand[g_gvCandN++], node.hash.data(), 16);
    }
}

int grpRowCount() {
    switch (g_gv) {
        case GV_LIST:    return g_grpCount + 1;
        case GV_PAGE:    return 5;
        case GV_MEMBERS:
        case GV_SHARE:   return g_gvCandN;
        default:         return 0;
    }
}

void grpOpenList() {
    g_gv = GV_LIST;
    g_gvSel = 0;
    g_gvScroll = 0;
    requestRedraw();
    Serial.printf("[GRP] list (%d groups)\n", g_grpCount);
}

void grpOpenPage(int gi) {
    g_gvGroup = gi;
    g_gv = GV_PAGE;
    g_gvSel = 0;
    g_gvScroll = 0;
    g_gvDelAt = 0;
    requestRedraw();
}

void grpClose() { g_gv = GV_NONE; requestRedraw(); }

void grpBack() {
    if (g_gv == GV_MEMBERS || g_gv == GV_SHARE) { const int row = g_gv == GV_MEMBERS ? 1 : 3; grpOpenPage(g_gvGroup); g_gvSel = row; }
    else if (g_gv == GV_PAGE) { const int gi = g_gvGroup; grpOpenList(); g_gvSel = gi; clampScroll(g_gvSel, g_gvScroll, grpRowCount()); }
    else grpClose();
    requestRedraw();
}

void grpPrint() {
    for (int i = 0; i < g_grpCount; i++) {
        const Group& g = g_grp[i];
        Serial.printf("[GRP] %d: \"%s\" #%02x%02x%02x%02x %d members\n", i + 1, g.name, g.id[0], g.id[1], g.id[2], g.id[3], (int)g.n);
        for (int k = 0; k < g.n; k++) {
            char nm[24], hex[33];
            peerName(g.m[k], nm, sizeof(nm));
            hashToHex(g.m[k], hex);
            Serial.printf("[GRP]    %s %s\n", hex, nm);
        }
    }
    Serial.printf("[GRP] %d groups\n", g_grpCount);
}

int grpCreate(const char* name) {
    if (g_grpCount >= kGrpMax) { toast("Group list full (%d)", kGrpMax); return -1; }
    Group g = {};
    do {
        const uint32_t r = esp_random();
        memcpy(g.id, &r, 4);
    } while (grpFind(g.id) >= 0 || !memcmp(g.id, "\0\0\0", 4));
    snprintf(g.name, sizeof(g.name), "%.16s", name);
    g_grp[g_grpCount] = g;
    grpSave();
    Serial.printf("[GRP] new \"%s\" #%02x%02x%02x%02x\n", g.name, g.id[0], g.id[1], g.id[2], g.id[3]);
    return g_grpCount++;
}

void grpDelete(int gi) {
    if (gi < 0 || gi >= g_grpCount) return;
    Serial.printf("[GRP] deleted \"%s\"\n", g_grp[gi].name);
    for (int i = gi; i < g_grpCount - 1; i++) g_grp[i] = g_grp[i + 1];
    g_grpCount--;
    grpSave();
}

// In or out of the group. Returns 1 added, 0 removed, -1 refused.
int grpToggleMember(int gi, const uint8_t h[16]) {
    Group& g = g_grp[gi];
    char nm[24];
    peerName(h, nm, sizeof(nm));
    const int at = grpMember(g, h);
    if (at >= 0) {
        for (int i = at; i < g.n - 1; i++) memcpy(g.m[i], g.m[i + 1], 16);
        g.n--;
        grpSave();
        Serial.printf("[GRP] \"%s\" - %s (%d members)\n", g.name, nm, (int)g.n);
        return 0;
    }
    if (isSelf(h) || isGrpHash(h)) return -1;
    if (g.n >= kGrpMembers) { toast("A group holds %d members", kGrpMembers); return -1; }
    memcpy(g.m[g.n++], h, 16);
    grpSave();
    Serial.printf("[GRP] \"%s\" + %s (%d members)\n", g.name, nm, (int)g.n);
    return 1;
}

void openThread(const uint8_t peer[16]);
void goScreen(ScreenId s);
void openMenu();

void grpActivate(int i) {
    if (i < 0 || i >= grpRowCount()) return;
    g_gvSel = i;
    switch (g_gv) {
        case GV_LIST:
            if (i < g_grpCount) { grpOpenPage(i); break; }
            if (g_grpCount >= kGrpMax) { toast("Group list full (%d)", kGrpMax); break; }
            startEntry(Entry::GroupName, "", -1);
            break;
        case GV_PAGE: {
            const int gi = g_gvGroup;
            if (i != 4) g_gvDelAt = 0;
            if (i == 0) {
                if (!g_grp[gi].n) { toast("Add members first (2)"); break; }
                uint8_t h[16];
                grpHash(g_grp[gi].id, h);
                grpClose();
                openThread(h);
            } else if (i == 1) {
                grpCandLoad(false);
                g_gv = GV_MEMBERS;
                g_gvSel = g_gvScroll = 0;
                if (!g_gvCandN) toast("No contacts yet - see Peers");
            } else if (i == 2) {
                startEntry(Entry::GroupName, g_grp[gi].name, gi);
            } else if (i == 3) {
                if (!g_grp[gi].n) { toast("Add members first (2)"); break; }
                grpCandLoad(true);
                g_gv = GV_SHARE;
                g_gvSel = g_gvScroll = 0;
                Serial.println("[GRP] pick who gets the group");
            } else {
                if (!g_gvDelAt || millis() - g_gvDelAt > 3000) { g_gvDelAt = millis() ? millis() : 1; toast("Press again to delete"); break; }
                grpDelete(gi);
                grpOpenList();
                toast("Group deleted");
            }
            break;
        }
        case GV_MEMBERS: {
            const int r = grpToggleMember(g_gvGroup, g_gvCand[i]);
            if (r >= 0) {
                char nm[24];
                peerName(g_gvCand[i], nm, sizeof(nm));
                toast(r ? "+ %.16s" : "- %.16s", nm);
            }
            break;
        }
        case GV_SHARE: {
            uint8_t to[16];
            memcpy(to, g_gvCand[i], 16);
            if (grpShareTo(g_gvGroup, to)) { const int gi = g_gvGroup; grpOpenPage(gi); g_gvSel = 3; }
            break;
        }
        default: break;
    }
    requestRedraw();
}

void grpKey(const KeyEvent& ev) {
    const int n = grpRowCount();
    if (ev.enter) { grpActivate(g_gvSel); return; }
    if (ev.del || ev.space) return;
    const char raw = ev.character;
    if (raw >= '1' && raw <= '9') {
        const int idx = (g_gvSel / 9) * 9 + (raw - '1');
        if (idx >= n) return;
        g_gvSel = idx;
        clampScroll(g_gvSel, g_gvScroll, n);
        if (g_gv == GV_PAGE) grpActivate(idx);       // as on a contact's page: the digit acts
        requestRedraw();
        return;
    }
    switch (toupper((unsigned char)raw)) {
        case 'T': if (g_gvSel > 0) g_gvSel--; clampScroll(g_gvSel, g_gvScroll, n); requestRedraw(); break;
        case 'V': if (g_gvSel < n - 1) g_gvSel++; clampScroll(g_gvSel, g_gvScroll, n); requestRedraw(); break;
        case 'N': if (g_gv == GV_LIST) grpActivate(g_grpCount); break;      // new group
        case 'M': grpClose(); openMenu(); break;
        case 'C': grpClose(); goScreen(SCR_CONTACTS); break;
        case 'I': grpClose(); goScreen(SCR_INBOX); break;
        default: break;
    }
}

void renderGroups() {
    char hdr[48], title[40], sub[40];
    if (g_entry == Entry::GroupName) {               // new group / rename
        int y = HEADER_Y + 4;
        text(6, y, g_entryArg < 0 ? "Name the new group" : "Rename the group", true); y += 30;
        box(2, y - 4, W - 4, 30, 2);
        snprintf(title, sizeof(title), "%.16s_", g_buf);
        text(8, y + 2, title, true, W - 16); y += 40;
        text(6, y, "Enter = save   ^ = cancel"); y += 20;
        text(6, y, "Alt = change case   max 16");
        if (g_toast[0]) text(6, y + 24, g_toast, true);
        return;
    }
    const int n = grpRowCount();
    const Group* g = (g_gv != GV_LIST && g_gvGroup < g_grpCount) ? &g_grp[g_gvGroup] : nullptr;
    if (g_gv == GV_LIST) snprintf(hdr, sizeof(hdr), "Groups (%d)", g_grpCount);
    else if (g_gv == GV_PAGE) snprintf(hdr, sizeof(hdr), "Group: %s", g->name);
    else if (g_gv == GV_MEMBERS) snprintf(hdr, sizeof(hdr), "%.12s: %d of %d", g->name, (int)g->n, kGrpMembers);
    else snprintf(hdr, sizeof(hdr), "Send %.12s to:", g->name);
    clampScroll(g_gvSel, g_gvScroll, n);
    drawHeader(hdr, g_gvScroll > 0);
    if (!n) text(8, LIST_TOP + 8, "No contacts yet - see Peers");
    for (int i = g_gvScroll; i < n && i < g_gvScroll + VIS_ROWS; i++) {
        const int y = LIST_TOP + (i - g_gvScroll) * ROW_H;
        sub[0] = 0;
        if (g_gv == GV_LIST) {
            if (i < g_grpCount) {
                snprintf(title, sizeof(title), "%d : %s", i + 1, g_grp[i].name);
                snprintf(sub, sizeof(sub), g_grp[i].n == 1 ? "%d member" : "%d members", (int)g_grp[i].n);
            } else {
                snprintf(title, sizeof(title), "%d : New group", i + 1);
                snprintf(sub, sizeof(sub), "name it, then pick its members");
            }
        } else if (g_gv == GV_PAGE) {
            snprintf(title, sizeof(title), "%d : %s", i + 1, kGrpActs[i]);
            if (i == 1) snprintf(sub, sizeof(sub), "%d in it (up to %d)", (int)g->n, kGrpMembers);
            else snprintf(sub, sizeof(sub), "%s", kGrpSubs[i]);
        } else {
            char nm[24], hex[33];
            peerName(g_gvCand[i], nm, sizeof(nm));
            hashToHex(g_gvCand[i], hex);
            if (g_gv == GV_MEMBERS)
                snprintf(title, sizeof(title), "%d : [%c] %s", i % 9 + 1, grpMember(*g, g_gvCand[i]) >= 0 ? 'x' : ' ', nm);
            else
                snprintf(title, sizeof(title), "%d : %s", i % 9 + 1, nm);
            snprintf(sub, sizeof(sub), "%.24s", hex);
        }
        listRow(y, title, sub, i == g_gvSel);
        addHit(2, y, W - 4, ROW_H, H_GRP, i);
    }
    const bool more = g_gvScroll + VIS_ROWS < n;     // rows below the last one shown
    const char* hint = g_gv == GV_LIST    ? (more ? "V more  Ent open  ^ back" : "Ent open  N new  ^ back")
                     : g_gv == GV_PAGE    ? (more ? "1-5  V more  Ent  ^ back" : "1-5  T/V  Ent  ^ back")
                     : g_gv == GV_MEMBERS ? (more ? "V more  Ent in/out  ^" : "Ent = in / out  ^ done")
                                          : (more ? "V more  Ent send  ^" : "Ent = send  ^ cancel");
    box(0, FOOT_Y, W, FOOT_H, 1);
    label(0, FOOT_Y - 1, W, FOOT_H, g_toast[0] ? g_toast : hint);
}

void grpNameCommit() {
    char name[17];
    size_t n = g_bufLen;
    while (n && g_buf[n - 1] == ' ') n--;
    if (!n) { toast("Type a name first"); return; }
    snprintf(name, sizeof(name), "%.*s", (int)(n < 16 ? n : 16), g_buf);
    const int gi = g_entryArg;
    endEntry();
    if (gi < 0) {
        const int made = grpCreate(name);
        if (made < 0) return;
        grpOpenPage(made);
        grpCandLoad(false);                          // straight to picking members
        g_gv = GV_MEMBERS;
        toast("Enter = in or out");
    } else if (gi < g_grpCount) {
        strlcpy(g_grp[gi].name, name, sizeof(g_grp[gi].name));
        grpSave();
        Serial.printf("[GRP] renamed -> \"%s\"\n", name);
        toast("Saved \"%.16s\"", name);
    }
    requestRedraw();
}

// MIC in a group chat: who got the newest message sent from this radio.
void renderGrpWho(int y) {
    const int gi = grpOfHash(g_threadPeer);
    if (gi < 0 || !g_grpLast.valid) { text(6, y, "(nothing sent to this group yet)"); return; }
    char line[48];
    snprintf(line, sizeof(line), "\"%.26s\"", g_grpLast.text);
    text(6, y, line, false, W - 12);
    y += 24;
    const Group& g = g_grp[gi];
    for (int m = 0; m < g.n; m++) {
        char nm[24];
        peerName(g.m[m], nm, sizeof(nm));
        const uint8_t st = g_grpLast.st[m];
        const char* s = st == GS_DELIVERED ? "delivered" : st == GS_FAILED ? "failed" : st == GS_WAIT ? "waiting to send"
                      : st == GS_SENT ? "sent, no receipt yet" : st == GS_NOPROOF ? "no reply" : "not sent to";
        snprintf(line, sizeof(line), "%.11s: %s", nm, s);
        text(6, y, line, st == GS_FAILED, W - 12);
        y += 20;
    }
}

// Enter on an empty draft with a group item offered: join the shared group, or open the
// group's chat.
void grpThreadEnter() {
    if (g_threadGrp.join) {
        grpJoinFromThread(g_threadPeer);
        loadHistory(g_threadPeer, false);
        requestRedraw();
        return;
    }
    uint8_t h[16];
    grpHash(g_threadGrp.id, h);
    openThread(h);
}

// Console (bench / simulator scripts): grp | grp new <name> | grp add <n> <32 hex> |
// grp rm <n> <32 hex> | grp del <n> | grp send <n> <text> | grp share <n> <32 hex> |
// grp open <n> | grp who | grp hist     (n = the group's number in the list, from 1)
void grpConsole(const char* p) {
    int n = 0;
    char hx[40] = "";
    uint8_t h[16];
    if (!strncmp(p, "new ", 4) && p[4]) {
        grpCreate(p + 4);
    } else if (sscanf(p, "add %d %39s", &n, hx) == 2 && n >= 1 && n <= g_grpCount && strlen(hx) >= 32 && hexToHash(hx, h)) {
        if (grpMember(g_grp[n - 1], h) < 0) grpToggleMember(n - 1, h);
    } else if (sscanf(p, "rm %d %39s", &n, hx) == 2 && n >= 1 && n <= g_grpCount && strlen(hx) >= 32 && hexToHash(hx, h)) {
        if (grpMember(g_grp[n - 1], h) >= 0) grpToggleMember(n - 1, h);
    } else if (sscanf(p, "del %d", &n) == 1 && n >= 1 && n <= g_grpCount) {
        grpDelete(n - 1);
    } else if (sscanf(p, "send %d", &n) == 1 && n >= 1 && n <= g_grpCount && strchr(p + 5, ' ')) {
        const int sent = groupSend(n - 1, strchr(p + 5, ' ') + 1);
        if (!sent) Serial.printf("[GRP] not sent: %s\n", g_txStr);
        if (g_threadOpen) loadHistory(g_threadPeer, false);
    } else if (sscanf(p, "share %d %39s", &n, hx) == 2 && n >= 1 && n <= g_grpCount && strlen(hx) >= 32 && hexToHash(hx, h)) {
        grpShareTo(n - 1, h);
    } else if (sscanf(p, "open %d", &n) == 1 && n >= 1 && n <= g_grpCount) {
        grpHash(g_grp[n - 1].id, h);
        grpClose();
        g_menuOpen = false;
        g_idleActive = false;
        noteInput();                             // counts as input, like opening it with keys
        openThread(h);
    } else if (!strcmp(p, "who")) {
        const int gi = g_threadOpen ? grpOfHash(g_threadPeer) : -1;
        if (gi >= 0) loadHistory(g_threadPeer, false);
        if (gi < 0 || !g_grpLast.valid) { Serial.println("[GRP] who: nothing (open a group chat with a sent message)"); return; }
        Serial.printf("[GRP] who \"%s\":", g_grpLast.text);
        for (int m = 0; m < g_grp[gi].n; m++) {
            char nm[24];
            peerName(g_grp[gi].m[m], nm, sizeof(nm));
            const uint8_t st = g_grpLast.st[m];
            Serial.printf(" %s=%s", nm, st == GS_DELIVERED ? "delivered" : st == GS_FAILED ? "failed" : st == GS_WAIT ? "waiting"
                                        : st == GS_SENT ? "sent" : st == GS_NOPROOF ? "noreply" : "none");
        }
        Serial.println();
        return;
    } else if (!strcmp(p, "hist")) {
        char nm[24] = "-";
        if (g_threadOpen) { peerName(g_threadPeer, nm, sizeof(nm)); loadHistory(g_threadPeer, false); }
        Serial.printf("[GRP] hist %s: %d rows%s\n", nm, g_threadOpen ? g_histCount : 0, g_threadOpen ? "" : " (no chat open)");
        for (int i = 0; g_threadOpen && i < g_histCount; i++)
            Serial.printf("[GRP] row %d %s \"%s\" [%s]\n", i + 1, g_hist[i].incoming ? "in" : "out", g_hist[i].text, g_hist[i].status);
        return;
    } else if (p[0]) {
        Serial.println("[GRP] grp | new <name> | add <n> <hex> | rm <n> <hex> | del <n> | send <n> <text> | share <n> <hex> | open <n> | who | hist");
        return;
    }
    grpPrint();
    requestRedraw();
}

// ---- Setup wizard (fresh device, or Settings S.7 "Run setup wizard") --------------------
// One screen per step, "Step 3/10" in the header. Enter keeps what is shown and goes on, ^
// goes back a step, 0 finishes with the rest left as it is. A digit picks a row: on a
// choice step it also goes on.
enum WizStep : uint8_t { WZ_WELCOME, WZ_NAME, WZ_TIME, WZ_CLOCK, WZ_POWER, WZ_IDLE, WZ_MODE, WZ_LOCK,
                         WZ_IMPORT, WZ_DONE, WZ_COUNT };
const char* const kWizTitle[WZ_COUNT] = {"Welcome", "Device name", "Time zone", "Clock", "Energy mode",
                                         "Idle screen", "Simple / Advanced", "Password lock",
                                         "Import contacts", "Done"};
constexpr int kWizModeN = 5;
const uint8_t kWizModes[kWizModeN] = {power::MODE_FULL, power::MODE_BALANCED, power::MODE_ECO, power::MODE_SAVER,
                                      power::MODE_CHECKIN};

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
            if (k < 1 || k > kWizModeN) return false;
            g_pw.mode = kWizModes[k - 1]; power::save(g_pw); powerApply(true);
            return true;
        case WZ_IDLE:
            if (k < 1 || k > IDLE_COUNT) return false;
            g_cfg.idleScreen = (uint8_t)(k - 1); saveConfig();
            if (g_cfg.idleScreen == IDLE_FOUND) {      // the name right away; the rest in S.4.2.9
                g_foundField = 0;
                startEntry(Entry::FoundText, g_cfg.found[0]);
                g_entryReplace = true;
                return false;
            }
            return true;
        case WZ_MODE:
            if (k < 1 || k > 3) return false;
            g_cfg.opMode = (uint8_t)(k - 1); saveConfig();
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
            line("S.2 Time > Set RTC", false);
            break;
        }
        case WZ_CLOCK:
            row("1 24 h", "13:45", !g_cfg.clock12);
            row("2 12 h", "1:45 PM", g_cfg.clock12);
            break;
        case WZ_POWER:
            for (int i = 0; i < kWizModeN; i++) {
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
            row("1 Minimal", "Inbox, Contacts, talk", g_cfg.opMode == OP_MINIMAL);
            row("2 Typical", "most features (pick: S.1.5)", g_cfg.opMode == OP_NORMAL);
            row("3 Everything", "all features", g_cfg.opMode == OP_ADVANCED);
            y += 6;
            line("Change later: Settings S.1", false);
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
            snprintf(b, sizeof(b), g_wizListen ? "listening: %d received" : "the other: S.7 Share all", g_wizGot);
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
// Track back (Atlas B, Settings S.2.10): from the track point nearest to you, walk the
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
    if (g_trackN < 2) { toast("No track yet (S.2.10)"); return false; }
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

// Route points from GPX files (S.2 Track page row 5, console "gpximport"): every <wpt> and
// <rtept> of the .gpx files in <SD>/ratdeckpro/routes/ becomes a waypoint (name from <name>,
// else R1, R2 ...), in file order, until the 32 waypoints are full. A point that is already
// there (same name within 30 m) is skipped. Track points (<trkpt>) are not imported: plan a
// route with route points. P on the map steps through them, G shows the arrow to the one
// selected. Only the first 96 KB of a file are read. Returns the number added, -1 = no card.
int gpxImport() {
    if (!sdMount()) { toast("No SD card"); return -1; }
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/routes", SD_PATH_ROOT);
    g_sdStore.ensureDir(dir);
    File d = g_sdStore.openDir(dir);
    if (!d) { toast("No routes folder"); return -1; }
    constexpr size_t kMax = 96 * 1024;
    char* buf = (char*)heap_caps_malloc(kMax + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (char*)malloc(kMax + 1);
    if (!buf) { d.close(); return -1; }
    int added = 0, files = 0, seen = 0;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        const char* fn = strrchr(e.name(), '/');
        fn = fn ? fn + 1 : e.name();
        const size_t fl = strlen(fn);
        if (e.isDirectory() || fl < 5 || strcasecmp(fn + fl - 4, ".gpx") || fn[0] == '.') { e.close(); continue; }
        const size_t got = e.read((uint8_t*)buf, kMax);
        e.close();
        buf[got] = 0;
        files++;
        for (char* p = buf; (p = strchr(p, '<')) != nullptr; p++) {
            const bool wpt = !strncmp(p, "<wpt", 4) && (p[4] == ' ' || p[4] == '\n' || p[4] == '\r' || p[4] == '\t');
            const bool rte = !strncmp(p, "<rtept", 6) && (p[6] == ' ' || p[6] == '\n' || p[6] == '\r' || p[6] == '\t');
            if (!wpt && !rte) continue;
            char* tagEnd = strchr(p, '>');
            if (!tagEnd) break;
            const char* la = strstr(p, "lat=");
            const char* lo = strstr(p, "lon=");
            if (!la || !lo || la > tagEnd || lo > tagEnd) continue;
            const double lat = atof(la + 5), lon = atof(lo + 5);
            if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) continue;
            seen++;
            char name[16];
            snprintf(name, sizeof(name), "R%d", seen);
            const char* close = strstr(tagEnd, wpt ? "</wpt>" : "</rtept>");
            const bool selfClosed = tagEnd[-1] == '/';
            const char* nm = selfClosed ? nullptr : strstr(tagEnd, "<name>");
            if (nm && close && nm < close) {
                nm += 6;
                size_t k = 0;
                for (; nm[k] && nm[k] != '<' && k < sizeof(name) - 1; k++) name[k] = (nm[k] >= 32 && nm[k] < 127) ? nm[k] : '?';
                if (k) name[k] = 0;
            }
            bool dup = false;
            for (int i = 0; i < g_wptCount; i++)
                dup = dup || (!strcmp(g_wpt[i].name, name) && metersBetween(g_wpt[i].lat, g_wpt[i].lon, lat, lon) < 30.0);
            if (dup) continue;
            if (g_wptCount >= kWptMax) { Serial.println("[GPX] waypoints full (32)"); goto done; }
            Wpt& w = g_wpt[g_wptCount++];
            w.lat = (float)lat; w.lon = (float)lon; w.at = (uint32_t)time(nullptr);
            strlcpy(w.name, name, sizeof(w.name));
            added++;
            Serial.printf("[GPX] W%d %s %.5f, %.5f (%s)%c", g_wptCount, name, lat, lon, fn, 10);
        }
    }
done:
    d.close();
    free(buf);
    if (added) wptSave();
    Serial.printf("[GPX] %d file(s), %d point(s) read, %d waypoint(s) added, %d in all%c", files, seen, added, g_wptCount, 10);
    return added;
}

// ---- Bearing + distance (G on the Atlas) ------------------------------------------------
// For when the map is too much: a big arrow to the selected contact, waypoint or track-back
// point, the distance and the age of a contact's fix. No compass on board: while you move
// (GPS speed over 1 knot) the arrow is relative to your course over ground ("ahead" = up),
// standing still it is north-up.
enum NavKind : uint8_t { NAV_CONTACT, NAV_WPT, NAV_TRACK, NAV_HOME };
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
        case NAV_HOME:
            if (!g_home.set) return false;
            la = g_home.lat; lo = g_home.lon;
            snprintf(name, n, "Home");
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
    else if (g_home.set) { g_navKind = NAV_HOME; g_navIdx = 0; }
    else { toast("Pick a contact (N) or waypoint (P), or set Home (S.2)"); requestRedraw(); return; }
    g_navOpen = true;
    g_navKeysOpen = false;
    g_navShownM = -1;
    g_fullRefreshReq = true;
    requestRedraw();
    Serial.printf("[NAV] open kind=%d idx=%d\n", (int)g_navKind, g_navIdx);
}

// The arrow to Home, from anywhere (S.2 row 13, map Alt+H, console "home go").
void homeGo() {
    if (!g_home.set) { toast("Home is not set: M, S, 2, row 14"); requestRedraw(); return; }
    if (g_tbOn) tbStop("going home");
    if (!mapview::isOpen()) openMap();
    g_mapSel = -1;
    g_wptSel = -1;
    navOpen();
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
        label(0, 142, W, 18, "(GPS or saved position S.2)");
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

// ---- Range test (Settings S.8 LoRa > Range test) -----------------------------------------
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
    if (gpsCurrent() && ownPosition(la, lo)) snprintf(a, sizeof(a), "me %.5f,%.5f", la, lo);
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
    else if (g_wtOpen)                 renderWalkie();
    else if (g_vmOpen)                 renderVoiceMsg();
    else if (g_threadOpen)             renderThread();
    else if (g_entry == Entry::Name)   renderRename();
    else if (g_wizOpen)                renderWizard();
    else if (g_navOpen)                renderNav();
    else if (g_rtOpen)                 renderRangeTest();
    else if (g_cardOpen)               renderCard();
    else if (g_mk != MK_NONE)          renderMarks();
    else if (g_layOpen)                renderLayers();
    else if (g_gv != GV_NONE)          renderGroups();
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
    if (mapview::isOpen() && !g_threadOpen && !g_menuOpen && !g_libOpen && !g_gameOpen && !g_chessOpen && !g_placeOpen && !g_navOpen && g_mk == MK_NONE && !g_layOpen)
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
    const uint32_t sinceKey = g_lastInputMs ? millis() - g_lastInputMs : 0;
    Serial.printf("[UI] frame %ums (build %u, panel %u), %u ms after the last key / touch\n",
                  (unsigned)(millis() - t0), (unsigned)(t1 - t0), (unsigned)g_display.lastRefreshMs(),
                  (unsigned)sinceKey);
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
constexpr int kIdleMapTop = 56, kIdleMapBottom = 300;

// How long the unit has been up, as shown on screen. The simulator can start it later than 0
// (sim.ini "uptime = 1d2h") so videos do not show a radio that has run for two minutes.
uint32_t uptimeShownS() {
#if defined(SIMULATOR)
    return millis() / 1000UL + sim::uptimeOffsetS();
#else
    return millis() / 1000UL;
#endif
}

// The top banner of every idle screen (Splash, If found, Map, Library page; locked or not), the
// same on all of them, kIdleMapTop px high:
//   [battery]  100%+           1:45 PM EST      battery icon as in the status bar (bolt =
//   [ icon  ]  ~12h             11/22/2026      charging, plug = on USB), %, time left on battery
//   NAME OF THIS RADIO               3 NEW      the name in bold; new messages (bold when any)
// It is redrawn when the minute, the battery % or the new-message count changes (updateIdle),
// as the three text lines before it were: no more panel refreshes than before.
void drawIdleTop() {
    char t[12], line[40], bat[16] = "--%", left[10] = "", md[12] = "--/--/----", unr[12];
    fmtTime(t, sizeof(t));
    struct tm lt;
    if (localNow(lt)) snprintf(md, sizeof(md), "%d/%d/%d", lt.tm_mon + 1, lt.tm_mday, lt.tm_year + 1900);
    const int pct = g_bat.valid ? g_bat.pct : -1;
    const int unread = g_storeReady ? g_msgStore.totalUnreadCount() : 0;
    box(0, 0, W, kIdleMapTop, 1);
    // Left: the battery.
    drawBatteryIcon(5, 3, 16, 31, pct);
    if (pct >= 0) snprintf(bat, sizeof(bat), "%d%%%s", pct, g_bat.charging ? "+" : "");
    text(26, 1, bat, true);
    const int h10 = g_bat.usb ? -1 : batHoursLeft10(nullptr);
    if (h10 >= 0) snprintf(left, sizeof(left), h10 < 480 ? "~%dh" : "~%dd", h10 < 480 ? (h10 + 5) / 10 : (h10 + 120) / 240);
    else if (g_bat.usb) snprintf(left, sizeof(left), "USB");
    text(26, 18, left);
    // Right: the time (with the zone when it fits beside the battery) and the date.
    snprintf(line, sizeof(line), "%s %s", t, tzCur().name);
    if (textW(line) > W - 4 - 84) snprintf(line, sizeof(line), "%s", t);
    text(W - 5 - textW(line), 1, line);
    text(W - 5 - textW(md), 18, md);
    // Bottom line: the radio's name (how you tell units apart, also on a flat one) and the
    // new-message count. Only a name too wide to sit beside the count is cut.
    if (unread > 0) snprintf(unr, sizeof(unr), "%d NEW", unread); else snprintf(unr, sizeof(unr), "no new");
    const int uw = textW(unr) + (unread > 0 ? 2 : 0);
    text(W - 5 - uw, 36, unr, unread > 0);
    char nm[sizeof(g_selfName)];
    strlcpy(nm, g_selfName[0] ? g_selfName : "no name", sizeof(nm));
    const int nameW = W - 5 - uw - 10 - 5;
    for (size_t n = strlen(nm); n > 1 && textW(nm) + 2 > nameW; ) nm[--n] = 0;
    text(5, 36, nm, true);
    g_idleShownUnread = unread;
    g_idleShownPct = pct;
    g_idleShownMin = millis() / 60000UL;
}

void drawIdleFoot(const char* unlockedText) {
    box(0, kIdleMapBottom, W, H - kIdleMapBottom, 1);
    // About to go flat (under 1 %, on battery): say so instead of "press a key".
    const bool dying = g_bat.valid && !g_bat.usb && g_bat.pct < 1;
    const char* s = dying ? (g_locked ? "LOCKED | plug in USB" : "PLUG IN USB TO CHARGE")
                          : g_locked ? (g_standby ? "LOCKED | standby | key" : "LOCKED | press a key")
                          : g_standby && !strncmp(unlockedText, "press", 5) ? "STANDBY | press a KEY"
                          : unlockedText;
    label(0, kIdleMapBottom, W, H - kIdleMapBottom, s, g_locked || dying);
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
    const int y0 = 158, step = 20;
    label(0, y0, W, 20, "RatSpeak", true);
    char line[40];
    label(0, y0 + step, W, 18,
          g_bat.charging ? (g_bat.usb ? "charging | plugged in" : "charging")
                         : (g_bat.usb ? "plugged in" : "unplugged"));
    uint32_t s = uptimeShownS();
    label(0, y0 + 2 * step + 6, W, 16, "uptime");
    snprintf(line, sizeof(line), "%05luD : %02luH : %02luM", (unsigned long)(s / 86400UL),
             (unsigned long)((s / 3600UL) % 24UL), (unsigned long)((s / 60UL) % 60UL));
    label(0, y0 + 3 * step + 6, W, 16, line);   // (the time zone is beside the clock at the top)
    char hd[24];
    if (homeText(hd, sizeof(hd))) { snprintf(line, sizeof(line), "Home %s", hd); label(0, y0 + 4 * step + 8, W, 16, line); }
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
    static char (&lines)[kLibLines][kLibLineMax] = psram<char[kLibLineMax], kLibLines>();
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
    // The filled lines: Name, Phone, Address 1, Address 2, "Town, State ZIP", Country, Misc 1, 2.
    const auto& F = g_cfg.found;
    char place[96] = "";
    snprintf(place, sizeof(place), "%s%s%s%s%s", F[4], F[4][0] && (F[5][0] || F[6][0]) ? ", " : "", F[5],
             F[5][0] && F[6][0] ? " " : "", F[6]);
    const char* show[8] = {F[0], F[1], F[2], F[3], place, F[7], F[8], F[9]};
    const char* lines[8];
    int n = 0;
    for (const char* s : show) if (s[0]) lines[n++] = s;
    if (n) {
        const int bottom = g_cfg.foundAddr ? 222 : 296;
        if (n * 20 <= bottom - y) {                  // the normal font, the name in bold
            for (int i = 0; i < n; i++) { label(0, y, W, 20, lines[i], i == 0 && F[0][0]); y += 20; }
        } else {                                     // many lines: the 12 px font
            for (int i = 0; i < n; i++) {
                const int w = lv_txt_get_width(lines[i], strlen(lines[i]), &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE);
                smallText(w < W - 4 ? (W - w) / 2 : 2, y, lines[i], i == 0 && F[0][0]);
                y += 15;
            }
        }
    } else {
        label(0, y, W, 20, "(details not set:");
        label(0, y + 22, W, 20, "Settings S.4.2.9)");
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

// Idle screen "Map" (S.4.2 "Idle screen"): the map centred on the GPS fix, else the
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
        size_t n = mapReadTile("/maps/home.txt", 0, (uint8_t*)txt, sizeof(txt) - 1);
        if (!n) n = mapReadTile("/packs/home.txt", 0, (uint8_t*)txt, sizeof(txt) - 1);
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

// ---- Boot screen and empty battery ---------------------------------------------------
// E-ink keeps its last image with no power at all, so what is drawn last before the battery
// gives out stays on the panel for days. That must be the idle screen (If found ...), not a
// boot screen. So:
//  - the boot screen IS the idle screen, with a banner over its bottom ("BOOTING v0.1");
//  - on battery under kBatEmptyV the unit draws the idle screen with "BATTERY EMPTY / PLUG IN
//    A USB CHARGER" and switches itself off (charger ship mode: the battery is disconnected
//    until USB is plugged in), before the chip browns out;
//  - after that (or after a brownout reset) only USB starts it again: a rested empty cell
//    climbs back over 3.5 V (surface charge), boots, and dies again under load. Without USB the
//    boot check goes straight back to off without touching the panel.
bool chargerUpdateReg(uint8_t reg, uint8_t mask, uint8_t value);
constexpr float kBatEmptyV = 3.40f;      // running on battery: switch off under this (sustained)
constexpr float kBatBootV  = 3.50f;      // starting on battery needs at least this
constexpr float kBatBrownoutOkV = 3.80f; // a brownout reset with this much is not an empty cell

// NVS "rs_boot": what the boot screen needs before the protocol and vault are up, and the
// empty-battery state. empty: 0 = no, 1 = switched off empty, 2 = and the panel shows it.
uint8_t bootNvsGetU8(const char* k) {
    Preferences p;
    if (!p.begin("rs_boot", true)) return 0;
    const uint8_t v = p.getUChar(k, 0);
    p.end();
    return v;
}
void bootNvsSetU8(const char* k, uint8_t v) {
    if (bootNvsGetU8(k) == v) return;
    Preferences p;
    if (p.begin("rs_boot", false)) { p.putUChar(k, v); p.end(); }
}
void bootCacheDest() {                       // this radio's address, for If found at boot
    if (!g_destHex[0]) return;
    Preferences p;
    if (!p.begin("rs_boot", false)) return;
    if (p.getString("dest", "") != g_destHex) p.putString("dest", g_destHex);
    p.end();
}

// The idle screen as it can be drawn at boot: no SD card yet, so Map and Library page show
// If found (when filled in) or the splash; a password lock is taken from the last run.
void paintIdleEarly() {
    const uint8_t keepSc = g_cfg.idleScreen;
    const bool keepLock = g_locked;
    const bool noDest = !g_destHex[0];
    if (noDest) {
        Preferences p;
        if (p.begin("rs_boot", true)) { strlcpy(g_destHex, p.getString("dest", "").c_str(), sizeof(g_destHex)); p.end(); }
    }
    g_locked = bootNvsGetU8("lock") != 0;
    if (keepSc == IDLE_MAP || keepSc == IDLE_LIBRARY) g_cfg.idleScreen = foundCount() ? IDLE_FOUND : IDLE_SPLASH;
    paintIdle();
    g_cfg.idleScreen = keepSc;
    g_locked = keepLock;
    if (noDest) g_destHex[0] = 0;
}

// A two-line banner over the bottom of the idle screen (on top of its wake / LOCKED line).
void idleBanner(const char* line1, const char* line2) {
    constexpr int bh = 44;                   // clear of If found's last line (ends at y 276)
    box(0, H - bh, W, bh, 3);
    label(0, H - bh + 2, W, 20, line1, true);
    label(0, H - bh + 22, W, 18, line2);
    lv_refr_now(nullptr);
}

void drawBootSplash() {
    char l2[40];
    if (g_bat.valid && g_bat.usb) snprintf(l2, sizeof(l2), "USB power, battery %d%%", g_bat.pct);
    else if (g_bat.valid)         snprintf(l2, sizeof(l2), "on battery, %d%%", g_bat.pct);
    else                          snprintf(l2, sizeof(l2), "starting up");
    paintIdleEarly();
    idleBanner("BOOTING " RATSPEAK_FW_VERSION, l2);
    // Full refresh: after a reset (side button) the panel's previous-image RAM is cleared, so a
    // partial update showed nothing (white). Full is also quicker here (1.1 s vs 1.9 s).
    g_display.fullRefresh();
}

// Switch off for an empty battery: the idle screen + "plug in" banner (unless the panel shows
// it already), everything powered down, then the charger's ship mode cuts the battery off
// until USB is plugged in. If that does not take, deep sleep and look again in 30 min.
[[noreturn]] void batteryShutdown(bool redraw, bool early) {
    Serial.printf("[BAT] empty (%.2f V, no USB): %s, switching off until USB\n", g_bat.volts,
                  redraw ? "drawing the idle screen" : "the panel already shows it");
    soakLog("EMPTY %.3fV %d%%: switching off until USB", g_bat.volts, g_bat.pct);
    Serial.flush();
    bootNvsSetU8("empty", 1);
    if (redraw) {
        if (early) paintIdleEarly(); else paintIdle();
        idleBanner("BATTERY EMPTY", "PLUG IN A USB CHARGER");
        g_display.fullRefresh();
        bootNvsSetU8("empty", 2);
    }
    g_display.hibernate();
#if !defined(SIMULATOR)
    static const int kOff[] = {BOARD_LORA_EN, BOARD_GPS_EN, BOARD_6609_EN, BOARD_EPD_BACKLIGHT, BOARD_KEYBOARD_LED,
                               BOARD_MOTOR_PIN};
    for (int p : kOff) { pinMode(p, OUTPUT); digitalWrite(p, LOW); gpio_hold_en((gpio_num_t)p); }
    gpio_deep_sleep_hold_en();
    // BQ25896 REG09: BATFET_DIS (bit 5) = 1, BATFET_DLY (bit 3) = 0 -> battery off now; plugging
    // in USB turns it back on.
    chargerUpdateReg(0x09, 0x28, 0x20);
    delay(1000);
    Serial.println("[BAT] still on (ship mode refused or USB): deep sleep, check again in 30 min");
    Serial.flush();
    esp_sleep_enable_timer_wakeup(30ULL * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
#endif
    for (;;) delay(1000);
}

// Boot check, before anything is drawn: true = start; false = switch straight off.
bool bootBatteryOk(bool& panelShowsEmpty) {
#if !defined(SIMULATOR)
    chargerUpdateReg(0x09, 0x20, 0x00);   // battery back on (a switch-off on USB left it off)
#endif
    readBattery();
    const uint8_t empty = bootNvsGetU8("empty");
    panelShowsEmpty = empty == 2;
#if defined(SIMULATOR)
    return true;
#else
    const bool brownout = esp_reset_reason() == ESP_RST_BROWNOUT;
    if (g_bat.usb) {
        if (empty) Serial.println("[BAT] USB power: starting (was switched off empty)");
        bootNvsSetU8("empty", 0);
        return true;
    }
    if (!g_bat.valid) return !empty;                       // no gauge reading: only the flag
    if (empty) {
        Serial.printf("[BAT] %.2f V after an empty switch-off: waiting for USB\n", g_bat.volts);
        return false;
    }
    if (brownout && g_bat.volts < kBatBrownoutOkV) {
        Serial.printf("[BAT] brownout reset at %.2f V: waiting for USB\n", g_bat.volts);
        return false;
    }
    if (g_bat.volts < kBatBootV) {
        Serial.printf("[BAT] %.2f V is too low to start on battery: waiting for USB\n", g_bat.volts);
        return false;
    }
    return true;
#endif
}

// While running on battery: under kBatEmptyV for ~8 s in a row -> switch off cleanly.
void emptyBatteryTick() {
#if !defined(SIMULATOR)
    static uint8_t low = 0;
    static uint32_t darkAt = 0;          // soak listener told: switch off at this time
    if (!g_bat.valid || g_bat.usb || g_bat.volts >= kBatEmptyV) { low = 0; darkAt = 0; return; }
    if (low < 10) { low++; return; }
    if (!darkAt) {
        const uint32_t wait = soakGoingDark();
        if (wait) { darkAt = (millis() + wait) | 1; return; }
    } else if ((int32_t)(millis() - darkAt) < 0) {
        return;
    }
    batteryShutdown(true, false);
#endif
}

// ---- Switched off (S.5 "Switch off", console "poweroff") -------------------------------
// For a radio that is stored for weeks or months and used now and then: everything is
// powered down and the ESP32 is in deep sleep (a few hundred uA for the whole board, not
// measured yet). The panel keeps showing the idle screen with "SWITCHED OFF". On again:
// hold the top side button ~2 s. A charger or solar panel on USB charges the battery
// meanwhile (the charger chip works alone) and does NOT switch the radio on. Every 12 h it
// wakes for under a second to look at the battery: under kBatEmptyV the normal empty
// switch-off (ship mode) takes over, so a long storage cannot drain the cell flat.
// NVS rs_boot "off": 1 = switched off by the owner. "offT" / "offMv" / "offMah": clock,
// battery voltage and gauge charge at switch-off, for the report at the next switch-on.
constexpr uint32_t kOffHoldMs = 1200;        // button still down this long after the wake
constexpr uint16_t kOffCheckMin = 12 * 60;   // battery look; console "poweroff <min>" = another period (tests)
// "offMin": the period of this switch-off; "offLooks" / "offLowMv": battery looks since then
// and the lowest voltage one of them saw (the report shows both: proof the timer wake works).
char g_offReport[72] = "";                   // "off 13.2 h, 4.05 -> 4.03 V, 3 mAh" (pw, Power page)

void offcLoad() {
    Preferences p;
    if (!p.begin("rs_boot", true)) return;
    g_offc.on = p.getUChar("dcOn", 0) != 0;
    g_offc.atMin = (uint16_t)(p.getUShort("dcMin", 20 * 60) % 1440);
    g_offc.stayMin = (uint8_t)constrain((int)p.getUChar("dcStay", 5), 2, 30);
    p.end();
}
void offcSave() {
    Preferences p;
    if (!p.begin("rs_boot", false)) return;
    p.putUChar("dcOn", g_offc.on ? 1 : 0);
    p.putUShort("dcMin", g_offc.atMin);
    p.putUChar("dcStay", g_offc.stayMin);
    p.end();
    offcPrint();
}
// Local minute of the day and day number; false = the clock is not set.
bool offcClock(int& minOfDay, uint32_t& day) {
    const time_t now = time(nullptr);
    if (now < 1600000000) return false;
    struct tm t;
    localtime_r(&now, &t);
    minOfDay = t.tm_hour * 60 + t.tm_min;
    day = (uint32_t)(t.tm_year * 366 + t.tm_yday);
    return true;
}
// Seconds until the next contact time; 0 = daily contact off or no clock.
uint32_t offcSecsUntil() {
    int m; uint32_t day;
    if (!g_offc.on || !offcClock(m, day)) return 0;
    int d = (int)g_offc.atMin - m;
    if (d <= 0) d += 1440;
    const time_t now = time(nullptr);
    return (uint32_t)d * 60u - (uint32_t)(now % 60);
}
// A timer wake: is this the contact time (and not done today yet)? Marks today as done.
bool offcDue() {
    int m; uint32_t day;
#if defined(SIMULATOR)
    const char* e = getenv("RS_SIM_WAKE");
    const bool force = e && !strcmp(e, "contact");
#else
    const bool force = false;
#endif
    if (!g_offc.on || !offcClock(m, day)) return false;
    const int late = (m - (int)g_offc.atMin + 1440) % 1440;       // minutes after the contact time
    Preferences p;
    if (!p.begin("rs_boot", false)) return false;
    const bool due = force || (late < 30 && p.getULong("dcDay", 0) != day);
    if (due) p.putULong("dcDay", day);
    p.end();
    return due;
}
void offcPrint() {
    Serial.printf("[CONTACT] daily contact %s, at %02u:%02u, stays on %u min, sends to %d, next in %lu s%c",
                  g_offc.on ? "on" : "off", (unsigned)(g_offc.atMin / 60), (unsigned)(g_offc.atMin % 60),
                  (unsigned)g_offc.stayMin, ashareActive(), (unsigned long)offcSecsUntil(), 10);
}

enum OffWake : uint8_t { OFF_WAKE_POWER, OFF_WAKE_BUTTON, OFF_WAKE_TIMER };
OffWake offWakeCause() {
#if defined(SIMULATOR)
    const char* e = getenv("RS_SIM_WAKE");   // "power" / "timer"; default = the button, held
    return e && !strcmp(e, "power") ? OFF_WAKE_POWER
           : e && (!strcmp(e, "timer") || !strcmp(e, "contact")) ? OFF_WAKE_TIMER : OFF_WAKE_BUTTON;
#else
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT0:  return OFF_WAKE_BUTTON;
        case ESP_SLEEP_WAKEUP_TIMER: return OFF_WAKE_TIMER;
        default:                     return OFF_WAKE_POWER;   // power came back, restart button
    }
#endif
}

// Deep sleep until the top button (or the 12 h battery look). Nothing is drawn here.
[[noreturn]] void offSleep() {
    Serial.flush();
#if !defined(SIMULATOR)
    g_display.hibernate();
    static const int kOff[] = {BOARD_LORA_EN, BOARD_GPS_EN, BOARD_6609_EN, BOARD_1V8_EN, BOARD_EPD_BACKLIGHT,
                               BOARD_KEYBOARD_LED, BOARD_MOTOR_PIN};
    for (int p : kOff) { pinMode(p, OUTPUT); digitalWrite(p, LOW); gpio_hold_en((gpio_num_t)p); }
    gpio_deep_sleep_hold_en();
    pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
    for (uint32_t t0 = millis(); digitalRead(BOARD_BOOT_PIN) == LOW && millis() - t0 < 10000;) delay(20);
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BOOT_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BOOT_PIN);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BOOT_PIN, 0);
    {
        Preferences p;
        uint16_t m = kOffCheckMin;
        if (p.begin("rs_boot", true)) { m = p.getUShort("offMin", kOffCheckMin); p.end(); }
        uint64_t secs = (uint64_t)(m ? m : kOffCheckMin) * 60ULL;
        const uint32_t c = offcSecsUntil();              // wake for the daily contact, if sooner
        if (c && c + 15 < secs) secs = c + 15;
        esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
    }
    esp_deep_sleep_start();
#else
    ESP.restart();                           // the simulator ends here; the next run is the wake
#endif
    for (;;) delay(1000);
}

void offDrawScreen() {
    paintIdleEarly();
    char l1[40] = "SWITCHED OFF";
    if (g_offc.on) snprintf(l1, sizeof(l1), "OFF | CONTACT DAILY %02u:%02u", (unsigned)(g_offc.atMin / 60), (unsigned)(g_offc.atMin % 60));
    idleBanner(l1, "HOLD TOP BUTTON 2 S = ON");
    g_display.fullRefresh();
}

// Switch off now (the owner asked). Saves what is pending first.
[[noreturn]] void powerDown(const char* why, uint16_t lookMin = kOffCheckMin, bool resume = false) {
    Serial.printf("[POWER] off (%s): battery %.2f V %d%%; top button 2 s = on%c", why, g_bat.volts, g_bat.pct, 10);
    soakLog("OFF (%s) %.3fV %d%%", why, g_bat.volts, g_bat.pct);
    saveConfig();
    g_idMgr.flushPending();
    if (g_storeReady)
        for (uint32_t t0 = millis(); g_msgStore.writeQueue().drainCount() > 0 && millis() - t0 < 5000;) delay(20);
    readBattery();
    if (!resume) {                               // (after a contact window the first numbers stay)
        Preferences p;
        if (p.begin("rs_boot", false)) {
            p.putUChar("off", 1);
            p.putUShort("offMin", lookMin ? lookMin : kOffCheckMin);
            p.putUShort("offLooks", 0);
            p.putUShort("offLowMv", 0);
            p.putULong("offT", (uint32_t)time(nullptr));
            p.putUShort("offMv", g_bat.valid ? (uint16_t)(g_bat.volts * 1000.0f + 0.5f) : 0);
            p.putUShort("offMah", g_bat.valid ? g_bat.remMah : 0);
            p.end();
        }
    }
    offDrawScreen();
    Serial.println("[POWER] off: deep sleep");
    offSleep();
}

// Boot, after the battery check: true = the radio was switched off and stays off.
// A button wake that is held long enough clears the flag and writes the report.
bool offStaysOff() {
    offcLoad();
    if (bootNvsGetU8("off") != 1) return false;
    OffWake w = offWakeCause();
#if !defined(SIMULATOR)
    pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
    // After the restart button (or power coming back) the top button is watched for 3 s,
    // so "restart, then hold the top button" also switches on.
    if (w == OFF_WAKE_POWER)
        for (uint32_t t0 = millis(); millis() - t0 < 3000; delay(10))
            if (digitalRead(BOARD_BOOT_PIN) == LOW) { w = OFF_WAKE_BUTTON; break; }
#endif
    if (w == OFF_WAKE_BUTTON) {
        bool held = true;
#if !defined(SIMULATOR)
        for (uint32_t t0 = millis(); millis() - t0 < kOffHoldMs;) {
            if (digitalRead(BOARD_BOOT_PIN) != LOW) { held = false; break; }
            delay(10);
        }
#endif
        if (held) {
            Preferences p;
            uint32_t t = 0; uint16_t mv = 0, mah = 0, looks = 0, lowMv = 0;
            if (p.begin("rs_boot", false)) {
                t = p.getULong("offT", 0); mv = p.getUShort("offMv", 0); mah = p.getUShort("offMah", 0);
                looks = p.getUShort("offLooks", 0); lowMv = p.getUShort("offLowMv", 0);
                p.putUChar("off", 0);
                p.end();
            }
            const uint32_t now = (uint32_t)time(nullptr);
            char dur[20] = "off";
            if (t > 1600000000UL && now > t) snprintf(dur, sizeof(dur), "off %.1f h", (now - t) / 3600.0f);
            if (mv && g_bat.valid)
                snprintf(g_offReport, sizeof(g_offReport), "%s, %.2f -> %.2f V, %d mAh", dur, mv / 1000.0f,
                         g_bat.volts, (int)mah - (int)g_bat.remMah);
            else snprintf(g_offReport, sizeof(g_offReport), "%s", dur);
            Serial.printf("[POWER] on (top button): %s; %u battery looks, lowest %.2f V%c", g_offReport,
                          (unsigned)looks, lowMv / 1000.0f, 10);
            return false;
        }
        Serial.println("[POWER] off: button let go too soon, staying off");
        return true;
    }
    if (w == OFF_WAKE_TIMER) {
        Preferences p;
        if (p.begin("rs_boot", false)) {
            const uint16_t n = p.getUShort("offLooks", 0), low = p.getUShort("offLowMv", 0);
            const uint16_t mv = g_bat.valid ? (uint16_t)(g_bat.volts * 1000.0f + 0.5f) : 0;
            if (n < 0xFFFF) p.putUShort("offLooks", n + 1);
            if (mv && (!low || mv < low)) p.putUShort("offLowMv", mv);
            p.end();
        }
    }
    if (w == OFF_WAKE_TIMER && offcDue()) {
        g_contactWin = true;
        Serial.printf("[CONTACT] daily contact: starting for %u min (battery %.2f V)%c", (unsigned)g_offc.stayMin,
                      g_bat.volts, 10);
        return false;
    }
    Serial.printf("[POWER] off: %s, staying off (battery %.2f V%s)%c",
                  w == OFF_WAKE_TIMER ? "battery look" : "power or restart", g_bat.volts,
                  g_bat.usb ? ", charging" : "", 10);
    return true;
}

// The contact window after a daily-contact start: GPS on, the status goes to every contact
// with location auto-share (at the first fresh fix, or after 150 s with the last position),
// then off again when the time is up and nothing is waiting to be sent (2 min more at most).
void contactTick() {
    if (!g_contactWin || !g_protoReady) return;
    static uint32_t start = 0, input0 = 0;
    static bool sent = false;
    if (!start) {
        start = millis() | 1;
        input0 = g_lastInputMs;
        gpsRequestVisit("daily contact");
        toast("Daily contact: %u min, then off", (unsigned)g_offc.stayMin);
        return;
    }
    if (g_lastInputMs != input0) {               // somebody is using it: stay on
        g_contactWin = false;
        bootNvsSetU8("off", 0);
        Serial.println("[CONTACT] key pressed: staying on");
        toast("Staying on");
        return;
    }
    const uint32_t el = millis() - start;
    if (!sent && (gpsFresh() || el > 150000UL)) {
        sent = true;
        const uint32_t now = (uint32_t)time(nullptr);
        int n = 0;
        for (int i = 0; i < g_ashareCount; i++) {
            if (g_ashare[i].until && g_ashare[i].until < now) continue;
            shareMyStatus(g_ashare[i].dest, g_ashare[i].what ? g_ashare[i].what : (uint8_t)SH_POS);
            n++;
        }
        Serial.printf("[CONTACT] status sent to %d contact%s (%s)%c", n, n == 1 ? "" : "s",
                      gpsFresh() ? "fresh fix" : "no fresh fix", 10);
    }
    const uint32_t stay = (uint32_t)g_offc.stayMin * 60000UL;
    if (el < stay) return;
    if (g_proto.lxmfQueuedCount() > 0 && el < stay + 120000UL) return;
    Serial.printf("[CONTACT] window over after %lu s: switching off%c", (unsigned long)(el / 1000), 10);
    powerDown("daily contact done", kOffCheckMin, true);
}

// =============================================================================
// 7. Input — handlers only mutate state and request redraws
// =============================================================================

void noteInput() { g_lastInputMs = millis() ? millis() : 1; }

void openThread(const uint8_t peer[16]) {
    if (isSelf(peer)) { toast("Pick a remote peer"); return; }
    if (isGrpHash(peer) && (grpOfHash(peer) < 0 || !g_grp[grpOfHash(peer)].n)) { toast("Group has no members"); return; }
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
    if (g_screen == SCR_CONTACTS || g_screen == SCR_PEERS) loadRows(g_screen);   // the list on screen
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
    // Picking who gets a contact card: Contacts and Peers both offer people; anything else cancels.
    if (g_shareActive && (s == SCR_CONTACTS || s == SCR_PEERS)) {
        g_menuOpen = false;
        if (g_screen != s) { g_listScroll = 0; g_filter[0] = 0; }
        g_screen = s;
        loadRows(s);
        requestRedraw();
        Serial.printf("[UI] -> %s (still picking who gets the card)\n", screenName(s));
        return;
    }
    g_menuOpen = false;
    g_libOpen = false;
    g_gameOpen = false;
    g_chessOpen = false;
    g_cardOpen = false;
    g_gv = GV_NONE;
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
    while (g_menuSel < kMenuCount - 1 && !menuShown(g_menuSel)) g_menuSel++;
    for (int i = 0; i < kMenuMap; i++) if (kMenuScreens[i] == g_screen && menuShown(i)) g_menuSel = i;   // not Map / Library
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
    if (g_screen == SCR_INBOX || isGrpHash(g_rows[idx].peer)) openThread(g_rows[idx].peer);
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
            if (settingsRowCount() && g_sel < g_srowCount) activateSetting(g_srows[g_sel].num);
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
        int row = -1;                              // the shown row with that number (hidden: none)
        for (int i = 0; i < g_srowCount; i++) if (g_srows[i].num == idx) { row = i; break; }
        if (row < 0 || row >= n) return;           // no clamping to "nearest" row
        g_sel = row;
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
    if (g_mk != MK_NONE && !entryActive()) { markBack(); return; }
    if (g_layOpen) { layClose(); return; }
    if (g_gv != GV_NONE && !entryActive() && !g_threadOpen) { grpBack(); return; }
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
        if (!g_bufLen) {                         // Enter on an empty draft: group, map or save card
            if (grpOffered()) {
                grpThreadEnter();
            } else if (locOffered()) {
                const double la = g_threadLoc.lat, lo = g_threadLoc.lon;
                uint8_t peer[16];
                memcpy(peer, g_threadPeer, 16);
                closeThread();
                openMapAt(la, lo);
                g_mapSel = cposFind(peer);            // their marker selected (name + age below)
                for (int k = 0; k < g_markCount; k++)  // a shared marker there: select it instead
                    if (metersBetween(g_mark[k].lat, g_mark[k].lon, la, lo) < 15.0f) { g_markSel = k; g_mapSel = -1; }
                mapContactsFeed();
                Serial.printf("[LOC] show %.5f,%.5f on the map\n", la, lo);
            } else {
                saveThreadCard();
            }
            return;
        }
        char body[BUF_CAP + 1];
        memcpy(body, g_buf, g_bufLen + 1);
        if (isGrpHash(g_threadPeer)) {           // a group chat: one copy to every member
            int queued = 0;
            const bool held = sendHeld();
            if (groupSend(grpOfHash(g_threadPeer), body, &queued)) {
                bufClear();
                if (held) toast("Queued for the check-in");
                else if (g_cfg.beep) g_audio.playMessage();
            } else {
                toast("Not sent: %s", g_txStr);   // keep the draft for retry
            }
            loadHistory(g_threadPeer);
            requestRedraw();
            return;
        }
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
    case Entry::GroupName:
        grpNameCommit();
        return;
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
    case Entry::Note:
        noteCommit();
        return;
    case Entry::MarkDist:
    case Entry::MarkNote:
        markCommitEntry();
        return;
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
    case Entry::FoundText: {
        const int i = g_foundField < 0 || g_foundField > 9 ? 0 : g_foundField;
        strlcpy(g_cfg.found[i], g_buf, sizeof(g_cfg.found[i]));
        saveConfig();
        endEntry();
        toast(g_cfg.found[i][0] ? "%s saved" : "%s cleared", kFoundLabel[i]);
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
    case Entry::HaKey: {
        uint8_t k[32];
        if (!g_bufLen) { endEntry(); return; }
        if (!havoice::keyDecode(g_buf, k)) { toast("Key: 44 chars, ends with ="); return; }
        strlcpy(g_cfg.haKey, g_buf, sizeof(g_cfg.haKey));
        saveConfig();
        endEntry();
        toast("Key saved: use the same in HA");
        Serial.println("[HAV] key set (typed)");
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
                 g_entry == Entry::Search || g_entry == Entry::WptName || g_entry == Entry::MarkNote || g_entry == Entry::LibFind ||
                 g_entry == Entry::PlaceFind || g_entry == Entry::FoundText || g_entry == Entry::GroupName ||
                 g_entry == Entry::Note) c = ' ';
    } else if (ev.fn) {
        if (passEntry(g_entry)) { g_showPass = !g_showPass; requestRedraw(); return; }   // MIC = show / hide
        if (g_entry == Entry::Message && g_threadOpen) { g_tmenuOpen = true; g_tmenuGrpShare = false; g_tmenuSel = 0; requestRedraw(); return; }
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
    if (g_entry == Entry::GroupName && g_bufLen >= 16) { toast("Max 16 chars"); return; }
    if (g_entry == Entry::Name && isSelf(g_nameTarget)) {
        // Own name: short enough for the idle screen's second line at its widest,
        // "12/31/2026  <name>  999 NEW", so the name is never cut there.
        char probe[64];
        snprintf(probe, sizeof(probe), "12/31/2026  %.*s%c  999 NEW", (int)g_bufLen, g_buf, c);
        if (textW(probe) > W - 6) { toast("Max length (idle screen)"); return; }
    }
    if (g_entry == Entry::WifiPass && g_bufLen >= WIFI_PASS_MAX) { toast("Max 63 chars"); return; }
    if (passEntry(g_entry) && g_bufLen >= kLockPassMax) { toast("Max 32 chars"); return; }
    if (g_entry == Entry::Search && g_bufLen >= sizeof(g_filter) - 1) return;
    if ((g_entry == Entry::WptName || g_entry == Entry::FoundText || g_entry == Entry::CheckinTime || g_entry == Entry::MarkDist) &&
        g_entryReplace) {                                                                  // typing replaces the default
        bufClear(); g_entryReplace = false;
    }
    if (g_entry == Entry::WptName && g_bufLen >= 15) { toast("Max 15 chars"); return; }
    if (g_entry == Entry::MarkNote && g_bufLen >= 23) { toast("Max 23 chars"); return; }
    if (g_entry == Entry::MarkDist && g_bufLen >= 5) return;
    if (g_entry == Entry::TcpHost && g_bufLen >= sizeof(g_tcpCfg.host) - 1) { toast("Max 63 chars"); return; }
    if (g_entry == Entry::HaKey && g_bufLen >= havoice::kKeyB64Len) { toast("Key: 44 chars"); return; }
    if (g_entry == Entry::FoundText && g_bufLen >= sizeof(g_cfg.found[0]) - 1) { toast("Max 40 chars"); return; }
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
    if (c == 'T' || c == 'V') {                     // to the previous / next shown row
        for (int i = g_menuSel + (c == 'T' ? -1 : 1); i >= 0 && i < kMenuCount; i += c == 'T' ? -1 : 1)
            if (menuShown(i)) { g_menuSel = i; break; }
        requestRedraw();
        return;
    }
    for (int i = 0; i < kMenuCount; i++)
        if (c == kMenuKeys[i] && menuShown(i)) { menuGo(i); return; }
}

void browseKey(const KeyEvent& ev) {
    if (g_cardOpen) { cardKey(ev); return; }
    if (g_gv != GV_NONE) { grpKey(ev); return; }
    if (ev.enter) { activateCurrent(); return; }
    if (ev.del || ev.space) return;
    const char raw = ev.character;
    if (raw >= '0' && raw <= '9') { digitPressed(raw - '0'); return; }
    switch (toupper((unsigned char)raw)) {
        case 'T': moveSelection(-1); break;
        case 'V': moveSelection(+1); break;
        case 'M': openMenu(); break;
        // No global Home key: Home is M then H.
        // 'S' is intentionally absent: Settings is reached only via M -> S (guide §7).
        case 'C': goScreen(SCR_CONTACTS); break;
        case 'P': if (feature(F_PEERS)) goScreen(SCR_PEERS); break;
        case 'I': goScreen(SCR_INBOX); break;
        case 'A':                                   // Atlas; on a list row with a known position: there
            if (!feature(F_ATLAS)) break;
            if (isListScreen(g_screen) && g_listSel < g_rowCount && cposFind(g_rows[g_listSel].peer) >= 0 &&
                g_cpos[cposFind(g_rows[g_listSel].peer)].hasPos)
                mapShowContact(g_rows[g_listSel].peer);
            else openMap();
            break;
        case 'G':                                   // Contacts: the groups
            if (g_screen == SCR_CONTACTS && !g_shareActive) grpOpenList();
            break;
        case 'L':                                   // locate: type to filter the list
            if (isListScreen(g_screen)) { startEntry(Entry::Search, g_filter); }
            break;
        case 'N':                                   // Home: Announce ID; lists: next page
            if (g_screen == SCR_HOME) activateCurrent();
            else pageJump(+1);
            break;
        case 'B': pageJump(-1); break;
        case 'R':                                   // Peers: announce this radio (as Home's button)
            if (g_screen == SCR_PEERS) {
                doAnnounce();
                toast("Announce: %s", g_announceStr);
                Serial.printf("[PEERS] announce: %s\n", g_announceStr);
            }
            break;
        default: break;
    }
}

// Minimal mode: out of the idle screen (or the lock) straight to the Inbox. A message being
// typed (thread open) is left as it is.
void minimalToInbox() {
    if (!minimalMode() || g_threadOpen || entryActive()) return;
    if (g_menuOpen) closeMenu();
    if (mapview::isOpen()) closeMap();
    if (g_libOpen) libClose();
    if (g_gameOpen) gameClose();
    if (g_chessOpen) chessClose();
    if (g_screen != SCR_INBOX) goScreen(SCR_INBOX);
}

// ---- SOS (top side button held 3 s) ---------------------------------------------------
// Hold 3 s: a 5 s countdown (a buzz each second; any key or the button cancels), then
// "SOS! <name> needs help. <lat, lon> Bat N%" to the SOS target (Alerts page: all saved
// contacts, at most 8, or one group). A position update every 2 min and the full SOS every
// 10 min, until the button is held 3 s again ("SOS cancelled"). It survives a restart (NVS
// rs_sos). A received SOS sounds an alarm that ignores quiet hours and beep off, until a key.
constexpr const char* kSosPrefix = "SOS! ";
constexpr const char* kSosUpdPrefix = "SOS update: ";
constexpr const char* kSosEndPrefix = "SOS cancelled";
constexpr uint32_t kSosHoldMs = 3000, kSosCountMs = 5000, kSosUpdMs = 120000, kSosFullMs = 600000;
struct SosState { bool on; uint8_t to; uint32_t startedAt; bool btn; };   // to: 0 = saved contacts, n = group n
SosState g_sos = {false, 0, 0, true};      // btn: the 3 s hold starts an SOS (Alerts row 8)
uint32_t g_sosArmAt = 0, g_sosLastUpd = 0, g_sosLastFull = 0, g_sosResumeAt = 0;
uint8_t  g_sosArmSec = 0;
uint32_t g_sosRxUntil = 0, g_sosRxBeepAt = 0;
char     g_sosRxFrom[24] = "";
uint16_t g_sosSent = 0;

void sosSave() {
    Preferences p;
    if (!p.begin("rs_sos", false)) return;
    p.putBool("on", g_sos.on);
    p.putUChar("to", g_sos.to);
    p.putULong("at", g_sos.startedAt);
    p.putBool("btn", g_sos.btn);
    p.end();
}

void sosLoad() {
    Preferences p;
    if (p.begin("rs_sos", true)) {
        g_sos.on = p.getBool("on", false);
        g_sos.to = p.getUChar("to", 0);
        g_sos.startedAt = p.getULong("at", 0);
        g_sos.btn = p.getBool("btn", true);
        p.end();
    }
    if (g_sos.on) {
        g_sosResumeAt = millis() + 30000;    // let the radio and the store come up first
        Serial.println("[SOS] was active before the restart: resumes in 30 s");
    }
}

void sosTargetText(char* out, size_t n) {
    if (g_sos.to && g_sos.to <= g_grpCount) snprintf(out, n, "group %s", g_grp[g_sos.to - 1].name);
    else snprintf(out, n, "saved contacts (max 8)");
}

int sosTargets(uint8_t (*d)[16], int max) {
    int k = 0;
    if (g_sos.to && g_sos.to <= g_grpCount) {
        // Each member gets a plain message, not a group message: the group fields make it too
        // big for one LoRa packet, so the first one waited ~2.5 min for a link (real radios,
        // 2026-11-23); a plain SOS goes as a single packet.
        const Group& g = g_grp[g_sos.to - 1];
        for (int i = 0; i < g.n && k < max; i++)
            if (!isSelf(g.m[i])) memcpy(d[k++], g.m[i], 16);
        return k;
    }
    for (const auto& node : g_announceMgr.nodes()) {
        if (node.hash.size() != 16 || !node.saved || isSelf(node.hash.data())) continue;
        if (k >= max) break;
        memcpy(d[k++], node.hash.data(), 16);
    }
    return k;
}

uint32_t g_sosHoldUntil = 0;                 // after a cancel: LoRa stays on until it is out
bool sosActive() { return g_sos.on || g_sosHoldUntil; }
bool sosBtnOn() { return g_sos.btn; }

void sosToggleBtn() {                        // Alerts page row 8
    g_sos.btn = !g_sos.btn;
    sosSave();
    Serial.printf("[SOS] button hold %s\n", g_sos.btn ? "on" : "off");
}

void sosNextTarget() {                       // Alerts page: saved contacts, group 1, group 2, ...
    g_sos.to = (uint8_t)((g_sos.to + 1) % (g_grpCount + 1));
    sosSave();
    char t[40];
    sosTargetText(t, sizeof(t));
    Serial.printf("[SOS] sends to %s\n", t);
}

void sosPosText(char* out, size_t n) {
    if (g_gpsPosValid) snprintf(out, n, "%.5f, %.5f (%s)", g_gpsLat, g_gpsLon, gpsFresh() ? "GPS" : "last fix");
    else if (g_cfg.hasPos) snprintf(out, n, "%.5f, %.5f (saved position)", (double)g_cfg.lat, (double)g_cfg.lon);
    else snprintf(out, n, "position unknown");
}

// The outbox and an SOS (found on real radios: the SOS queued behind other messages and took
// minutes; updates still queued went out after "SOS cancelled"). Remove queued SOS texts
// (all, or only the updates), and move the SOS texts to the front so they go first.
int sosOutboxDrop(bool updatesOnly) {
    int k = 0, dropped = 0;
    for (int i = 0; i < g_outboxCount; i++) {
        const char* t = g_outbox[i].text;
        const bool upd = !strncmp(t, kSosUpdPrefix, strlen(kSosUpdPrefix));
        const bool full = !strncmp(t, kSosPrefix, strlen(kSosPrefix));
        if (upd || (!updatesOnly && full)) { dropped++; continue; }
        if (k != i) g_outbox[k] = g_outbox[i];
        k++;
    }
    g_outboxCount = k;
    if (dropped) { outboxSave(); Serial.printf("[SOS] %d queued SOS texts dropped\n", dropped); }
    return dropped;
}

void sosOutboxFront() {
    OutboxItem tmp[kOutboxCap];
    int n = 0;
    for (int i = 0; i < g_outboxCount; i++)
        if (!strncmp(g_outbox[i].text, "SOS", 3)) tmp[n++] = g_outbox[i];
    if (!n || n == g_outboxCount) return;
    for (int i = 0; i < g_outboxCount; i++)
        if (strncmp(g_outbox[i].text, "SOS", 3)) tmp[n++] = g_outbox[i];
    memcpy(g_outbox, tmp, sizeof(OutboxItem) * (size_t)g_outboxCount);
    outboxSave();
}

void sosSend(int kind) {                     // 0 = full SOS, 1 = position update, 2 = cancelled
    sosOutboxDrop(kind != 2);                // a newer position replaces a queued one; cancel drops all
    char pos[48], bs[16] = "", body[140], tgt[40];
    sosPosText(pos, sizeof(pos));
    if (g_bat.valid) snprintf(bs, sizeof(bs), " Bat %d%%", (int)g_bat.pct);
    if (kind == 0) snprintf(body, sizeof(body), "%s%s needs help. %s%s", kSosPrefix, g_selfName, pos, bs);
    else if (kind == 1) snprintf(body, sizeof(body), "%s%s%s", kSosUpdPrefix, pos, bs);
    else snprintf(body, sizeof(body), "%s: %s is OK now.", kSosEndPrefix, g_selfName);
    uint8_t d[8][16];
    const int n = sosTargets(d, 8);
    int ok = 0;
    for (int i = 0; i < n; i++)
        if (statusSend(d[i], body) != SendResult::Failed) ok++;
    sosOutboxFront();
    g_sosSent++;
    sosTargetText(tgt, sizeof(tgt));
    Serial.printf("[SOS] %s to %d of %d (%s): %s\n", kind == 0 ? "sent" : kind == 1 ? "update" : "cancel", ok, n, tgt, body);
    if (!n) toast("SOS: no contacts to send to");
}

void sosStart() {
    g_sos.on = true;
    g_sos.startedAt = (uint32_t)time(nullptr);
    sosSave();
    powerApply(true);                        // LoRa on now, also in Check-in mode
    g_sosLastUpd = g_sosLastFull = millis();
    sosSend(0);
    toast("SOS sent. Hold 3 s = stop");
    vibrate(400);
    requestRedraw();
}

void sosStop(const char* why) {
    if (!g_sos.on) return;
    sosSend(2);
    g_sos.on = false;
    g_sosResumeAt = 0;
    sosSave();
    g_sosHoldUntil = millis() + 120000;      // sosTick: back to the chosen mode once it is sent
    Serial.printf("[SOS] off (%s)\n", why);
    toast("SOS cancelled");
    requestRedraw();
}

void sosArmCancel(const char* why) {
    g_sosArmAt = 0;
    Serial.printf("[SOS] countdown cancelled (%s)\n", why);
    toast("SOS cancelled");
    requestRedraw();
}

// The top button held 3 s (or "btn hold"): start the countdown, or end an SOS.
void sosHold(const char* src) {
    if (g_sosArmAt) { sosArmCancel(src); return; }
    if (g_sos.on) { sosStop(src); return; }
    if (g_wizOpen) { Serial.println("[SOS] hold ignored (setup wizard)"); return; }
    g_sosArmAt = millis();
    g_sosArmSec = 0;
    Serial.printf("[SOS] countdown started (%s)\n", src);
    if (g_idleActive) { g_idleActive = false; g_fullRefreshReq = true; }   // show the countdown
    noteInput();                             // and keep it shown (no idle timeout right away)
    toast("SOS in 5 s, a key cancels");
    vibrate(80);
    requestRedraw();
}

void sosAlarmBeep() {                        // loud, whatever the beep / quiet hours settings
    const uint8_t vol = g_audio.volume();
    g_audio.setEnabled(true);
    g_audio.setVolume(100);
    g_audio.playMessage();
    g_audio.setVolume(vol);
    g_audio.setEnabled(g_cfg.beep);
    vibrate(200);
}

void sosTick() {
    const uint32_t now = millis();
    if (g_sosHoldUntil && !g_sos.on) {
        bool queued = false;
        for (int i = 0; i < g_outboxCount; i++) queued |= !strncmp(g_outbox[i].text, "SOS", 3);
        if (!queued || (int32_t)(now - g_sosHoldUntil) >= 0) {
            g_sosHoldUntil = 0;
            powerApply(true);                // back to the chosen energy mode
        }
    }
    if (g_sosArmAt) {
        const uint32_t el = now - g_sosArmAt;
        if (el >= kSosCountMs) { g_sosArmAt = 0; sosStart(); return; }   // "now" is older than the send
        else if (el / 1000 > g_sosArmSec) {
            g_sosArmSec = (uint8_t)(el / 1000);
            vibrate(60);
            toast("SOS in %u s, a key cancels", (unsigned)((kSosCountMs - el + 999) / 1000));
            requestRedraw();
        }
    }
    if (g_sosResumeAt && (int32_t)(now - g_sosResumeAt) >= 0) {
        g_sosResumeAt = 0;
        if (g_sos.on) { powerApply(true); g_sosLastUpd = g_sosLastFull = now; sosSend(0); toast("SOS still active"); }
    }
    if (g_sos.on && !g_sosResumeAt) {
        if (now - g_sosLastFull >= kSosFullMs) { g_sosLastFull = g_sosLastUpd = now; sosSend(0); }
        else if (now - g_sosLastUpd >= kSosUpdMs) { g_sosLastUpd = now; sosSend(1); }
    }
    if (g_sosRxUntil) {
        if ((int32_t)(now - g_sosRxUntil) >= 0) { g_sosRxUntil = 0; Serial.println("[SOS] alarm ended (timeout)"); }
        else if (!g_sosRxBeepAt || now - g_sosRxBeepAt >= 3000) { g_sosRxBeepAt = now ? now : 1; sosAlarmBeep(); }
    }
}

// A key while a countdown runs cancels it; a key while an alarm sounds silences it. Both
// consume the key.
bool sosKey() {
    if (g_sosArmAt) { sosArmCancel("key"); return true; }
    if (g_sosRxUntil) {
        g_sosRxUntil = 0;
        Serial.println("[SOS] alarm silenced (key)");
        toast("SOS from %.12s: see chat", g_sosRxFrom);
        requestRedraw();
        return true;
    }
    return false;
}

// An incoming message (inboundTick): an SOS starts the alarm and puts the sender on the map.
void sosIncoming(const uint8_t from[16], const char* text) {
    const bool full = !strncmp(text, kSosPrefix, strlen(kSosPrefix));
    const bool upd = !strncmp(text, kSosUpdPrefix, strlen(kSosUpdPrefix));
    const bool end = !strncmp(text, kSosEndPrefix, strlen(kSosEndPrefix));
    if (!full && !upd && !end) return;
    char nm[24];
    peerName(from, nm, sizeof(nm));
    if (end) {
        if (g_sosRxUntil && !strcmp(nm, g_sosRxFrom)) g_sosRxUntil = 0;
        Serial.printf("[SOS] %s: cancelled\n", nm);
        toast("%.14s: SOS cancelled", nm);
        requestRedraw();
        return;
    }
    const char* p = full ? strstr(text, "help. ") : text + strlen(kSosUpdPrefix);
    if (p && full) p += 6;
    double la = 0, lo = 0;
    if (p && sscanf(p, "%lf, %lf", &la, &lo) == 2 && la >= -90 && la <= 90 && lo >= -180 && lo <= 180 && (la || lo))
        cposNote(from, la, lo, (uint32_t)time(nullptr));
    Serial.printf("[SOS] %s from %s\n", full ? "ALARM" : "update", nm);
    if (full) {
        strlcpy(g_sosRxFrom, nm, sizeof(g_sosRxFrom));
        g_sosRxUntil = millis() + 60000;
        g_sosRxBeepAt = 0;
        if (g_idleActive) { g_idleActive = false; g_fullRefreshReq = true; }
        noteInput();                         // stay awake: no idle timeout right away
        toast("SOS from %.10s! Key = quiet", nm);
    } else {
        toast("SOS update from %.14s", nm);
    }
    requestRedraw();
}

void handleKey(const KeyEvent& ev) {
    if (sosKey()) { noteInput(); return; }
    if (g_idleActive) {                 // any key wakes; the key itself is consumed
        g_idleActive = false;
        noteInput();
        if (!g_locked) minimalToInbox();
        g_fullRefreshReq = true;
        requestRedraw();
        Serial.println(g_locked ? "[IDLE] wake (locked)" : "[IDLE] wake");
        return;
    }
    noteInput();
    if (g_locked) { lockKey(ev); return; }
    if (g_usbDrive) { usbDriveKey(ev); return; }
    if (g_wtOpen) { wtKey(ev); return; }
    if (g_vmOpen) { vmKey(ev); return; }
    if (g_threadOpen && g_tmenuOpen) { tmenuKey(ev); return; }
    if (g_wizOpen && !entryActive()) { wizKey(ev); return; }   // setup wizard (^ = step back)
    if (ev.up) { goBack(); return; }                          // lone Shift = ^
    if (g_menuOpen) { menuKey(ev); return; }
    if (entryActive()) { entryKey(ev); return; }
    if (g_libOpen) { libKey(ev); return; }
    if (g_gameOpen) { gameKey(ev); return; }
    if (g_chessOpen) { chessKey(ev); return; }
    if (ev.fn && g_cfg.haVoice && feature(F_HAVOICE) && havoice::connected()) {   // MIC = talk to Home Assistant
        // Tap to talk (as ESPHome voice devices): MIC starts listening, Home Assistant stops it
        // when you pause, MIC again stops it at once. (Holding MIC was cut short on COM9.)
        if (havoice::talking()) havoice::stopTalk("MIC pressed");
        else if (havoice::startTalk()) toast("HA: listening, speak now");
        return;
    }
    if (ev.fn) { g_fullRefreshReq = true; requestRedraw(); return; }   // MIC = full refresh
    if (g_mk != MK_NONE) { markKey(ev); return; }
    if (g_layOpen) { layKey(ev); return; }
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
    if (mc == 'C' || mc == 'I') {
        closeMap();
        goScreen(mc == 'C' ? SCR_CONTACTS : SCR_INBOX);
        return;
    }
    if (mc == 'A') return;
    if (mc == kKeyBack) { if (g_tbOn) tbStop("stopped"); else tbStart(); requestRedraw(); return; }
    if (mc == kKeyGo) { navOpen(); return; }
    if (mc == 'H' && ev.alt) { homeGo(); return; }        // Alt+H: the arrow to Home
    if (mc == kKeyFind) { startEntry(Entry::PlaceFind); return; }   // locate a place
    if (toupper((unsigned char)ev.character) == 'O' && !ev.alt) { layOpen(); return; }   // O: map layers
    if (markMapKey(ev)) return;                           // J marker list / Alt+J new marker
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
        const uint32_t t0 = millis();
        g_kb.update();
        // Stress test 2026-11-16: processKeys / watchBattery took exactly 1000 ms now and then
        // with no key pressed - an I2C wait. Say which read and the bus error when it happens.
        if (millis() - t0 > 100) g_i2cSlowCount++;
        if (millis() - t0 > 100)
            Serial.printf("[I2C] keyboard read %lu ms\n", (unsigned long)(millis() - t0));
        if (!g_kb.hasEvent()) break;
        uint8_t next = (uint8_t)((g_keyqHead + 1) % KEYQ_CAP);
        if (next == g_keyqTail) break;                  // queue full: leave rest in FIFO
        g_keyq[g_keyqHead] = g_kb.getEvent();
        g_keyqHead = next;
        g_useSeen = true;                               // a real key: someone is using the unit
    }
}

void processKeys() {
    readKeyboardIntoQueue();
    if (g_useSeen) { g_useSeen = false; standbyNoteUse("key"); }
    while (g_keyqTail != g_keyqHead) {
        KeyEvent ev = g_keyq[g_keyqTail];
        g_keyqTail = (uint8_t)((g_keyqTail + 1) % KEYQ_CAP);
        handleKey(ev);
    }
}

// ---- Top side button (BOOT, GPIO0, low while pressed) = lock ---------------------------
// A short press locks the unit and shows the idle screen in its locked form (lockNow, the
// same path as "Lock now" and the idle lock). With no password there is nothing to lock: it
// shows the idle screen. The bottom side button is the chip reset; a restart ends on the idle
// screen too (begin()), locked if a password is set. Holding this button during a reset is
// still the ESP32 USB download mode (a strap read by the ROM, not by this code).
constexpr uint32_t kBtnDebounceMs = 30;
bool g_btnRaw = false, g_btnDown = false;     // last reading / debounced state
uint32_t g_btnChangeAt = 0;
volatile bool g_btnPressed = false;           // a debounced short press waits for the loop
uint32_t g_btnDownAt = 0;
bool g_btnHoldDone = false;                   // this press was a 3 s hold (SOS): no lock on release

// Read the pin (loop and panel busy-wait); never draws.
void sideButtonPoll() {
    const bool raw = digitalRead(BOARD_BOOT_PIN) == LOW;
    const uint32_t now = millis();
    if (raw != g_btnRaw) { g_btnRaw = raw; g_btnChangeAt = now; return; }
    if (raw == g_btnDown || now - g_btnChangeAt < kBtnDebounceMs) return;
    g_btnDown = raw;
    if (raw) { g_btnDownAt = now; g_btnHoldDone = false; }
    else if (!g_btnHoldDone) g_btnPressed = true;   // a short press acts on release
}

void sideButtonAction(const char* src) {
    if (g_wizOpen || g_usbDrive) {            // first-run wizard / USB drive mode stay on screen
        Serial.printf("[BTN] top button (%s): ignored (%s)\n", src, g_wizOpen ? "setup wizard" : "USB drive mode");
        return;
    }
    if (g_vmOpen) vmClose();                  // a voice recording stops, nothing is sent
    if (g_wtOpen) wtClose();                  // a walkie-talkie call is hung up
    if (lockHasPassword()) {
        // Locked = not "use": a unit in standby stays in standby.
        if (!g_locked) {
            Serial.printf("[BTN] top button (%s): lock\n", src);
            lockNow();
        } else if (!g_idleActive) {           // at the password prompt: back to the idle screen
            Serial.printf("[BTN] top button (%s): locked already, idle screen\n", src);
            lockWipe();
            enterIdle();
        } else {
            Serial.printf("[BTN] top button (%s): locked already\n", src);
        }
        return;
    }
    Serial.printf("[BTN] top button (%s): idle screen (no password)\n", src);
    noteInput();
    standbyNoteUse("button");                 // unlocked: someone is using it
    if (!g_idleActive) enterIdle();
}

void sideButtonTick() {
    sideButtonPoll();
    if (g_btnDown && !g_btnHoldDone && (g_sos.btn || g_sos.on || g_sosArmAt) && millis() - g_btnDownAt >= kSosHoldMs) {
        g_btnHoldDone = true;
        sosHold("button");
    }
    sosTick();
    if (!g_btnPressed) return;
    g_btnPressed = false;
    if (g_sosArmAt) { sosArmCancel("button"); return; }
    sideButtonAction("button");
}

// Runs while the panel is busy refreshing. Must only read input, never draw.
void onPanelBusy(const void*) {
    static uint32_t last = 0;
    if (millis() - last >= 8) {
        last = millis();
        sideButtonPoll();
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
    if (g_idleActive || g_locked || g_cfg.touchOff) return;   // key-only wake; none while locked or off
    noteInput();
    standbyNoteUse("touch");
    const Hit* hit = nullptr;
    for (int i = g_hitCount - 1; i >= 0; i--) {      // last drawn = topmost
        const Hit& h = g_hits[i];
        if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) { hit = &h; break; }
    }
    if (g_menuOpen && (!hit || (hit->act != H_MENU_GO && hit->act != H_MENU))) { closeMenu(); return; }
    if (!hit) return;
    // Typing (a message, a name, If-found text ...): a thumb brushing the bottom tabs, just
    // above the keyboard, jumped to Contacts / Inbox and lost the text. While typing, touch
    // cannot leave the screen (tabs, menu); other touches on settings wait until Enter / ^.
    if (entryActive() && (hit->act == H_TAB || hit->act == H_MENU || hit->act == H_BACK ||
                          g_entry != Entry::Message)) {
        Serial.printf("[TOUCH] %d,%d act=%u ignored while typing\n", x, y, hit->act);
        return;
    }
    Serial.printf("[TOUCH] %d,%d act=%u arg=%d\n", x, y, hit->act, hit->arg);
    switch (hit->act) {
        case H_REFRESH:    g_fullRefreshReq = true; requestRedraw(); break;
        case H_MENU:       if (g_menuOpen) closeMenu(); else openMenu(); break;
        case H_MENU_GO:    if (hit->arg >= 0 && hit->arg < kMenuCount && menuShown(hit->arg)) menuGo(hit->arg); break;
        case H_TAB:        goScreen((ScreenId)hit->arg); break;
        case H_BACK:       goBack(); break;
        case H_ANNOUNCE:   activateCurrent(); break;
        case H_UP:         moveSelection(-1); break;
        case H_DOWN:       moveSelection(+1); break;
        case H_CARD:       if (g_cardOpen) { g_cardSel = hit->arg; cardAct(hit->arg); } break;
        case H_MARK:                                  // marker pages: as the keys
            if (g_mk == MK_NONE) break;
            if (hit->arg == g_mkSel || g_mk == MK_ONE || g_mk == MK_NEW) markActivate(hit->arg);
            else { g_mkSel = hit->arg; requestRedraw(); }
            break;
        case H_GRP:                                   // groups: tap selects, a second tap (or the page's rows) acts
            if (g_gv == GV_NONE) break;
            if (hit->arg == g_gvSel || g_gv == GV_PAGE) grpActivate(hit->arg);
            else { g_gvSel = hit->arg; requestRedraw(); }
            break;
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
                activateSetting(hit->arg < g_srowCount ? g_srows[hit->arg].num : hit->arg);
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
    // Standby: the chosen mode's profile with GPS / WiFi / key light off, also on USB power.
    if (g_standby) want = power::standbyProfile(power::selected(g_pw));
    if (!want.lora && want.loraOnUse && (millis() < power::kLoraOnUseMs ||
                                         (g_lastInputMs && millis() - g_lastInputMs < power::kLoraOnUseMs)))
        want.lora = true;                        // on demand: LoRa on for 2 min after a key
    if (want.checkin && !want.lora && checkinNow().inWindow) want.lora = true;   // check-in window
    if (sosActive()) { want.lora = true; want.checkin = false; }   // an SOS: LoRa on, no waiting for a window
    const bool onUsb = usb && g_pw.fullOnUsb && g_pw.mode != power::MODE_FULL && !g_standby;
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
                  power::modeName(g_pw.mode), g_standby ? " (standby)" : g_pwOnUsb ? " (USB -> Full)" : "",
                  (unsigned)g_pwEff.cpuMhz, w,
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
        // No fix in the last 2 h (cold receiver: it must download orbit data first, which takes
        // minutes) or the Atlas open: stay on up to 12 min while satellites are heard, not 3.
        const bool cold = !g_gpsPosValid || now - g_gpsPosAt > 2UL * 3600UL * 1000UL;
        // Only where the map may wake the GPS (Full, Balanced): the saving modes keep 3 min.
        const bool mapOn = mapview::isOpen() && g_pwEff.mapGps;
        const uint32_t window = (cold || mapOn) && g_pwEff.mapGps && heard > 0 ? 12UL * 60000UL : power::kGpsWindowMs;
        const bool done = (fixSince && now - fixSince >= (mapOn ? 600000UL : 20000UL)) ||
                          now - g_gpsVisitStart >= window || hopeless;
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
    const bool isGrp = o.gseq || memcmp(o.gid, "\0\0\0", 4);
    const bool sent = sendMessage(o.dest, o.text, isGrp ? o.gid : nullptr, o.gseq);
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

// ---- Standby when idle (S.5.11 / S.5.12, console "standby") ---------------------------
// With the option on, standbyHours without a real key or touch puts the standby profile in
// force (power::standbyProfile: GPS, WiFi, key light off; LoRa still listens) and shows the
// idle screen. It stays until a real key or touch: incoming messages, console commands and
// console keys ("k", the soak test) do not end it. Off = never entered.
uint32_t standbyTimeoutMs() {
    return g_standbyTestS ? g_standbyTestS * 1000UL : (uint32_t)g_pw.standbyHours * 3600000UL;
}

void standbyPrint() {
    const uint32_t idleS = g_lastUseMs ? (millis() - g_lastUseMs) / 1000UL : 0;
    const uint32_t limS = standbyTimeoutMs() / 1000UL;
    char in[24] = "-";
    if (g_pw.standby && !g_standby) snprintf(in, sizeof(in), "%lu s", (unsigned long)(idleS < limS ? limS - idleS : 0));
    Serial.printf("[STANDBY] option=%s after=%u h%s state=%s idle=%lu s enters_in=%s gps=%s wifi=%s\n",
                  g_pw.standby ? "on" : "off", (unsigned)g_pw.standbyHours, g_standbyTestS ? " (test timeout)" : "",
                  g_standby ? "standby" : "normal", (unsigned long)idleS, in,
                  g_gpsSerial ? "powered" : "off", g_pwEff.wifi ? "allowed" : "off");
}

void standbyEnter(const char* why) {
    if (g_standby) return;
    g_standby = true;
    g_standbySince = millis();
    Serial.printf("[STANDBY] enter (%s): GPS off, WiFi off, key light off, LoRa %s\n", why,
                  power::standbyProfile(power::selected(g_pw)).lora ? "listening" : "as the mode has it");
    powerApply(true);
    powerGpsTick();                              // receiver off now, not at the next tick
    if (!g_idleActive) {
        if (lockOnIdle() && !g_locked) lockNow(); else enterIdle();
    } else {                                     // already on the idle screen: show the standby line
        paintIdle();
        g_display.refreshIfDirty();
    }
}

void standbyLeave(const char* why) {
    if (!g_standby) return;
    g_standby = false;
    Serial.printf("[STANDBY] leave (%s) after %lu s: normal mode\n", why,
                  (unsigned long)((millis() - g_standbySince) / 1000UL));
    powerApply(true);                            // GPS visit now, WiFi auto-join, key light
    powerGpsTick();
    // A key is waiting: it wakes the idle screen with its own full refresh, no repaint here.
    if (g_idleActive && g_keyqTail == g_keyqHead) { paintIdle(); g_display.refreshIfDirty(); }
    else if (!g_idleActive) requestRedraw();
}

// A real key or touch (not the console).
void standbyNoteUse(const char* why) {
    g_lastUseMs = millis() ? millis() : 1;
    if (g_standby) standbyLeave(why);
}

void standbyTick() {
    if (!g_lastUseMs) g_lastUseMs = millis() ? millis() : 1;
    if (!g_pw.standby || g_standby) return;
    if (millis() - g_lastUseMs < standbyTimeoutMs()) return;
    // Not in the middle of something that holds the audio path or the USB drive.
    if (g_usbDrive || g_wtOpen || g_vmOpen || walkie::state() != walkie::State::Idle || havoice::busy()) return;
    standbyEnter("idle");
}

void powerTick() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    standbyTick();
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
                 power::modeName(g_pw.mode), g_standby ? " (STANDBY)" : g_pwOnUsb ? " (USB -> Full)" : "",
                 tot ? (unsigned)((uint64_t)g_pwSleepMs * 100 / tot) : 0u, (unsigned long)g_pwSleeps,
                 (unsigned)g_pwEff.cpuMhz, loraOnline() ? "on" : "off");
        ::sim::setPowerInfo(line);
    }
#endif
    checkinTick();
    usbDriveTick();
    chessTick();
    txLateTick();
    markReadTick();
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
    const bool want = !g_cfg.touchOff && !g_idleActive && (!g_pwEff.touchOnKey || millis() - g_lastInputMs < 60000UL);
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
    if (havoice::busy()) return "HA voice";
    if (walkie::state() != walkie::State::Idle || g_wtOpen) return "walkie-talkie";
    if (g_vmOpen) return "voice message";
    if (g_cfg.haVoice && havoice::connected()) return "HA connected";   // must answer HA
    if (g_cThinking) return "chess engine";
    // While the idle splash shows, loop() does not render, so a pending redraw waits for the wake
    // key anyway and must not keep the CPU awake.
    if (!g_idleActive && (g_redraw || g_redrawAt)) return "redraw";
    if (g_keyqTail != g_keyqHead) return "keys";
    if (g_btnRaw || g_btnDown || g_btnPressed) return "side button";
    if (g_pwEff.checkin && g_outboxCount) {
        const power::CheckinPhase ph = checkinNow();
        if (ph.inWindow && ph.sinceS >= (int32_t)power::kCheckinSendS) return "check-in send";
    }
    if (HWCDC::isPlugged() && !g_usbSleepUntil) return "usb host";   // console drops while asleep
    if (mirror::active()) return "mirror";
    // WiFi keeps the CPU awake only while its radio is on: joining, joined or scanning. After a
    // failed join (wrong password, the network gone or out of reach) the state stays Failed until
    // the next try 3 min later, with the radio off: that must not keep it awake (2026-11-24:
    // leaving the home WiFi kept Balanced at ~70 mA, no sleep, until a join worked again).
    if (g_wifiSt == WifiSt::Connecting || g_wifiSt == WifiSt::Connected || g_wifiScanning ||
        WiFi.getMode() != WIFI_OFF) return "wifi";
    // The SDK's Bluetooth controller has no modem sleep (CONFIG_BT_CTRL_MODEM_SLEEP is off in
    // the Arduino build): light sleep would drop the phone.
    if (cotble::active()) return "bluetooth";
    if (g_gpsSerial) {
        if (!gpsSleepsWithCpu()) return "gps";
        // The UART is off in light sleep: about every 30 s stay awake until one RMC sentence
        // (position + time) is in (at most 2.5 s), so the position is never > ~30 s old.
        static uint32_t winAt = 0, lastRead = 0;
        if (!winAt && (!lastRead || now - lastRead >= 30000)) winAt = now | 1;
        if (winAt) {
            if ((g_gpsRmcAt && (int32_t)(g_gpsRmcAt - winAt) > 0) || now - winAt > 2500) {
                winAt = 0;
                lastRead = now;
            } else {
                return "gps read";
            }
        }
    }
    // Outgoing messages: awake only while one is being worked on or its next step is due within
    // 1.5 s. A message that only waits (a retry or path request 1-10 s later, a delivery proof
    // up to ~28 s) let the radio sleep in 1 s steps (powerIdle); a packet wakes it over LoRa.
    // Before 2026-11-24 any queued message kept the CPU awake: a message to someone out of
    // reach, or a status report every 5 min, held Balanced and Eco at 50-90 mA (battery run 2).
    if (g_protoReady && g_proto.lxmfQueuedCount() > 0 && g_proto.lxmfMsUntilDue() < 1500) return "lxmf queue";
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
// ---- Hardware self-test ------------------------------------------------------------
// Settings > Diagnostics > Check this unit (console "selftest"): checks every part it can
// without a person and says PASS / WARN / FAIL per line, so a broken unit shows at once
// (e.g. a GPS that sends nothing or has the backup-cell fault, an I2C bus that stalls,
// a radio that does not answer). Everything runs at once except the GPS, which is watched
// for 20 s (powered for the test if the power plan had it resting). The report goes to
// <SD>/ratdeckpro/selftest/<name>-<date>.txt (no card: /selftest.txt in flash) and to the
// console. Not covered yet (need a person): screen patterns, every key, audio, touch.
enum : uint8_t { ST_NONE, ST_PASS, ST_WARN, ST_FAIL };
enum : uint8_t { STI_RAM, STI_PSRAM, STI_FLASH, STI_NVS, STI_SD, STI_I2C, STI_BAT, STI_CHG, STI_LORA, STI_GPS,
                 STI_CLOCK, STI_N };
struct StResult { uint8_t v; char detail[72]; };
struct {
    bool running = false, ran = false, console = false, gpsForced = false;
    uint32_t startMs = 0, gpsBytes0 = 0, gpsGood0 = 0, gpsBad0 = 0;
    char when[20] = "";
    StResult r[STI_N] = {};
} g_st;
const char* const kStName[STI_N] = {"RAM", "PSRAM", "Flash", "Settings store", "SD card", "I2C bus",
                                     "Battery gauge", "Charger", "LoRa radio", "GPS", "Clock"};
const char* const kStVerdict[] = {"...", "PASS", "WARN", "FAIL"};
constexpr uint32_t kStGpsMs = 20000;

void stSet(int i, uint8_t v, const char* fmt, ...) {
    g_st.r[i].v = v;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_st.r[i].detail, sizeof(g_st.r[i].detail), fmt, ap);
    va_end(ap);
    if (g_st.console) Serial.printf("[TEST] %-14s %s  %s\n", kStName[i], kStVerdict[v], g_st.r[i].detail);
}

// Pattern test of a block: address-mixed words, then their inverse. Returns bad words.
uint32_t stPattern(uint32_t* p, size_t words) {
    uint32_t bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t inv = pass ? 0xFFFFFFFFu : 0;
        for (size_t i = 0; i < words; i++) p[i] = ((uint32_t)i * 2654435761u ^ 0xA5A5A5A5u) ^ inv;
        for (size_t i = 0; i < words; i++)
            if (p[i] != (((uint32_t)i * 2654435761u ^ 0xA5A5A5A5u) ^ inv)) bad++;
    }
    return bad;
}

void stRam() {
    // A busy unit (messages, the stress test) normally has 20-40 KB internal RAM free in blocks of
    // 10-30 KB, so test what is there (up to 16 KB) instead of asking for one big block. Measured
    // before the test block is taken.
    const unsigned freeK = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    const unsigned minK = (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    const size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const unsigned blkK = (unsigned)(blk / 1024);
    size_t n = blk > 1024 ? blk - 1024 : 0;              // leave a little for everyone else
    if (n > 16 * 1024) n = 16 * 1024;
    n &= ~(size_t)3;
    uint32_t* p = n >= 8 * 1024 ? (uint32_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
    if (!p) { stSet(STI_RAM, ST_FAIL, "no 8 KB block free (free %u KB, block %u KB)", freeK, blkK); return; }
    const uint32_t bad = stPattern(p, n / 4);
    heap_caps_free(p);
    if (bad) stSet(STI_RAM, ST_FAIL, "%lu bad words in %u KB", (unsigned long)bad, (unsigned)(n / 1024));
    else stSet(STI_RAM, freeK < 20 || blkK < 10 ? ST_WARN : ST_PASS, "%u KB ok; free %u KB, lowest %u, block %u",
               (unsigned)(n / 1024), freeK, minK, blkK);
}

void stPsram() {
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (!total) { stSet(STI_PSRAM, ST_FAIL, "not found"); return; }
    size_t n = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 2;
    if (n > 2 * 1024 * 1024) n = 2 * 1024 * 1024;
    n &= ~(size_t)3;
    uint32_t* p = n ? (uint32_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : nullptr;
    if (!p) { stSet(STI_PSRAM, ST_FAIL, "no free block"); return; }
    const uint32_t t0 = millis();
    const uint32_t bad = stPattern(p, n / 4);
    const uint32_t ms = millis() - t0;
    heap_caps_free(p);
    if (bad) stSet(STI_PSRAM, ST_FAIL, "%lu bad words in %u KB", (unsigned long)bad, (unsigned)(n / 1024));
    else stSet(STI_PSRAM, ST_PASS, "%u KB tested in %lu ms, %u KB free", (unsigned)(n / 1024), (unsigned long)ms,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void stFlash() {
    static uint8_t buf[2048];
    const char* path = "/selftest.tmp";
    File w = LittleFS.open(path, FILE_WRITE);
    if (!w) { stSet(STI_FLASH, ST_FAIL, "cannot create a file"); return; }
    bool ok = true;
    for (int k = 0; k < 4 && ok; k++) {
        for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 7 + k * 31);
        ok = w.write(buf, sizeof(buf)) == sizeof(buf);
    }
    w.close();
    File r = ok ? LittleFS.open(path, FILE_READ) : File();
    const bool opened = (bool)r;                  // remembered: a closed File reads as false
    uint32_t bad = 0;
    for (int k = 0; k < 4 && r; k++) {
        if (r.read(buf, sizeof(buf)) != sizeof(buf)) { bad++; break; }
        for (size_t i = 0; i < sizeof(buf); i++) if (buf[i] != (uint8_t)(i * 7 + k * 31)) bad++;
    }
    if (r) r.close();
    LittleFS.remove(path);
    const unsigned freeK = (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);
    if (!ok || !opened) stSet(STI_FLASH, ST_FAIL, "%s failed, %u KB free", ok ? "read back" : "write", freeK);
    else if (bad) stSet(STI_FLASH, ST_FAIL, "%lu bytes differ on read back", (unsigned long)bad);
    else stSet(STI_FLASH, freeK < 100 ? ST_WARN : ST_PASS, "8 KB write / read ok, %u KB free", freeK);
}

void stNvs() {
    Preferences p;
    if (!p.begin("rs_selftest", false)) { stSet(STI_NVS, ST_FAIL, "cannot open"); return; }
    const uint32_t n = p.getUInt("runs", 0) + 1;
    p.putUInt("runs", n);
    const uint32_t back = p.getUInt("runs", 0);
    p.end();
    if (back != n) stSet(STI_NVS, ST_FAIL, "wrote %lu, read %lu", (unsigned long)n, (unsigned long)back);
    else stSet(STI_NVS, ST_PASS, "write / read ok (test run %lu)", (unsigned long)n);
}

void stSd() {
    if (!(g_sdMounted || sdMount())) { stSet(STI_SD, ST_WARN, "no card (or it does not mount)"); return; }
    static uint8_t buf[4096];
    const char* path = SD_PATH_ROOT "/selftest.tmp";
    const int chunks = 64;                       // 256 KB
    uint32_t t0 = millis();
    File w = g_sdStore.openFile(path, FILE_WRITE);
    bool ok = (bool)w;
    for (int k = 0; k < chunks && ok; k++) {
        for (size_t i = 0; i < sizeof(buf); i += 4) *(uint32_t*)(buf + i) = (uint32_t)(k * 4096 + i) * 2654435761u;
        ok = w.write(buf, sizeof(buf)) == sizeof(buf);
    }
    if (w) w.close();
    const uint32_t wMs = millis() - t0 ? millis() - t0 : 1;
    uint32_t bad = 0;
    t0 = millis();
    File r = ok ? g_sdStore.openFile(path, FILE_READ) : File();
    for (int k = 0; k < chunks && r; k++) {
        if (r.read(buf, sizeof(buf)) != sizeof(buf)) { bad++; break; }
        for (size_t i = 0; i < sizeof(buf); i += 4)
            if (*(uint32_t*)(buf + i) != (uint32_t)(k * 4096 + i) * 2654435761u) bad++;
    }
    const uint32_t rMs = millis() - t0 ? millis() - t0 : 1;
    const bool rOk = (bool)r;
    if (r) r.close();
    g_sdStore.remove(path);
    const unsigned freeM = (unsigned)((g_sdStore.totalBytes() - g_sdStore.usedBytes()) >> 20);
    if (!ok || !rOk) stSet(STI_SD, ST_FAIL, "write / read back failed");
    else if (bad) stSet(STI_SD, ST_FAIL, "%lu words differ on read back", (unsigned long)bad);
    else stSet(STI_SD, freeM < 50 ? ST_WARN : ST_PASS, "write %lu KB/s read %lu KB/s, %u MB free",
               (unsigned long)(256000UL / wMs), (unsigned long)(256000UL / rMs), freeM);
}

bool stProbe(uint8_t addr, uint32_t& ms) {
    const uint32_t t0 = millis();
    Wire.beginTransmission(addr);
    const bool ok = Wire.endTransmission() == 0;
    ms = millis() - t0;
    return ok;
}

void stI2c() {
    uint32_t ms = 0, worst = 0;
    const bool kb = stProbe(KB_I2C_ADDR, ms);    if (ms > worst) worst = ms;
    const bool ga = stProbe(BQ27220_I2C_ADDR, ms); if (ms > worst) worst = ms;
    const bool ch = stProbe(BQ25896_I2C_ADDR, ms); if (ms > worst) worst = ms;
    const bool tp = stProbe(TOUCH_I2C_ADDR, ms);   if (ms > worst) worst = ms;
    // 30 gauge reads in a row: any slow one is the bus stalling (COM5's 1 s waits).
    uint32_t slow = 0;
    for (int i = 0; i < 30; i++) {
        uint16_t v = 0;
        const uint32_t t0 = millis();
        bqReadU16(0x08, v);
        const uint32_t d = millis() - t0;
        if (d > worst) worst = d;
        if (d > 50) slow++;
    }
    const uint8_t v = (!kb || !ga || !ch) ? ST_FAIL : (slow || worst > 50 || !tp) ? ST_WARN : ST_PASS;
    stSet(STI_I2C, v, "kbd %s gauge %s chg %s touch %s, max %lu ms, %lu slow", kb ? "ok" : "MISSING",
          ga ? "ok" : "MISSING", ch ? "ok" : "MISSING", tp ? "ok" : "asleep?", (unsigned long)worst, (unsigned long)slow);
}

void stBattery() {
    readBattery();
    if (!g_bat.valid) { stSet(STI_BAT, ST_FAIL, "gauge gives no voltage"); return; }
    uint16_t t = 0;
    const bool tOk = bqReadU16(0x06, t);
    const int c = tOk ? (int)t / 10 - 273 : -99;
    uint8_t v = ST_PASS;
    if (g_bat.volts < 3.0f || g_bat.volts > 4.45f || g_bat.rawSoc < 0) v = ST_FAIL;
    else if (!tOk || c < 0 || c > 45) v = ST_WARN;
    // 3000 mAh is the gauge chip's factory value: it has not learned this 1500 mAh battery, so
    // its own % is off (the screen % is corrected; a full charge / run-down cycle teaches it)
    const bool unlearned = g_bat.fccMah >= 2500;
    if (v == ST_PASS && unlearned) v = ST_WARN;
    stSet(STI_BAT, v, "%.2fV %d%% (gauge %d%%) %dC %dmA fcc %u%s", g_bat.volts, g_bat.pct, g_bat.rawSoc, c,
          (int)g_bat.avgMa, (unsigned)g_bat.fccMah, unlearned ? " (gauge not learned: full charge + run down)" : "");
}

void stCharger() {
    uint8_t chrg = 0;
    bool pg = false;
    if (!chargerReadStatus(chrg, pg)) { stSet(STI_CHG, ST_FAIL, "does not answer"); return; }
    static const char* const kChrg[] = {"not charging", "pre-charge", "charging", "charge done"};
    // On USB but not charging while not full: cable / port that does not charge, or a fault.
    const bool stuck = pg && chrg == 0 && g_bat.pct < 95;
    stSet(STI_CHG, stuck ? ST_WARN : ST_PASS, "%s, %s%s", pg ? "USB power" : "no USB power", kChrg[chrg & 3],
          stuck ? " (port / cable?)" : "");
}

void stLora() {
    if (!g_radio || !loraOnline()) { stSet(STI_LORA, ST_FAIL, "radio does not answer"); return; }
    // Noise floor: the channel with nobody sending. High = interference nearby (or a
    // transmission in progress: run again).
    long sum = 0;
    int n = 0, top = -200;
    for (int i = 0; i < 10; i++) {
        const int r = g_radio->currentRssi();
        if (r < 0 && r > -160) { sum += r; n++; if (r > top) top = r; }
        delay(15);
    }
    if (!n) { stSet(STI_LORA, ST_WARN, "online, no RSSI reading, %d dBm TX", loraTxPower()); return; }
    const int floor = (int)(sum / n);
    stSet(STI_LORA, floor > -85 ? ST_WARN : ST_PASS, "online, noise %d dBm (top %d), TX %d dBm", floor, top,
          loraTxPower());
}

void stGpsBegin() {
#if defined(HAS_GPS) && HAS_GPS
    g_st.gpsForced = false;
    if (g_cfg.gpsOn && !g_gpsSerial) {          // resting in the power plan: power it for the test
        g_gpsAllowed = true;
        gpsApplyPower();
        g_st.gpsForced = true;
    }
    g_st.gpsBytes0 = g_gpsBytes;
    g_st.gpsGood0 = g_gpsGood;
    g_st.gpsBad0 = g_gpsBad;
#endif
}

void stGpsEnd() {
#if defined(HAS_GPS) && HAS_GPS
    if (!g_cfg.gpsOn) { stSet(STI_GPS, ST_WARN, "off in Settings: turn it on to test it"); return; }
    const uint32_t bytes = g_gpsBytes - g_st.gpsBytes0, good = g_gpsGood - g_st.gpsGood0, bad = g_gpsBad - g_st.gpsBad0;
    if (g_gpsBbrFault)
        stSet(STI_GPS, ST_FAIL, "backup cell fault (CANNOT USE BBR), %lu sentences", (unsigned long)good);
    else if (!bytes)
        stSet(STI_GPS, ST_FAIL, "silent: no data in %lu s", (unsigned long)(kStGpsMs / 1000));
    else if (!good)
        stSet(STI_GPS, ST_FAIL, "%lu bytes but no good sentence (%lu bad)", (unsigned long)bytes, (unsigned long)bad);
    else if (!g_gpsFix)
        stSet(STI_GPS, ST_WARN, "talking (%lu ok, %lu bad), %u sats, no fix yet (indoors?)", (unsigned long)good,
              (unsigned long)bad, (unsigned)g_gpsSats);
    else if (g_gpsSats < 3)                       // a fix flag with no satellites in view is not a real fix
        stSet(STI_GPS, ST_WARN, "says fix but %u sats (HDOP %.1f), %lu ok %lu bad: check outdoors",
              (unsigned)g_gpsSats, g_gpsHdop, (unsigned long)good, (unsigned long)bad);
    else
        stSet(STI_GPS, ST_PASS, "fix, %u sats, HDOP %.1f, %lu ok %lu bad", (unsigned)g_gpsSats, g_gpsHdop,
              (unsigned long)good, (unsigned long)bad);
    if (g_st.gpsForced) {                         // back to the power plan
        g_st.gpsForced = false;
        powerApply(true);
    }
#else
    stSet(STI_GPS, ST_WARN, "no GPS in this build");
#endif
}

void stClock() {
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (now < 1735689600) { stSet(STI_CLOCK, ST_WARN, "not set (no GPS time or WiFi yet)"); return; }
    stSet(STI_CLOCK, ST_PASS, "%04d-%02d-%02d %02d:%02d%s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
          t.tm_min, g_gpsSyncedOnce ? ", set from GPS" : "");
}

void selftestStart(bool console) {
    if (g_st.running) { if (console) Serial.println("[TEST] already running"); return; }
    for (auto& r : g_st.r) { r.v = ST_NONE; r.detail[0] = 0; }
    g_st.console = console;
    g_st.running = true;
    g_st.startMs = millis();
    if (console) Serial.printf("[TEST] self-test %s, GPS watched for %lu s\n", RATSPEAK_FW_VERSION,
                               (unsigned long)(kStGpsMs / 1000));
    stGpsBegin();
    stRam();
    stPsram();
    stFlash();
    stNvs();
    stSd();
    stI2c();
    stBattery();
    stCharger();
    stLora();
    stClock();
    requestRedraw();
}

void selftestFinish() {
    stGpsEnd();
    g_st.running = false;
    g_st.ran = true;
    int cnt[4] = {0, 0, 0, 0};
    for (auto& r : g_st.r) cnt[r.v & 3]++;
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (now > 1735689600) snprintf(g_st.when, sizeof(g_st.when), "%02d:%02d", t.tm_hour, t.tm_min);
    else snprintf(g_st.when, sizeof(g_st.when), "up %lum", (unsigned long)(millis() / 60000UL));
    // The report: one line per part.
    String rep;
    rep.reserve(1600);
    char line[160];
    snprintf(line, sizeof(line), "RatSpeak self-test  %s  fw %s  %s\n", g_selfName, RATSPEAK_FW_VERSION, g_destHex);
    rep += line;
    if (now > 1735689600) {
        snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
        rep += line;
    }
    for (int i = 0; i < STI_N; i++) {
        snprintf(line, sizeof(line), "%-14s %s  %s\n", kStName[i], kStVerdict[g_st.r[i].v & 3], g_st.r[i].detail);
        rep += line;
    }
    snprintf(line, sizeof(line), "PASS %d  WARN %d  FAIL %d\n", cnt[ST_PASS], cnt[ST_WARN], cnt[ST_FAIL]);
    rep += line;
    rep += "Not tested here (needs a person): screen, keys, audio, touch, antenna by ping.\n";
    bool saved = false;
    if (g_sdMounted && g_sdStore.ensureDir(SD_PATH_ROOT "/selftest")) {
        char path[96];
        char name[24];
        size_t k = 0;
        for (const char* c = g_selfName; *c && k < sizeof(name) - 1; c++)
            name[k++] = isalnum((unsigned char)*c) ? *c : '_';
        name[k] = 0;
        if (now > 1735689600)
            snprintf(path, sizeof(path), SD_PATH_ROOT "/selftest/%s-%04d%02d%02d-%02d%02d.txt", k ? name : "unit",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
        else
            snprintf(path, sizeof(path), SD_PATH_ROOT "/selftest/%s-up%lu.txt", k ? name : "unit",
                     (unsigned long)(millis() / 1000));
        saved = g_sdStore.writeString(path, rep);
        if (g_st.console) Serial.printf("[TEST] report %s %s\n", saved ? "saved to" : "NOT saved to", path);
    }
    if (!saved) {
        File f = LittleFS.open("/selftest.txt", FILE_WRITE);
        if (f) { f.print(rep); f.close(); if (g_st.console) Serial.println("[TEST] report saved to flash /selftest.txt"); }
    }
    if (g_st.console) {
        Serial.print(rep);
        Serial.println("[TEST] done");
    }
    if (g_page == PG_SELFTEST) toast("Self-test: %d pass, %d warn, %d fail", cnt[ST_PASS], cnt[ST_WARN], cnt[ST_FAIL]);
    requestRedraw();
}

void selftestTick() {
    if (g_st.running && millis() - g_st.startMs >= kStGpsMs) selftestFinish();
}

bool selftestRan() { return g_st.ran || g_st.running; }

// Settings > Diagnostics > Check this unit: row 1 runs it again, then a row per part.
void selftestRows() {
    if (g_st.running) {
        srow(RK_ACTION, "Checking...", "GPS for %lu s more",
             (unsigned long)((kStGpsMs - (millis() - g_st.startMs)) / 1000 + 1));
    } else if (g_st.ran) {
        int cnt[4] = {0, 0, 0, 0};
        for (auto& r : g_st.r) cnt[r.v & 3]++;
        srow(RK_ACTION, "Run again", "%s: %d pass %d warn %d fail", g_st.when, cnt[ST_PASS], cnt[ST_WARN], cnt[ST_FAIL]);
    } else {
        srow(RK_ACTION, "Run the check", "about 20 s");
    }
    for (int i = 0; i < STI_N; i++)
        srow(RK_INFO, kStName[i], "%s %s", kStVerdict[g_st.r[i].v & 3], g_st.r[i].detail);
}

// ---- Soak test (unplugged group chat) ------------------------------------------------
// "soak on <mean min> <hex> [<hex> ...]": this unit chats with those radios by itself until
// "soak off" (kept in NVS, so it carries on after a restart): a new message to a random one
// about every <mean min>, and an answer to most "PP" messages it gets (10-60 s later). Half
// are typed through the key queue into the thread (the real UI path, checked against what
// went out), half sent directly. Everything goes to <SD>/ratdeckpro/soak.log: a STAT line
// a minute (battery, heap, counts, pauses) and a line per event, so a run on battery until
// it is empty can be read afterwards. tools/stress/soaklog.py sums the logs up.
// Each message is a live status line (uptime, battery, current, heap, counts, stalls, mode),
// only letters, digits and spaces so typing it is plain. "soak listen <hex> [min]" adds a
// listener (a unit left on USB, its console logging "[MSG] in from"): it gets a "PS" status
// report every <min> (default 5), at boot, at the first low battery, and "dark" just before
// the empty-battery switch-off. It does not answer (it ignores "PS").
constexpr int kSoakPeers = 6;
// TEST FIRMWARE: a unit with no soak setting yet starts with the soak ON, chatting with the
// test units below (all but itself). Settings > Diagnostics > Stress test turns it off (kept).
// Set kSoakDefaultOn = false before a release.
#if defined(SIMULATOR)
constexpr bool kSoakDefaultOn = false;   // simulated radios (how-to videos, tests) never chat by themselves
#else
constexpr bool kSoakDefaultOn = true;
#endif
constexpr uint8_t kSoakDefaultMean = 2;
const char* const kSoakFleet[] = {
    "67a79f234db89b27e96310e3b3b25aa1",   // COM3
    "39543b0f5fb5a72a2cb23b3014bd650a",   // COM5
    "b5302a609fcd930edd82108f32b468ff",   // COM6
    "3bb4b09700e7c1d5a7c50111caf10175",   // COM9
};
struct SoakState {
    bool on = false;
    uint8_t meanMin = 5;
    uint8_t n = 0;
    uint8_t peer[kSoakPeers][16] = {};
    uint32_t next = 0;               // millis of the next new message
    uint32_t replyAt = 0;            // pending answer
    int replyTo = -1;
    // typing
    int typePeer = -1;
    char typeText[BUF_CAP + 1] = "";
    size_t typePos = 0;
    uint32_t typeStep = 0;           // millis of the next typing step
    uint8_t typePhase = 0;           // 0 none, 1 typing, 2 Enter sent (waiting for the send)
    // counts (since boot)
    uint32_t sent = 0, typedOk = 0, typedBad = 0, rx = 0, sendFail = 0;
    uint32_t slowMax = 0;
    const char* slowName = "";
    // settings tour: keys pressed one at a time like a person browsing Settings
    char tourKeys[12] = "";
    uint8_t tourPos = 0;
    uint32_t tourStep = 0, nextTour = 0, nextIdleSwap = 0;
    uint32_t tours = 0;
    // listener (a unit on USB that records the reports)
    bool listen = false;
    uint8_t listener[16] = {};
    uint8_t listenMin = 5;
    uint32_t nextListen = 0;
    bool lowSent = false;
    uint32_t reports = 0;
} g_soak;

void soakLog(const char* fmt, ...) {
    if (!g_soak.on) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    char ts[24] = "--";
    struct tm lt;
    if (localNow(lt)) strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &lt);
    Serial.printf("[SOAK] %s\n", line);
    // SD card if there is one (tried once: a missing card costs a slow mount each time), else
    // the flash: /soak.log, moved to /soak.old at 600 KB. Console "soakdump" prints it.
    static int8_t sd = -1;
    if (sd < 0) sd = (g_sdMounted || sdMount()) ? 1 : 0;
    if (sd == 1 && g_sdMounted) {
        File f = g_sdStore.openFile(SD_PATH_ROOT "/soak.log", FILE_APPEND);
        if (f) { f.printf("%s %lu %s\n", ts, (unsigned long)(millis() / 1000), line); f.close(); }
        return;
    }
    File f = LittleFS.open("/soak.log", FILE_APPEND);
    if (!f) return;
    f.printf("%s %lu %s\n", ts, (unsigned long)(millis() / 1000), line);
    const size_t size = f.size();
    f.close();
    if (size > 600000) {
        LittleFS.remove("/soak.old");
        LittleFS.rename("/soak.log", "/soak.old");
    }
}

int soakPeerIndex(const uint8_t peer[16]) {
    for (int i = 0; i < g_soak.n; i++) if (!memcmp(g_soak.peer[i], peer, 16)) return i;
    return -1;
}

uint32_t soakCounter(int i, bool bump) {            // per-peer message number, kept in NVS
    char key[8];
    snprintf(key, sizeof(key), "c%02x%02x", g_soak.peer[i][0], g_soak.peer[i][1]);
    Preferences p;
    if (!p.begin("rs_soak", !bump)) return 0;
    uint32_t v = p.getUInt(key, 0);
    if (bump) p.putUInt(key, ++v);
    p.end();
    return v;
}

void soakSave() {
    Preferences p;
    if (!p.begin("rs_soak", false)) return;
    p.putBool("on", g_soak.on);
    p.putUChar("mean", g_soak.meanMin);
    p.putUChar("n", g_soak.n);
    p.putBytes("peers", g_soak.peer, sizeof(g_soak.peer));
    p.putBool("listen", g_soak.listen);
    p.putBytes("lpeer", g_soak.listener, 16);
    p.putUChar("lmin", g_soak.listenMin);
    p.end();
}

uint32_t soakRandMs(uint32_t meanMs) {              // exponential, 10 s .. 4 x mean
    const float u = (float)(esp_random() % 10000 + 1) / 10001.0f;
    float ms = -logf(u) * (float)meanMs;
    if (ms < 10000) ms = 10000;
    if (ms > 4.0f * meanMs) ms = 4.0f * meanMs;
    return (uint32_t)ms;
}

// The soak needs the idle screen (the e-ink sleep a real user sees): on, 30 s, if it was off.
void soakSetup() {
    if (!g_cfg.idleMode) {
        g_cfg.idleMode = 1;
        g_cfg.idleSecs = 30;
        saveConfig();
        soakLog("SETTING idle screen was off: on, 30 s");
    }
    g_soak.nextTour = millis() + 120000 + esp_random() % 300000;
    g_soak.nextListen = millis() + 60000;
    g_soak.nextIdleSwap = millis() + 30UL * 60000UL;
}

// The test units (all but this one) as soak peers. Returns how many.
uint8_t soakFleetPeers() {
    uint8_t n = 0;
    for (const char* h : kSoakFleet) {
        if (n >= kSoakPeers) break;
        uint8_t d[16];
        for (int i = 0; i < 16; i++) { char b[3] = {h[2 * i], h[2 * i + 1], 0}; d[i] = (uint8_t)strtoul(b, nullptr, 16); }
        if (isSelf(d)) continue;
        memcpy(g_soak.peer[n++], d, 16);
    }
    return n;
}

bool soakIsOn() { return g_soak.on; }
uint8_t soakPeerCount() { return g_soak.n; }

// Settings > Diagnostics > Stress test: Enter turns it off / on (on again with the old peers,
// or the test units when there are none).
void soakToggle() {
    if (g_soak.on) {
        soakLog("STOP (Settings)");
        g_soak.on = false;
        g_soak.typePhase = 0;
        g_soak.tourKeys[0] = 0;
        g_soak.replyTo = -1;
        soakSave();
        toast("Stress test off");
        return;
    }
    if (!g_soak.n) { g_soak.n = soakFleetPeers(); g_soak.meanMin = kSoakDefaultMean; }
    if (!g_soak.n) { toast("No peers for the stress test"); return; }
    g_soak.on = true;
    soakSave();
    g_soak.next = millis() + soakRandMs(g_soak.meanMin * 60000UL);
    soakLog("START (Settings) peers=%u mean=%umin bat=%.3fV %d%% usb=%d", (unsigned)g_soak.n, (unsigned)g_soak.meanMin,
            g_bat.volts, g_bat.pct, g_bat.usb ? 1 : 0);
    soakSetup();
    toast("Stress test on");
}

void soakLoad() {
    Preferences p;
    if (!p.begin("rs_soak", true)) {      // never set on this unit: the test-firmware default
        if (!kSoakDefaultOn) return;
        g_soak.n = soakFleetPeers();
        g_soak.meanMin = kSoakDefaultMean;
        g_soak.on = g_soak.n > 0;
        if (!g_soak.on) return;
        soakSave();
        g_soak.next = millis() + soakRandMs(g_soak.meanMin * 60000UL);
        soakLog("START default (test firmware) peers=%u mean=%umin", (unsigned)g_soak.n, (unsigned)g_soak.meanMin);
        soakSetup();
        return;
    }
    g_soak.on = p.getBool("on", false);
    g_soak.meanMin = p.getUChar("mean", 5);
    g_soak.n = p.getUChar("n", 0);
    if (g_soak.n > kSoakPeers) g_soak.n = kSoakPeers;
    p.getBytes("peers", g_soak.peer, sizeof(g_soak.peer));
    g_soak.listen = p.getBool("listen", false);
    p.getBytes("lpeer", g_soak.listener, 16);
    g_soak.listenMin = p.getUChar("lmin", 5);
    if (g_soak.listenMin < 1) g_soak.listenMin = 5;
    p.end();
    if (!g_soak.on || !g_soak.n) { g_soak.on = false; return; }
    g_soak.next = millis() + soakRandMs(g_soak.meanMin * 60000UL);
#if !defined(SIMULATOR)
    const int rr = (int)esp_reset_reason();
#else
    const int rr = 0;
#endif
    soakLog("BOOT reset=%d fw=%s peers=%u mean=%umin bat=%.3fV %d%% usb=%d", rr, RATSPEAK_FW_VERSION,
            (unsigned)g_soak.n, (unsigned)g_soak.meanMin, g_bat.volts, g_bat.pct, g_bat.usb ? 1 : 0);
    soakSetup();
}

// Live status, letters / digits / spaces only (it is typed too), under 90 chars:
// "up 192m 3712mv 48pct 42ma dsg heap 104 98 s12 r11 f0 bad0 slow2 i2c14 bal"
void soakStatus(char* out, size_t n) {
    char mode[4] = "";
    const char* m = power::modeName(g_pw.mode);
    for (int k = 0; k < 3 && m[k] && m[k] != ' '; k++) mode[k] = (char)tolower((unsigned char)m[k]);
    snprintf(out, n, "up %lum %dmv %dpct %dma %s heap %u %u s%lu r%lu f%lu bad%lu slow%lu i2c%lu %s",
             (unsigned long)(millis() / 60000UL), (int)(g_bat.volts * 1000.0f + 0.5f), g_bat.pct, abs((int)g_bat.avgMa),
             g_bat.usb ? "usb" : (g_bat.avgMa > 0 ? "chg" : "dsg"),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned long)g_soak.sent,
             (unsigned long)g_soak.rx, (unsigned long)g_soak.sendFail, (unsigned long)g_soak.typedBad,
             (unsigned long)g_slowCount, (unsigned long)g_i2cSlowCount, mode);
}

// A report to the listener (sent directly, never typed): "PS <tag> <kind> <status>".
void soakReport(const char* kind) {
    if (!g_soak.on || !g_soak.listen) return;
    char tag[5], st[100], text[BUF_CAP + 1];
    snprintf(tag, sizeof(tag), "%.4s", g_destHex[0] ? g_destHex : "xxxx");
    soakStatus(st, sizeof(st));
    snprintf(text, sizeof(text), "PS %s %s %s", tag, kind, st);
    const SendResult r = sendOrQueue(g_soak.listener, text);
    g_soak.reports++;
    soakLog("REPORT %s: %s", kind, r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "outbox" : "FAILED");
}

// The empty-battery switch-off is coming: tell the listener, and say how long to wait so the
// radio gets it out (0 = no listener, switch off now).
uint32_t soakGoingDark() {
    if (!g_soak.on || !g_soak.listen) return 0;
    soakReport("dark");
    return 20000;
}

void soakSend(int i, bool reply) {
    const uint32_t num = soakCounter(i, true);
    char text[BUF_CAP + 1];
    char tag[5], st[100];
    snprintf(tag, sizeof(tag), "%.4s", g_destHex[0] ? g_destHex : "xxxx");
    soakStatus(st, sizeof(st));
    snprintf(text, sizeof(text), "PP %s %lu %s", tag, (unsigned long)num, st);
    char to[9];
    snprintf(to, sizeof(to), "%02x%02x%02x%02x", g_soak.peer[i][0], g_soak.peer[i][1], g_soak.peer[i][2], g_soak.peer[i][3]);
    // Typed: only from a plain screen (no menu, entry, game, map ...), like a person would.
    const bool canType = !g_locked && !g_menuOpen && !entryActive() && !g_wtOpen && !g_vmOpen && !g_libOpen &&
                         !g_gameOpen && !g_chessOpen && !mapview::isOpen() && !g_usbDrive;
    if (canType && (esp_random() & 1)) {
        g_idleActive = false;
        noteInput();
        openThread(g_soak.peer[i]);
        if (g_threadOpen) {
            strlcpy(g_soak.typeText, text, sizeof(g_soak.typeText));
            g_soak.typePeer = i;
            g_soak.typePos = 0;
            g_soak.typePhase = 1;
            g_soak.typeStep = millis() + 1500;
            soakLog("TYPE %s %lu to %s%s", tag, (unsigned long)num, to, reply ? " (reply)" : "");
            return;
        }
    }
    const SendResult r = sendOrQueue(g_soak.peer[i], text);
    if (r == SendResult::Failed) g_soak.sendFail++; else g_soak.sent++;
    soakLog("SEND %s %lu to %s%s: %s (%s)", tag, (unsigned long)num, to, reply ? " (reply)" : "",
            r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "outbox" : "FAILED", g_txStr);
}

// sendMessage saw this body go out: was it the typed one, letter for letter?
void soakSent(const char* body) {
    if (!g_soak.on || g_soak.typePhase != 2) return;
    g_soak.typePhase = 0;
    if (!strcmp(body, g_soak.typeText)) { g_soak.typedOk++; g_soak.sent++; soakLog("TYPED ok"); }
    else { g_soak.typedBad++; soakLog("TYPED DIFF want \"%s\" got \"%s\"", g_soak.typeText, body); }
}

void soakIncoming(const uint8_t peer[16], const char* text) {
    if (!g_soak.on || strncmp(text, "PP ", 3)) return;
    char tag[16] = "";
    unsigned long num = 0;
    sscanf(text, "PP %15s %lu", tag, &num);
    g_soak.rx++;
    soakLog("RECV %s %lu", tag, num);
    const int i = soakPeerIndex(peer);
    if (i >= 0 && g_soak.replyTo < 0 && esp_random() % 10 < 6) {
        g_soak.replyTo = i;
        g_soak.replyAt = millis() + 10000 + esp_random() % 50000;
    }
}

void soakNoteSlow(const char* name, uint32_t ms) {
    if (ms > g_soak.slowMax) { g_soak.slowMax = ms; g_soak.slowName = name; }
    if (ms >= 3000) soakLog("SLOW %s %lu ms", name, (unsigned long)ms);
}

void soakTick() {
    if (!g_soak.on) return;
    const uint32_t now = millis();
    static uint32_t lastStat = 0;
    if (!lastStat || now - lastStat >= 60000) {
        lastStat = now | 1;
        soakLog("STAT %.3fV %d%% %dmA usb=%d heap=%u min=%u blk=%u psram=%u q=%d ob=%d sent=%lu ok=%lu bad=%lu "
                "fail=%lu rx=%lu slow=%lu max=%lu(%s) i2c=%lu unread=%d tours=%lu idle=%s mode=%s",
                g_bat.volts, g_bat.pct, g_bat.avgMa, g_bat.usb ? 1 : 0,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                g_protoReady ? g_proto.lxmfQueuedCount() : -1, g_outboxCount, (unsigned long)g_soak.sent,
                (unsigned long)g_soak.typedOk, (unsigned long)g_soak.typedBad, (unsigned long)g_soak.sendFail,
                (unsigned long)g_soak.rx, (unsigned long)g_slowCount, (unsigned long)g_soak.slowMax, g_soak.slowName,
                (unsigned long)g_i2cSlowCount, g_storeReady ? g_msgStore.totalUnreadCount() : -1,
                (unsigned long)g_soak.tours, kIdleScreenName[g_cfg.idleScreen < IDLE_COUNT ? g_cfg.idleScreen : 0],
                power::modeName(g_pw.mode));
        g_soak.slowMax = 0;
        g_soak.slowName = "";
    }
    // Typing: 6 keys every 0.8 s (a fast typist), then Enter, then back out of the thread.
    if (g_soak.typePhase == 1 && (int32_t)(now - g_soak.typeStep) < 0) return;   // not yet (fell through to Back)
    if (g_soak.typePhase == 1) {
        if (!g_threadOpen) {                       // something closed the thread: give up
            g_soak.typePhase = 0;
            g_soak.typedBad++;
            soakLog("TYPED LOST thread closed at %u of %u", (unsigned)g_soak.typePos, (unsigned)strlen(g_soak.typeText));
            bufClear();
            return;
        }
        const size_t len = strlen(g_soak.typeText);
        for (int k = 0; k < 6 && g_soak.typePos <= len; k++) {
            uint8_t next = (uint8_t)((g_keyqHead + 1) % KEYQ_CAP);
            if (next == g_keyqTail) break;
            KeyEvent ev{};
            if (g_soak.typePos == len) ev.enter = true; else ev.character = g_soak.typeText[g_soak.typePos];
            g_keyq[g_keyqHead] = ev;
            g_keyqHead = next;
            g_soak.typePos++;
        }
        g_soak.typeStep = now + 800;
        if (g_soak.typePos > len) { g_soak.typePhase = 2; g_soak.typeStep = now + 30000; }
        return;
    }
    if (g_soak.typePhase == 2) {
        if ((int32_t)(now - g_soak.typeStep) >= 0) {   // no send seen in 30 s
            g_soak.typePhase = 0;
            g_soak.typedBad++;
            soakLog("TYPED NOT SENT in 30 s: %s", g_txStr);
            bufClear();
        }
        return;
    }
    if (g_threadOpen && g_soak.typePeer >= 0) {     // done typing: back to the list after a moment
        g_soak.typePeer = -1;
        g_soak.next = g_soak.next < now + 3000 ? now + 3000 : g_soak.next;
        KeyEvent ev{};
        ev.up = true;
        uint8_t next = (uint8_t)((g_keyqHead + 1) % KEYQ_CAP);
        if (next != g_keyqTail) { g_keyq[g_keyqHead] = ev; g_keyqHead = next; }
        return;
    }
    // Settings tour in progress: one key every 1.5 s.
    if (g_soak.tourKeys[g_soak.tourPos]) {
        if ((int32_t)(now - g_soak.tourStep) < 0) return;
        KeyEvent ev{};
        const char c = g_soak.tourKeys[g_soak.tourPos++];
        if (c == '^') ev.up = true; else ev.character = c;
        uint8_t next = (uint8_t)((g_keyqHead + 1) % KEYQ_CAP);
        if (next != g_keyqTail) { g_keyq[g_keyqHead] = ev; g_keyqHead = next; }
        g_soak.tourStep = now + 1500;
        if (!g_soak.tourKeys[g_soak.tourPos]) {   // last key: say where it ended
            g_soak.tourKeys[0] = 0;
            g_soak.tourPos = 0;
        }
        return;
    }
    if ((int32_t)(now - g_soak.nextTour) >= 0) {   // about every 10 min: browse a Settings page
        g_soak.nextTour = now + soakRandMs(10UL * 60000UL);
        const bool plain = !g_locked && !g_menuOpen && !entryActive() && !g_threadOpen && !g_wtOpen && !g_vmOpen &&
                           !g_libOpen && !g_gameOpen && !g_chessOpen && !mapview::isOpen() && !g_usbDrive;
        if (plain) {
            static const char kPages[] = "123456789";
            const char page = kPages[esp_random() % 9];
            // ^ wakes / clears, M menu, S Settings, a page, look, back out to the list.
            snprintf(g_soak.tourKeys, sizeof(g_soak.tourKeys), "^MS%c^^^", page);
            g_soak.tourPos = 0;
            g_soak.tourStep = now;
            g_soak.tours++;
            soakLog("TOUR settings page %c (screen %s)", page, screenName(g_screen));
            return;
        }
    }
    if ((int32_t)(now - g_soak.nextIdleSwap) >= 0) {   // every 30 min: the next idle screen type
        g_soak.nextIdleSwap = now + 30UL * 60000UL;
        g_cfg.idleScreen = (uint8_t)((g_cfg.idleScreen + 1) % IDLE_COUNT);
        saveConfig();
        soakLog("SETTING idle screen -> %s", kIdleScreenName[g_cfg.idleScreen]);
        if (g_idleActive) { paintIdle(); g_display.fullRefresh(); }
    }
    if (g_soak.listen && (int32_t)(now - g_soak.nextListen) >= 0) {   // status to the listener
        const bool first = g_soak.reports == 0;
        g_soak.nextListen = now + g_soak.listenMin * 60000UL;
        soakReport(first ? "boot" : "stat");
        return;
    }
    if (g_soak.listen && !g_soak.lowSent && g_bat.valid && !g_bat.usb && (g_bat.pct <= 10 || g_bat.volts < kBatBootV)) {
        g_soak.lowSent = true;
        soakReport("low");
        return;
    }
    if (g_soak.replyTo >= 0 && (int32_t)(now - g_soak.replyAt) >= 0) {
        const int i = g_soak.replyTo;
        g_soak.replyTo = -1;
        soakSend(i, true);
        return;
    }
    if ((int32_t)(now - g_soak.next) >= 0) {
        g_soak.next = now + soakRandMs(g_soak.meanMin * 60000UL);
        soakSend((int)(esp_random() % g_soak.n), false);
    }
}

// Light sleep must wake for the next soak step.
uint32_t soakMsUntilNext() {
    if (g_st.running) return 50;                   // self-test: watching the GPS
    if (!g_soak.on) return UINT32_MAX;
    if (g_soak.typePhase || g_soak.tourKeys[0]) return 50;
    const uint32_t now = millis();
    uint32_t ms = (int32_t)(g_soak.next - now) > 0 ? g_soak.next - now : 1;
    if (g_soak.replyTo >= 0) {
        const uint32_t r = (int32_t)(g_soak.replyAt - now) > 0 ? g_soak.replyAt - now : 1;
        if (r < ms) ms = r;
    }
    if (g_soak.listen) {
        const uint32_t l = (int32_t)(g_soak.nextListen - now) > 0 ? g_soak.nextListen - now : 1;
        if (l < ms) ms = l;
    }
    const uint32_t stat = 60000;                     // the STAT line once a minute
    return ms < stat ? ms : stat;
}

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
    {                                                  // soak test: next message / STAT line
        const uint32_t soak = soakMsUntilNext();
        if (soak < ms) ms = soak;
    }
    if (g_protoReady && g_proto.lxmfQueuedCount() > 0) {   // outgoing messages waiting: wake for
        uint32_t due = g_proto.lxmfMsUntilDue();             // the next step, at least every 1 s
        if (due > 1000) due = 1000;
        if (due < ms) ms = due;
    }
    if (g_protoReady) {                                // wake for a scheduled path response
        const uint32_t due = g_proto.msUntilScheduledTx();
        if (due < 50) { delay(5); return; }           // due now: stay awake and send it
        if (due < ms) ms = due;
    }

    // Acknowledge the keyboard interrupt; if the line stays low a key is waiting.
    if (g_kbOk) g_kb.clearInterrupt();
    if (digitalRead(KB_INT) == LOW) { delay(2); return; }
    if (digitalRead(BOARD_BOOT_PIN) == LOW) { delay(2); return; }   // top side button down

    power::WakePins pins;
    pins.key = KB_INT;
    pins.button = BOARD_BOOT_PIN;
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
    standbyPrint();
    Serial.printf("[POWER] mode=%s%s cpu=%u MHz sleep=%s awake=%us lora=%s gps=%s(%s) wifi=%s keylight=%s\n",
                  power::modeName(g_pw.mode), g_standby ? " (standby)" : g_pwOnUsb ? " (USB->Full)" : "",
                  (unsigned)g_pwEff.cpuMhz, w,
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
    if (g_storeReady) {
        char wq[40];
        g_msgStore.writeQueue().describe(wq, sizeof(wq));   // F free Q queued R running D done (result not taken)
        Serial.printf("[POWER] storage writes pending=%d slots %s\n", g_msgStore.writeQueue().drainCount(), wq);
    }
    if (g_offReport[0]) Serial.printf("[POWER] last switched off: %s%c", g_offReport, 10);
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
    handheld::storage::StorageLease lease(false);   // wait for the storage worker (see CardWait)
    for (const uint32_t t0 = millis(); !lease.tryAcquire() && millis() - t0 < 3000;) delay(2);
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
        static const char* kOk[] = {"pw", "clk", "id", "lock", "ev", "rl", "sdinfo", "energy", "btn", "standby",
                                    "btn hold", "sos", "sos now", "sos off"};
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
    if (!strncmp(p, "grp", 3) && (p[3] == 0 || p[3] == ' ')) { grpConsole(p[3] ? p + 4 : ""); return; }
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
    if (!strcmp(p, "slow")) {                          // slow: the newest loop steps over 0.2 s
        Serial.printf("[SLOW] %lu so far\n", (unsigned long)g_slowCount);
        for (int k = 0; k < 12; k++) {
            const SlowRec& r = g_slow[(g_slowHead + k) % 12];
            if (r.name) Serial.printf("[SLOW]   %s %u ms at %lus\n", r.name, (unsigned)r.ms, (unsigned long)r.atS);
        }
        return;
    }
    if (!strcmp(p, "hav")) {                           // hav: Home Assistant voice status
        Serial.printf("[HAV] %s, %s, connected %d, busy %d, heard \"%s\", answer \"%s\"\n",
                      g_cfg.haVoice ? "on" : "off", havoice::status(), havoice::connected(),
                      havoice::busy(), havoice::heard(), havoice::answer());
        return;
    }
    if (!strcmp(p, "selftest")) { selftestStart(true); return; }   // hardware self-test, report when done
    if (!strcmp(p, "soak") || !strncmp(p, "soak ", 5)) {   // soak on <mean min> <hex> [...] | soak off
        const char* q = p[4] ? p + 5 : "";
        if (!strncmp(q, "on ", 3)) {
            int mean = atoi(q + 3);
            const char* h = strchr(q + 3, ' ');
            uint8_t n = 0;
            while (h && n < kSoakPeers) {
                while (*h == ' ') h++;
                if (strlen(h) < 32) break;
                for (int i = 0; i < 16; i++) { char b[3] = {h[2 * i], h[2 * i + 1], 0}; g_soak.peer[n][i] = (uint8_t)strtoul(b, nullptr, 16); }
                n++;
                h = strchr(h + 32, ' ');
            }
            if (mean < 1 || !n) { Serial.println("[SOAK] soak on <mean minutes> <32 hex> [<32 hex> ...]"); return; }
            g_soak.meanMin = (uint8_t)(mean > 120 ? 120 : mean);
            g_soak.n = n;
            g_soak.on = true;
            soakSave();
            g_soak.next = millis() + soakRandMs(g_soak.meanMin * 60000UL);
            soakLog("START peers=%u mean=%umin bat=%.3fV %d%% usb=%d", (unsigned)n, (unsigned)g_soak.meanMin, g_bat.volts,
                    g_bat.pct, g_bat.usb ? 1 : 0);
            soakSetup();
        } else if (!strncmp(q, "listen", 6)) {        // soak listen <32 hex> [min] | soak listen off
            const char* h = q + 6;
            while (*h == ' ') h++;
            if (!strcmp(h, "off")) {
                g_soak.listen = false;
            } else if (strlen(h) >= 32) {
                for (int i = 0; i < 16; i++) { char b[3] = {h[2 * i], h[2 * i + 1], 0}; g_soak.listener[i] = (uint8_t)strtoul(b, nullptr, 16); }
                const int m = h[32] ? atoi(h + 33) : 0;
                g_soak.listenMin = (uint8_t)(m >= 1 && m <= 120 ? m : 5);
                g_soak.listen = true;
                g_soak.nextListen = millis() + 5000;
                g_soak.reports = 0;
            } else { Serial.println("[SOAK] soak listen <32 hex> [minutes] | soak listen off"); return; }
            soakSave();
            soakLog("LISTEN %s every %u min", g_soak.listen ? "on" : "off", (unsigned)g_soak.listenMin);
            Serial.printf("[SOAK] listener %s\n", g_soak.listen ? "on" : "off");
        } else if (!strcmp(q, "dump")) {             // soak dump: the log (card, else flash)
            File f = (g_sdMounted || sdMount()) ? g_sdStore.openFile(SD_PATH_ROOT "/soak.log", FILE_READ) : File();
            if (!f) f = LittleFS.open("/soak.log", FILE_READ);
            if (!f) { Serial.println("[SOAK] no log"); return; }
            Serial.printf("[SOAK] ---- log %u bytes ----\n", (unsigned)f.size());
            char buf[256];
            while (f.available()) { const size_t n = f.read((uint8_t*)buf, sizeof(buf)); Serial.write((const uint8_t*)buf, n); }
            f.close();
            Serial.println("[SOAK] ---- end ----");
        } else if (!strcmp(q, "clear")) {            // soak clear: start a fresh log
            if (g_sdMounted || sdMount()) g_sdStore.remove(SD_PATH_ROOT "/soak.log");
            LittleFS.remove("/soak.log");
            LittleFS.remove("/soak.old");
            Serial.println("[SOAK] log cleared");
        } else if (!strcmp(q, "off")) {
            soakLog("STOP");
            g_soak.on = false;
            soakSave();
            Serial.println("[SOAK] off");
        } else {
            Serial.printf("[SOAK] %s, %u peers, mean %u min, sent %lu typed ok %lu bad %lu rx %lu, listener %s (%lu reports)\n",
                          g_soak.on ? "on" : "off", (unsigned)g_soak.n, (unsigned)g_soak.meanMin, (unsigned long)g_soak.sent,
                          (unsigned long)g_soak.typedOk, (unsigned long)g_soak.typedBad, (unsigned long)g_soak.rx,
                          g_soak.listen ? "on" : "off", (unsigned long)g_soak.reports);
        }
        return;
    }
    if (!strncmp(p, "chat ", 5) && strlen(p) >= 5 + 32) {   // chat <32 hex>: open that thread (stress tests)
        uint8_t d[16];
        for (int i = 0; i < 16; i++) { char h[3] = {p[5 + 2 * i], p[6 + 2 * i], 0}; d[i] = (uint8_t)strtoul(h, nullptr, 16); }
        g_menuOpen = false;
        g_idleActive = false;
        noteInput();                        // counts as input, like opening it with keys
        openThread(d);
        Serial.printf("[UI] chat %.8s %s\n", p + 5, g_threadOpen ? "open" : "NOT open");
        return;
    }
    if (!strncmp(p, "vm", 2) && (p[2] == ' ' || !p[2])) {   // vm <32 hex> | vm rec | vm send | vm play | vm close
        const char* q = p[2] ? p + 3 : "";
        if (strlen(q) >= 32) {
            uint8_t d[16];
            for (int i = 0; i < 16; i++) { char h[3] = {q[2 * i], q[2 * i + 1], 0}; d[i] = (uint8_t)strtoul(h, nullptr, 16); }
            openThread(d);
            vmOpen();
        } else if (!strcmp(q, "rec")) vmStartRec();
        else if (!strcmp(q, "send")) vmStopRec(true);
        else if (!strcmp(q, "play")) { if (g_threadOpen) loadHistory(g_threadPeer); vmPlayLast(); }
        else if (!strcmp(q, "close")) vmClose();
        Serial.printf("[VM] open %d rec %d len %u: %s\n", g_vmOpen, g_vmRec, (unsigned)g_vmLen, g_vmStatus);
        return;
    }
    if (!strncmp(p, "wt ", 3)) {                       // wt call <32 hex> | wt answer | wt talk | wt hang
        if (!strncmp(p + 3, "hotspot", 7)) {     // wt hotspot: leave the WiFi so the next call makes a hotspot
            g_wifiUserOff = true;
            wifiDisconnect();
        } else if (!strncmp(p + 3, "call ", 5) && strlen(p + 8) >= 32) {
            uint8_t d[16];
            for (int i = 0; i < 16; i++) { char h[3] = {p[8 + 2 * i], p[9 + 2 * i], 0}; d[i] = (uint8_t)strtoul(h, nullptr, 16); }
            wtCall(d);
        } else if (!strcmp(p + 3, "answer")) { KeyEvent e{}; e.enter = true; if (g_wtOpen) wtKey(e); }
        else if (!strcmp(p + 3, "talk")) { walkie::setTalking(!walkie::talking()); requestRedraw(); }
        else if (!strcmp(p + 3, "hang")) { wtClose(); }
        Serial.printf("[WT] state %d: %s\n", (int)walkie::state(), walkie::status());
        return;
    }
    if (!strcmp(p, "mesh")) {                          // mesh: Reticulum over WiFi state
#if !defined(SIMULATOR)
        {
            auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_ip6_addr_t ll = {};
            const esp_err_t e = sta ? esp_netif_get_ip6_linklocal(sta, &ll) : ESP_FAIL;
            const IPv6Address a = WiFi.localIPv6();
            Serial.printf("[MESH] link-local: netif %s err %d %08lx..%08lx; Arduino %02x%02x::%02x%02x\n", sta ? "ok" : "none",
                          (int)e, (unsigned long)ll.addr[0], (unsigned long)ll.addr[3], a[0], a[1], a[14], a[15]);
        }
        const auto& st = g_auto.stats();
        Serial.printf("[MESH] %s, up %d, peers %d, rx %lu tx %lu beacons %lu/%lu bad %lu\n",
                      g_cfg.meshWifi ? "on" : "off", g_autoUp, meshPeers(), (unsigned long)st.rxFrames,
                      (unsigned long)st.txFrames, (unsigned long)st.rxBeacons, (unsigned long)st.txBeacons,
                      (unsigned long)st.rxBadBeacons);
#endif
        return;
    }
    if (!strcmp(p, "hakey")) { Serial.printf("[HAV] key %s\n", g_cfg.haKey[0] ? g_cfg.haKey : "(none)"); return; }
    if (!strncmp(p, "hakey ", 6)) {                    // hakey new | none | <44-char base64>
        uint8_t k[32];
        if (!strcmp(p + 6, "new")) haNewKey();
        else if (!strcmp(p + 6, "none")) g_cfg.haKey[0] = 0;
        else if (havoice::keyDecode(p + 6, k)) strlcpy(g_cfg.haKey, p + 6, sizeof(g_cfg.haKey));
        else { Serial.println("[HAV] hakey new | none | <44-char base64 key>"); return; }
        saveConfig();
        Serial.printf("[HAV] key %s\n", g_cfg.haKey[0] ? g_cfg.haKey : "(none)");
        return;
    }
    if (!strcmp(p, "haselftest")) { havoice::selfTest(); return; }
    if (!strcmp(p, "apitunnel")) { havoice::tunnelStart(g_cfg.haKey); return; }   // bench test
    if (!strcmp(p, "apitunnel off")) { havoice::tunnelStop(); return; }
    if (!strncmp(p, "api ", 4)) { havoice::tunnelFeed(p + 4); return; }
    if (!strncmp(p, "havoice ", 8)) {                  // havoice 0|1
        g_cfg.haVoice = atoi(p + 8) != 0;
        if (g_cfg.haVoice && !g_cfg.haKey[0]) haNewKey();
        saveConfig();
        Serial.printf("[HAV] voice %s\n", g_cfg.haVoice ? "on" : "off");
        return;
    }
    if (!strncmp(p, "telesend ", 9)) {                // bench: telesend <hex32> <lat> <lon> <pct>: a message with
        char hex[40] = "";                              // only a telemetry field (as Sideband sends), no position text
        double la = 0, lo = 0; int pct = 0;
        uint8_t dest[16];
        if (sscanf(p + 9, "%39s %lf %lf %d", hex, &la, &lo, &pct) != 4 || !hexToHash(hex, dest)) {
            Serial.println("[LOC] telesend <hex32> <lat> <lon> <pct>");
            return;
        }
        handheld::lxmf::Telemetry tm;
        const time_t now = time(nullptr);
        tm.time = now > 1600000000 ? (uint32_t)now : 1;
        tm.hasPosition = true; tm.latitude = la; tm.longitude = lo; tm.positionTime = tm.time; tm.accuracyM = 8;
        tm.hasBattery = true; tm.percent = (uint8_t)pct;
        g_txExtraLen = handheld::lxmf::makeTelemetryEntry(tm, g_txExtra, sizeof(g_txExtra));
        const SendResult r = statusSend(dest, "telemetry test");
        g_txExtraLen = 0;
        Serial.printf("[LOC] telesend: %s\n", r == SendResult::Sent ? "sent" : r == SendResult::Queued ? "queued" : "failed");
        return;
    }
    if (!strcmp(p, "psramth") || !strncmp(p, "psramth ", 8)) {   // psramth [bytes]: PSRAM threshold (main.cpp), 0 = default 1024, 4096 = framework
        Preferences pr;
        if (p[7] && pr.begin("rs_mem", false)) {
            const long v = atol(p + 8);
            pr.putUInt("extmem", v >= 256 && v <= 4096 ? (uint32_t)v : 0u);
            pr.end();
            Serial.println("[MEM] saved: restart to apply");
        }
        uint32_t th = 0;
        if (pr.begin("rs_mem", true)) { th = pr.getUInt("extmem", 0); pr.end(); }
        Serial.printf("[MEM] PSRAM threshold %s%lu; internal free %u, lowest %u, biggest block %u\n",
                      th ? "" : "default 1024, saved ", (unsigned long)th,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    if (!strcmp(p, "hasensors") || !strncmp(p, "hasensors ", 10)) {   // hasensors [0|1|2]
        if (p[9]) { g_cfg.haSensors = (uint8_t)constrain(atoi(p + 10), 0, 2); saveConfig(); }
        Serial.printf("[HAV] sensors setting: %s\n", kHaSensName[g_cfg.haSensors]);
        return;
    }
    if (!strcmp(p, "quiet") || !strncmp(p, "quiet ", 6)) {      // quiet [0-4] | quiet hour <h|-1> (bench)
        if (!strncmp(p, "quiet hour ", 11)) g_quietTestHour = atoi(p + 11);
        else if (p[5]) { g_cfg.quiet = (uint8_t)constrain(atoi(p + 6), 0, kQuietN - 1); saveConfig(); }
        Serial.printf("[ALERT] quiet hours: %u-%u%s, now %s\n", (unsigned)kQuiet[g_cfg.quiet][0], (unsigned)kQuiet[g_cfg.quiet][1],
                      g_cfg.quiet ? "" : " (off)", quietNow() ? "quiet" : "not quiet");
        return;
    }
    if (!strcmp(p, "relay") || !strncmp(p, "relay ", 6)) {   // relay [0|1|2] (off, on USB power, always)
        if (p[5]) { g_cfg.relay = (uint8_t)constrain(atoi(p + 6), 0, 2); saveConfig(); }
        Serial.printf("[RELAY] setting: %s, relaying %s\n", kRelayName[g_cfg.relay], g_proto.relay() ? "yes" : "no");
        rs_handheld_transport_stats_t st;
        if (g_proto.transportStats(st))
            Serial.printf("[RELAY] node: accepted %llu, duplicates %llu, announces learned %llu, queued out %llu, "
                          "dropped %llu, out dropped %llu, rate dropped %llu, waiting %lu\n",
                          (unsigned long long)st.accepted, (unsigned long long)st.duplicates,
                          (unsigned long long)st.learned_announces, (unsigned long long)st.queued_outbound,
                          (unsigned long long)st.dropped, (unsigned long long)st.outbound_dropped,
                          (unsigned long long)st.announces_rate_dropped, (unsigned long)st.outbound_len);
        return;
    }
    if (!strcmp(p, "hawake") || !strncmp(p, "hawake ", 7)) {   // hawake [0|1|2] (off, on USB power only, always)
        if (p[6]) { g_cfg.haWake = (uint8_t)constrain(atoi(p + 7), 0, 2); saveConfig(); }
        Serial.printf("[HAV] wake word setting: %s\n", kHaWakeName[g_cfg.haWake]);
        return;
    }
    if (!strncmp(p, "micgain ", 8)) {                  // micgain 1-32
        g_cfg.micGain = (uint8_t)constrain(atoi(p + 8), 1, 32);
        havoice::setMicGain(g_cfg.micGain);
    walkie::begin(&g_audio);
        saveConfig();
        Serial.printf("[HAV] mic gain %u\n", (unsigned)g_cfg.micGain);
        return;
    }
    if (!strncmp(p, "c2test", 6)) {                    // c2test [3200|1600|700]: record 3 s, Codec2, play
#if !defined(SIMULATOR)
        const int b = p[6] ? atoi(p + 7) : 3200;
        if (!voicecodec::begin(b)) { Serial.println("[C2] c2test 3200|1600|700"); return; }
        const int fs = voicecodec::frameSamples16k(), fb = voicecodec::frameBytes();
        const int frames = 3000 / voicecodec::frameMs();
        int16_t* rec = (int16_t*)ps_malloc(sizeof(int16_t) * fs * frames);
        uint8_t* bits = (uint8_t*)ps_malloc(fb * frames);
        if (!rec || !bits || !g_audio.micStart()) { free(rec); free(bits); Serial.println("[C2] no memory / mic"); return; }
        Serial.println("[C2] recording 3 s: speak now");
        int got = 0;
        const uint32_t t0 = millis();
        while (got < fs * frames && millis() - t0 < 4000) {
            got += (int)g_audio.micRead(rec + got, fs * frames - got, g_cfg.micGain);
            delay(5);
        }
        g_audio.micStop();
        uint32_t enc = 0, dec = 0, worst = 0;
        for (int i = 0; i < frames; i++) {
            const uint32_t a = micros();
            voicecodec::encode(rec + i * fs, bits + i * fb);
            const uint32_t d = micros() - a;
            enc += d; if (d > worst) worst = d;
        }
        for (int i = 0; i < frames; i++) {
            const uint32_t a = micros();
            voicecodec::decode(bits + i * fb, rec + i * fs);
            dec += micros() - a;
        }
        Serial.printf("[C2] %d bps: %d frames, %d bytes for 3 s; encode %lu us/frame (worst %lu), decode %lu us/frame, frame %d ms\n",
                      b, frames, fb * frames, (unsigned long)(enc / frames), (unsigned long)worst,
                      (unsigned long)(dec / frames), voicecodec::frameMs());
        Serial.println("[C2] playing the decoded speech");
        g_audio.writePcm(rec, fs * frames);
        free(rec); free(bits);
#endif
        return;
    }
    if (!strcmp(p, "mictest")) {                       // mictest: record 2 s (with micgain), print the level, play it back
#if !defined(SIMULATOR)
        int16_t buf[512];
        int16_t* rec = (int16_t*)ps_malloc(32000 * sizeof(int16_t));   // played back after
        uint32_t recN = 0;
        if (!g_audio.micStart()) { Serial.println("[MIC] start failed"); free(rec); return; }
        delay(100);
        while (g_audio.micRead(buf, 512, 1)) {}          // the first samples settle: drop
        uint32_t n = 0; int32_t peak = 0; double sq = 0; long long sum = 0;
        const uint32_t t0 = millis();
        while (millis() - t0 < 2000) {
            const size_t k = g_audio.micRead(buf, 512, g_cfg.micGain);
            for (size_t i = 0; i < k && rec && recN < 32000; i++) rec[recN++] = buf[i];
            for (size_t i = 0; i < k; i++) {
                const int32_t v = buf[i];
                sum += v; sq += (double)v * v;
                if (abs(v) > peak) peak = abs(v);
            }
            n += k;
            delay(5);
        }
        g_audio.micStop();
        const double mean = n ? (double)sum / n : 0;
        Serial.printf("[MIC] 2 s: %lu samples (%lu/s), peak %ld, rms %.0f, dc %.0f\n", (unsigned long)n,
                      (unsigned long)(n / 2), (long)peak, n ? sqrt(sq / n) : 0.0, mean);
        if (rec && recN) { Serial.println("[MIC] playing it back"); g_audio.writePcm(rec, recN); }
        free(rec);
#endif
        return;
    }
    if (!strcmp(p, "talk")) { havoice::startTalk(); return; }        // talk / talkstop: test without MIC
    if (!strcmp(p, "talkstop")) { havoice::stopTalk("console"); return; }
    if (!strncmp(p, "k ", 2)) {                        // k <keys>: press keys (bench timing); ^ back, ~ Enter, ! MIC
        for (const char* q = p + 2; *q; q++) {
            uint8_t next = (uint8_t)((g_keyqHead + 1) % KEYQ_CAP);
            if (next == g_keyqTail) { Serial.printf("[KEY] queue full: %u keys dropped\n", (unsigned)strlen(q)); break; }
            KeyEvent ev{};
            if (*q == '^') ev.up = true; else if (*q == '~') ev.enter = true; else if (*q == '!') ev.fn = true; else ev.character = *q;
            g_keyq[g_keyqHead] = ev;
            g_keyqHead = next;
        }
        return;
    }
    if (!strncmp(p, "found ", 6)) {                    // found <1-10> [text]: an If-found line
        const int i = atoi(p + 6) - 1;
        const char* t = strchr(p + 6, ' ');
        if (i < 0 || i > 9) { Serial.println("[FOUND] found <1-10> [text]"); return; }
        strlcpy(g_cfg.found[i], t ? t + 1 : "", sizeof(g_cfg.found[i]));
        saveConfig();
        requestRedraw();
        Serial.printf("[FOUND] %s = \"%s\" (%d of 10 set)\n", kFoundLabel[i], g_cfg.found[i], foundCount());
        return;
    }
    if (!strncmp(p, "opmode ", 7)) {                   // opmode 0|1|2 (Minimal / Normal / Advanced)
        const int m = atoi(p + 7);
        if (m >= 0 && m <= 2) { g_cfg.opMode = (uint8_t)m; saveConfig(); minimalToInbox(); requestRedraw(); }
        Serial.printf("[MODE] %s\n", kOpModeName[g_cfg.opMode]);
        return;
    }
    if (!strncmp(p, "touchscreen ", 12)) {             // touchscreen 0|1
        g_cfg.touchOff = atoi(p + 12) == 0;
        saveConfig();
        Serial.printf("[TOUCH] screen %s\n", g_cfg.touchOff ? "off (keys only)" : "on");
        return;
    }
    if (!strncmp(p, "idlescr", 7)) {                   // idlescr [0-3]: idle screen; "idle now"
        const int v = p[7] ? atoi(p + 8) : -1;
        if (v >= 0 && v < IDLE_COUNT) { g_cfg.idleScreen = (uint8_t)v; saveConfig(); }
        Serial.printf("[IDLE] screen %s\n", kIdleScreenName[g_cfg.idleScreen]);
        return;
    }
    if (!strcmp(p, "idle now")) { enterIdle(); return; }
    if (!strcmp(p, "btn")) { sideButtonAction("console"); return; }
    if (!strcmp(p, "gpximport")) { gpxImport(); return; }       // routes/*.gpx -> waypoints
    if (!strncmp(p, "sun", 3) && (p[3] == 0 || p[3] == ' ')) {   // sun | sun <lat> <lon> <day of year>: UT hours
        double la, lo, r1 = -1, r2 = -1; int doy;
        if (sscanf(p + 3, "%lf %lf %d", &la, &lo, &doy) == 3) {
            const bool a = sunUt(la, lo, doy, true, r1), b = sunUt(la, lo, doy, false, r2);
            Serial.printf("[SUN] %.4f %.4f day %d: rise %s%.2f UT, set %s%.2f UT%c", la, lo, doy, a ? "" : "none ", r1, b ? "" : "none ", r2, 10);
            return;
        }
        char sun[40] = "needs the clock and a position";
        sunText(sun, sizeof(sun));
        Serial.printf("[SUN] today: %s%c", sun, 10);
        return;
    }
    if (!strcmp(p, "trip")) { tripRun(); return; }             // S.10 Ready for a trip?
    if (!strncmp(p, "held", 4) && (p[4] == 0 || p[4] == ' ')) {   // held | held on | off | clear
        const char* a = p[4] ? p + 5 : "";
        if (!strcmp(a, "on")) g_heldOn = true;
        else if (!strcmp(a, "off")) g_heldOn = false;
        else if (!strcmp(a, "clear")) g_heldCount = 0;
        if (a[0]) heldSave();
        heldPrint();
        return;
    }
    if (!strncmp(p, "home", 4) && (p[4] == 0 || p[4] == ' ')) {   // home | home set <lat> <lon> | here | clear | go
        const char* a = p[4] ? p + 5 : "";
        double la, lo;
        if (!strncmp(a, "set ", 4) && sscanf(a + 4, "%lf %lf", &la, &lo) == 2) homeSet(la, lo, "console");
        else if (!strcmp(a, "here")) { if (ownPosition(la, lo)) homeSet(la, lo, "console, here"); }
        else if (!strcmp(a, "clear")) homeClear();
        else if (!strcmp(a, "go")) homeGo();
        char hd[24] = "no own position";
        homeText(hd, sizeof(hd));
        if (g_home.set) Serial.printf("[HOME] %.5f, %.5f: %s%c", g_home.lat, g_home.lon, hd, 10);
        else Serial.println("[HOME] not set");
        requestRedraw();
        return;
    }
    if (!strncmp(p, "poweroff", 8) && (p[8] == 0 || p[8] == ' '))   // "poweroff 2": battery look every 2 min (test)
        powerDown("console", p[8] ? (uint16_t)constrain(atoi(p + 9), 1, 1440) : 12 * 60, false);
    if (!strncmp(p, "offc", 4) && (p[4] == 0 || p[4] == ' ')) {      // offc | on | off | at <hhmm> | stay <min>
        const char* a = p[4] ? p + 5 : "";
        if (!strcmp(a, "on")) g_offc.on = true;
        else if (!strcmp(a, "off")) g_offc.on = false;
        else if (!strncmp(a, "at ", 3)) { const int v = atoi(a + 3); g_offc.atMin = (uint16_t)((v / 100 % 24) * 60 + v % 100 % 60); }
        else if (!strncmp(a, "stay ", 5)) g_offc.stayMin = (uint8_t)constrain(atoi(a + 5), 2, 30);
        if (a[0]) offcSave(); else offcPrint();
        requestRedraw();
        return;
    }
    // S.5 Switch off; top button 2 s = on
    if (!strcmp(p, "btn hold")) { sosHold("console"); return; }
    if (!strcmp(p, "lib")) { libOpen(); return; }                          // the Library
    if (!strcmp(p, "libnote")) { libOpen(); startEntry(Entry::Note, ""); requestRedraw(); return; }   // + New note        // the top button held 3 s (SOS)
    if (!strcmp(p, "sos") || !strncmp(p, "sos ", 4)) {                 // sos [now|off|update|to all|to g<n>]
        if (!strcmp(p, "sos now")) { g_sosArmAt = 0; if (!g_sos.on) sosStart(); else sosSend(0); }
        else if (!strcmp(p, "sos off")) { g_sosArmAt = 0; sosStop("console"); }
        else if (!strcmp(p, "sos update")) { if (g_sos.on) sosSend(1); }
        else if (!strcmp(p, "sos btn on") || !strcmp(p, "sos btn off")) {
            if (g_sos.btn != (p[8] == 'o' && p[9] == 'n')) sosToggleBtn();
        }
        else if (!strncmp(p, "sos to ", 7)) {
            const char* a = p + 7;
            if (!strcmp(a, "all")) g_sos.to = 0;
            else if (a[0] == 'g' && atoi(a + 1) >= 1 && atoi(a + 1) <= g_grpCount) g_sos.to = (uint8_t)atoi(a + 1);
            sosSave();
        }
        char t[40];
        sosTargetText(t, sizeof(t));
        Serial.printf("[SOS] %s; button %s; sends to %s; %u sent; countdown %s; alarm %s\n", g_sos.on ? "ACTIVE" : "off",
                      g_sos.btn ? "on" : "off", t,
                      (unsigned)g_sosSent, g_sosArmAt ? "running" : "no", g_sosRxUntil ? g_sosRxFrom : "none");
        return;
    }
    if (!strncmp(p, "standby", 7) && (p[7] == 0 || p[7] == ' ')) {
        // standby                 print the option and the state
        // standby on | off        the option (saved), like S.5.11
        // standby after <hours>   1, 3, 6, 12 or 24 (saved), like S.5.12
        // standby now             enter standby at once (bench; works with the option off)
        // standby wake            leave it, as a key press would
        // standby test <secs>     bench: idle timeout in seconds instead of hours (0 = back to hours; not saved)
        const char* a = p[7] ? p + 8 : "";
        int v = 0;
        if (!strcmp(a, "on") || !strcmp(a, "off")) {
            g_pw.standby = a[1] == 'n';
            power::save(g_pw);
            if (!g_pw.standby) standbyLeave("option off");
            requestRedraw();
        }
        else if (sscanf(a, "after %d", &v) == 1) {
            if (v == 1 || v == 3 || v == 6 || v == 12 || v == 24) { g_pw.standbyHours = (uint16_t)v; power::save(g_pw); requestRedraw(); }
            else Serial.println("[STANDBY] after 1|3|6|12|24");
        }
        else if (sscanf(a, "test %d", &v) == 1 && v >= 0) g_standbyTestS = (uint32_t)v;
        else if (!strcmp(a, "now")) standbyEnter("console");
        else if (!strcmp(a, "wake")) standbyLeave("console");
        standbyPrint();
        return;
    }
    if (!strncmp(p, "lock", 4) && (p[4] == 0 || p[4] == ' ')) {
        if (!strcmp(p, "lock now")) lockNow();
        Serial.printf("[LOCK] locked=%d password=%d idleMode=%u fails=%u wait=%us vault=%s\n", g_locked,
                      lockHasPassword(), (unsigned)g_cfg.idleMode, (unsigned)g_lockFails, (unsigned)lockWaitS(),
                      vault::status());
        return;
    }
    if ((p[0] == 't' || p[0] == 'T') && p[1] == ':') {
        if (!g_threadOpen) { Serial.println("[CMD] open a thread first"); return; }
        if (isGrpHash(g_threadPeer)) { groupSend(grpOfHash(g_threadPeer), p + 2); loadHistory(g_threadPeer); }
        else if (sendOrQueue(g_threadPeer, p + 2) != SendResult::Failed) loadHistory(g_threadPeer);
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
        uint32_t lt, lsn, lc, lb; mapview::layeredStats(lt, lsn, lc, lb);
        Serial.printf("[MAP] layered packs: %lu tiles, %lu layer streams (%lu compressed), %lu bad; layers %lu\n",
                      (unsigned long)lt, (unsigned long)lsn, (unsigned long)lc, (unsigned long)lb,
                      (unsigned long)mapview::layers());
        if (lb) Serial.printf("[MAP] last bad stream: %s\n", mapview::layeredLastBad());
        Serial.printf("[MAP] card waits: longest %lu ms, %lu timed out; %lu layer reads put off\n",
                      (unsigned long)g_cardWaitMaxMs, (unsigned long)g_cardWaitFails,
                      (unsigned long)mapview::layeredReadFails());
    } else if (!strcmp(p, "txrows")) {
        txDescribe();                            // why sends are "busy": rows and store slots
    } else if (!strcmp(p, "maplayers") || !strncmp(p, "maplayers ", 10)) {
        // maplayers [mask]: overlay layers of the layered map packs. 1 labels, 2 contours,
        // 4 woods, 8 marsh marks, 16 hillshading, 32 any other; 63 = all, 0 = base map only.
        if (p[9]) { mapview::setLayers((uint32_t)atoi(p + 10) & (mapview::kLayerAll | mapview::kLayerCoverage)); laySave(); requestRedraw(); }
        Serial.printf("[MAP] layers %lu (1 labels 2 contours 4 woods 8 marsh 16 hillshade 32 other 64 coverage)\n",
                      (unsigned long)mapview::layers());
    } else if (!strncmp(p, "maptest ", 8)) {
        // Bench: write labelled grid test tiles (3x3 at z14 and z15) around <lat> <lon> on the SD card.
        double la = 0, lo = 0;
        if (sscanf(p + 8, "%lf %lf", &la, &lo) != 2 || !sdMount()) { Serial.println("[MAP] maptest <lat> <lon> (needs SD)"); return; }
        static uint8_t (&buf)[mapview::kHeaderBytes + mapview::kTileBytes] = psram<uint8_t, mapview::kHeaderBytes + mapview::kTileBytes>();
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
            if (g_cpos[i].bat) Serial.printf("[LOC] cbat #%u %s %u%% at %lu\n", (unsigned)g_cpos[i].num, hex,
                                             (unsigned)(g_cpos[i].bat - 1), (unsigned long)g_cpos[i].batAt);
        }
        Serial.printf("[LOC] cpos %d contacts\n", g_cposCount);
    } else if (!strcmp(p, "loc")) {
        char pos[56];
        gpsPosText(pos, sizeof(pos));
        Serial.printf("[LOC] %s (hdop %.1f)\n", pos, g_gpsHdop);
        for (int i = 0; i < g_ashareCount; i++) {
            char hex[33]; hashToHex(g_ashare[i].dest, hex);
            Serial.printf("[LOC] auto %s every %u min %s until %lu last %lu\n", hex, (unsigned)g_ashare[i].mins,
                          shareWhatName(ashareWhat(g_ashare[i])), (unsigned long)g_ashare[i].until, (unsigned long)g_ashare[i].lastAt);
        }
        Serial.printf("[LOC] auto-share: %d on\n", g_ashareCount);
#if defined(SIMULATOR)
    } else if (!strncmp(p, "fake ", 5)) {    // tests: fake bat <pct|off> | fake move <metres north>
        if (!strncmp(p + 5, "bat ", 4)) { g_fakeBat = isdigit((unsigned char)p[9]) ? atoi(p + 9) : -1; readBattery(); }
        else if (!strncmp(p + 5, "move ", 5)) g_fakeNorthM = atof(p + 10);
        Serial.printf("[FAKE] bat %d (now %d%%) north %.0f m\n", g_fakeBat, g_bat.pct, g_fakeNorthM);
#endif
    } else if (!strcmp(p, "cot") || !strncmp(p, "cot ", 4)) {
        cotConsole(p[3] ? p + 4 : "");
    } else if (!strcmp(p, "wifi off")) {                // = S.3 row 5 Disconnect (auto-join paused)
        wifiDisconnect();
        Serial.println("[WIFI] off (auto-join paused; 'wifi auto' resumes it)");
    } else if (!strcmp(p, "wifi auto")) {               // auto-join again (battery tests)
        g_wifiUserOff = false;
        Serial.println("[WIFI] auto-join on");
    } else if (!strncmp(p, "wifi join ", 10)) {         // wifi join <ssid> [password] (bench)
        char ssid[33] = "", pass[65] = "";
        if (sscanf(p + 10, "%32s %64s", ssid, pass) >= 1) { wifiConnect(ssid, pass, false, false); Serial.printf("[WIFI] joining \"%s\"\n", ssid); }
    } else if (!strcmp(p, "mark") || !strncmp(p, "mark ", 5)) {
        markConsole(p[4] ? p + 5 : "");
    } else if (!strncmp(p, "locshare ", 9) || !strncmp(p, "ashare ", 7)) {
        // Bench: locshare <to> [p|b|pb] = share once (default: position)
        //        ashare <to> <minutes, 0 = off> [p|b|pb] (default: both) | ashare off
        //        <to> = <hex32>, or g<n> = group number n in the list
        const bool share = p[0] == 'l';
        const char* a = p + (share ? 9 : 7);
        if (!share && !strcmp(a, "off")) { ashareStopAll(); return; }
        uint8_t dest[16];
        const char* rest = a;
        if (a[0] == 'g' && isdigit((unsigned char)a[1])) {
            const int gn = atoi(a + 1);
            if (gn < 1 || gn > g_grpCount) { Serial.println("[LOC] no such group"); return; }
            grpHash(g_grp[gn - 1].id, dest);
            rest = a + 1;
            while (isdigit((unsigned char)*rest)) rest++;
        } else {
            if (strlen(a) < 32 || !hexToHash(a, dest)) { Serial.println("[LOC] need <hex32> or g<n>"); return; }
            rest = a + 32;
        }
        auto whatOf = [](const char* s, uint8_t dflt) -> uint8_t {
            const char* e = s + strlen(s);
            while (e > s && e[-1] == ' ') e--;
            if (e - s >= 3 && !strncmp(e - 3, " pb", 3)) return SH_POS | SH_BAT;
            if (e - s >= 2 && !strncmp(e - 2, " p", 2)) return SH_POS;
            if (e - s >= 2 && !strncmp(e - 2, " b", 2)) return SH_BAT;
            return dflt;
        };
        if (share) { shareMyStatus(dest, whatOf(rest, SH_POS)); return; }
        const int mins = atoi(rest);
        int i = ashareFind(dest);
        if (!mins) { if (i >= 0) { ashareRemove(i); ashareSave(); } Serial.println("[LOC] auto-share off"); requestRedraw(); return; }
        if (i < 0) {
            if (g_ashareCount >= kAutoShareMax) return;
            i = g_ashareCount++;
            memset(&g_ashare[i], 0, sizeof(AutoShare));
            memcpy(g_ashare[i].dest, dest, 16);
        }
        g_ashare[i].mins = (uint8_t)mins;
        g_ashare[i].what = whatOf(rest, SH_POS | SH_BAT);
        g_ashare[i].until = 0;
        ashareSave();
        Serial.printf("[LOC] auto-share every %d min, %s\n", mins, shareWhatName(g_ashare[i].what));
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
        Serial.printf("[SD] %s (checking the card)%c", g_sdStatus, 10);
        sdProbe();
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
        // "restore <folder name in /backups> | newest": as S.11.5 > Restore backup, without
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
        // As S.11.5 > Update firmware, without asking twice.
        sdMount();
        sdToolsScan(true);
        fwUpdate();
    } else if (!strcmp(p, "usbdrive")) {
        // The SD card becomes a USB drive on the PC (as S.11.5 row 6); this COM port goes away.
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
        const size_t fTotal = g_flash.totalBytes(), fUsed = fTotal ? g_flash.usedBytes() : 0;
        if (fTotal)
            Serial.printf("[FLASH] used %u of %u KB (%u%%)\n", (unsigned)(fUsed >> 10), (unsigned)(fTotal >> 10),
                          (unsigned)(100ull * fUsed / fTotal));
        else   // the storage task holds the flash (a save or a clean-up pass): sizes cannot be read now
            Serial.printf("[FLASH] busy (storage task working): last estimate %u%% used\n",
                          (unsigned)handheld::storage::MessageTransactions::spaceStats().usedPct);
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
        // Bench (full-flash test): "msgfill <count> [peers] [bytes] [unread] [outStatus] [ageS]"
        // stores <count> fake incoming messages spread over [peers] fake conversations through the
        // normal save path (unread 1: as new messages, for the idle screen's count). outStatus 1-6:
        // outgoing messages with that status instead (3 sent, 6 unconfirmed), ageS seconds old.
        // Stops at the first failed save and prints flash use.
        using namespace handheld::storage;
        int count = 0, peers = 4, bytes = 200, unread = 0, outStatus = 0, ageS = 0;
        sscanf(p + 8, "%d %d %d %d %d %d", &count, &peers, &bytes, &unread, &outStatus, &ageS);
        const bool outgoing = outStatus >= 1 && outStatus <= 6;
        peers = peers < 1 ? 1 : (peers > 200 ? 200 : peers);
        bytes = bytes < 1 ? 1 : (bytes > 3000 ? 3000 : bytes);
        std::string body(bytes, 'x');
        int saved = 0, failed = 0;
        const unsigned long t0 = millis();
        for (int i = 0; i < count; i++) {
            Request rq; rq.operation = outgoing ? Operation::CreateOutgoing : Operation::CreateIncoming;
            rq.key.incoming = !outgoing;
            memset(rq.key.peer, 0xF0, 16); rq.key.peer[15] = (uint8_t)(i % peers);
            uint8_t self[16];
            for (int b = 0; b < 16; b++) { unsigned v = 0; sscanf(g_destHex + 2 * b, "%2x", &v); self[b] = (uint8_t)v; }
            memcpy(rq.source, outgoing ? self : rq.key.peer, 16);
            memcpy(rq.destination, outgoing ? rq.key.peer : self, 16);
            for (int b = 0; b < 32; b++) rq.messageId[b] = (uint8_t)esp_random();
            rq.hasMessageId = true; rq.timestamp = (double)time(nullptr) - ageS + i * 0.001;
            rq.status = outgoing ? (uint8_t)outStatus : (uint8_t)LXMFStatus::DELIVERED; rq.read = outgoing || !unread;
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
    } else if (!strcmp(p, "batempty")) {     // preview the empty-battery screen (does not switch off)
        paintIdle();
        idleBanner("BATTERY EMPTY", "PLUG IN A USB CHARGER");
        g_display.fullRefresh();
        Serial.println("[BAT] empty screen shown (preview; a key wakes)");
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

// Low-battery notice (S.6 row 12): once per discharge, at the chosen %, one contact gets
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

void tripAdd(bool ok, const char* name, const char* fmt, ...) {
    if (g_tripN >= (int)(sizeof(g_trip) / sizeof(g_trip[0]))) return;
    TripItem& t = g_trip[g_tripN++];
    t.ok = ok;
    strlcpy(t.name, name, sizeof(t.name));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t.note, sizeof(t.note), fmt, ap);
    va_end(ap);
    if (!ok) g_tripTodo++;
}

void tripRun() {
    g_tripN = g_tripTodo = 0;
    readBattery();
    if (!g_bat.valid) tripAdd(false, "Battery", "no reading");
    else tripAdd(g_bat.pct >= 80, "Battery", g_bat.pct >= 80 ? "%d%%" : "%d%%, charge it first", g_bat.pct);
    tripAdd(time(nullptr) > 1600000000, "Clock", time(nullptr) > 1600000000 ? "set" : "not set (S.2, or a GPS fix)");
    if (gpsFresh()) tripAdd(true, "GPS", "fix, %u satellites", (unsigned)g_gpsSats);
    else if (!g_cfg.gpsOn) tripAdd(false, "GPS", "switched off (S.2 row 3)");
    else tripAdd(false, "GPS", "no fix now: try under open sky");
    const bool sd = sdProbe();
    tripAdd(sd, "SD card", sd ? "present" : "none: no maps, no Library");
    bool maps = false, plan = false;
    int lib = 0;
    if (sd) {
        for (const char* d : {SD_PATH_ROOT "/packs", SD_PATH_ROOT "/maps"}) {
            File f = g_sdStore.openDir(d);
            if (f) { File e = f.openNextFile(); maps = maps || (bool)e; if (e) e.close(); f.close(); }
        }
        File d = g_sdStore.openDir(kLibDir);
        if (d) {
            for (File e = d.openNextFile(); e; e = d.openNextFile()) {
                const char* nm = strrchr(e.name(), '/');
                nm = nm ? nm + 1 : e.name();
                if (!e.isDirectory() && nm[0] != '.') { lib++; plan = plan || !strncasecmp(nm, "Emergency", 9); }
                e.close();
            }
            d.close();
        }
    }
    tripAdd(maps, "Maps", maps ? "on the card (check your area: A)" : "none on the card");
    char hd[24];
    if (!g_home.set) tripAdd(false, "Home point", "not set (S.2 rows 14, 15)");
    else if (homeText(hd, sizeof(hd))) tripAdd(true, "Home point", "set, %s from here", hd);
    else tripAdd(true, "Home point", "set");
    int contacts = 0;
    for (const auto& node : g_announceMgr.nodes())
        if (node.hash.size() == 16 && node.saved && !isSelf(node.hash.data())) contacts++;
    tripAdd(contacts > 0, "Contacts", contacts ? "%d saved" : "none saved (meet a radio: Help-3)", contacts);
    tripAdd(g_ashareCount > 0, "Daily contact to", g_ashareCount ? "%d with auto-share" : "nobody: chat, MIC, row 2", g_ashareCount);
    tripAdd(g_offc.on, "Daily contact", g_offc.on ? "on, %02u:00 for %u min" : "off (S.5 row 13)",
            (unsigned)(g_offc.atMin / 60), (unsigned)g_offc.stayMin);
    tripAdd(foundCount() > 0, "If-found details", foundCount() ? "%d lines" : "empty (S.4, 2, row 9)", foundCount());
    tripAdd(plan, "Emergency plan", plan ? "in the Library (is it filled in?)" : lib ? "no Emergency-plan.txt" : "Library is empty");
    Serial.printf("[TRIP] %d checks, %d to do%c", g_tripN, g_tripTodo, 10);
    for (int i = 0; i < g_tripN; i++)
        Serial.printf("[TRIP] %-5s %s: %s%c", g_trip[i].ok ? "OK" : "TO DO", g_trip[i].name, g_trip[i].note, 10);
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
    emptyBatteryTick();
    contactTick();
    if (g_idleActive) return;
    const bool changed = chg != g_bat.charging || usb != g_bat.usb || wasValid != g_bat.valid ||
                         (g_screen == SCR_HOME && pct != g_bat.pct);
    if (changed && browseMode()) requestRedraw();
}

// ---- Held messages: for a contact that is switched off ---------------------------------
// A message to a saved contact that failed (no path) or got no delivery proof is kept here
// and sent again by itself when that contact's announce is heard (a radio announces when it
// starts, so a daily-contact start or a switch-on brings its held messages). Up to 8
// messages, 4 tries each, 14 days; NVS rs_held. Position / battery shares are not held (a
// newer one replaces them anyway). A held message can arrive twice when only the delivery
// proof was lost. Console "held" | "held on|off" | "held clear".
struct HeldMsg { uint8_t dest[16]; char text[161]; uint32_t at, seenRef; uint8_t tries, state; };   // state 1 = resent, result open
constexpr int kHeldCap = 8, kHeldTries = 4;
constexpr uint32_t kHeldMaxAgeS = 14UL * 86400UL;
HeldMsg g_held[kHeldCap];
int g_heldCount = 0;
bool g_heldOn = true;
struct HeldEv { uint8_t peer[16]; uint32_t counter; uint8_t status; };
HeldEv g_heldEv[8];
volatile uint8_t g_heldEvHead = 0, g_heldEvTail = 0;

void heldSave() {
    Preferences p;
    if (!p.begin("rs_held", false)) return;
    p.putBool("on", g_heldOn);
    p.putUChar("n", (uint8_t)g_heldCount);
    if (g_heldCount) p.putBytes("q", g_held, sizeof(HeldMsg) * (size_t)g_heldCount);
    else if (p.isKey("q")) p.remove("q");
    p.end();
}

void heldLoad() {
    Preferences p;
    if (!p.begin("rs_held", true)) return;
    g_heldOn = p.getBool("on", true);
    const int n = p.getUChar("n", 0);
    if (n > 0 && n <= kHeldCap && p.getBytes("q", g_held, sizeof(HeldMsg) * (size_t)n) == sizeof(HeldMsg) * (size_t)n)
        g_heldCount = n;
    p.end();
    for (int i = 0; i < g_heldCount; i++) { g_held[i].seenRef = 0; g_held[i].state = 0; g_held[i].text[160] = 0; }
    if (g_heldCount) Serial.printf("[HELD] %d message(s) waiting for their contact%c", g_heldCount, 10);
}

// The protocol's status callback: only notes it; heldTick does the work in the main loop.
void heldOnStatus(const std::string& peerHex, double, uint32_t counter, LXMFStatus status) {
    if (status != LXMFStatus::DELIVERED && status != LXMFStatus::FAILED && status != LXMFStatus::UNCONFIRMED) return;
    if (peerHex.size() != 32) return;
    const uint8_t next = (uint8_t)((g_heldEvHead + 1) % 8);
    if (next == g_heldEvTail) return;            // full: this one is not held
    HeldEv& e = g_heldEv[g_heldEvHead];
    for (int i = 0; i < 16; i++) {
        auto hx = [](char c) { return (uint8_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); };
        e.peer[i] = (uint8_t)(hx(peerHex[2 * i]) << 4 | hx(peerHex[2 * i + 1]));
    }
    e.counter = counter;
    e.status = (uint8_t)status;
    g_heldEvHead = next;
}

uint32_t heldSeen(const uint8_t dest[16]) {
    const DiscoveredNode* node = g_announceMgr.findNode(rs::Bytes(dest, 16));
    return node ? (uint32_t)node->lastSeen : 0;
}

void heldRemove(int i) {
    for (int j = i; j + 1 < g_heldCount; j++) g_held[j] = g_held[j + 1];
    g_heldCount--;
}

void heldPrint() {
    Serial.printf("[HELD] %s, %d waiting%c", g_heldOn ? "on" : "off", g_heldCount, 10);
    for (int i = 0; i < g_heldCount; i++) {
        char nm[24];
        peerName(g_held[i].dest, nm, sizeof(nm));
        Serial.printf("[HELD]  %d to %s, try %u, %s: \"%s\"%c", i + 1, nm, (unsigned)g_held[i].tries,
                      g_held[i].state ? "resent" : "waiting to hear them", g_held[i].text, 10);
    }
}

void heldEvent(const HeldEv& ev) {
    using namespace handheld::storage;
    if (isGrpHash(ev.peer) || isSelf(ev.peer) || !cardIsSaved(ev.peer)) return;
    int resent = 0;
    for (int i = 0; i < g_heldCount; i++) resent += !memcmp(g_held[i].dest, ev.peer, 16) && g_held[i].state == 1;
    const bool failed = ev.status != (uint8_t)LXMFStatus::DELIVERED;
    if (!failed && !resent) return;              // an ordinary delivery: nothing to look up
    RecordKey key{};
    memcpy(key.peer, ev.peer, 16);
    key.counter = ev.counter;
    key.incoming = false;
    StoredRecordHeader hdr;
    char text[161];
    size_t len = 0;
    if (!readRecordText(key, hdr, text, sizeof(text), len) || !text[0]) return;
    char nm[24];
    peerName(ev.peer, nm, sizeof(nm));
    int at = -1;
    for (int i = 0; i < g_heldCount; i++)
        if (!memcmp(g_held[i].dest, ev.peer, 16) && !strcmp(g_held[i].text, text)) { at = i; break; }
    if (!failed) {
        if (at < 0 || g_held[at].state != 1) return;
        Serial.printf("[HELD] delivered to %s after %u resend(s): \"%s\"%c", nm, (unsigned)g_held[at].tries, text, 10);
        toast("Held message reached %s", nm);
        heldRemove(at);
        heldSave();
        return;
    }
    static const char* const kSkip[] = {kLocPrefix, kLocLastPrefix, kLocAutoPrefix, kBatPrefix, kBatAutoPrefix};
    for (const char* k : kSkip) if (!strncmp(text, k, strlen(k))) return;
    if (at >= 0) {                               // a resend that failed again
        if (g_held[at].state != 1) return;
        g_held[at].state = 0;
        g_held[at].seenRef = heldSeen(ev.peer);
        if (g_held[at].tries >= kHeldTries) {
            Serial.printf("[HELD] gave up after %d tries, to %s: \"%s\"%c", kHeldTries, nm, text, 10);
            heldRemove(at);
        }
        heldSave();
        return;
    }
    if (g_heldCount >= kHeldCap) { Serial.println("[HELD] list full: the oldest is dropped"); heldRemove(0); }
    HeldMsg& h = g_held[g_heldCount++];
    memset(&h, 0, sizeof(h));
    memcpy(h.dest, ev.peer, 16);
    strlcpy(h.text, text, sizeof(h.text));
    h.at = (uint32_t)time(nullptr);
    h.seenRef = heldSeen(ev.peer);
    heldSave();
    Serial.printf("[HELD] kept for %s (%s): \"%s\"%c", nm,
                  ev.status == (uint8_t)LXMFStatus::FAILED ? "not reached" : "no delivery proof", text, 10);
    toast("Kept for %s: resends when heard", nm);
}

void heldTick() {
    static uint32_t last = 0;
    if (millis() - last < 2000 || !g_storeReady || !g_protoReady) return;
    last = millis();
    while (g_heldEvTail != g_heldEvHead) {
        const HeldEv ev = g_heldEv[g_heldEvTail];
        g_heldEvTail = (uint8_t)((g_heldEvTail + 1) % 8);
        if (g_heldOn) heldEvent(ev);
    }
    if (!g_heldOn || !g_heldCount) return;
    const uint32_t now = (uint32_t)time(nullptr);
    for (int i = 0; i < g_heldCount; i++) {
        HeldMsg& h = g_held[i];
        if (now > 1600000000UL && h.at > 1600000000UL && now - h.at > kHeldMaxAgeS) {
            Serial.printf("[HELD] too old, dropped: \"%s\"%c", h.text, 10);
            heldRemove(i);
            heldSave();
            return;
        }
        if (h.state) continue;
        const uint32_t seen = heldSeen(h.dest);
        if (!seen || seen == h.seenRef || millis() - seen > 120000UL) continue;   // not heard since
        char nm[24];
        peerName(h.dest, nm, sizeof(nm));
        if (!sendMessage(h.dest, h.text)) {
            if (!g_txTransient) h.seenRef = seen;    // refused for good this time: wait for the next announce
            return;
        }
        h.state = 1;
        h.tries++;
        h.seenRef = seen;
        Serial.printf("[HELD] %s was heard: resent (try %u): \"%s\"%c", nm, (unsigned)h.tries, h.text, 10);
        toast("%s is back: held message sent", nm);
        return;                                  // one per tick (3 s spacing between sends)
    }
}

void watchMessages() {
    heldTick();
    if (!g_storeReady) return;
    // Alert on newly saved incoming messages (not on a rising unread count: a save that also
    // removed old unread messages for space leaves the count flat and the alert was missed).
    static uint32_t lastIncoming = 0;
    static uint32_t lastRev = 0, lastRedraw = 0;
    const uint32_t incoming = g_msgStore.incomingCommitted();
    if (incoming != lastIncoming) lastIncoming = incoming;   // the alert itself: inboundTick()
    static int lastUnread = -1;
    const int unread = g_msgStore.totalUnreadCount();
    if (g_locked && !g_idleActive && unread != lastUnread) requestRedraw();   // lock prompt count
    lastUnread = unread;

    const uint32_t rev = g_msgStore.revision();
    if (rev == lastRev || g_idleActive) return;
    if (millis() - lastRedraw < 2500) return;
    if (g_threadOpen) {
        if (g_bufLen) return;            // never clobber a draft; retry later
        loadHistory(g_threadPeer, incoming != g_histMarkedAt);   // mark read only for new arrivals
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
    // Keys still waiting (the loop was busy drawing): the user is typing. Going idle here made
    // the first of them the wake key, which is swallowed - a lost letter (stress test 2026-11-16).
    if (g_keyqTail != g_keyqHead) return;
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
// Big global buffers in PSRAM (see PsramBuf.h): allocated here, the first thing setup does
// after PSRAM is up, before anything reads them.
void psramGlobals() {
    if (!g_track) g_track = psram<float, kTrackRam * 2>();
    if (!g_libText) g_libText = psram<char[kLibLineMax], kLibLines>();
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
    grpLoad();
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
    psramGlobals();
    bootMark("config, power, I2C");

    // Empty battery and no USB: do not start (and do not draw "BOOTING" over the idle screen).
    loadBatteryCal();
    bool panelShowsEmpty = false;
    const bool panelShowedEmpty = bootNvsGetU8("empty") == 2;
    const bool batOk = bootBatteryOk(panelShowsEmpty);
    // Switched off by the owner: only the top button (held) starts it. The panel still shows
    // the off screen, unless an empty battery wrote over it in between.
    const bool stayOff = batOk && offStaysOff();
    if (stayOff && !panelShowedEmpty) offSleep();

    // Boot screen first, so the unit shows it is starting (~1 s after power-on). The radio
    // shares this SPI bus; its own SPI.begin() later is then a no-op.
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    if (!batOk && panelShowsEmpty) batteryShutdown(false, true);   // the panel already says it
    if (!g_display.begin()) { Serial.println("[BOOT] display failed"); return false; }
    if (!LvglPort::begin(g_display)) { Serial.println("[BOOT] LVGL failed"); return false; }
    if (!batOk) batteryShutdown(true, true);
    if (stayOff) { offDrawScreen(); offSleep(); }
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
    if (sdMount()) Serial.printf("[SD] maps %s, library %s\n", g_haveMaps ? "yes" : "no", g_haveLib ? "yes" : "no");

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
    havoice::begin(&g_audio);
    havoice::setMicGain(g_cfg.micGain);
    walkie::begin(&g_audio);
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
    capLoad();
    readBattery();
    powerApply(true);                     // CPU clock, LoRa, WiFi, key light for the mode
    powerGpsTick();                       // decides whether the GPS starts powered
    gpsApplyPower();
    vaultWait();                          // custom password -> stays locked until typed
    lockLoadFails();
    if (lockHasPassword()) { g_locked = true; Serial.println("[LOCK] boot locked (password set)"); }
    bootNvsSetU8("lock", lockHasPassword() ? 1 : 0);   // the next boot screen shows LOCKED too
    if (vault::unlocked()) wifiLoad();    // else after unlock (lockTry)
    tcpLoad();
    ashareLoad();
    markLoad();
    cotLoad();
    layLoad();
    sosLoad();
    handheld::lxmf::g_onTelemetry = onTelemetry;
    cposLoad();
    wptLoad();
    geofLoad();
    bootMark("battery, power mode, GPS, vault, settings");
    mapview::setCoordFormatter(fmtCoord);
    g_proto.setMessageCallback(onCommittedMessage);
    g_proto.setStatusCallback(heldOnStatus);
    heldLoad();
    g_wifiNextAuto = millis() + 8000;    // auto-connect after the boot announce
    noteInput();
    pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);             // top side button (lock); has a pull-up on the board too
    g_btnRaw = g_btnDown = digitalRead(BOARD_BOOT_PIN) == LOW;   // held through the boot: not a press
    goScreen(minimalMode() ? SCR_INBOX : SCR_HOME);
    // A boot ends on the idle screen (its locked form with a password), like the top side
    // button: reset or button, the unit is left locked on the same screen. A key wakes it as
    // usual. Not on a new unit (the setup wizard shows) and not when a restore has a result to
    // show. Either way the first screen replaces the boot splash with a full refresh: a partial
    // one left the splash showing through it (after the side-button reset the panel's old-image
    // RAM did not hold the splash, so its pixels were never cleared).
    const bool bootWiz = g_wizNeeded || (g_cfgFresh && !g_hadIdentity);
    restoreReport();
    if (!bootWiz && !g_toast[0]) {
        enterIdle();
    } else {
        g_fullRefreshReq = true;
        requestRedraw();
        render();
    }
    soakLoad();                           // an unplugged soak test carries on after a restart
    Serial.println("[BOOT] ready");
    if (sysimg::pendingVerify()) g_markValidAt = millis() + 10000;   // new firmware: confirm if it keeps running
    if (bootWiz) wizStart();              // a new device (or unfinished)
    return true;
}

// Home Assistant voice: run the satellite, stop talking when MIC is let go, show what
// HA heard and answered.
// Wake word wanted now: the setting, power (USB only unless "always"), no walkie-talkie call
// (it needs the mic), not while locked (nothing is heard or answered on a locked unit).
bool haWakeWanted() {
    if (!g_cfg.haVoice || !feature(F_HAVOICE) || !g_cfg.haWake || g_locked) return false;
    if (g_cfg.haWake == 1 && !g_bat.usb) return false;
    return walkie::state() == walkie::State::Idle;
}

// Relay (Reticulum transport node): forward other units' messages and announces, so units
// out of each other's range reach each other through this one. Follows the setting and USB
// power; a change waits until it has held 10 s (no flapping on a loose cable).
void relayTick() {
    if (!g_protoReady) return;
    const bool want = g_cfg.relay == 2 || (g_cfg.relay == 1 && powerUsbPowered());
    static uint32_t since = 0;
    if (want == g_proto.relay()) { since = 0; return; }
    if (!since) { since = millis() | 1; return; }
    if (millis() - since < 10000) return;
    since = 0;
    if (g_proto.setRelay(want)) {
        Serial.printf("[RELAY] %s\n", want ? "on: relaying for other units" : "off");
        if (g_screen == SCR_SETTINGS && g_page == PG_LORA) requestRedraw();
    }
}

void haVoiceTick() {
    havoice::setWakeWord(haWakeWanted());
    havoice::setSensors(g_cfg.haVoice ? g_cfg.haSensors : 0);
    double la = 0, lo = 0;
    const bool pos = g_cfg.haSensors >= 2 && ownPosition(la, lo);
    havoice::setSensorValues((float)g_bat.pct, pos, la, lo);
    havoice::setQuiet(quietNow());
    havoice::tick(wifiConnected() && g_pwEff.wifi, g_cfg.haVoice && feature(F_HAVOICE), g_selfName, g_cfg.haKey);
    static uint32_t shown = 0;
    if (havoice::changedAt() != shown) {
        shown = havoice::changedAt();
        if (havoice::answer()[0] && !havoice::busy()) toast("HA: %.40s", havoice::answer());
        else if (havoice::heard()[0] && havoice::busy()) toast("Heard: %.36s", havoice::heard());
        else if (g_cfg.haVoice) toast("HA: %s", havoice::status());
        if (g_screen == SCR_SETTINGS && g_page == PG_WIFI) requestRedraw();
    }
}

void walkieTick() {
    walkie::tick();
    if (g_wtJoining) {                           // callee: on the caller's hotspot yet?
        if (WiFi.status() == WL_CONNECTED) {
            g_wtJoining = false;
            Serial.printf("[WT] on the hotspot as %s\n", WiFi.localIP().toString().c_str());
            if (!walkie::answer(g_wtIp, g_wtPort, g_wtKey)) toast("Walkie-talkie: %s", walkie::status());
            requestRedraw();
        } else if (millis() - g_wtJoinAt > 20000) {
            Serial.println("[WT] hotspot not reached");
            wtReleaseWifi();
            toast("Too far for WiFi: come closer");
            g_wtOpen = false;
            requestRedraw();
        }
    }
    // A call that ended (hung up here or there) gives the WiFi back.
    if (g_wtWifiOwned && !g_wtJoining && !g_wtIncoming && walkie::state() == walkie::State::Idle &&
        (!g_wtOpen || millis() - walkie::changedAt() > 3000)) wtReleaseWifi();
    static uint32_t shown = 0;
    if (walkie::changedAt() != shown) { shown = walkie::changedAt(); if (g_wtOpen) requestRedraw(); }
    if (g_wtIncoming && millis() - g_wtInviteAt > 60000) { g_wtIncoming = false; g_wtOpen = false; requestRedraw(); }
}

// Loop profiler: a step that holds the loop over 200 ms is logged ("[SLOW] watchPeers 850 ms"),
// so a 2-3 s pause between a key and the screen can be traced to its cause.
void slowNote(const char* name, uint32_t ms) {
    Serial.printf("[SLOW] %s %u ms\n", name, (unsigned)ms);
    soakNoteSlow(name, ms);
    if (!strcmp(name, "render")) return;   // the panel refresh: logged per frame already
    g_slow[g_slowHead] = {name, (uint16_t)(ms > 65535 ? 65535 : ms), millis() / 1000};
    g_slowHead = (uint8_t)((g_slowHead + 1) % 12);
    g_slowCount++;
}
#define SLOW_STEP(name, call) do { const uint32_t t_ = millis(); call; const uint32_t d_ = millis() - t_; if (d_ > 200) slowNote(name, d_); } while (0)

void loop() {
    SLOW_STEP("keys", processKeys());   // input first: lowest latency to the next render
    sideButtonTick();
    SLOW_STEP("soak", soakTick());
    SLOW_STEP("selftest", selftestTick());
    SLOW_STEP("pumpOnce", pumpOnce());
    SLOW_STEP("g_announceMgr.loop", g_announceMgr.loop());
    SLOW_STEP("g_audio.loop", g_audio.loop());
    SLOW_STEP("relay", relayTick());
    SLOW_STEP("havoice", haVoiceTick());
    SLOW_STEP("walkie", walkieTick());
    SLOW_STEP("cot", cotTick());
    SLOW_STEP("vm", vmTick());
    SLOW_STEP("gpsPoll", gpsPoll());
    SLOW_STEP("gpsWatchLink", gpsWatchLink());

    static uint32_t bootAnnounceAt = millis() + 3000;
    if (bootAnnounceAt && (int32_t)(millis() - bootAnnounceAt) >= 0) {
        bootAnnounceAt = 0;
        doAnnounce();
        if (g_screen == SCR_HOME && !g_idleActive) requestRedraw();
    }

    SLOW_STEP("watchBattery", watchBattery());
    SLOW_STEP("batLogTick", batLogTick());
    SLOW_STEP("saveEpochThrottled", saveEpochThrottled());
    SLOW_STEP("watchMessages", watchMessages());
    SLOW_STEP("watchPeers", watchPeers());
    SLOW_STEP("watchClockAndToast", watchClockAndToast());
    SLOW_STEP("watchWifi", watchWifi());
    SLOW_STEP("tcpTick", tcpTick());
    SLOW_STEP("meshTick", meshTick());
    SLOW_STEP("autoShareTick", autoShareTick());
    SLOW_STEP("inboundTick", inboundTick());
    SLOW_STEP("trackTick", trackTick());
    SLOW_STEP("navTick", navTick());
    SLOW_STEP("rtTick", rtTick());

    SLOW_STEP("processKeys", processKeys());         // again, in case a watcher above took a while
    SLOW_STEP("pollTouch", pollTouch());
    SLOW_STEP("pollSerial", pollSerial());
    SLOW_STEP("watchIdle", watchIdle());
    SLOW_STEP("powerTick", powerTick());

    if (!g_idleActive) {
        const bool debounced = g_redrawAt && (int32_t)(millis() - g_redrawAt) >= 0;
        if (g_redraw || debounced) {
            processKeys();  // fold any just-arrived keys into this frame
            SLOW_STEP("render", render());
        }
    }
    SLOW_STEP("mirrorTick", mirrorTick());
    powerIdle();            // (sleeps on purpose: not timed)
}

}  // namespace standalone
