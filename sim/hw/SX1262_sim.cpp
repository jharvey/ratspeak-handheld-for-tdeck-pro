// Simulated SX1262 (same class/header as the firmware driver).
// "The air" is UDP multicast 239.255.42.99:47999: every simulator instance on this
// PC (loopback enabled) or LAN hears every packet. A receiver only accepts packets
// sent with the same frequency / SF / bandwidth, like a real radio. TX holds the
// radio busy for the real LoRa airtime so protocol timing stays realistic.
#include "radio/SX1262.h"
#include "hw/SimHw.h"

#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "hw/AirLink.h"

SX1262* SX1262::_instance = nullptr;

namespace {

struct AirPacket { std::vector<uint8_t> data; int rssi; float snr; };

std::mutex g_mu;
std::deque<AirPacket> g_rx;
volatile bool* g_rxFlag = nullptr;   // the radio's public packetAvailable (DIO1 IRQ)

// Live modem settings (the class's own fields store chip register codes).
uint32_t g_freq = 915000000, g_bw = 125000;
int g_sf = 9, g_cr = 5, g_txp = 14;
long g_preamble = 18;
bool g_crc = true;
std::vector<uint8_t> g_tx;
int g_lastRssi = -120;
float g_lastSnr = 0;

bool netStart() {
    const bool ok = sim::air::start([](sim::air::Frame&& f) {
        std::lock_guard<std::mutex> l(g_mu);
        if (f.freq != g_freq || f.bw != g_bw || f.sf != g_sf) return;   // not on our channel
        AirPacket p;
        p.data = std::move(f.payload);
        p.rssi = -60 - (int)(std::random_device{}() % 30);
        p.snr = 9.5f - (float)(std::random_device{}() % 60) / 10.0f;
        if (g_rx.size() < 16) g_rx.push_back(std::move(p));
        if (g_rxFlag) *g_rxFlag = true;
    });
    if (ok) Serial.printf("[SIM-RADIO] air = %s (id %08x)\n", sim::air::address(), (unsigned)sim::air::selfId());
    return ok;
}

void netSend(const std::vector<uint8_t>& payload) {
    sim::air::Frame f;
    f.freq = g_freq; f.bw = g_bw; f.sf = (uint8_t)g_sf; f.cr = (uint8_t)g_cr; f.txp = (int8_t)g_txp;
    f.payload = payload;
    sim::air::send(f);
}

}  // namespace

SX1262::SX1262(SPIClass* spi, int ss, int sclk, int mosi, int miso, int reset, int irq, int busy, int rxen,
               bool tcxo, bool dio2)
    : _spiSettings(8000000, MSBFIRST, SPI_MODE0), _spiModem(spi), _ss(ss), _sclk(sclk), _mosi(mosi),
      _miso(miso), _reset(reset), _irq(irq), _busy(busy), _rxen(rxen), _tcxo(tcxo), _dio2_as_rf_switch(dio2) {
    _instance = this;
}

bool SX1262::begin(uint32_t frequency) {
    g_rxFlag = &packetAvailable;
    _radioOnline = netStart();
    setFrequency(frequency);
    return _radioOnline;
}
void SX1262::end() { _radioOnline = false; }

int SX1262::beginPacket(int implicitHeader) {
    _implicitHeaderMode = implicitHeader;
    std::lock_guard<std::mutex> l(g_mu);
    g_tx.clear();
    return 1;
}
size_t SX1262::write(uint8_t b) { return write(&b, 1); }
size_t SX1262::write(const uint8_t* buffer, size_t size) {
    std::lock_guard<std::mutex> l(g_mu);
    size_t room = MAX_PACKET_SIZE > g_tx.size() ? MAX_PACKET_SIZE - g_tx.size() : 0;
    if (size > room) size = room;
    g_tx.insert(g_tx.end(), buffer, buffer + size);
    return size;
}
int SX1262::endPacket(bool async) {
    std::vector<uint8_t> out;
    { std::lock_guard<std::mutex> l(g_mu); out = g_tx; }
    _txFailed = false;
    _txActive = true;
    _txStartMs = millis();
    _txBudgetMs = (uint32_t)std::ceil(getAirtime((uint16_t)out.size()));
    netSend(out);
    if (!async) {
        while (isTxBusy()) delay(2);
    }
    return 1;
}
bool SX1262::isTxBusy() {
    if (!_txActive) return false;
    if (millis() - _txStartMs < _txBudgetMs) return true;
    _txActive = false;
    return false;
}

void SX1262::receive(int size) {
    (void)size;
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_rx.empty()) packetAvailable = true;
}
int SX1262::parsePacket(int size) {
    (void)size;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_rx.empty()) { _payloadLength = 0; return 0; }
    AirPacket p = std::move(g_rx.front());
    g_rx.pop_front();
    _payloadLength = (int)std::min(p.data.size(), (size_t)MAX_PACKET_SIZE);
    memcpy(_packet, p.data.data(), (size_t)_payloadLength);
    _packetIndex = 0;
    g_lastRssi = p.rssi;
    g_lastSnr = p.snr;
    packetAvailable = !g_rx.empty();
    return _payloadLength;
}
int SX1262::available() { return _payloadLength - _packetIndex; }
int SX1262::read() { return _packetIndex < _payloadLength ? _packet[_packetIndex++] : -1; }
int SX1262::peek() { return _packetIndex < _payloadLength ? _packet[_packetIndex] : -1; }
void SX1262::readBytes(uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) { int c = read(); buffer[i] = c < 0 ? 0 : (uint8_t)c; }
}
void SX1262::readBuffer(uint8_t* buffer, size_t size) { memcpy(buffer, _packet, std::min(size, sizeof(_packet))); }

void SX1262::setFrequency(uint32_t f) { std::lock_guard<std::mutex> l(g_mu); g_freq = f; _frequency = f; }
uint32_t SX1262::getFrequency() { return g_freq; }
void SX1262::setTxPower(int level) { g_txp = level; _txp = (int8_t)level; }
int8_t SX1262::getTxPower() { return (int8_t)g_txp; }
void SX1262::setSpreadingFactor(int sf) {
    std::lock_guard<std::mutex> l(g_mu);
    g_sf = std::max(5, std::min(12, sf)); _sf = (uint8_t)g_sf;
    _ldro = (double)(1u << g_sf) / g_bw * 1000.0 > 16.0;
}
uint8_t SX1262::getSpreadingFactor() { return (uint8_t)g_sf; }
void SX1262::setSignalBandwidth(uint32_t bw) {
    std::lock_guard<std::mutex> l(g_mu);
    g_bw = bw;
    _ldro = (double)(1u << g_sf) / g_bw * 1000.0 > 16.0;
}
uint32_t SX1262::getSignalBandwidth() { return g_bw; }
void SX1262::setCodingRate4(int d) { g_cr = std::max(5, std::min(8, d)); _cr = (uint8_t)g_cr; }
uint8_t SX1262::getCodingRate4() { return (uint8_t)g_cr; }
void SX1262::setPreambleLength(long len) { g_preamble = len; _preambleLength = len; }
void SX1262::setInvertIQ(bool invert) { _invertIq = invert; }
void SX1262::enableCrc() { g_crc = true; }
void SX1262::disableCrc() { g_crc = false; }
int SX1262::currentRssi() { return -118; }
int SX1262::packetRssi() { return g_lastRssi; }
float SX1262::packetSnr() { return g_lastSnr; }
uint8_t SX1262::readRegister(uint16_t) { return 0; }
uint16_t SX1262::getDeviceErrors() { return 0; }
void SX1262::clearDeviceErrors() {}
uint8_t SX1262::getStatus() { return isTxBusy() ? 0x52 : 0x5C; }   // TX / RX mode bits
uint8_t SX1262::getPacketType() { return 0x01; }                      // LoRa
uint16_t SX1262::getIrqFlags() { return 0; }

// Semtech AN1200.13 time-on-air (ms).
float SX1262::getAirtime(uint16_t written) {
    const double tsym = (double)(1u << g_sf) / (double)g_bw * 1000.0;
    const int de = _ldro ? 1 : 0, ih = _implicitHeaderMode ? 1 : 0, crc = g_crc ? 1 : 0;
    const double num = 8.0 * written - 4.0 * g_sf + 28 + 16 * crc - 20 * ih;
    const double den = 4.0 * (g_sf - 2 * de);
    const double payloadSyms = 8 + std::max(std::ceil(num / den) * g_cr, 0.0);
    return (float)((g_preamble + 4.25) * tsym + payloadSyms * tsym);
}
uint32_t SX1262::getBitrate() {
    return (uint32_t)(g_sf * (4.0 / g_cr) / ((double)(1u << g_sf) / g_bw));
}
void SX1262::onReceive(void (*cb)(int)) { _onReceive = cb; }
void SX1262::standby() {}
void SX1262::sleep() {}

// RX duty cycle: the simulated air has no preambles to miss; keep the flags so the
// app and energy monitor see the same state as on hardware.
void SX1262::setRxDutyCycle(bool on, uint8_t minSymbols) {
    _dcWanted = on;
    _dcMinSym = minSymbols;
    _dcOn = on;
    _dcRxUs = on ? 46000 : 0;
    _dcSleepUs = on ? 82000 : 0;
}
void SX1262::rxHeal() {}
uint16_t SX1262::tcxoProbe(uint32_t) { return 0; }
uint8_t SX1262::random() { return (uint8_t)(std::random_device{}() & 0xFF); }
