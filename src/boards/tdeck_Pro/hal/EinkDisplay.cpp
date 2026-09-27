#include "EinkDisplay.h"

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
// The panel is supported by GxEPD2 as:
//   GxEPD2_310_GDEQ031T10
//
// IMPORTANT:
// The four GPIOs below must match the T-Deck Pro board definition.
// Keep them in one place so we can replace them with the verified LilyGO
// board pin definitions without changing the rest of the driver.
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

bool tdeck_pro::eink::begin() {

  Serial.println("[EINK] Starting GDEQ031T10...");

  display.init(
      115200,
      true,
      2,
      false);

  display.setRotation(0);
  display.setTextColor(GxEPD_BLACK);

  displayReady = true;

  Serial.printf(
      "[EINK] display ready %dx%d\r\n",
      display.width(),
      display.height());

  return true;
}


// ============================================================================
// clear
// ============================================================================

void tdeck_pro::eink::clear() {

  if (!displayReady) {
    return;
  }

  Serial.println("[EINK] clearing display...");

  display.setFullWindow();

  display.firstPage();

  do {

    display.fillScreen(GxEPD_WHITE);

  } while (display.nextPage());

  Serial.println("[EINK] clear complete");
}


// ============================================================================
// showBootScreen
// ============================================================================

void tdeck_pro::eink::showBootScreen() {

  if (!displayReady) {
    return;
  }

  display.setFullWindow();

  display.firstPage();

  do {

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

void tdeck_pro::eink::showTestScreen() {

  if (!displayReady) {
    return;
  }

  Serial.println("[EINK] running display test...");

  display.setFullWindow();

  display.firstPage();

  do {

    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeMonoBold12pt7b);

    display.setCursor(15, 35);
    display.print("RATSPEAK");

    display.setFont(&FreeMonoBold9pt7b);

    display.setCursor(15, 65);
    display.print("T-Deck Pro");

    display.drawRect(
        10,
        80,
        display.width() - 20,
        100);

    display.setCursor(20, 110);
    display.print("E-INK DRIVER");

    display.setCursor(20, 140);
    display.print("TEST PASS");

    display.setCursor(20, 170);
    display.print("GDEQ031T10");

    display.setCursor(20, 200);
    display.print("UC8253");

    display.setCursor(20, 250);
    display.print("240 x 320");

    display.drawLine(
        10,
        270,
        display.width() - 10,
        270,
        GxEPD_BLACK);

    display.setCursor(20, 300);
    display.print("Ratspeak");

  } while (display.nextPage());

  Serial.println("[EINK] display test complete");
}


// ============================================================================
// sleep
// ============================================================================

void tdeck_pro::eink::sleep() {

  if (!displayReady) {
    return;
  }

  Serial.println("[EINK] sleeping...");

  display.hibernate();
}


// ============================================================================
// wake
// ============================================================================

void tdeck_pro::eink::wake() {

  if (!displayReady) {
    return;
  }

  Serial.println("[EINK] waking...");

  display.init(
      115200,
      false,
      2,
      false);
}


// ============================================================================
// isReady
// ============================================================================

bool tdeck_pro::eink::isReady() {

  return displayReady;
}