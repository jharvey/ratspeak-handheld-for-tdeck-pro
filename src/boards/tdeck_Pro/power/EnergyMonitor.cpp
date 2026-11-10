#include "EnergyMonitor.h"

#include <Arduino.h>
#include <string.h>

#if defined(SIMULATOR)
#define RS_RTC_NOINIT
#else
#include <esp_attr.h>
#define RS_RTC_NOINIT RTC_NOINIT_ATTR
#endif

namespace energy {
namespace {

constexpr uint32_t kMagic = 0x454E5247;   // "ENRG"
constexpr int kCap = 48;

// Bench currents (mA at the battery), see RatSpeak_TDeck-Pro_Description.md section 6.4.
constexpr float kBoard      = 5.0f;    // always-on hardware (ESP32 off, rails off)
constexpr float kCpuAsleep  = 1.5f;    // ESP32 + PSRAM in light sleep
constexpr float kCpu80      = 23.0f;   // awake, by clock (240 MHz: 48 mA total awake with LoRa + touch)
constexpr float kCpu160     = 29.0f;
constexpr float kCpu240     = 35.0f;
constexpr float kLoraRxAwake  = 6.0f;  // receiver while the CPU is awake (bench: ~8 incl. SPI)
constexpr float kLoraRxAsleep = 2.0f;
constexpr float kGps        = 19.0f;   // measured 67 vs 48 mA awake
constexpr float kWifi       = 30.0f;
constexpr float kKeyLight   = 15.0f;
constexpr float kTouch      = 2.0f;
constexpr float kAudio      = 18.0f;
constexpr float kSd         = 2.0f;

struct Acc {
    float    mAs[CAT_COUNT];
    float    measuredMas;
    uint32_t measuredMs;
    uint32_t awakeMs;
    uint32_t totalMs;
    uint32_t startMs;            // millis() at the period start
};

RS_RTC_NOINIT uint32_t g_magic;
RS_RTC_NOINIT uint32_t g_periodS;
RS_RTC_NOINIT uint16_t g_head, g_count;
RS_RTC_NOINIT Record   g_ring[kCap];
RS_RTC_NOINIT Acc      g_acc;

Record g_empty = {};

uint16_t tenths(float mAs) {
    const float mAh10 = mAs / 360.0f;        // mA*s -> mAh*10
    return mAh10 < 0 ? 0 : (mAh10 > 65535.0f ? 65535 : (uint16_t)(mAh10 + 0.5f));
}

Record makeRecord(const Acc& a, uint32_t epoch, int battPct) {
    Record r = {};
    r.epoch = epoch;
    r.lenS = a.totalMs / 1000;
    r.measuredS = a.measuredMs / 1000;
    r.measured10 = tenths(a.measuredMas);
    for (int c = 0; c < CAT_COUNT; c++) r.est10[c] = tenths(a.mAs[c]);
    r.awakePct = a.totalMs ? (uint8_t)((uint64_t)a.awakeMs * 100 / a.totalMs) : 0;
    r.battPct = battPct < 0 ? 0 : (uint8_t)battPct;
    return r;
}

void resetAcc(uint32_t nowMs) {
    memset(&g_acc, 0, sizeof(g_acc));
    g_acc.startMs = nowMs;
}

}  // namespace

const char* catName(uint8_t cat) {
    static const char* const kNames[CAT_COUNT] = {"CPU", "Board base", "LoRa listen", "LoRa send", "GPS",
                                                  "WiFi", "Display", "Keys + touch", "Audio", "SD card"};
    return cat < CAT_COUNT ? kNames[cat] : "?";
}

float stateMa(const State& s) {
    float ma = kBoard;
    ma += s.awake ? (s.cpuMhz >= 240 ? kCpu240 : s.cpuMhz >= 160 ? kCpu160 : kCpu80) : kCpuAsleep;
    if (s.loraRx) ma += s.awake ? kLoraRxAwake : kLoraRxAsleep;
    if (s.gps) ma += kGps;
    if (s.wifi) ma += kWifi;
    if (s.keyLight) ma += kKeyLight;
    if (s.touch) ma += kTouch;
    if (s.audio) ma += kAudio;
    if (s.sd) ma += kSd;
    return ma;
}

void begin() {
    if (g_magic != kMagic || g_head >= kCap || g_count > kCap || g_periodS < 60 || g_periodS > 86400) {
        g_magic = kMagic;
        g_periodS = 3600;
        g_head = g_count = 0;
        resetAcc(millis());
    } else {
        g_acc.startMs = millis() - (g_acc.totalMs < millis() ? g_acc.totalMs : 0);   // continue the period
    }
}

void account(uint32_t dtMs, const State& s) {
    if (!dtMs) return;
    const float sec = dtMs / 1000.0f;
    g_acc.totalMs += dtMs;
    if (s.awake) g_acc.awakeMs += dtMs;
    g_acc.mAs[E_BOARD] += kBoard * sec;
    g_acc.mAs[E_CPU] += (s.awake ? (s.cpuMhz >= 240 ? kCpu240 : s.cpuMhz >= 160 ? kCpu160 : kCpu80) : kCpuAsleep) * sec;
    if (s.loraRx) g_acc.mAs[E_LORA_RX] += (s.awake ? kLoraRxAwake : kLoraRxAsleep) * sec;
    if (s.gps) g_acc.mAs[E_GPS] += kGps * sec;
    if (s.wifi) g_acc.mAs[E_WIFI] += kWifi * sec;
    if (s.keyLight) g_acc.mAs[E_KEYS_TOUCH] += kKeyLight * sec;
    if (s.touch) g_acc.mAs[E_KEYS_TOUCH] += kTouch * sec;
    if (s.audio) g_acc.mAs[E_AUDIO] += kAudio * sec;
    if (s.sd) g_acc.mAs[E_SD] += kSd * sec;
}

void addEvent(uint8_t cat, float mAs) {
    if (cat < CAT_COUNT && mAs > 0) g_acc.mAs[cat] += mAs;
}

void addMeasured(int16_t avgMa, uint32_t dtMs) {
    if (avgMa >= 0 || !dtMs) return;              // charging / USB: not a battery measurement
    g_acc.measuredMas += (-avgMa) * (dtMs / 1000.0f);
    g_acc.measuredMs += dtMs;
}

void tick(uint32_t nowMs, uint32_t epoch, int battPct) {
    if (g_acc.totalMs < g_periodS * 1000UL) return;
    g_ring[g_head] = makeRecord(g_acc, epoch, battPct);
    g_head = (uint16_t)((g_head + 1) % kCap);
    if (g_count < kCap) g_count++;
    resetAcc(nowMs);
}

void setPeriod(uint32_t seconds) {
    g_periodS = seconds < 60 ? 60 : (seconds > 86400 ? 86400 : seconds);
}

uint32_t period() { return g_periodS; }
int count() { return g_count; }

const Record& closed(int i) {
    if (i < 0 || i >= g_count) return g_empty;
    return g_ring[(g_head + kCap - 1 - i) % kCap];
}

Record current(uint32_t, int battPct) { return makeRecord(g_acc, 0, battPct); }

void clear() {
    g_head = g_count = 0;
    resetAcc(millis());
}

void dump() {
    Serial.printf("[ENERGY] period %lu s, %d closed records (newest first), mAh\n", (unsigned long)g_periodS, g_count);
    auto line = [](const char* tag, const Record& r) {
        float est = 0;
        for (int c = 0; c < CAT_COUNT; c++) est += r.est10[c] / 10.0f;
        Serial.printf("[ENERGY] %s len %lus awake %u%% bat %u%% measured %.1f (%lus) est %.1f :", tag,
                      (unsigned long)r.lenS, (unsigned)r.awakePct, (unsigned)r.battPct, r.measured10 / 10.0f,
                      (unsigned long)r.measuredS, est);
        for (int c = 0; c < CAT_COUNT; c++)
            if (r.est10[c]) Serial.printf(" %s %.1f", catName(c), r.est10[c] / 10.0f);
        Serial.println();
    };
    line("now ", current(0, -1));
    for (int i = 0; i < g_count; i++) {
        char tag[8];
        snprintf(tag, sizeof(tag), "-%-3d", i + 1);
        line(tag, closed(i));
    }
}

}  // namespace energy
