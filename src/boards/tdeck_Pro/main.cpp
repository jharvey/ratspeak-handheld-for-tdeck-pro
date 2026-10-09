#include <Arduino.h>
#include "StandaloneApp.h"

void setup() {
    Serial.begin(115200);
    delay(800);  // USB-CDC: give Putty time after reset
    Serial.println();
    Serial.println("[MAIN] setup");
    Serial.flush();

    if (!standalone::begin()) {
        Serial.println("[MAIN] begin FAILED — looping without UI");
        Serial.flush();
    } else {
        Serial.println("[MAIN] begin OK");
        Serial.flush();
    }
}

void loop() {
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 5000) {
        lastBeat = millis();
        Serial.printf("[MAIN] beat free_int=%u\n",
                      (unsigned)ESP.getFreeHeap());
        Serial.flush();
    }
    standalone::loop();
}