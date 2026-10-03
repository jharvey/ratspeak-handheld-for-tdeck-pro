#include <Arduino.h>
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include "config/BoardConfig.h"

DisplayEink display;

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  E-INK BRING-UP");
    Serial.println("========================================");

    // Power gates
    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(50);

    // Shared SPI (same pins as cooperative build)
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    if (!LvglPort::begin(display)) {
        Serial.println("[BOOT] Display / LVGL init FAILED");
        while (true) delay(1000);
    }

    Serial.println("[BOOT] Drawing test pattern...");

    // White background
    display.fillScreen(false);

    // Black title bar
    for (int y = 0; y < 28; y++)
        for (int x = 0; x < EPD_WIDTH; x++)
            display.setPixel(x, y, true);

    // Horizontal test bars
    for (int y = 60; y < 80; y++)
        for (int x = 20; x < 300; x++)
            display.setPixel(x, y, true);

    for (int y = 100; y < 120; y++)
        for (int x = 20; x < 300; x++)
            display.setPixel(x, y, (x / 8) & 1);   // checker

    // Border
    for (int x = 0; x < EPD_WIDTH; x++) {
        display.setPixel(x, 0, true);
        display.setPixel(x, EPD_HEIGHT - 1, true);
    }
    for (int y = 0; y < EPD_HEIGHT; y++) {
        display.setPixel(0, y, true);
        display.setPixel(EPD_WIDTH - 1, y, true);
    }

    display.fullRefresh();
    Serial.println("[BOOT] Test pattern sent to panel");
    Serial.println("[BOOT] If you see black bars / border on the e-ink, driver works");
}

void loop() {
    LvglPort::tick();
    delay(50);
}