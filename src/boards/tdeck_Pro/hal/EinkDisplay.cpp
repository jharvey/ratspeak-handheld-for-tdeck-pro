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

static void drawBatteryBar(int x, int y, int w, int h, int percent) {
    display.drawRect(x, y, w, h, GxEPD_BLACK);
    display.fillRect(x + w, y + h / 4, 3, h / 2, GxEPD_BLACK);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int fillW = (w - 4) * percent / 100;
    if (fillW > 0) {
        display.fillRect(x + 2, y + 2, fillW, h - 4, GxEPD_BLACK);
    }
}

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

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    display.epd2.selectSPI(SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0));
    display.init(0, true, 2, false);
    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    initialized = true;
    Serial.printf("[EINK] Ready (%d x %d)\r\n", display.width(), display.height());
    return true;
}

void clear() {
    if (!initialized) return;
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());
    display.powerOff();
}

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
        display.print("T-Deck Pro V1.1");
        display.setCursor(20, 110);
        display.print("NODE");
        display.setCursor(20, 140);
        display.print("E-INK UI");
        display.setCursor(20, 180);
        display.print(FIRMWARE_VERSION);
    } while (display.nextPage());
    display.powerOff();
}

void showTestScreen() {
    if (!initialized) return;
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMonoBold9pt7b);
        display.setCursor(20, 35);
        display.print("RATSPEAK");
        display.setCursor(20, 70);
        display.print("E-PAPER TEST");
        display.setCursor(20, 105);
        display.print("GDEQ031T10");
        display.setCursor(20, 140);
        display.print("240 x 320");
    } while (display.nextPage());
    display.powerOff();
}

void showStatusScreen(
    const char* destShort,
    int batteryPct,
    bool loraOnline,
    unsigned pathCount,
    const char* version) {
    showNodeHome(destShort, batteryPct, loraOnline, pathCount, 0, nullptr, version);
}

void showNodeHome(
    const char* destShort,
    int batteryPct,
    bool loraOnline,
    unsigned pathCount,
    unsigned linkCount,
    const char* lastEvent,
    const char* version) {

    if (!initialized) return;

    Serial.println("[EINK] Updating node home...");
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);

        display.setFont(&FreeMonoBold9pt7b);
        display.setCursor(12, 28);
        display.print("RATSPEAK NODE");

        display.setFont(&FreeMono9pt7b);
        display.setCursor(12, 48);
        display.print(version ? version : "----");

        display.drawFastHLine(10, 56, 220, GxEPD_BLACK);

        display.setCursor(12, 80);
        display.print("Dest ");
        display.print(destShort && destShort[0] ? destShort : "(none)");

        display.setCursor(12, 104);
        display.print("LoRa ");
        display.print(loraOnline ? "ONLINE" : "OFFLINE");

        display.setCursor(12, 128);
        display.print("Batt ");
        drawBatteryBar(70, 116, 100, 16, batteryPct);
        display.setCursor(180, 128);
        if (batteryPct >= 0) display.printf("%d%%", batteryPct);
        else display.print("--");

        display.setCursor(12, 152);
        display.printf("Paths %u  Links %u", pathCount, linkCount);

        display.drawFastHLine(10, 164, 220, GxEPD_BLACK);

        display.setCursor(12, 188);
        display.print("Event:");
        display.setCursor(12, 212);
        if (lastEvent && lastEvent[0]) {
            char line[28];
            snprintf(line, sizeof(line), "%.27s", lastEvent);
            display.print(line);
        } else {
            display.print("(idle)");
        }

        display.setCursor(12, 250);
        display.print("ENT=announce");
        display.setCursor(12, 274);
        display.print("msg <dest> <text>");
    } while (display.nextPage());

    display.powerOff();
    Serial.println("[EINK] Node home done");
}

// Bottom strip only — does not overwrite RATSPEAK title
void showUptime(uint32_t elapsedSeconds) {
    if (!initialized) return;

    uint32_t hours = elapsedSeconds / 3600UL;
    uint32_t minutes = (elapsedSeconds % 3600UL) / 60UL;
    uint32_t seconds = elapsedSeconds % 60UL;

    char uptime[24];
    snprintf(
        uptime,
        sizeof(uptime),
        "UP %02lu:%02lu:%02lu",
        (unsigned long)hours,
        (unsigned long)minutes,
        (unsigned long)seconds);

    const int x = 12;
    const int y = 295;
    const int w = 216;
    const int h = 22;

    display.setPartialWindow(x, y, w, h);
    display.firstPage();
    do {
        display.fillRect(x, y, w, h, GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMono9pt7b);
        display.setCursor(x + 2, y + 16);
        display.print(uptime);
    } while (display.nextPage());
    display.powerOff();
}

void sleep() {
    if (!initialized) return;
    display.powerOff();
}

void wake() {
    if (!initialized) return;
    display.epd2.selectSPI(SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0));
    display.init(0, true, 2, false);
    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
}

bool isReady() {
    return initialized;
}

}  // namespace eink
}  // namespace tdeck_pro