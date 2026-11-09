#pragma once
// "The air" shared by simulated radios: UDP multicast 239.255.42.99:47999.
// Kept free of Arduino.h so it can include the OS socket headers.
#include <cstdint>
#include <functional>
#include <vector>

namespace sim::air {

struct Frame {
    uint32_t sender = 0, freq = 0, bw = 0;
    uint8_t sf = 0, cr = 0;
    int8_t txp = 0;
    std::vector<uint8_t> payload;
};

bool start(std::function<void(Frame&&)> onFrame);   // onFrame runs on the RX thread
uint32_t selfId();
void send(const Frame& f);
const char* address();

}  // namespace sim::air
