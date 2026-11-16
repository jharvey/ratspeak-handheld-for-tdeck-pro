#pragma once
// Simulated WiFi network between simulator windows on one PC (the walkie-talkie needs it).
// Like the LoRa "air" (AirLink), it is a local UDP multicast group: 239.255.42.98 on the air
// port + 1, so RS_SIM_AIR_PORT keeps separate test pairs apart here too.
//  - Every unit has its own address: on a network from the side panel / sim.ini
//    (HomeNet ...) 192.168.1.<n>; a unit running a hotspot is 10.42.<k>.1 and the units that
//    join it 10.42.<k>.<n>. A packet reaches only the unit with that address.
//  - A hotspot sends a beacon (name, password, address) every second, so other simulated
//    units see it in a WiFi scan and can join it with the password.
//  - UDP only (what the walkie-talkie uses); the firmware reaches it through
//    sim/shim/include/lwip/sockets.h.
// Addresses here are host order: a.b.c.d = a << 24 | b << 16 | c << 8 | d.
// Kept free of Arduino.h so the implementation can include the OS socket headers.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sim::wifinet {

struct Ap {
    std::string ssid, pass;
    uint32_t ip = 0;          // the hotspot's own address
};

uint8_t hostId();                           // this unit's host number (2..201), random per run
void setStationIp(uint32_t ip);             // 0 = not on a network
void startAp(const std::string& ssid, const std::string& pass);   // sets the hotspot address
void stopAp();
uint32_t apIp();                            // 0 = no hotspot
std::vector<Ap> heardAps();                 // hotspots heard in the last 3 s

// UDP sockets (non-blocking receive).
int open();
bool bind(int s, uint16_t port);
int sendTo(int s, const void* data, size_t n, uint32_t ip, uint16_t port);
int recvFrom(int s, void* buf, size_t cap, uint32_t* ip, uint16_t* port);   // -1 = nothing
void close(int s);

}  // namespace sim::wifinet
