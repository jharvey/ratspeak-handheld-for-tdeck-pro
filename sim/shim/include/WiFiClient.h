#pragma once
// Inert TCP client (never connected) - the T-Deck Pro UI does not use TCP yet.
#include "Arduino.h"
class WiFiClient : public Stream {
public:
    int connect(const char*, uint16_t, int32_t = 0) { return 0; }
    int connect(IPAddress, uint16_t, int32_t = 0) { return 0; }
    uint8_t connected() { return 0; }
    void stop() {}
    int available() override { return 0; }
    int read() override { return -1; }
    int read(uint8_t*, size_t) { return -1; }
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
    using Print::write;
    void flush() override {}
    int setNoDelay(bool) { return 0; }
    void setTimeout(uint32_t) {}
    IPAddress remoteIP() const { return IPAddress(); }
    uint16_t remotePort() const { return 0; }
    explicit operator bool() { return false; }
    bool operator==(const WiFiClient&) const { return true; }
};
