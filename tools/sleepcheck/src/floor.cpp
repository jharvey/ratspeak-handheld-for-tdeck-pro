#include "floor.h"

#include <Wire.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_sleep.h>

namespace floorcheck {
namespace {

constexpr uint8_t kGauge = 0x55, kCharger = 0x6B;
constexpr int kSda = 13, kScl = 14;
constexpr uint32_t kCycleS = 30;
constexpr uint32_t kMagic = 0x464C4F52;   // "FLOR"
constexpr int kCap = 120;

// Outputs driven low for the measurement: LoRa EN 46, GPS EN 39, audio rail 41, IO38 (T-RST
// on v1.1), IO45 (EPD front light on v1.1), key light 42, motor 2, LoRa CS 3 stays high.
const int kLowPins[] = {46, 39, 41, 38, 45, 42, 2, 43, 44};   // 43/44 = GPS UART (back-powers the GPS if high)

struct Sample { uint16_t mv; int16_t ma; uint8_t mode; uint8_t pad; uint16_t cycle; };

RTC_NOINIT_ATTR uint32_t g_magic;
RTC_NOINIT_ATTR uint8_t g_mode;          // 0 idle, 'l' light, 'd' deep
RTC_NOINIT_ATTR uint16_t g_cyclesLeft, g_cycle, g_count;
RTC_NOINIT_ATTR Sample g_log[kCap];

bool rd16(uint8_t addr, uint8_t reg, uint16_t& v) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)addr, 2) != 2) return false;
    v = (uint16_t)(Wire.read() | (Wire.read() << 8));
    return true;
}

bool update8(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)addr, 1) != 1) return false;
    const uint8_t r = (uint8_t)((Wire.read() & ~mask) | (val & mask));
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(r);
    return Wire.endTransmission() == 0;
}

void chargerInputOff(bool off) {
    update8(kCharger, 0x07, 0x30, off ? 0x00 : 0x10);   // I2C watchdog off / 40 s
    update8(kCharger, 0x00, 0x80, off ? 0x80 : 0x00);   // EN_HIZ
}

void railsLow() {
    // LoRa CS low too: with the LoRa rail (46) off, a high CS back-powers the SX1262 (+20 mA).
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);
    for (int p : kLowPins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
}

void holdRails(bool on) {
    for (int p : kLowPins) {
        if (on) gpio_hold_en((gpio_num_t)p); else gpio_hold_dis((gpio_num_t)p);
    }
    if (on) gpio_hold_en((gpio_num_t)3); else gpio_hold_dis((gpio_num_t)3);   // LoRa CS (RTC pin: hold survives resets)
    if (on) gpio_deep_sleep_hold_en(); else gpio_deep_sleep_hold_dis();
}

void logSample() {
    uint16_t mv = 0, cur = 0;
    rd16(kGauge, 0x08, mv);
    rd16(kGauge, 0x14, cur);
    if (g_count < kCap) g_log[g_count++] = {mv, (int16_t)cur, g_mode, 0, g_cycle};
}

void finish() {
    g_mode = 0;
    holdRails(false);
    chargerInputOff(false);
    Serial.println("[FLOOR] done - charger input back on");
}

void runCycles() {
    while (g_mode && g_cyclesLeft) {
        g_cyclesLeft--;
        g_cycle++;
        esp_sleep_enable_timer_wakeup((uint64_t)kCycleS * 1000000ULL);
        if (g_mode == 'd') {
            holdRails(true);
            esp_deep_sleep_start();             // wakes through a reboot -> boot()
        }
        esp_light_sleep_start();
        logSample();
    }
    if (g_mode) finish();
}

void print() {
    Serial.printf("[FLOOR] %u samples (mode %c, %u cycles left)\n", (unsigned)g_count,
                  g_mode ? (char)g_mode : '-', (unsigned)g_cyclesLeft);
    for (int i = 0; i < g_count; i++)
        Serial.printf("[FLOOR] %c cycle %u: %u mV %+d mA\n", (char)g_log[i].mode, (unsigned)g_log[i].cycle,
                      (unsigned)g_log[i].mv, (int)g_log[i].ma);
}

}  // namespace

void boot() {
    if (g_magic != kMagic) { g_magic = kMagic; g_mode = 0; g_count = g_cycle = g_cyclesLeft = 0; }
    Wire.begin(kSda, kScl);
    if (g_mode == 'd' && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        holdRails(false);
        railsLow();
        logSample();
        runCycles();                             // next deep sleep (or finish)
    } else if (g_mode) {
        finish();                                // interrupted run (reset / reflash)
    }
}

// BHI260AP sensor hub (I2C 0x28) on the always-on 1.8 V rail. LilyGO's factory firmware
// uploads a program into it; the suspicion was that it kept running after reflashing. It does
// not: boot status reads 0x18 (host interface ready, no firmware) = idle in its boot ROM, and
// a soft reset (host register 0x14 = 1) changes nothing on the floor (2026-11-10).
void bhiInfo(const char* tag) {
    uint8_t v[2] = {0xFF, 0xFF};
    const uint8_t regs[2] = {0x2B, 0x25};    // product id, boot status
    for (int i = 0; i < 2; i++) {
        Wire.beginTransmission(0x28);
        Wire.write(regs[i]);
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x28, 1) == 1) v[i] = Wire.read();
    }
    Serial.printf("[FLOOR] BHI260 %s: product 0x%02X boot status 0x%02X\n", tag, v[0], v[1]);
}

bool command(const char* line) {
    if (!strcmp(line, "bhi reset")) {
        bhiInfo("before");
        Wire.beginTransmission(0x28);
        Wire.write(0x14);
        Wire.write(0x01);
        Serial.printf("[FLOOR] BHI260 reset request: %s\n", Wire.endTransmission() == 0 ? "sent" : "no ACK");
        delay(100);
        bhiInfo("after");
        return true;
    }
    if (!strcmp(line, "bhi")) { bhiInfo("now"); return true; }
    if (strncmp(line, "floor", 5) != 0) return false;
    const char* a = line + 5;
    while (*a == ' ') a++;
    if (!*a) { print(); return true; }
    if (*a == 'x') { finish(); return true; }
    if (*a == 'l' || *a == 'd') {
        const int n = atoi(a + 1);
        g_mode = (uint8_t)*a;
        g_cyclesLeft = (uint16_t)(n > 0 && n < 100 ? n : 10);
        g_cycle = 0;
        g_count = 0;
        railsLow();
        chargerInputOff(true);
        Serial.printf("[FLOOR] %s sleep, %u cycles of %lu s, charger input off\n",
                      *a == 'd' ? "deep" : "light", (unsigned)g_cyclesLeft, (unsigned long)kCycleS);
        Serial.flush();
        delay(3000);                             // let the gauge see battery current
        logSample();
        runCycles();
        return true;
    }
    return false;
}

}  // namespace floorcheck
