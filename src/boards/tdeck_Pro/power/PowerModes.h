#pragma once
// Energy modes for the T-Deck Pro (Settings > Power).
//
// A mode is a Profile: a set of knobs the app applies to the hardware.
//   Full      everything on, CPU never sleeps (also used whenever USB power is in)
//   Balanced  160 MHz, light sleep between 1 s wake-ups, GPS every 15 min
//   Saver     80 MHz, light sleep between 60 s wake-ups, GPS hourly, no WiFi / key light
//   Custom    the user's own knob settings
// In every sleeping mode a key press, a touch or an incoming LoRa packet wakes the CPU at
// once, so typing and receiving stay immediate; what slows down is background work
// (clock / battery updates, GPS, protocol housekeeping).
//
// This file is policy only (no hardware calls), shared with the desktop simulator.
#include <stdint.h>

namespace power {

enum Mode : uint8_t { MODE_FULL, MODE_BALANCED, MODE_SAVER, MODE_CUSTOM, MODE_COUNT };
enum GpsUse : uint8_t { GPS_ON, GPS_15MIN, GPS_HOURLY, GPS_OFF, GPS_COUNT };

struct Profile {
    uint16_t cpuMhz;      // CPU clock while awake: 80, 160 or 240
    uint16_t wakeSecs;    // 0 = never sleep; else light-sleep, waking at least every N s
    uint16_t awakeSecs;   // stay awake this long after a key or touch before sleeping
    bool     lora;        // LoRa receiver on (off = no messages in or out)
    uint8_t  gps;         // GpsUse (only if GPS is enabled in Time & Location)
    bool     wifi;        // WiFi allowed (a connected WiFi keeps the CPU awake)
    bool     keyLight;    // key backlight allowed
};

struct Settings {
    uint8_t mode      = MODE_FULL;
    bool    fullOnUsb = true;     // USB power present -> Full, whatever the mode
    bool    customSet = false;    // custom profile has been initialised
    Profile custom    = {};
};

const Profile& preset(uint8_t mode);           // MODE_FULL / MODE_BALANCED / MODE_SAVER
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

void load(Settings& s);         // NVS "rs_power"
void save(const Settings& s);

}  // namespace power
