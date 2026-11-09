#pragma once
// ESP32-S3 light sleep for the T-Deck Pro.
//
// Light sleep stops the CPU clocks but keeps RAM, PSRAM, GPIO levels and every powered
// peripheral as they are: the e-ink keeps its image, the LoRa radio keeps receiving, the
// GPS / LoRa enable pins stay put. Wake-up takes ~1 ms and the program simply continues.
//
// Wake sources: a timer, the keyboard INT line (TCA8418, low), the touch INT line
// (CST328, low) and the LoRa DIO1 line (SX1262 RX done, high).
//
// Not used while a USB host is attached: the USB-Serial-JTAG console stops during
// light sleep. On the desktop simulator this is a short delay.
#include <stdint.h>

namespace power {

enum Wake : uint8_t { WAKE_NONE, WAKE_TIMER, WAKE_KEY, WAKE_TOUCH, WAKE_LORA, WAKE_OTHER };

struct WakePins {
    int key   = -1;   // active low
    int touch = -1;   // active low
    int lora  = -1;   // active high (has an edge ISR attached; restored after wake)
};

// Sleep up to `ms`. Returns why it woke; `sleptMs` gets the time actually spent asleep.
Wake lightSleep(uint32_t ms, const WakePins& pins, uint32_t& sleptMs);

const char* wakeName(Wake w);

}  // namespace power
