#pragma once

#include <Arduino.h>
#include <stdint.h>

// Thin standalone shell API — implementation in StandaloneApp.cpp
// Keep main.cpp as setup/loop only.

namespace standalone {

bool begin();          // power, radio, proto, display, LVGL, first screen
void loop();           // proto, input, boot announce, msg auto-redraw

}  // namespace standalone