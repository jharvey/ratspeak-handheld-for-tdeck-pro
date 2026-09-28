#include "EinkDisplay.h"

#include <Arduino.h>
#include <SPI.h>

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>

#include "config/BoardConfig.h"

namespace tdeck_pro {
namespace eink {

#ifndef TDECK_PRO_EPD_CS
#error "TDECK_PRO_EPD_CS must be defined"
#endif
#ifndef TDECK_PRO_EPD_DC
#error "TDECK_PRO_EPD_DC must be defined"
#endif
#ifndef TDECK_PRO_EPD_RST
#error "TDECK_PRO_EPD_RST must be defined"
#endif
#ifndef TDECK_PRO_EPD_BUSY
#error "TDECK_PRO_EPD_BUSY must be defined"
#endif

// -----------------------------------------------------------------------------
// GDEQ031T10 / UC8253 — T-Deck Pro V1.1
// -----------------------------------------------------------------------------

using InkPanel = GxEPD2_310_GDEQ031T10;
using InkDisplay = GxEPD2_BW<InkPanel, InkPanel::HEIGHT>;

static InkDisplay display(
    InkPanel(
        TDECK_PRO_EPD_CS,
        TDECK_PRO_EPD_DC,
        TDECK_PRO_EPD_RST,
        TDECK_PRO_EPD_BUSY));

static bool initialized = false;

// -----------------------------------------------------------------------------
// Initialize display
// -----------------------------------------------------------------------------

bool begin() {
    Serial.println("[EINK] Initializing GDEQ031T10...");

    // Keep other SPI devices deselected
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    pinMode(TDECK_PRO_EPD_CS, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_CS, HIGH);
    pinMode(TDECK_PRO_EPD_DC, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_DC, LOW);
    pinMode(TDECK_PRO_EPD_BUSY, INPUT);

#if TDECK_PRO_EPD_RST >= 0
    pinMode(TDECK_PRO_EPD_RST, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_RST, HIGH);
#endif

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    display.epd2.selectSPI(
        SPI,
        SPISettings(2000000, MSBFIRST, SPI_MODE0));

    // First argument 0 = disable GxEPD2 diagnostic Serial output
    display.init(0, true, 2, false);

    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    initialized = true;

    Serial.printf("[EINK] Ready (%d x %d)\r\n",
                  display.width(), display.height());

    return true;
}

// -----------------------------------------------------------------------------
// Clear
// -----------------------------------------------------------------------------

void clear() {
    if (!initialized) return;

    Serial.println("[EINK] Clearing...");
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());
    display.powerOff();
    Serial.println("[EINK] Clear complete");
}

// -----------------------------------------------------------------------------
// Boot / status screen (headless)
// -----------------------------------------------------------------------------

void showBootScreen() {
    if (!initialized) return;

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);

        display.setCursor(20, 40);
        display.print("RATSPEAK");
        display.setCursor(20, 70);
        display.print("T-Deck Pro");
        display.setCursor(20, 110);
        display.print("HEADLESS");
        display.setCursor(20, 140);
        display.print("COOPERATIVE");
        display.setCursor(20, 180);
        display.print(FIRMWARE_VERSION);
    } while (display.nextPage());

    display.powerOff();
}

// -----------------------------------------------------------------------------
// Simple test screen (kept for diagnostics)
// -----------------------------------------------------------------------------

void showTestScreen() {
    if (!initialized) {
        Serial.println("[EINK] Test skipped - not initialized");
        return;
    }

    Serial.println("[EINK] Running display test...");

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);

        display.setCursor(20, 35);
        display.print("RATSPEAK");
        display.setCursor(20, 70);
        display.print("T-Deck Pro V1.1");
        display.setCursor(20, 105);
        display.print("E-PAPER TEST");
        display.setCursor(20, 140);
        display.print("GDEQ031T10");
        display.setCursor(20, 175);
        display.print("UC8253");
        display.setCursor(20, 210);
        display.print("240 x 320");
        display.setCursor(20, 245);
        display.print("SPI MODE0");
    } while (display.nextPage());

    display.powerOff();
    Serial.println("[EINK] Test complete");
}

// -----------------------------------------------------------------------------
// Sleep / Wake
// -----------------------------------------------------------------------------

void sleep() {
    if (!initialized) return;
    display.powerOff();
}

void wake() {
    if (!initialized) return;

    Serial.println("[EINK] Waking...");
    display.epd2.selectSPI(
        SPI,
        SPISettings(2000000, MSBFIRST, SPI_MODE0));
    display.init(0, true, 2, false);   // quiet
    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    Serial.println("[EINK] Awake");
}

bool isReady() {
    return initialized;
}

}  // namespace eink
}  // namespace tdeck_pro