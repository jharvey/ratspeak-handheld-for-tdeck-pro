#include "EinkDisplay.h"

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

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
// Helpers
// -----------------------------------------------------------------------------

static void drawBatteryBar(int x, int y, int w, int h, int percent) {
    // Outer frame
    display.drawRect(x, y, w, h, GxEPD_BLACK);

    // Terminal nub
    display.fillRect(
        x + w,
        y + h / 4,
        3,
        h / 2,
        GxEPD_BLACK);

    if (percent < 0) {
        percent = 0;
    }

    if (percent > 100) {
        percent = 100;
    }

    int fillW = (w - 4) * percent / 100;

    if (fillW > 0) {
        display.fillRect(
            x + 2,
            y + 2,
            fillW,
            h - 4,
            GxEPD_BLACK);
    }
}

// -----------------------------------------------------------------------------
// Initialize
// -----------------------------------------------------------------------------

bool begin() {
    Serial.println("[EINK] Initializing GDEQ031T10...");

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

    SPI.begin(
        SPI_SCK,
        SPI_MISO,
        SPI_MOSI);

    display.epd2.selectSPI(
        SPI,
        SPISettings(
            2000000,
            MSBFIRST,
            SPI_MODE0));

    // 0 = quiet diagnostics
    display.init(
        0,
        true,
        2,
        false);

    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    initialized = true;

    Serial.printf(
        "[EINK] Ready (%d x %d)\r\n",
        display.width(),
        display.height());

    return true;
}

// -----------------------------------------------------------------------------
// Clear
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
// Simple boot screen (pre-protocol)
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

        display.setCursor(20, 40);
        display.print("RATSPEAK");

        display.setCursor(20, 70);
        display.print("T-Deck Pro V1.1 (not V1l.0)");

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
// Diagnostic test screen
// -----------------------------------------------------------------------------

void showTestScreen() {
    if (!initialized) {
        Serial.println(
            "[EINK] Test skipped - not initialized");

        return;
    }

    Serial.println(
        "[EINK] Running display test...");

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
// Headless status screen
// -----------------------------------------------------------------------------

void showStatusScreen(
    const char* destShort,
    int batteryPct,
    bool loraOnline,
    unsigned pathCount,
    const char* version) {

    if (!initialized) {
        return;
    }

    Serial.println(
        "[EINK] Updating status screen...");

    display.setFullWindow();

    display.firstPage();

    do {
        display.fillScreen(GxEPD_WHITE);

        display.setTextColor(GxEPD_BLACK);

        // Title
        display.setFont(&FreeMonoBold9pt7b);

        display.setCursor(12, 28);
        display.print("RATSPEAK");

        display.setCursor(12, 48);
        display.print("T-Deck Pro");

        display.setFont(&FreeMono9pt7b);

        display.setCursor(12, 72);
        display.print("HEADLESS / COOP");

        // Version
        display.setCursor(12, 96);
        display.print(
            version ? version : "----");

        // Divider
        display.drawFastHLine(
            10,
            108,
            220,
            GxEPD_BLACK);

        // Destination
        display.setCursor(12, 132);
        display.print("Dest ");

        if (destShort && destShort[0]) {
            display.print(destShort);
        } else {
            display.print("(none)");
        }

        // LoRa
        display.setCursor(12, 156);
        display.print("LoRa ");
        display.print(
            loraOnline ? "ONLINE" : "OFFLINE");

        // Battery bar + percent
        display.setCursor(12, 180);
        display.print("Batt ");

        drawBatteryBar(
            70,
            168,
            100,
            16,
            batteryPct);

        display.setCursor(180, 180);

        if (batteryPct >= 0) {
            display.printf(
                "%d%%",
                batteryPct);
        } else {
            display.print("--");
        }

        // Paths
        display.setCursor(12, 204);
        display.printf(
            "Paths %u",
            pathCount);

        // Footer hint
        display.setCursor(12, 240);
        display.print("Serial: help");

    } while (display.nextPage());

    display.powerOff();

    Serial.println(
        "[EINK] Status screen done");
}

// -----------------------------------------------------------------------------
// Small partial-window uptime display
// -----------------------------------------------------------------------------
//
// The current T-Deck Pro port does not yet have a synchronized wall clock.
// Therefore this deliberately displays uptime.
//
// Only this small area of the e-ink panel is refreshed. The rest of the
// status screen remains untouched.
//
// Display example:
//
//     UP 00:03:27
//
// This is intended to replace the old one-second keyboard LED heartbeat.
// -----------------------------------------------------------------------------

void showUptime(uint32_t elapsedSeconds) {
    if (!initialized) {
        return;
    }

    uint32_t hours =
        elapsedSeconds / 3600UL;

    uint32_t minutes =
        (elapsedSeconds % 3600UL) / 60UL;

    uint32_t seconds =
        elapsedSeconds % 60UL;

    // Keep the displayed value useful after long uptimes.
    // HH is allowed to grow beyond 99 if necessary.
    char uptime[24];

    snprintf(
        uptime,
        sizeof(uptime),
        "UP %02lu:%02lu:%02lu",
        (unsigned long)hours,
        (unsigned long)minutes,
        (unsigned long)seconds);

    // Small region in the upper-right of the 240x320 display.
    //
    // The existing title occupies the upper-left, so this region does not
    // interfere with "RATSPEAK" or "T-Deck Pro".
    const int x = 85;
    const int y = 5;
    const int w = 150;
    const int h = 34;

    display.setPartialWindow(
        x,
        y,
        w,
        h);

    display.firstPage();

    do {
        // Completely erase the old clock before drawing the new one.
        display.fillRect(
            x,
            y,
            w,
            h,
            GxEPD_WHITE);

        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMono9pt7b);

        display.setCursor(
            x + 2,
            y + 23);

        display.print(uptime);

    } while (display.nextPage());

    display.powerOff();
}

// -----------------------------------------------------------------------------
// Sleep / Wake
// -----------------------------------------------------------------------------

void sleep() {
    if (!initialized) {
        return;
    }

    display.powerOff();
}

void wake() {
    if (!initialized) {
        return;
    }

    Serial.println("[EINK] Waking...");

    display.epd2.selectSPI(
        SPI,
        SPISettings(
            2000000,
            MSBFIRST,
            SPI_MODE0));

    display.init(
        0,
        true,
        2,
        false);

    display.setRotation(0);

    display.setFont(
        &FreeMonoBold9pt7b);

    display.setTextColor(
        GxEPD_BLACK);

    Serial.println("[EINK] Awake");
}

bool isReady() {
    return initialized;
}

}  // namespace eink
}  // namespace tdeck_pro