#pragma once

#include <Arduino.h>

namespace tdeck_pro {
namespace eink {

bool begin();
void clear();
void showBootScreen();
void showTestScreen();

// Headless status screen
// destShort  – first 12–16 hex chars of destination hash (or nullptr)
// batteryPct – 0..100, or -1 if unknown
// loraOnline – true if radio is up
// pathCount  – known paths
// version    – firmware version string
void showStatusScreen(
    const char* destShort,
    int batteryPct,
    bool loraOnline,
    unsigned pathCount,
    const char* version);

// Small once-per-second uptime display.
// This updates only a small partial e-ink window.
void showUptime(uint32_t elapsedSeconds);

void sleep();
void wake();
bool isReady();

}  // namespace eink
}  // namespace tdeck_pro