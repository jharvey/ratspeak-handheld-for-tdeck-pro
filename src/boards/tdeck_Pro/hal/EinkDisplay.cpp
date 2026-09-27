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
// LilyGO T-Deck Pro V1.1
//
// Display:
//   GDEQ031T10
//   240 x 320
//   UC8253
//
// Official LilyGO V1.1 wiring:
//   SCK  = 36
//   MOSI = 33
//   MISO = 47
//   CS   = 34
//   DC   = 35
//   BUSY = 37
//   RST  = 16
//
// The official LilyGO V1.1 factory firmware uses:
//   SPISettings(2000000, MSBFIRST, SPI_MODE0)
//   display.init(115200, true, 2, false)
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

bool begin() {
    Serial.println();
    Serial.println("[EINK] ========================================");
    Serial.println("[EINK] Starting GDEQ031T10 diagnostic");
    Serial.println("[EINK] ========================================");

    Serial.printf(
        "[EINK] SPI SCK=%d MOSI=%d MISO=%d\n",
        SPI_SCK,
        SPI_MOSI,
        SPI_MISO);

    Serial.printf(
        "[EINK] EPD CS=%d DC=%d BUSY=%d RST=%d\n",
        TDECK_PRO_EPD_CS,
        TDECK_PRO_EPD_DC,
        TDECK_PRO_EPD_BUSY,
        TDECK_PRO_EPD_RST);

    Serial.printf(
        "[EINK] SD CS=%d LORA CS=%d\n",
        SD_CS,
        LORA_CS);

    // -------------------------------------------------------------------------
    // Explicitly isolate every other device on the shared SPI bus.
    // This mirrors the LilyGO factory firmware.
    // -------------------------------------------------------------------------

    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);

    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    pinMode(TDECK_PRO_EPD_CS, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_CS, HIGH);

    // -------------------------------------------------------------------------
    // EPD control pins.
    // -------------------------------------------------------------------------

    pinMode(TDECK_PRO_EPD_DC, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_DC, LOW);

    pinMode(TDECK_PRO_EPD_BUSY, INPUT);

    // RST is physically connected on V1.1.
    // GxEPD2 will control it during display.init().
#if TDECK_PRO_EPD_RST >= 0
    pinMode(TDECK_PRO_EPD_RST, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_RST, HIGH);
#endif

    Serial.printf(
        "[EINK] BUSY before SPI = %d\n",
        digitalRead(TDECK_PRO_EPD_BUSY));

    // -------------------------------------------------------------------------
    // Start the shared SPI bus.
    //
    // LilyGO's factory firmware does this before EPD initialization.
    // -------------------------------------------------------------------------

    SPI.begin(
        SPI_SCK,
        SPI_MISO,
        SPI_MOSI);

    Serial.printf(
        "[EINK] BUSY after SPI = %d\n",
        digitalRead(TDECK_PRO_EPD_BUSY));

    // -------------------------------------------------------------------------
    // IMPORTANT:
    //
    // LilyGO's official V1.1 firmware explicitly selects SPI for the EPD at
    // 2 MHz, MODE0.
    // -------------------------------------------------------------------------

    display.epd2.selectSPI(
        SPI,
        SPISettings(
            2000000,
            MSBFIRST,
            SPI_MODE0));

    Serial.println("[EINK] EPD SPI selected: 2 MHz MODE0");

    // -------------------------------------------------------------------------
    // Official LilyGO V1.1 initialization:
    //
    //     display.init(115200, true, 2, false);
    //
    // The 'true' enables the initial hardware-reset sequence.
    // -------------------------------------------------------------------------

    Serial.println("[EINK] Calling display.init()...");

    display.init(
        115200,
        true,
        2,
        false);

    Serial.println("[EINK] display.init() returned");

    Serial.printf(
        "[EINK] BUSY after init = %d\n",
        digitalRead(TDECK_PRO_EPD_BUSY));

    Serial.printf(
        "[EINK] display size = %d x %d\n",
        display.width(),
        display.height());

    display.setRotation(0);

    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    initialized = true;

    Serial.println("[EINK] Initialization finished");

    return true;
}

void clear() {
    if (!initialized) {
        Serial.println("[EINK] clear() skipped - display not initialized");
        return;
    }

    Serial.println("[EINK] Clearing display...");

    display.setFullWindow();

    display.firstPage();

    do {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());

    Serial.println("[EINK] Clear complete");

    display.powerOff();
}

void showBootScreen() {
    if (!initialized) {
        Serial.println(
            "[EINK] showBootScreen() skipped - display not initialized");
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

void showTestScreen() {
    if (!initialized) {
        Serial.println(
            "[EINK] showTestScreen() skipped - display not initialized");
        return;
    }

    Serial.println();
    Serial.println("[EINK] ========================================");
    Serial.println("[EINK] E-INK TEST");
    Serial.println("[EINK] ========================================");

    Serial.printf(
        "[EINK] BUSY before test = %d\n",
        digitalRead(TDECK_PRO_EPD_BUSY));

    display.setFullWindow();

    Serial.println("[EINK] firstPage()");

    display.firstPage();

    do {
        Serial.println("[EINK] drawing test page");

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

    Serial.println("[EINK] test page complete");

    Serial.printf(
        "[EINK] BUSY after test = %d\n",
        digitalRead(TDECK_PRO_EPD_BUSY));

    // Do not use hibernate here.
    //
    // LilyGO's V1.1 firmware uses powerOff() because of the display-reset
    // behavior on this hardware revision.
    display.powerOff();

    Serial.println("[EINK] display powerOff() complete");
}

void sleep() {
    if (!initialized) {
        return;
    }

    // The official V1.1 firmware uses powerOff(), not hibernate().
    display.powerOff();
}

void wake() {
    if (!initialized) {
        return;
    }

    Serial.println("[EINK] Waking display...");

    // Reinitialize using the same sequence as LilyGO V1.1.
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

    Serial.printf(
        "[EINK] BUSY after wake = %d\n",
        digitalRead(TDECK_PRO_EPD_BUSY));
}

bool isReady() {
    return initialized;
}

}  // namespace eink
}  // namespace tdeck_pro