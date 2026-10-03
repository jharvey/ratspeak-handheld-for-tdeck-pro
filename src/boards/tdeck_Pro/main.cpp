#include <Arduino.h>
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"

// Optional – only if you already pushed the UIManager + screens
#if __has_include("ui/lvgl/UIManager.h")
  #include "ui/lvgl/UIManager.h"
  #define HAS_UIMANAGER 1
#else
  #define HAS_UIMANAGER 0
#endif

DisplayEink display;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("[BOOT] T-Deck Pro Standalone e-ink");

    if (!LvglPort::begin(display)) {
        Serial.println("[BOOT] Display / LVGL init failed");
        while (1) delay(1000);
    }

#if HAS_UIMANAGER
    UIManager::begin();
    Serial.println("[BOOT] UIManager ready");
#else
    // Minimal test – fill screen white then draw a black rectangle
    display.fillScreen(false);          // white
    for (int y = 40; y < 80; y++)
        for (int x = 40; x < 280; x++)
            display.setPixel(x, y, true); // black bar
    display.fullRefresh();
    Serial.println("[BOOT] Minimal e-ink test drawn (no UIManager yet)");
#endif
}

void loop() {
    LvglPort::tick();

#if HAS_UIMANAGER
    UIManager::loop();
#endif

    delay(5);
}