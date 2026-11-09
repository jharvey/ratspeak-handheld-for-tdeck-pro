#pragma once
#include "Arduino.h"
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3
#define MSBFIRST 1
#define LSBFIRST 0
#define FSPI 0
#define HSPI 1
#define VSPI 2
class SPISettings {
public:
    SPISettings(uint32_t = 1000000, uint8_t = MSBFIRST, uint8_t = SPI_MODE0) {}
};
class SPIClass {
public:
    explicit SPIClass(uint8_t = 0) {}
    void begin(int8_t = -1, int8_t = -1, int8_t = -1, int8_t = -1) {}
    void end() {}
    void beginTransaction(const SPISettings&) {}
    void endTransaction() {}
    uint8_t transfer(uint8_t) { return 0xFF; }
    uint16_t transfer16(uint16_t) { return 0xFFFF; }
    void transfer(void* buf, size_t n) { memset(buf, 0xFF, n); }
    void transferBytes(const uint8_t*, uint8_t* out, uint32_t n) { if (out) memset(out, 0xFF, n); }
    void writeBytes(const uint8_t*, uint32_t) {}
    void setFrequency(uint32_t) {}
    void setDataMode(uint8_t) {}
    void setBitOrder(uint8_t) {}
};
extern SPIClass SPI;
