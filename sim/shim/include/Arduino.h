#pragma once
// Host shim of the Arduino-ESP32 core API used by the RatSpeak firmware.

#ifndef __cplusplus
/* C translation units (LVGL's tick source) only need the clock. */
unsigned long millis(void);
#else

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <time.h>
#include <sys/time.h>

#include "WString.h"
#include "Print.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using std::min;
using std::max;

typedef uint8_t byte;
typedef bool boolean;

#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09
#define RISING 0x01
#define FALLING 0x02
#define CHANGE 0x03

#define PI 3.1415926535897932384626433832795
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define EXT_RAM_ATTR
#define PROGMEM
#define PSTR(s) (s)
#define F(s) (s)
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define digitalPinToInterrupt(p) (p)
#define SERIAL_8N1 0x800001c
inline bool psramFound() { return true; }

extern "C" unsigned long millis();
unsigned long micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield();

void pinMode(int pin, int mode);
void digitalWrite(int pin, int val);
int digitalRead(int pin);
int analogRead(int pin);
void analogReadResolution(int bits);
void attachInterrupt(int pin, void (*fn)(), int mode);
void detachInterrupt(int pin);

double ledcSetup(uint8_t ch, double freq, uint8_t bits);
void ledcAttachPin(uint8_t pin, uint8_t ch);
void ledcWrite(uint8_t ch, uint32_t duty);
void ledcDetachPin(uint8_t pin);

long random(long max);
long random(long min, long max);
void randomSeed(unsigned long);

inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void* ps_malloc(size_t size);
void* ps_calloc(size_t n, size_t size);
void* ps_realloc(void* p, size_t size);

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() { return -1; }
    void setTimeout(unsigned long) {}
    size_t readBytes(uint8_t* buf, size_t len) {
        size_t n = 0;
        while (n < len) { int c = read(); if (c < 0) break; buf[n++] = (uint8_t)c; }
        return n;
    }
    size_t readBytes(char* buf, size_t len) { return readBytes((uint8_t*)buf, len); }
    String readStringUntil(char t) {
        String s; int c;
        while ((c = read()) >= 0 && c != t) s += (char)c;
        return s;
    }
};

class HardwareSerial : public Stream {
public:
    explicit HardwareSerial(int port) : port_(port) {}
    void begin(unsigned long, uint32_t = 0, int8_t = -1, int8_t = -1, bool = false, unsigned long = 0) {}
    void end() {}
    void setRxBufferSize(size_t) {}
    void setTxBufferSize(size_t) {}
    int available() override;
    int read() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* b, size_t n) override;
    using Print::write;
    void flush() override;
    operator bool() const { return true; }
private:
    int port_;
};

class HWCDC : public HardwareSerial { public: HWCDC() : HardwareSerial(0) {} };

extern HardwareSerial Serial;     // USB console -> simulator log pane
extern HardwareSerial Serial1;    // GPS UART   -> simulated NMEA source
extern HardwareSerial Serial2;

class EspClass {
public:
    uint32_t getFreeHeap();
    uint32_t getHeapSize() { return 320 * 1024; }
    uint32_t getMinFreeHeap() { return getFreeHeap(); }
    uint32_t getMaxAllocHeap() { return 110 * 1024; }
    uint32_t getFreePsram() { return 7 * 1024 * 1024; }
    uint32_t getPsramSize() { return 8 * 1024 * 1024; }
    uint32_t getMaxAllocPsram() { return 4 * 1024 * 1024; }
    uint32_t getFlashChipSize() { return 16 * 1024 * 1024; }
    const char* getChipModel() { return "ESP32-S3 (simulated)"; }
    uint8_t getChipRevision() { return 0; }
    uint8_t getChipCores() { return 2; }
    uint32_t getCpuFreqMHz() { return 240; }
    const char* getSdkVersion() { return "sim"; }
    uint64_t getEfuseMac();
    uint32_t getCycleCount() { return (uint32_t)(micros() * 240); }
    [[noreturn]] void restart();
};
extern EspClass ESP;

class IPAddress {
public:
    IPAddress() = default;
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : b_{a, b, c, d} {}
    explicit IPAddress(uint32_t v) { memcpy(b_, &v, 4); }
    String toString() const {
        char s[16]; snprintf(s, sizeof(s), "%u.%u.%u.%u", b_[0], b_[1], b_[2], b_[3]); return String(s);
    }
    uint8_t operator[](int i) const { return b_[i]; }
    operator uint32_t() const { uint32_t v; memcpy(&v, b_, 4); return v; }
    bool operator==(const IPAddress& o) const { return memcmp(b_, o.b_, 4) == 0; }
private:
    uint8_t b_[4] = {0, 0, 0, 0};
};

// main.cpp entry points (the firmware's own setup()/loop()).
void setup();
void loop();

#endif  // __cplusplus
