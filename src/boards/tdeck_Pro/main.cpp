#include <Arduino.h>
#include "StandaloneApp.h"

void setup() {
    Serial.begin(115200);
    delay(400);
    standalone::begin();
}

void loop() {
    standalone::loop();
}