#pragma once
// Board power-floor measurement for the T-Deck Pro, without RatSpeak.
//
// The battery gauge (BQ27220) is the only current sensor, so the board runs from its
// battery with USB attached: the charger's input is switched off (BQ25896 EN_HIZ, with its
// I2C watchdog off so it stays off). Every rail the firmware controls is driven low, then
// the ESP32 sleeps in cycles and logs the gauge's average current at each wake into RTC
// memory (kept through deep sleep).
//
//   floor l <cycles>   light sleep, 30 s per cycle
//   floor d <cycles>   deep sleep, 30 s per cycle (the ESP32 itself is ~10 uA)
//   floor              print the log            floor x   stop + charger back on
#include <Arduino.h>

namespace floorcheck {
void boot();                         // call early in setup(): continues a deep-sleep run
bool command(const char* line);      // true if the line was a floor command
}
