#pragma once
// I2C bus routed to simulated chips (TCA8418 keyboard, BQ27220 gauge,
// BQ25896 charger, CST328 touch). See sim/hw/I2cDevices.cpp.
#include "Arduino.h"
class TwoWire : public Stream {
public:
    explicit TwoWire(uint8_t bus) : bus_(bus) {}
    bool begin(int sda = -1, int scl = -1, uint32_t freq = 0);
    bool end() { return true; }
    bool setClock(uint32_t) { return true; }
    void setTimeOut(uint16_t) {}
    void setTimeout(uint16_t) {}
    void beginTransmission(uint16_t addr);
    void beginTransmission(int addr) { beginTransmission((uint16_t)addr); }
    uint8_t endTransmission(bool stop = true);
    size_t requestFrom(uint16_t addr, size_t len, bool stop = true);
    uint8_t requestFrom(uint8_t addr, uint8_t len) {
        return (uint8_t)requestFrom((uint16_t)addr, (size_t)len, true);
    }
    uint8_t requestFrom(int addr, int len) { return (uint8_t)requestFrom((uint16_t)addr, (size_t)len, true); }
    uint8_t requestFrom(uint8_t addr, uint8_t len, uint8_t stop) {
        return (uint8_t)requestFrom((uint16_t)addr, (size_t)len, stop != 0);
    }
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* b, size_t n) override;
    using Print::write;
    int available() override;
    int read() override;
    int peek() override;
    void flush() override {}
private:
    uint8_t bus_;
    uint16_t txAddr_ = 0;
    uint8_t txBuf_[64];
    size_t txLen_ = 0;
    uint8_t rxBuf_[64];
    size_t rxLen_ = 0, rxPos_ = 0;
};
extern TwoWire Wire;
extern TwoWire Wire1;
