#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <gdeq/GxEPD2_310_GDEQ031T10.h>
#include "config/BoardConfig.h"

// GxEPD2: CS, DC, RST, BUSY — BUSY is active LOW on this panel
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT>
  display(GxEPD2_310_GDEQ031T10(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" RATSPEAK  T-Deck Pro  GxEPD2 TEST");
  Serial.println("========================================");

  pinMode(BOARD_1V8_EN, OUTPUT);
  pinMode(BOARD_LORA_EN, OUTPUT);
  digitalWrite(BOARD_1V8_EN, HIGH);
  digitalWrite(BOARD_LORA_EN, HIGH);
  delay(50);

  // Shared SPI bus (LoRa + e-ink)
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  // Keep LoRa CS high so it does not steal the bus
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  Serial.println("[BOOT] init GxEPD2 GDEQ031T10...");
  display.init(115200, true, 50, false);
  display.setRotation(0);

  Serial.println("[BOOT] clear white...");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());

  delay(500);

  Serial.println("[BOOT] draw test pattern...");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    // Title bar
    display.fillRect(0, 0, display.width(), 28, GxEPD_BLACK);
    // Bars
    display.fillRect(20, 60, 280, 20, GxEPD_BLACK);
    display.fillRect(20, 100, 280, 20, GxEPD_BLACK);
    // Border
    display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
    display.drawRect(1, 1, display.width() - 2, display.height() - 2, GxEPD_BLACK);
  } while (display.nextPage());

  Serial.println("[BOOT] done — check e-ink panel for black bars/border");
}

void loop() {
  delay(1000);
}