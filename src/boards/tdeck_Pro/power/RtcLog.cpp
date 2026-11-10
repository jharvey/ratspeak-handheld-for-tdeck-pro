#include "RtcLog.h"

#include <Arduino.h>
#include <time.h>

#if defined(SIMULATOR)
#define RS_RTC_NOINIT
#else
#include <esp_attr.h>
#define RS_RTC_NOINIT RTC_NOINIT_ATTR
#endif

namespace rtclog {
namespace {

struct Entry {
    uint32_t epoch;     // wall clock (s), 0 = unknown
    uint32_t upS;       // uptime of that boot (s)
    uint32_t sleeps;    // light sleeps so far in that boot
    uint32_t sleptS;    // seconds asleep so far in that boot
    uint16_t mv;        // 0 = boot marker
    int16_t  ma;        // battery current (gauge average); boot marker: reset reason
    uint8_t  usb;
    uint8_t  pad[3];
};

constexpr uint32_t kMagic = 0x52534C47;   // "RSLG"
constexpr uint16_t kCap = 150;            // 150 x 24 B = 3.6 KB of the 8 KB RTC slow memory

RS_RTC_NOINIT uint32_t g_magic;
RS_RTC_NOINIT uint16_t g_head;
RS_RTC_NOINIT uint16_t g_count;
RS_RTC_NOINIT Entry    g_log[kCap];

void push(const Entry& e) {
    g_log[g_head] = e;
    g_head = (uint16_t)((g_head + 1) % kCap);
    if (g_count < kCap) g_count++;
}

uint32_t nowEpoch() {
    const time_t t = time(nullptr);
    return t > 100000 ? (uint32_t)t : 0;
}

}  // namespace

void clear() {
    g_magic = kMagic;
    g_head = g_count = 0;
}

void boot(int resetReason) {
    if (g_magic != kMagic || g_head >= kCap || g_count > kCap) clear();
    Entry e = {};
    e.epoch = nowEpoch();
    e.ma = (int16_t)resetReason;
    push(e);
}

void sample(uint16_t mv, int16_t ma, bool usb, uint32_t sleeps, uint32_t sleptS) {
    Entry e = {};
    e.epoch = nowEpoch();
    e.upS = millis() / 1000;
    e.sleeps = sleeps;
    e.sleptS = sleptS;
    e.mv = mv ? mv : 1;
    e.ma = ma;
    e.usb = usb ? 1 : 0;
    push(e);
}

void dump() {
    Serial.println("[RTCLOG] ---- begin (local time, uptime s, mV, mA, usb, sleeps, slept s) ----");
    const uint16_t start = (uint16_t)((g_head + kCap - g_count) % kCap);
    for (uint16_t i = 0; i < g_count; i++) {
        const Entry& e = g_log[(start + i) % kCap];
        char when[20] = "?";
        if (e.epoch) {
            const time_t t = (time_t)e.epoch;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(when, sizeof(when), "%m-%d %H:%M:%S", &tm);
        }
        if (e.mv == 0)
            Serial.printf("[RTCLOG] %s BOOT reset reason %d\n", when, (int)e.ma);
        else
            Serial.printf("[RTCLOG] %s up %lu %u mV %+d mA usb %u sleeps %lu slept %lu\n", when,
                          (unsigned long)e.upS, (unsigned)e.mv, (int)e.ma, (unsigned)e.usb,
                          (unsigned long)e.sleeps, (unsigned long)e.sleptS);
    }
    Serial.printf("[RTCLOG] ---- %u entries ----\n", (unsigned)g_count);
}

}  // namespace rtclog
