// Simulator stand-in for hal/UsbDrive.cpp: there is no USB port to hand over, so "start"
// only reports the card size and switches to the drive screen; leaving restarts the sim
// like the unit does. "serial usbeject" (console) plays the PC's Eject.
#include "hal/UsbDrive.h"

#include <Arduino.h>
#include <SD.h>

namespace usbdrive {
namespace {
State g_state = OFF;
const char* g_msg = "off";
uint32_t g_sectors = 0;
}

bool start(int) {
    g_sectors = (uint32_t)(SD.cardSize() / 512);
    if (!g_sectors) { g_state = FAILED; g_msg = "no SD card"; return false; }
    g_state = ACTIVE;
    g_msg = "on the PC";
    Serial.printf("[USB] drive mode: %lu MB card (sim: no USB)\n", (unsigned long)(g_sectors / 2048));
    return true;
}
State state() { return g_state; }
const char* message() { return g_msg; }
uint32_t sectors() { return g_sectors; }
uint32_t readKb() { return 0; }
uint32_t writtenKb() { return 0; }
uint32_t lastWriteMs() { return 0; }
void simEject() { if (g_state == ACTIVE) { g_state = EJECTED; g_msg = "ejected"; } }
void restartToNormal() { ESP.restart(); }

}  // namespace usbdrive
