#pragma once

#include <Arduino.h>

namespace tdeck_pro {
namespace eink {

bool begin();
void clear();
void showBootScreen();
void showTestScreen();

void showStatusScreen(
    const char* destShort,
    int batteryPct,
    bool loraOnline,
    unsigned pathCount,
    const char* version);

void showNodeHome(
    const char* destShort,
    int batteryPct,
    bool loraOnline,
    unsigned pathCount,
    unsigned linkCount,
    const char* lastEvent,
    const char* version);

void showUptime(uint32_t elapsedSeconds);

void sleep();
void wake();
bool isReady();

}  // namespace eink
}  // namespace tdeck_pro