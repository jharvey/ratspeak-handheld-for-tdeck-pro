// T-Deck Pro light-sleep check: does a bare Arduino sketch survive esp_light_sleep_start()
// on this board? LoRa and GPS stay unpowered. Counters live in RTC memory, so they survive
// a reset and are printed at every boot together with the reset reason.
//
// Console (115200):  s <ms>   one timer light sleep
//                    loop     sleep 2 s, stay awake 3 s, repeat (any key stops)
//                    r        print counters        c   clear counters
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include "floor.h"

namespace {
constexpr int kGpsEn = 39, kLoraEn = 46;
constexpr uint32_t kMagic = 0x534C4350;   // "SLCP"

RTC_NOINIT_ATTR uint32_t g_magic;
RTC_NOINIT_ATTR uint32_t g_boots;
RTC_NOINIT_ATTR uint32_t g_sleepsStarted;
RTC_NOINIT_ATTR uint32_t g_sleepsDone;
RTC_NOINIT_ATTR uint32_t g_lastMs;          // length of the last sleep started

bool g_loop = false;
uint32_t g_nextSleep = 0;

void report(const char* why) {
    Serial.printf("[SLEEPCHECK] %s: boots=%lu reset=%d sleeps started=%lu done=%lu last=%lu ms uptime=%lu s\n", why,
                  (unsigned long)g_boots, (int)esp_reset_reason(), (unsigned long)g_sleepsStarted,
                  (unsigned long)g_sleepsDone, (unsigned long)g_lastMs, (unsigned long)(millis() / 1000));
}

void sleepOnce(uint32_t ms, bool gpio = false) {
    Serial.printf("[SLEEPCHECK] light sleep %lu ms...\n", (unsigned long)ms);
    Serial.flush();
    delay(20);
    g_lastMs = ms;
    g_sleepsStarted++;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    if (gpio) esp_sleep_enable_gpio_wakeup();
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = esp_light_sleep_start();
    const int64_t dt = esp_timer_get_time() - t0;
    g_sleepsDone++;
    Serial.printf("[SLEEPCHECK] woke: err=%d cause=%d slept=%lld ms\n", (int)err, (int)esp_sleep_get_wakeup_cause(),
                  (long long)(dt / 1000));
}
}  // namespace

void setup() {
    floorcheck::boot();                  // continues a deep-sleep floor run
    pinMode(kLoraEn, OUTPUT);
    digitalWrite(kLoraEn, LOW);
    pinMode(kGpsEn, OUTPUT);
    digitalWrite(kGpsEn, LOW);
    if (g_magic != kMagic) { g_magic = kMagic; g_boots = g_sleepsStarted = g_sleepsDone = g_lastMs = 0; }
    g_boots++;
    Serial.begin(115200);
    delay(1500);
    report("boot");
    Serial.printf("[SLEEPCHECK] libs: wifi mode %d, fs %d\n", (int)WiFi.getMode(), (int)LittleFS.totalBytes());
}

void loop() {
    static char line[32];
    static size_t n = 0;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (g_loop) { g_loop = false; Serial.println("[SLEEPCHECK] loop stopped"); }
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            n = 0;
            if (floorcheck::command(line)) {}
            else if (line[0] == 's' && line[1] == ' ') sleepOnce((uint32_t)atoi(line + 2));
            else if (line[0] == 'g' && line[1] == ' ') sleepOnce((uint32_t)atoi(line + 2), true);
            else if (line[0] == 'p' && line[1] == ' ') {
                // PSRAM in use across a light sleep: fill 1 MB, sleep, verify.
                const size_t n = 1024 * 1024;
                uint32_t* buf = (uint32_t*)ps_malloc(n);
                Serial.printf("[SLEEPCHECK] psram found=%d size=%lu buf=%p\n", (int)psramFound(),
                              (unsigned long)ESP.getPsramSize(), buf);
                if (buf) {
                    for (size_t i = 0; i < n / 4; i++) buf[i] = 0xA5A50000u ^ (uint32_t)i;
                    sleepOnce((uint32_t)atoi(line + 2), true);
                    size_t bad = 0;
                    for (size_t i = 0; i < n / 4; i++) if (buf[i] != (0xA5A50000u ^ (uint32_t)i)) bad++;
                    Serial.printf("[SLEEPCHECK] psram after sleep: %u bad words of %u\n", (unsigned)bad, (unsigned)(n / 4));
                    free(buf);
                }
            }
            else if (line[0] == 'e' && line[1] == ' ') {
                // Power rails as RatSpeak has them: e <bits> 1 = LoRa EN (46), 2 = GPS EN (39), 4 = GPIO 38.
                const int b = atoi(line + 2);
                pinMode(46, OUTPUT); digitalWrite(46, (b & 1) ? HIGH : LOW);
                pinMode(39, OUTPUT); digitalWrite(39, (b & 2) ? HIGH : LOW);
                pinMode(38, OUTPUT); digitalWrite(38, (b & 4) ? HIGH : LOW);
                Serial.printf("[SLEEPCHECK] rails: lora %d gps %d io38 %d\n", b & 1, (b >> 1) & 1, (b >> 2) & 1);
            }
            else if (!strcmp(line, "loop")) { g_loop = true; g_nextSleep = millis(); }
            else if (!strcmp(line, "r")) report("now");
            else if (!strcmp(line, "c")) { g_boots = 1; g_sleepsStarted = g_sleepsDone = 0; report("cleared"); }
        } else if (n + 1 < sizeof(line)) {
            line[n++] = c;
        }
    }
    if (g_loop && (int32_t)(millis() - g_nextSleep) >= 0) {
        sleepOnce(2000);
        g_nextSleep = millis() + 3000;
    }
    delay(5);
}
