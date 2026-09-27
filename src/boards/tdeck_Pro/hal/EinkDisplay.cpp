#include "EinkDisplay.h"
#include "config/BoardConfig.h"

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>

// ============================================================================
// T-Deck Pro GDEQ031T10
// ============================================================================
//
// Display:
//   GDEQ031T10
//   240 x 320
//   UC8253
//
// GxEPD2 driver:
//   GxEPD2_310_GDEQ031T10
//
// T-Deck Pro V1.1 verified pins:
//   CS   = 34
//   DC   = 35
//   BUSY = 37
//   RST  = -1
//
// Shared SPI:
//   SCK  = 36
//   MOSI = 33
//   MISO = 47
// ============================================================================

#ifndef TDECK_PRO_EPD_CS
#error "TDECK_PRO_EPD_CS must be defined by the T-Deck Pro board configuration"
#endif

#ifndef TDECK_PRO_EPD_DC
#error "TDECK_PRO_EPD_DC must be defined by the T-Deck Pro board configuration"
#endif

#ifndef TDECK_PRO_EPD_RST
#error "TDECK_PRO_EPD_RST must be defined by the T-Deck Pro board configuration"
#endif

#ifndef TDECK_PRO_EPD_BUSY
#error "TDECK_PRO_EPD_BUSY must be defined by the T-Deck Pro board configuration"
#endif


// ============================================================================
// GxEPD2 display object
// ============================================================================

static GxEPD2_BW<
    GxEPD2_310_GDEQ031T10,
    GxEPD2_310_GDEQ031T10::HEIGHT
> display(
    GxEPD2_310_GDEQ031T10(
        TDECK_PRO_EPD_CS,
        TDECK_PRO_EPD_DC,
        TDECK_PRO_EPD_RST,
        TDECK_PRO_EPD_BUSY));


static bool displayReady = false;


// ============================================================================
// begin
// ============================================================================

bool tdeck_pro::eink::begin()
{
    Serial.println("[EINK] Starting GDEQ031T10...");

    // ------------------------------------------------------------------------
    // Configure the display GPIOs explicitly.
    //
    // T-Deck Pro V1.1 uses a shared SPI bus:
    //   SCK  36
    //   MOSI 33
    //   MISO 47
    //
    // Display:
    //   CS   34
    //   DC   35
    //   BUSY 37
    //
    // The display reset line is not physically connected on this hardware,
    // therefore EPD_RST remains -1.
    // ------------------------------------------------------------------------

    pinMode(TDECK_PRO_EPD_CS, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_CS, HIGH);

    pinMode(TDECK_PRO_EPD_DC, OUTPUT);
    digitalWrite(TDECK_PRO_EPD_DC, HIGH);

    pinMode(TDECK_PRO_EPD_BUSY, INPUT);

    // ------------------------------------------------------------------------
    // Initialize the shared SPI bus using the verified T-Deck Pro pins.
    // ------------------------------------------------------------------------

    SPI.begin(
        SPI_SCK,
        SPI_MISO,
        SPI_MOSI,
        TDECK_PRO_EPD_CS);

    delay(100);

    Serial.println("[EINK] calling GxEPD2 init...");

    // ------------------------------------------------------------------------
    // Initialize GxEPD2.
    //
    // IMPORTANT:
    // EPD_RST is -1 on the T-Deck Pro V1.1.
    // Do not request a hardware reset through a nonexistent GPIO.
    //
    // Parameters:
    //   115200 = diagnostic serial baud
    //   false  = don't perform hardware reset
    //   10     = reset duration parameter
    //   false  = no reset pulldown
    // ------------------------------------------------------------------------

    display.init(
        115200,
        false,
        10,
        false);

    display.setRotation(0);
    display.setTextColor(GxEPD_BLACK);

    displayReady = true;

    Serial.printf(
        "[EINK] display ready %dx%d BUSY=%d CS=%d DC=%d RST=%d\r\n",
        display.width(),
        display.height(),
        TDECK_PRO_EPD_BUSY,
        TDECK_PRO_EPD_CS,
        TDECK_PRO_EPD_DC,
        TDECK_PRO_EPD_RST);

    return true;
}


// ============================================================================
// clear
// ============================================================================

void tdeck_pro::eink::clear()
{
    if (!displayReady)
    {
        return;
    }

    Serial.println("[EINK] clearing display...");

    display.setFullWindow();

    display.firstPage();

    do
    {
        display.fillScreen(GxEPD_WHITE);

    } while (display.nextPage());

    Serial.println("[EINK] clear complete");
}


// ============================================================================
// showBootScreen
// ============================================================================

void tdeck_pro::eink::showBootScreen()
{
    if (!displayReady)
    {
        return;
    }

    display.setFullWindow();

    display.firstPage();

    do
    {
        display.fillScreen(GxEPD_WHITE);

        display.setFont(&FreeMonoBold12pt7b);

        display.setCursor(20, 45);
        display.print("RATSPEAK");

        display.setFont(&FreeMonoBold9pt7b);

        display.setCursor(20, 75);
        display.print("T-Deck Pro");

        display.setCursor(20, 105);
        display.print("E-INK OK");

        display.setCursor(20, 135);
        display.print("GDEQ031T10");

        display.setCursor(20, 180);
        display.print("240 x 320");

    } while (display.nextPage());

    Serial.println("[EINK] boot screen displayed");
}


// ============================================================================
// showTestScreen
// ============================================================================

void tdeck_pro::eink::showTestScreen()
{
    if (!displayReady)
    {
        return;
    }

    Serial.println("[EINK] running display test...");

    display.setRotation(0);
    display.setFullWindow();

    display.firstPage();

    do
    {
        display.fillScreen(GxEPD_WHITE);

        display.setFont(&FreeMonoBold12pt7b);

        display.setCursor(20, 40);
        display.print("RATSPEAK");

        display.setFont(&FreeMonoBold9pt7b);

        display.setCursor(20, 70);
        display.print("T-Deck Pro");

        display.drawRect(
            10,
            90,
            display.width() - 20,
            display.height() - 110,
            GxEPD_BLACK);

        display.setCursor(20, 120);
        display.print("E-INK DRIVER");

        display.setCursor(20, 150);
        display.print("TEST");

        display.setCursor(20, 180);
        display.print("GDEQ031T10");

        display.setCursor(20, 210);
        display.print("UC8253");

        display.setCursor(20, 240);
        display.print("240 x 320");

        display.drawLine(
            10,
            260,
            display.width() - 10,
            260,
            GxEPD_BLACK);

        display.setCursor(20, 290);
        display.print("Ratspeak");

    } while (display.nextPage());

    Serial.println("[EINK] display test complete");
}


// ============================================================================
// sleep
// ============================================================================

void tdeck_pro::eink::sleep()
{
    if (!displayReady)
    {
        return;
    }

    Serial.println("[EINK] sleeping...");

    display.hibernate();
}


// ============================================================================
// wake
// ============================================================================

void tdeck_pro::eink::wake()
{
    if (!displayReady)
    {
        return;
    }

    Serial.println("[EINK] waking...");

    // EPD_RST is not physically connected on the T-Deck Pro V1.1,
    // so don't request a hardware reset here either.

    display.init(
        115200,
        false,
        10,
        false);

    display.setRotation(0);
    display.setTextColor(GxEPD_BLACK);
}


// ============================================================================
// isReady
// ============================================================================

bool tdeck_pro::eink::isReady()
{
    return displayReady;
}