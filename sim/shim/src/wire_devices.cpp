// I2C bus with simulated chips, register-compatible with what the firmware reads:
//   0x34 TCA8418 keyboard matrix (FIFO of key events)
//   0x1A CST328 touch controller
//   0x55 BQ27220 fuel gauge
//   0x6B BQ25896 charger
#include "Wire.h"
#include "hw/SimHw.h"

#include <deque>

TwoWire Wire(0);
TwoWire Wire1(1);

namespace {

constexpr uint16_t kTca = 0x34, kTouch = 0x1A, kGauge = 0x55, kCharger = 0x6B;
std::deque<uint8_t> g_tcaFifo;     // device FIFO (max 10 like the chip)

bool present(uint16_t a) { return a == kTca || a == kTouch || a == kGauge || a == kCharger; }

void tcaFill() {   // caller holds hw().mu
    auto& q = sim::hw().keyEvents;
    while (!q.empty() && g_tcaFifo.size() < 10) {
        const auto e = q.front(); q.pop_front();
        const uint8_t code = (uint8_t)(e.row * 10 + e.col + 1);
        g_tcaFifo.push_back((uint8_t)(code | (e.pressed ? 0x80 : 0)));
    }
}

// Answers a register read; returns number of bytes produced.
size_t readRegs(uint16_t addr, const uint8_t* tx, size_t txLen, uint8_t* out, size_t n) {
    auto& h = sim::hw();
    std::lock_guard<std::mutex> l(h.mu);
    const uint8_t reg = txLen ? tx[0] : 0;
    size_t k = 0;
    auto put16 = [&](int v) { if (k < n) out[k++] = (uint8_t)(v & 0xFF); if (k < n) out[k++] = (uint8_t)((v >> 8) & 0xFF); };
    switch (addr) {
    case kTca:
        tcaFill();
        for (size_t i = 0; i < n; i++) {
            uint8_t v = 0;
            if (reg == 0x03) v = (uint8_t)(g_tcaFifo.size() & 0x0F);               // KEY_LCK_EC
            else if (reg == 0x04) {                                                // KEY_EVENT_A
                if (!g_tcaFifo.empty()) { v = g_tcaFifo.front(); g_tcaFifo.pop_front(); }
            } else if (reg == 0x02) v = g_tcaFifo.empty() ? 0 : 0x01;              // INT_STAT
            out[k++] = v;
        }
        return k;
    case kTouch: {
        // 0xD000: d0 low nibble 0x06 = valid point; 12-bit x/y packed in d1..d3.
        const int x = h.touchX, y = h.touchY;
        uint8_t d[7] = {(uint8_t)(h.touchDown ? 0x06 : 0x00), (uint8_t)(x >> 4), (uint8_t)(y >> 4),
                        (uint8_t)(((x & 0x0F) << 4) | (y & 0x0F)), 0, 0, 0xAB};
        for (; k < n && k < sizeof(d); k++) out[k] = d[k];
        return k;
    }
    case kGauge: {
        const int soc = h.chargeDone ? std::max(h.batterySoc, 92) : h.batterySoc;
        const int rem = h.fccMah * soc / 100;
        const bool dsg = !h.usb;
        uint16_t flags = (dsg ? 0x0001 : 0) | (h.chargeDone ? 0x0200 : 0);
        switch (reg) {
            case 0x08: put16(h.batteryMv); break;               // Voltage
            case 0x0A: put16(flags); break;                     // BatteryStatus
            case 0x10: put16(rem); break;                       // RemainingCapacity
            case 0x12: put16(h.fccMah); break;                  // FullChargeCapacity
            case 0x14: put16((int16_t)h.batteryMa); break;      // AverageCurrent
            case 0x2C: put16(soc); break;                       // StateOfCharge
            default: put16(0); break;
        }
        return k;
    }
    case kCharger: {
        uint8_t chrg = h.chargeDone ? 3 : h.charging ? 2 : 0;
        uint8_t r0b = (uint8_t)((h.usb ? (1 << 5) : 0) | (chrg << 3) | (h.usb ? 0x04 : 0));
        for (; k < n; k++) out[k] = reg == 0x0B ? r0b : 0;
        return k;
    }
    }
    return 0;
}

}  // namespace

bool TwoWire::begin(int, int, uint32_t) { return true; }

void TwoWire::beginTransmission(uint16_t addr) { txAddr_ = addr; txLen_ = 0; }

uint8_t TwoWire::endTransmission(bool) {
    if (bus_ != 0 || !present(txAddr_)) return 2;   // address NACK
    return 0;                                     // register writes are accepted and ignored
}

size_t TwoWire::requestFrom(uint16_t addr, size_t len, bool) {
    rxLen_ = rxPos_ = 0;
    if (bus_ != 0 || !present(addr)) return 0;
    if (len > sizeof(rxBuf_)) len = sizeof(rxBuf_);
    rxLen_ = readRegs(addr, txBuf_, addr == txAddr_ ? txLen_ : 0, rxBuf_, len);
    return rxLen_;
}

size_t TwoWire::write(uint8_t c) { if (txLen_ < sizeof(txBuf_)) txBuf_[txLen_++] = c; return 1; }
size_t TwoWire::write(const uint8_t* b, size_t n) { for (size_t i = 0; i < n; i++) write(b[i]); return n; }
int TwoWire::available() { return (int)(rxLen_ - rxPos_); }
int TwoWire::read() { return rxPos_ < rxLen_ ? rxBuf_[rxPos_++] : -1; }
int TwoWire::peek() { return rxPos_ < rxLen_ ? rxBuf_[rxPos_] : -1; }
