#include <Arduino.h>
#include "board/tdeck_pro/DisplayEink.h"
#include "board/tdeck_pro/LvglPort.h"
#include "ui/lvgl/UIManager.h"

DisplayEink display;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[BOOT] T-Deck Pro Standalone e-ink");

    if (!LvglPort::begin(display)) {
        Serial.println("Display / LVGL init failed");
        while (1) delay(1000);
    }
    UIManager::begin();
    Serial.println("[BOOT] UI ready");
}

void loop() {
    LvglPort::tick();
    UIManager::loop();
    delay(5);
}