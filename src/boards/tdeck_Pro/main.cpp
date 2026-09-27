#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include "config/BoardConfig.h"
#include "hal/Power.h"
#include "hal/Keyboard.h"

static Keyboard keyboard;

static void heartbeatLed(bool on) {
  pinMode(BOARD_KEYBOARD_LED, OUTPUT);
  digitalWrite(BOARD_KEYBOARD_LED, on ? HIGH : LOW);
}

static void scanI2C() {
  Serial.println("[I2C] Scanning...");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]  device at 0x%02X\r\n", addr);
      found++;
    }
  }
  Serial.printf("[I2C]  %u device(s)\r\n", found);
}

static void waitBusy(uint32_t timeoutMs = 1000) {
  uint32_t start = millis();
  while (digitalRead(LORA_BUSY) == HIGH) {
    if (millis() - start > timeoutMs) return;
  }
}

static uint8_t loraReadReg(uint16_t addr) {
  waitBusy();
  digitalWrite(LORA_CS, LOW);
  SPI.transfer(0x1D);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  SPI.transfer(0x00);
  uint8_t val = SPI.transfer(0x00);
  digitalWrite(LORA_CS, HIGH);
  waitBusy();
  return val;
}

static void loraBeginSmoke() {
  pinMode(BOARD_LORA_EN, OUTPUT);
  digitalWrite(BOARD_LORA_EN, HIGH);
  delay(10);
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);
  pinMode(LORA_RST, OUTPUT);
  pinMode(LORA_BUSY, INPUT);
  pinMode(LORA_IRQ, INPUT);
  digitalWrite(LORA_RST, LOW);
  delay(10);
  digitalWrite(LORA_RST, HIGH);
  delay(20);
  waitBusy();
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  uint8_t msb = loraReadReg(0x0740);
  uint8_t lsb = loraReadReg(0x0741);
  SPI.endTransaction();
  Serial.printf("[LORA] syncword=0x%02X%02X %s\r\n", msb, lsb,
                (msb == 0xFF && lsb == 0xFF) ? "FAIL" : "OK");
}

static void sdBeginSmoke() {
  digitalWrite(LORA_CS, HIGH);
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  if (!SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("[SD] FAIL");
    return;
  }
  Serial.printf("[SD] OK size=%llu MB\r\n",
                (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
}

// --- GPS quiet summary (same as before, shortened) ---
static HardwareSerial GPSSerial(2);
static uint32_t gpsBytes = 0;
static char gpsLine[128];
static size_t gpsLineLen = 0;
static bool gpsFixValid = false;
static char gpsTime[16] = "";
static char gpsLat[16] = "";
static char gpsNs[2] = "";
static char gpsLon[16] = "";
static char gpsEw[2] = "";
static int gpsSats = -1;

static bool getField(const char* line, int idx, char* out, size_t outSz) {
  int f = 0;
  const char* p = line;
  while (*p && f < idx) {
    if (*p++ == ',') f++;
  }
  if (f != idx) return false;
  size_t n = 0;
  while (*p && *p != ',' && *p != '*' && n + 1 < outSz) out[n++] = *p++;
  out[n] = '\0';
  return true;
}

static void gpsHandleLine(const char* line) {
  if (!strncmp(line, "$GNRMC", 6) || !strncmp(line, "$GPRMC", 6)) {
    char st[4] = "";
    getField(line, 1, gpsTime, sizeof(gpsTime));
    getField(line, 2, st, sizeof(st));
    getField(line, 3, gpsLat, sizeof(gpsLat));
    getField(line, 4, gpsNs, sizeof(gpsNs));
    getField(line, 5, gpsLon, sizeof(gpsLon));
    getField(line, 6, gpsEw, sizeof(gpsEw));
    gpsFixValid = (st[0] == 'A');
  }
  if (!strncmp(line, "$GNGGA", 6) || !strncmp(line, "$GPGGA", 6)) {
    char sats[8] = "";
    if (getField(line, 7, sats, sizeof(sats)) && sats[0]) gpsSats = atoi(sats);
  }
}

static void gpsBegin() {
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);
  delay(200);
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("[GPS] started");
}

static void gpsPoll() {
  while (GPSSerial.available() > 0) {
    char c = (char)GPSSerial.read();
    gpsBytes++;
    if (c == '\r') continue;
    if (c == '\n') {
      if (gpsLineLen) {
        gpsLine[gpsLineLen] = '\0';
        gpsHandleLine(gpsLine);
        gpsLineLen = 0;
      }
      continue;
    }
    if (gpsLineLen + 1 < sizeof(gpsLine)) gpsLine[gpsLineLen++] = c;
    else gpsLineLen = 0;
  }
}

static void gpsPrintSummary(const char* tag) {
  Serial.printf("[GPS] %s bytes=%lu fix=%s sats=%d time=%s lat=%s%s lon=%s%s\r\n",
                tag, (unsigned long)gpsBytes, gpsFixValid ? "YES" : "no", gpsSats,
                gpsTime[0] ? gpsTime : "-",
                gpsLat[0] ? gpsLat : "-", gpsNs,
                gpsLon[0] ? gpsLon : "-", gpsEw);
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=================================");
  Serial.printf("  %s  (keyboard HAL)\r\n", DEVICE_NAME);
  Serial.println("=================================");

  Power::enablePeripherals();
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQUENCY);
  Wire.setTimeOut(50);
  scanI2C();

  keyboard.begin();
  keyboard.backlightOn();

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  loraBeginSmoke();
  sdBeginSmoke();
  gpsBegin();

  uint32_t t0 = millis();
  while (millis() - t0 < 2500) {
    gpsPoll();
    delay(1);
  }
  gpsPrintSummary("boot");

  Serial.println("[BOOT] type on keyboard → KeyEvent via HAL");
}

void loop() {
  keyboard.update();
  if (keyboard.hasEvent()) {
    const KeyEvent& e = keyboard.getEvent();
    if (e.enter) Serial.println("[KEY] ENTER");
    else if (e.del) Serial.println("[KEY] DEL");
    else if (e.space) Serial.println("[KEY] SPACE");
    else if (e.character) Serial.printf("[KEY] '%c'\r\n", e.character);
    else Serial.println("[KEY] (modifier/other)");
  }

  gpsPoll();

  static uint32_t lastLed = 0, lastGps = 0;
  static bool on = false;
  uint32_t now = millis();
  if (now - lastLed >= 1000) {
    lastLed = now;
    on = !on;
    heartbeatLed(on);
  }
  if (now - lastGps >= 60000UL) {
    lastGps = now;
    gpsPrintSummary("1min");
  }
}