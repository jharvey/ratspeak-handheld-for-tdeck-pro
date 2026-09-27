#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include "config/BoardConfig.h"
#include "hal/Power.h"

// =============================================================================
// LED — blink only, no Serial every second
// =============================================================================
static void heartbeatLed(bool on) {
  pinMode(BOARD_KEYBOARD_LED, OUTPUT);
  digitalWrite(BOARD_KEYBOARD_LED, on ? HIGH : LOW);
}

// =============================================================================
// I2C scan (boot only)
// =============================================================================
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

// =============================================================================
// TCA8418 keyboard
// =============================================================================
static constexpr uint8_t TCA_ADDR = 0x34;

static bool tcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t tcaRead(uint8_t reg) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom(TCA_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

static bool keyboardBegin() {
  if (!tcaWrite(0x01, 0x11)) return false;
  tcaWrite(0x1D, 0x0F);
  tcaWrite(0x1E, 0xFF);
  tcaWrite(0x1F, 0x03);
  tcaWrite(0x01, 0x11);
  tcaWrite(0x02, 0x00);
  while (tcaRead(0x03) & 0x0F) {
    (void)tcaRead(0x04);
  }
  Serial.println("[KB] ready — press keys");
  return true;
}

static void keyboardPoll() {
  uint8_t pending = tcaRead(0x03) & 0x0F;
  while (pending) {
    uint8_t ev = tcaRead(0x04);
    bool pressed = (ev & 0x80) != 0;
    uint8_t code = ev & 0x7F;
    if (code == 0) break;
    uint8_t idx = code - 1;
    Serial.printf("[KB] %s row=%u col=%u code=%u\r\n",
                  pressed ? "PRESS " : "RELEASE",
                  idx / 10, idx % 10, code);
    pending = tcaRead(0x03) & 0x0F;
  }
}

// =============================================================================
// LoRa SPI smoke (boot only)
// =============================================================================
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

static bool loraBeginSmoke() {
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
  return !(msb == 0xFF && lsb == 0xFF);
}

// =============================================================================
// GPS — quiet: parse in background, report on boot window + every 60s
// =============================================================================
static HardwareSerial GPSSerial(2);
static bool gpsOnline = false;
static uint32_t gpsBytes = 0;
static char gpsLine[128];
static size_t gpsLineLen = 0;

// Last parsed fields
static bool gpsHasRmc = false;
static bool gpsFixValid = false;
static char gpsTime[16] = "";
static char gpsLat[16] = "";
static char gpsNs[2] = "";
static char gpsLon[16] = "";
static char gpsEw[2] = "";
static int gpsSats = -1;

static void gpsClearLine() { gpsLineLen = 0; }

static bool startsWith(const char* s, const char* pfx) {
  while (*pfx) {
    if (*s++ != *pfx++) return false;
  }
  return true;
}

// Very small CSV field getter (0-based)
static bool getField(const char* line, int idx, char* out, size_t outSz) {
  int f = 0;
  const char* p = line;
  while (*p && f < idx) {
    if (*p == ',') f++;
    p++;
  }
  if (f != idx) return false;
  size_t n = 0;
  while (*p && *p != ',' && *p != '*' && n + 1 < outSz) {
    out[n++] = *p++;
  }
  out[n] = '\0';
  return true;
}

static void gpsHandleLine(const char* line) {
  // $GNRMC / $GPRMC
  if (startsWith(line, "$GNRMC") || startsWith(line, "$GPRMC")) {
    char status[4] = "";
    getField(line, 1, gpsTime, sizeof(gpsTime));
    getField(line, 2, status, sizeof(status));
    getField(line, 3, gpsLat, sizeof(gpsLat));
    getField(line, 4, gpsNs, sizeof(gpsNs));
    getField(line, 5, gpsLon, sizeof(gpsLon));
    getField(line, 6, gpsEw, sizeof(gpsEw));
    gpsFixValid = (status[0] == 'A');
    gpsHasRmc = true;
  }
  // $GNGGA / $GPGGA — field 7 = sat count
  if (startsWith(line, "$GNGGA") || startsWith(line, "$GPGGA")) {
    char sats[8] = "";
    if (getField(line, 7, sats, sizeof(sats)) && sats[0]) {
      gpsSats = atoi(sats);
    }
  }
}

static void gpsPrintSummary(const char* tag) {
  Serial.printf("[GPS] %s bytes=%lu fix=%s sats=%d time=%s lat=%s%s lon=%s%s\r\n",
                tag,
                (unsigned long)gpsBytes,
                gpsFixValid ? "YES" : "no",
                gpsSats,
                gpsTime[0] ? gpsTime : "-",
                gpsLat[0] ? gpsLat : "-",
                gpsNs,
                gpsLon[0] ? gpsLon : "-",
                gpsEw);
}

static bool gpsBegin() {
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);
  delay(200);
#if GPS_PPS >= 0
  pinMode(GPS_PPS, INPUT);
#endif
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  GPSSerial.setTimeout(0);
  gpsOnline = true;
  gpsBytes = 0;
  gpsClearLine();
  Serial.printf("[GPS] UART %u baud RX=%d TX=%d EN=%d\r\n",
                (unsigned)GPS_BAUD, GPS_RX, GPS_TX, GPS_EN);
  return true;
}

static void gpsPoll() {
  if (!gpsOnline) return;
  while (GPSSerial.available() > 0) {
    char c = (char)GPSSerial.read();
    gpsBytes++;
    if (c == '\r') continue;
    if (c == '\n') {
      if (gpsLineLen > 0) {
        gpsLine[gpsLineLen] = '\0';
        gpsHandleLine(gpsLine);
        gpsLineLen = 0;
      }
      continue;
    }
    if (gpsLineLen + 1 < sizeof(gpsLine)) {
      gpsLine[gpsLineLen++] = c;
    } else {
      gpsLineLen = 0;
    }
  }
}

// =============================================================================
// SD card smoke test (shared SPI — keep LoRa/EPD CS high)
// =============================================================================
static bool sdBeginSmoke() {
  // Ensure other slaves are deselected
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(5);

  // SD library uses its own SPI settings; bus pins already set via SPI.begin
  if (!SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("[SD] FAIL — not detected (check card / CS=48 / SPI pins)");
    return false;
  }

  uint8_t type = SD.cardType();
  const char* typeStr = "UNKNOWN";
  if (type == CARD_NONE) typeStr = "NONE";
  else if (type == CARD_MMC) typeStr = "MMC";
  else if (type == CARD_SD) typeStr = "SD";
  else if (type == CARD_SDHC) typeStr = "SDHC";

  uint64_t sizeMb = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("[SD] OK type=%s size=%llu MB\r\n", typeStr, (unsigned long long)sizeMb);

  // List a few root entries
  File root = SD.open("/");
  if (!root) {
    Serial.println("[SD] open / failed");
    return true;  // card still detected
  }
  Serial.println("[SD] root:");
  int n = 0;
  for (File f = root.openNextFile(); f && n < 12; f = root.openNextFile(), n++) {
    Serial.printf("  %s%s  %u\r\n", f.name(), f.isDirectory() ? "/" : "", (unsigned)f.size());
    f.close();
  }
  root.close();

  // Optional write probe (creates /ratspeak_pro_test.txt)
  File w = SD.open("/ratspeak_pro_test.txt", FILE_WRITE);
  if (w) {
    w.printf("tdeck-pro sd ok t=%lu\r\n", (unsigned long)millis());
    w.close();
    Serial.println("[SD] write /ratspeak_pro_test.txt OK");
  } else {
    Serial.println("[SD] write test failed (card may be locked)");
  }

  return true;
}

// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=================================");
  Serial.printf("  %s  (headless Pro)\r\n", DEVICE_NAME);
  Serial.println("=================================");

  Power::enablePeripherals();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQUENCY);
  Wire.setTimeOut(50);
  scanI2C();

  keyboardBegin();

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  loraBeginSmoke();
  sdBeginSmoke();   // after SPI begin; CS isolation inside

  gpsBegin();

  // Quiet LED on briefly
  heartbeatLed(true);
  delay(150);
  heartbeatLed(false);

  // Give GPS a few seconds of quiet parsing, then one summary
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    gpsPoll();
    delay(1);
  }
  gpsPrintSummary("boot");

  Serial.println("[BOOT] ready — keys logged; GPS summary every 60s; LED silent blink");
  Serial.println();
}

void loop() {
  keyboardPoll();
  gpsPoll();

  static uint32_t lastLed = 0;
  static uint32_t lastGps = 0;
  static bool ledOn = false;
  uint32_t now = millis();

  // LED blink only — no Serial
  if (now - lastLed >= 1000) {
    lastLed = now;
    ledOn = !ledOn;
    heartbeatLed(ledOn);
  }

  // GPS summary once per minute
  if (now - lastGps >= 60000UL) {
    lastGps = now;
    gpsPrintSummary("1min");
  }
}