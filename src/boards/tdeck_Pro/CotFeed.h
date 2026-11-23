#pragma once
// ATAK / TAK feed: Cursor-on-Target (CoT) XML events over UDP on WiFi.
// ATAK, WinTAK and iTAK listen for "SA" events on 239.2.3.1:6969 (multicast) by default, so a
// phone running ATAK on the same WiFi sees this unit, its contacts' last shared positions and
// the markers, without a TAK server. A unicast address (the phone's) works too.
// Kept apart from StandaloneApp.cpp because the simulator's socket shim (lwip/sockets.h)
// defines bind / close as macros.
#include <stddef.h>
#include <stdint.h>

namespace cot {

constexpr uint32_t kSaGroup = 0xEF020301u;   // 239.2.3.1, host order
constexpr uint16_t kSaPort = 6969;

struct Item {
    double lat = 0, lon = 0;
    uint32_t at = 0;            // UTC seconds of the position
    uint32_t stale = 0;         // UTC seconds after which ATAK greys it out
    const char* uid = "";       // stable id: the same uid moves the same icon
    const char* type = "";      // CoT type, e.g. a-f-G-U-C (friendly ground unit), b-m-p-s-m (spot)
    const char* callsign = "";
    int battery = -1;           // 0..100, -1 = none
    int32_t argb = 0;           // marker colour (spot map), 0 = none
    const char* remarks = nullptr;
};

// One CoT event as XML (no trailing zero counted). 0 when it does not fit.
size_t format(const Item& it, char* out, size_t cap);
// Send one datagram; ip / port in host order. False: no socket or send failed.
bool send(const char* data, size_t n, uint32_t ip, uint16_t port);
// Bench / test: receive CoT on a port (non-blocking poll). listen(0) stops.
bool listen(uint16_t port);
int poll(char* buf, size_t cap, uint32_t* fromIp);   // bytes, or -1 = nothing
void stop();

}  // namespace cot
