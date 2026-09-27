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
// GDEQ031T10 / UC8253
// T-Deck Pro V1.1
//
// Known-good configuration verified on hardware:
//   CS   = GPIO 34
//   DC   = GPIO 35
//   BUSY = GPIO 37
//   RST  = GPIO 16
//   SCK  = GPIO 36
//   MOSI = GPIO 33
//   MISO = GPIO 47
//   SPI  = 2 MHz, MODE0
// -----------------------------------------------------------------------------

using InkPanel = GxEPD2_310_GDEQ031T10;

using InkDisplay =
    GxEPD2_BW<InkPanel, InkPanel::HEIGHT>;

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

    // Shared SPI bus:
    // Keep the other devices deselected while initializing the e-paper.
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

    // Use the ESP32 SPI bus with the T-Deck Pro V1.1 pin assignment.
    SPI.begin(
        SPI_SCK,
        SPI_MISO,
        SPI_MOSI);

    // This exact SPI configuration is known-good with the V1.1 display.
    display.epd2.selectSPI(
        SPI,
        SPISettings(
            2000000,
            MSBFIRST,
            SPI_MODE0));

    // Hardware reset enabled.
    //
    // This is important for the V1.1 GDEQ031T10.
    display.init(
        115200,
        true,
        2,
        false);

    display.setRotation(0);

    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    initialized = true;

    Serial.printf(
        "[EINK] Ready (%d x %d)\n",
        display.width(),
        display.height());

    return true;
}

// -----------------------------------------------------------------------------
// Clear display
// -----------------------------------------------------------------------------

void clear() {
    if (!initialized) {
        return;
    }

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
// Boot screen
// -----------------------------------------------------------------------------

void showBootScreen() {
    if (!initialized) {
        return;
    }

    display.setFullWindow();

    display.firstPage();

    do {
        display.fillScreen(GxEPD_WHITE);

        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);

        display.setCursor(20, 45);
        display.print("RATSPEAK");

        display.setCursor(20, 75);
        display.print("T-Deck Pro");

        display.setCursor(20, 105);
        display.print("GDEQ031T10");

        display.setCursor(20, 135);
        display.print("V1.1");

    } while (display.nextPage());

    display.powerOff();
}

// -----------------------------------------------------------------------------
// Diagnostic test screen
// -----------------------------------------------------------------------------
//
// Kept intentionally simple. This can be removed later once the normal
// Ratspeak UI is being rendered on the display.
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
// Sleep
// -----------------------------------------------------------------------------

void sleep() {
    if (!initialized) {
        return;
    }

    display.powerOff();
}

// -----------------------------------------------------------------------------
// Wake
// -----------------------------------------------------------------------------

void wake() {
    if (!initialized) {
        return;
    }

    Serial.println("[EINK] Waking...");

    // Re-select the known-good SPI configuration.
    display.epd2.selectSPI(
        SPI,
        SPISettings(
            2000000,
            MSBFIRST,
            SPI_MODE0));

    display.init(
        115200,
        true,
        2,
        false);

    display.setRotation(0);

    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    Serial.println("[EINK] Awake");
}

// -----------------------------------------------------------------------------
// Status
// -----------------------------------------------------------------------------

bool isReady() {
    return initialized;
}

}  // namespace eink
}  // namespace tdeck_pro