#pragma once
#include "WiFiClient.h"
class WiFiServer {
public:
    explicit WiFiServer(uint16_t port = 80, uint8_t = 4) : port_(port) {}
    void begin(uint16_t = 0) {}
    void stop() {}
    void end() {}
    void setNoDelay(bool) {}
    WiFiClient available() { return WiFiClient(); }
    WiFiClient accept() { return WiFiClient(); }
    bool hasClient() { return false; }
    explicit operator bool() { return true; }
private:
    uint16_t port_;
};
