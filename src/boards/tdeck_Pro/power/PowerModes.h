#pragma once
// Energy modes for the T-Deck Pro (Settings > Power).
//
// A mode is a Profile: a set of knobs the app applies to the hardware.
//   Full       everything on, CPU never sleeps (also used whenever USB power is in)
//   Balanced   light sleep between 1 s wake-ups, GPS every 15 min (the default)
//   Saver      light sleep between 60 s wake-ups, GPS hourly, no WiFi / key light
//   Ultra-low  LoRa listening and nothing else: 5 min wake-ups, GPS / WiFi / lights off
//   On demand  radios off until used: a key turns LoRa on for 2 min (send, replies), and
//              GPS takes a fix if the last one is over 15 min old
//   Check-in   radios off except a scheduled window (e.g. every hour at :30): LoRa on from
//              1 min before to 3 min after; messages written meanwhile wait for the window,
//              and clocks are nudged from peers' announces / messages heard in it
//   Custom     the user's own knob settings
// In every sleeping mode a key press, a touch or an incoming LoRa packet wakes the CPU at
// once, so typing and receiving stay immediate; what slows down is background work
// (clock / battery updates, GPS, protocol housekeeping).
//
// This file is policy only (no hardware calls), shared with the desktop simulator.
#include <stdint.h>

namespace power {

enum Mode : uint8_t { MODE_FULL, MODE_BALANCED, MODE_SAVER, MODE_ULTRA, MODE_ONDEMAND, MODE_CHECKIN,
                     MODE_CUSTOM, MODE_COUNT };
// GPS_ON_USE: one visit when a key is pressed, at most every 15 min.
enum GpsUse : uint8_t { GPS_ON, GPS_15MIN, GPS_HOURLY, GPS_OFF, GPS_ON_USE, GPS_COUNT };
constexpr uint32_t kLoraOnUseMs = 120000;      // LoRa stays on this long after a key (on use)

struct Profile {
    uint16_t cpuMhz;      // CPU clock while awake: 80, 160 or 240
    uint16_t wakeSecs;    // 0 = never sleep; else light-sleep, waking at least every N s
    uint16_t awakeSecs;   // stay awake this long after a key or touch before sleeping
    bool     lora;        // LoRa receiver on (off = no messages in or out)
    uint8_t  gps;         // GpsUse (only if GPS is enabled in Time & Location)
    bool     wifi;        // WiFi allowed (a connected WiFi keeps the CPU awake)
    bool     keyLight;    // key backlight allowed
    bool     touchOnKey;  // touch held in reset; a key press enables it for 1 min (~2 mA)
    bool     loraOnUse;   // lora off: power it for kLoraOnUseMs after each key press
    bool     checkin;     // lora off: power it only in the scheduled check-in windows
};

// Check-in schedule: windows at checkinMin past midnight local time + k * checkinEvery.
constexpr uint32_t kCheckinLeadS = 60;      // LoRa on this early (clock differences)
constexpr uint32_t kCheckinWindowS = 180;   // and this long after the check-in time
constexpr uint32_t kCheckinSendS = 20;      // queued messages go this long after it

struct Settings {
    uint8_t mode      = MODE_BALANCED;
    bool    fullOnUsb = true;     // USB power present -> Full, whatever the mode
    bool    customSet = false;    // custom profile has been initialised
    uint8_t checkinMin = 30;      // check-in minute (0-59)
    uint16_t checkinEvery = 60;   // minutes between check-ins (15 ... 1440)
    Profile custom    = {};
};

const Profile& preset(uint8_t mode);           // any mode below MODE_CUSTOM
const Profile& selected(const Settings& s);    // the chosen mode's profile (custom included)
// The profile in force: Full while on USB power (if fullOnUsb), else the chosen one.
const Profile& effective(const Settings& s, bool usbPower);

const char* modeName(uint8_t mode);
const char* modeBlurb(uint8_t mode);           // one-line summary for the settings row
const char* gpsName(uint8_t gps);
uint32_t    gpsIntervalMs(uint8_t gps);        // 0 = continuous or off
constexpr uint32_t kGpsWindowMs = 180000;      // periodic GPS: on for up to 3 min per visit

// Knob steppers (Enter on a Custom row moves to the next value, wrapping).
uint16_t nextCpu(uint16_t mhz);
uint16_t nextWake(uint16_t secs);
uint16_t nextAwake(uint16_t secs);
void     fmtWake(char* out, unsigned n, uint16_t secs);   // "never", "1 s", "1 min"

bool sameProfile(const Profile& a, const Profile& b);

// Where secOfDay (local) is in the check-in cycle: inWindow, seconds until that changes,
// and seconds since the check-in time (only meaningful in the window; can be "negative" in
// the lead minute, returned as -1..-60 via sinceS).
struct CheckinPhase { bool inWindow; uint32_t toChangeS; int32_t sinceS; uint32_t toNextS; };
CheckinPhase checkinPhase(const Settings& s, uint32_t secOfDay);
uint8_t  nextCheckinMin(uint8_t m);          // +5 min, wrapping
uint16_t nextCheckinEvery(uint16_t mins);    // 15, 30, 60, 120, 180, 360, 720, 1440

// Estimated average battery draw (mA) of a profile, from bench measurements on the
// T-Deck Pro (2026-11-09/10); WiFi traffic and LoRa activity are not included.
uint16_t estimateMa(const Profile& p);
// Runtime text for a full battery of capacityMah: "~12 h" or "~1.2 days".
void fmtRuntime(char* out, unsigned n, uint16_t capacityMah, uint16_t ma);

void load(Settings& s);         // NVS "rs_power"
void save(const Settings& s);

}  // namespace power
