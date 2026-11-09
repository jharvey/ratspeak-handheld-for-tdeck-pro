#include "PowerModes.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

namespace power {
namespace {

//                         cpu  wake awake  lora  gps         wifi   keyLight
const Profile kFull     = {240,   0,   0,  true, GPS_ON,     true,  true};
const Profile kBalanced = {160,   1,  15,  true, GPS_15MIN,  true,  true};
const Profile kSaver    = { 80,  60,   5,  true, GPS_HOURLY, false, false};

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
        case MODE_CUSTOM:   return "Custom";
        default:            return "?";
    }
}

const char* modeBlurb(uint8_t mode) {
    switch (mode) {
        case MODE_FULL:     return "always awake, all on";
        case MODE_BALANCED: return "160MHz wake 1s GPS 15m";
        case MODE_SAVER:    return "80MHz wake 60s GPS 1h";
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
        default:         return "?";
    }
}

uint32_t gpsIntervalMs(uint8_t gps) {
    switch (gps) {
        case GPS_15MIN:  return 15UL * 60000UL;
        case GPS_HOURLY: return 60UL * 60000UL;
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
           a.lora == b.lora && a.gps == b.gps && a.wifi == b.wifi && a.keyLight == b.keyLight;
}

void load(Settings& s) {
    s = Settings{};
    s.custom = kBalanced;
    Preferences p;
    if (!p.begin("rs_power", true)) return;
    s.mode      = p.getUChar("mode", MODE_FULL);
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
    p.end();
    if (s.mode >= MODE_COUNT) s.mode = MODE_FULL;
    if (!inSteps(kCpuSteps, c.cpuMhz))      c.cpuMhz = kBalanced.cpuMhz;
    if (!inSteps(kWakeSteps, c.wakeSecs))   c.wakeSecs = kBalanced.wakeSecs;
    if (!inSteps(kAwakeSteps, c.awakeSecs)) c.awakeSecs = kBalanced.awakeSecs;
    if (c.gps >= GPS_COUNT)                 c.gps = kBalanced.gps;
}

void save(const Settings& s) {
    Preferences p;
    if (!p.begin("rs_power", false)) return;
    p.putUChar("mode", s.mode);
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
    p.end();
}

}  // namespace power
