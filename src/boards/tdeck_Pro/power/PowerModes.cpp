#include "PowerModes.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

namespace power {
namespace {

//                         cpu  wake awake  lora   gps         wifi   keyLight touchOnKey loraOnUse checkin
const Profile kFull     = {240,   0,   0,  true,  GPS_ON,     true,  true,    false,     false,    false};
const Profile kBalanced = {240,   1,  15,  true,  GPS_15MIN,  true,  true,    false,     false,    false};
const Profile kSaver    = {240,  60,   5,  true,  GPS_HOURLY, false, false,   true,      false,    false};
const Profile kUltra    = {240, 300,   5,  true,  GPS_OFF,    false, false,   true,      false,    false};
const Profile kOnDemand = {240, 300,  10,  false, GPS_ON_USE, false, false,   true,      true,     false};
const Profile kCheckin  = {240, 300,  10,  false, GPS_ON_USE, false, false,   true,      false,    true};

const uint16_t kEverySteps[] = {15, 30, 60, 120, 180, 360, 720, 1440};

const uint16_t kCpuSteps[]   = {240, 160, 80};
const uint16_t kWakeSteps[]  = {0, 1, 5, 10, 30, 60, 300};
const uint16_t kAwakeSteps[] = {3, 5, 10, 15, 30, 60};

template <size_t N>
uint16_t stepNext(const uint16_t (&steps)[N], uint16_t cur) {
    for (size_t i = 0; i < N; i++)
        if (steps[i] == cur) return steps[(i + 1) % N];
    return steps[0];
}

template <size_t N>
bool inSteps(const uint16_t (&steps)[N], uint16_t v) {
    for (size_t i = 0; i < N; i++) if (steps[i] == v) return true;
    return false;
}

}  // namespace

const Profile& preset(uint8_t mode) {
    switch (mode) {
        case MODE_BALANCED: return kBalanced;
        case MODE_SAVER:    return kSaver;
        case MODE_ULTRA:    return kUltra;
        case MODE_ONDEMAND: return kOnDemand;
        case MODE_CHECKIN:  return kCheckin;
        default:            return kFull;
    }
}

const Profile& selected(const Settings& s) {
    return s.mode == MODE_CUSTOM ? s.custom : preset(s.mode);
}

const Profile& effective(const Settings& s, bool usbPower) {
    if (usbPower && s.fullOnUsb) return kFull;
    return selected(s);
}

const char* modeName(uint8_t mode) {
    switch (mode) {
        case MODE_FULL:     return "Full power";
        case MODE_BALANCED: return "Balanced";
        case MODE_SAVER:    return "Saver";
        case MODE_ULTRA:    return "Ultra-low";
        case MODE_ONDEMAND: return "On demand";
        case MODE_CHECKIN:  return "Check-in";
        case MODE_CUSTOM:   return "Custom";
        default:            return "?";
    }
}

const char* modeBlurb(uint8_t mode) {
    switch (mode) {
        case MODE_FULL:     return "always awake, all on";
        case MODE_BALANCED: return "wake 1s, GPS 15m";
        case MODE_SAVER:    return "wake 60s, GPS 1h";
        case MODE_ULTRA:    return "LoRa only, wake 5 min";
        case MODE_ONDEMAND: return "radios on when used";
        case MODE_CHECKIN:  return "LoRa at set times";
        case MODE_CUSTOM:   return "your own settings";
        default:            return "";
    }
}

const char* gpsName(uint8_t gps) {
    switch (gps) {
        case GPS_ON:     return "Always on";
        case GPS_15MIN:  return "Every 15 min";
        case GPS_HOURLY: return "Every hour";
        case GPS_OFF:    return "Off";
        case GPS_ON_USE: return "When used";
        default:         return "?";
    }
}

uint32_t gpsIntervalMs(uint8_t gps) {
    switch (gps) {
        case GPS_15MIN:  return 15UL * 60000UL;
        case GPS_HOURLY: return 60UL * 60000UL;
        case GPS_ON_USE: return 15UL * 60000UL;  // at most one visit per 15 min
        default:         return 0;
    }
}

uint16_t nextCpu(uint16_t mhz)    { return stepNext(kCpuSteps, mhz); }
uint16_t nextWake(uint16_t secs)  { return stepNext(kWakeSteps, secs); }
uint16_t nextAwake(uint16_t secs) { return stepNext(kAwakeSteps, secs); }

void fmtWake(char* out, unsigned n, uint16_t secs) {
    if (secs == 0)          snprintf(out, n, "never sleep");
    else if (secs % 60 == 0) snprintf(out, n, "%u min", (unsigned)(secs / 60));
    else                    snprintf(out, n, "%u s", (unsigned)secs);
}

bool sameProfile(const Profile& a, const Profile& b) {
    return a.cpuMhz == b.cpuMhz && a.wakeSecs == b.wakeSecs && a.awakeSecs == b.awakeSecs &&
           a.lora == b.lora && a.gps == b.gps && a.wifi == b.wifi && a.keyLight == b.keyLight &&
           a.touchOnKey == b.touchOnKey && a.loraOnUse == b.loraOnUse && a.checkin == b.checkin;
}

CheckinPhase checkinPhase(const Settings& s, uint32_t secOfDay) {
    const uint32_t period = (uint32_t)(s.checkinEvery ? s.checkinEvery : 60) * 60u;
    const uint32_t offset = ((uint32_t)s.checkinMin * 60u) % period;
    const uint32_t phase = (secOfDay % 86400u + 86400u - offset) % period;   // since check-in time
    CheckinPhase r;
    if (phase < kCheckinWindowS) {
        r = {true, kCheckinWindowS - phase, (int32_t)phase, period - phase};
    } else if (phase >= period - kCheckinLeadS) {
        r = {true, period - phase + kCheckinWindowS, -(int32_t)(period - phase), period - phase};
    } else {
        r = {false, period - kCheckinLeadS - phase, 0, period - phase};
    }
    return r;
}

uint8_t nextCheckinMin(uint8_t m) { return (uint8_t)((m / 5 * 5 + 5) % 60); }
uint16_t nextCheckinEvery(uint16_t mins) { return stepNext(kEverySteps, mins); }

uint16_t estimateMa(const Profile& p) {
    // Measured 2026-11-10 after the GPS UART, audio and front-light fixes: awake at 240 MHz
    // with LoRa listening and touch on ~48 mA (CPU ~35, -12 at 80 MHz), GPS ~19 mA more.
    // Asleep: ~8 mA floor with touch on, LoRa listening ~+1-2 mA, waking every 1 s ~+1 mA;
    // touch held in reset saves ~2 mA. A powered GPS keeps the CPU awake.
    const bool sleeps = p.wakeSecs > 0 && p.gps != GPS_ON;
    uint32_t ma;
    if (!sleeps) {
        ma = 7 + (p.cpuMhz >= 240 ? 35 : p.cpuMhz >= 160 ? 29 : 23) + (p.lora ? 6 : 0);
    } else {
        ma = 6 + (p.touchOnKey ? 0 : 2) + (p.lora ? 2 : 0) + (p.wakeSecs <= 1 ? 1 : 0);
        if (p.wakeSecs >= 300 && ma > 6) ma -= 1;     // few wake-ups: measured ~7 mA with LoRa
    }
    switch (p.gps) {
        case GPS_ON:     ma += 19; break;
        case GPS_15MIN:  ma += 1; break;     // ~1 min visits every 15 min
        case GPS_HOURLY: ma += 1; break;
        default: break;
    }
    return (uint16_t)ma;
}

void fmtRuntime(char* out, unsigned n, uint16_t capacityMah, uint16_t ma) {
    if (!ma) { snprintf(out, n, "?"); return; }
    const uint32_t tenthsH = (uint32_t)capacityMah * 10u / ma;     // hours x10
    if (tenthsH < 360) snprintf(out, n, "~%lu h", (unsigned long)((tenthsH + 5) / 10));
    else               snprintf(out, n, "~%lu.%lu days", (unsigned long)(tenthsH / 240),
                                (unsigned long)((tenthsH % 240) / 24));
}

void load(Settings& s) {
    s = Settings{};
    s.custom = kBalanced;
    Preferences p;
    if (!p.begin("rs_power", true)) return;
    // "mode2" holds the 6-mode numbering; the old "mode" key had Custom at 3.
    if (p.isKey("mode2")) {
        s.mode = p.getUChar("mode2", MODE_BALANCED);
    } else {
        const uint8_t old = p.getUChar("mode", MODE_BALANCED);
        s.mode = old == 3 ? MODE_CUSTOM : old;
    }
    s.fullOnUsb = p.getBool("usbFull", true);
    s.customSet = p.getBool("cSet", false);
    Profile& c = s.custom;
    c.cpuMhz    = p.getUShort("cCpu", kBalanced.cpuMhz);
    c.wakeSecs  = p.getUShort("cWake", kBalanced.wakeSecs);
    c.awakeSecs = p.getUShort("cAwake", kBalanced.awakeSecs);
    c.lora      = p.getBool("cLora", kBalanced.lora);
    c.gps       = p.getUChar("cGps", kBalanced.gps);
    c.wifi      = p.getBool("cWifi", kBalanced.wifi);
    c.keyLight  = p.getBool("cKbl", kBalanced.keyLight);
    c.touchOnKey = p.getBool("cTouch", kBalanced.touchOnKey);
    c.loraOnUse = p.getBool("cLoraUse", kBalanced.loraOnUse);
    c.checkin   = false;                       // check-in is a mode of its own, not a knob
    s.checkinMin   = p.getUChar("ciMin", 30);
    s.checkinEvery = p.getUShort("ciEvery", 60);
    p.end();
    if (s.mode >= MODE_COUNT) s.mode = MODE_BALANCED;
    if (!inSteps(kCpuSteps, c.cpuMhz))      c.cpuMhz = kBalanced.cpuMhz;
    if (!inSteps(kWakeSteps, c.wakeSecs))   c.wakeSecs = kBalanced.wakeSecs;
    if (!inSteps(kAwakeSteps, c.awakeSecs)) c.awakeSecs = kBalanced.awakeSecs;
    if (c.gps >= GPS_COUNT)                 c.gps = kBalanced.gps;
    if (s.checkinMin > 59)                  s.checkinMin = 30;
    if (!inSteps(kEverySteps, s.checkinEvery)) s.checkinEvery = 60;
}

void save(const Settings& s) {
    Preferences p;
    if (!p.begin("rs_power", false)) return;
    p.putUChar("mode2", s.mode);
    p.putBool("usbFull", s.fullOnUsb);
    p.putBool("cSet", s.customSet);
    const Profile& c = s.custom;
    p.putUShort("cCpu", c.cpuMhz);
    p.putUShort("cWake", c.wakeSecs);
    p.putUShort("cAwake", c.awakeSecs);
    p.putBool("cLora", c.lora);
    p.putUChar("cGps", c.gps);
    p.putBool("cWifi", c.wifi);
    p.putBool("cKbl", c.keyLight);
    p.putBool("cTouch", c.touchOnKey);
    p.putBool("cLoraUse", c.loraOnUse);
    p.putUChar("ciMin", s.checkinMin);
    p.putUShort("ciEvery", s.checkinEvery);
    p.end();
}

}  // namespace power
