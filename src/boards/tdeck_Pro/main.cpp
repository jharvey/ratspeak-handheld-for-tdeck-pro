#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include "config/BoardConfig.h"
#include "hal/Power.h"
#include "hal/Keyboard.h"
#include "hal/EinkDisplay.h"

// =============================================================================
// Global hardware objects
// =============================================================================

static Keyboard keyboard;

// =============================================================================
// LED heartbeat
// =============================================================================

static void heartbeatLed(bool on) {
  pinMode(BOARD_KEYBOARD_LED, OUTPUT);
  digitalWrite(BOARD_KEYBOARD_LED, on ? HIGH : LOW);
}

// =============================================================================
// I2C scan
// =============================================================================

static void scanI2C() {
  Serial.println("[I2C] Scanning...");

  uint8_t found = 0;

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);

    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] device at 0x%02X\r\n", addr);
      found++;
    }
  }

  Serial.printf("[I2C] %u device(s)\r\n", found);
}


// =============================================================================
// CST328 touch controller
// =============================================================================
//
// T-Deck Pro V1.1:
//   I2C address : 0x1A
//   INT         : GPIO 12
//   RESET       : GPIO 45
//
// For the current headless bring-up milestone, the touch test intentionally
// stays simple: verify the controller responds and poll the finger-count
// register for a touched yes/no result. No display or LVGL dependency.
//
// CST328 register:
//   0xD005 = finger count
// =============================================================================

#define CST328_REG_FINGER_COUNT 0xD005

static bool cst328Read(
    uint16_t reg,
    uint8_t* data,
    size_t length) {

  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(
          (int)TOUCH_I2C_ADDR,
          (int)length,
          (int)true) != (int)length) {
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    data[i] = Wire.read();
  }

  return true;
}

static bool cst328BeginSmoke() {
  Serial.println("[TOUCH] Starting CST328 test...");

  uint8_t fingerCount = 0;

  if (!cst328Read(
          CST328_REG_FINGER_COUNT,
          &fingerCount,
          1)) {

    Serial.printf(
        "[TOUCH] CST328 @0x%02X read FAIL\r\n",
        TOUCH_I2C_ADDR);

    return false;
  }

  Serial.printf(
      "[TOUCH] CST328 @0x%02X OK fingers=%u\r\n",
      TOUCH_I2C_ADDR,
      fingerCount & 0x0F);

  return true;
}

static void cst328Poll() {
  static uint32_t lastPoll = 0;
  static bool lastTouched = false;

  uint32_t now = millis();

  // Poll at 50 Hz. The guide only calls for a short touched yes/no test,
  // so there is no need to continuously read the full touch packet yet.
  if (now - lastPoll < 20) {
    return;
  }

  lastPoll = now;

  uint8_t fingerCount = 0;

  if (!cst328Read(
          CST328_REG_FINGER_COUNT,
          &fingerCount,
          1)) {
    return;
  }

  bool touched = (fingerCount & 0x0F) != 0;

  // Only print on state changes so Serial remains usable for the other
  // headless bring-up tests.
  if (touched != lastTouched) {
    lastTouched = touched;

    Serial.printf(
        "[TOUCH] touched=%s fingers=%u\r\n",
        touched ? "YES" : "NO",
        fingerCount & 0x0F);
  }
}

// =============================================================================
// Shared SPI / LoRa
// =============================================================================

static void waitBusy(uint32_t timeoutMs = 1000) {
  uint32_t start = millis();

  while (digitalRead(LORA_BUSY) == HIGH) {
    if (millis() - start > timeoutMs) {
      return;
    }
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

  SPI.beginTransaction(
      SPISettings(2000000, MSBFIRST, SPI_MODE0));

  uint8_t msb = loraReadReg(0x0740);
  uint8_t lsb = loraReadReg(0x0741);

  SPI.endTransaction();

  bool ok = !((msb == 0xFF) && (lsb == 0xFF));

  Serial.printf(
      "[LORA] syncword=0x%02X%02X %s\r\n",
      msb,
      lsb,
      ok ? "OK" : "FAIL");

  return ok;
}

// =============================================================================
// SD card / shared SPI smoke test
// =============================================================================

static bool sdBeginSmoke() {
  Serial.println("[SD] Starting shared-SPI test...");

  // Keep other SPI chip selects inactive.
  digitalWrite(LORA_CS, HIGH);

  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  if (!SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("[SD] mount FAIL");
    return false;
  }

  uint64_t sizeMB =
      SD.cardSize() / (1024ULL * 1024ULL);

  Serial.printf(
      "[SD] mounted type=%s size=%llu MB\r\n",
      SD.cardType() == CARD_MMC ? "MMC" :
      SD.cardType() == CARD_SD  ? "SDSC" :
      SD.cardType() == CARD_SDHC ? "SDHC" :
      "UNKNOWN",
      (unsigned long long)sizeMB);

  Serial.println("[SD] root entries:");

  File root = SD.open("/");

  if (root) {
    File file = root.openNextFile();

    while (file) {
      Serial.printf(
          "  %s size=%llu\r\n",
          file.name(),
          (unsigned long long)file.size());

      file.close();
      file = root.openNextFile();
    }

    root.close();
  }

  // ---------------------------------------------------------------------------
  // Write test
  // ---------------------------------------------------------------------------

  const char* testPath = "/ratspeak_pro_battery_spi_test.txt";
  const char* testText = "tdeck-pro shared SPI test";

  SD.remove(testPath);

  File writeFile = SD.open(
      testPath,
      FILE_WRITE);

  if (!writeFile) {
    Serial.println("[SD] write open FAIL");
    return false;
  }

  writeFile.print(testText);
  writeFile.close();

  // ---------------------------------------------------------------------------
  // Readback
  // ---------------------------------------------------------------------------

  File readFile = SD.open(testPath, FILE_READ);

  if (!readFile) {
    Serial.println("[SD] read open FAIL");
    return false;
  }

  String contents = readFile.readString();
  readFile.close();

  contents.trim();

  Serial.printf(
      "[SD] readback: %s\r\n",
      contents.c_str());

  if (contents != testText) {
    Serial.println("[SD] readback FAIL");
    return false;
  }

  Serial.println("[SD] filesystem test PASS");

  // ---------------------------------------------------------------------------
  // Verify LoRa again after SD access.
  // ---------------------------------------------------------------------------

  Serial.println(
      "[SPI] Rechecking LoRa after SD operations...");

  digitalWrite(SD_CS, HIGH);

  bool loraOk = loraBeginSmoke();

  Serial.printf(
      "[SPI] post-SD LoRa = %s\r\n",
      loraOk ? "OK" : "FAIL");

  return loraOk;
}

// =============================================================================
// DRV2605 haptic motor
// =============================================================================

#define DRV2605_ADDR       0x5A
#define DRV_REG_MODE       0x01
#define DRV_REG_RTPIN      0x02
#define DRV_REG_STATUS     0x00

static bool i2cWrite8(
    uint8_t addr,
    uint8_t reg,
    uint8_t value) {

  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);

  return Wire.endTransmission() == 0;
}

static bool i2cRead8(
    uint8_t addr,
    uint8_t reg,
    uint8_t& value) {

  Wire.beginTransmission(addr);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(
          (int)addr,
          1,
          (int)true) != 1) {
    return false;
  }

  value = Wire.read();

  return true;
}

static bool hapticBegin() {
  Serial.println("[HAPTIC] enabling motor power...");

  pinMode(BOARD_MOTOR_PIN, OUTPUT);
  digitalWrite(BOARD_MOTOR_PIN, HIGH);

  delay(100);

  Serial.printf(
      "[HAPTIC] MOTOR_EN GPIO=%d state=%s\r\n",
      BOARD_MOTOR_PIN,
      digitalRead(BOARD_MOTOR_PIN) ? "HIGH" : "LOW");

  uint8_t status = 0;

  if (!i2cRead8(
          DRV2605_ADDR,
          DRV_REG_STATUS,
          status)) {

    Serial.println(
        "[HAPTIC] DRV2605 status read FAIL");

    return false;
  }

  Serial.printf(
      "[HAPTIC] DRV2605 status=0x%02X\r\n",
      status);

  // Real-time playback mode.
  if (!i2cWrite8(
          DRV2605_ADDR,
          DRV_REG_MODE,
          0x05)) {

    Serial.println(
        "[HAPTIC] DRV2605 mode write FAIL");

    return false;
  }

  // Start with zero output.
  i2cWrite8(
      DRV2605_ADDR,
      DRV_REG_RTPIN,
      0x00);

  Serial.println(
      "[HAPTIC] controller ready; motor enable HIGH");

  return true;
}

static void hapticPulse(
    uint8_t amplitude,
    uint32_t durationMs) {

  i2cWrite8(
      DRV2605_ADDR,
      DRV_REG_RTPIN,
      amplitude);

  delay(durationMs);

  i2cWrite8(
      DRV2605_ADDR,
      DRV_REG_RTPIN,
      0x00);
}

static void hapticSmokeTest() {
  if (!hapticBegin()) {
    return;
  }

  Serial.println(
      "[HAPTIC] starting 5-pulse test");

  for (int i = 0; i < 5; i++) {
    Serial.printf(
        "[HAPTIC] pulse %d/5 amplitude=0x7F\r\n",
        i + 1);

    hapticPulse(0x7F, 200);

    delay(800);
  }

  i2cWrite8(
      DRV2605_ADDR,
      DRV_REG_RTPIN,
      0x00);

  Serial.println(
      "[HAPTIC] test complete");
}

// =============================================================================
// BQ27220 fuel gauge
// =============================================================================
//
// BQ27220 standard data commands:
//   0x06 Temperature       0.1 K
//   0x08 Voltage           mV
//   0x0A BatteryStatus     flags
//   0x0C Current           signed mA
//   0x10 RemainingCapacity mAh
//   0x12 FullChargeCapacity mAh
//   0x14 AverageCurrent    mA
//   0x16 TimeToEmpty       minutes
//   0x18 TimeToFull        minutes
//   0x2A CycleCount        cycles
//   0x2C Relative SOC     %
//   0x2E State of Health   %
//   0x3A OperationStatus   flags
//
// All are read-only for this smoke test.
// =============================================================================

#define BQ27220_REG_TEMPERATURE       0x06
#define BQ27220_REG_VOLTAGE           0x08
#define BQ27220_REG_STATUS            0x0A
#define BQ27220_REG_CURRENT           0x0C
#define BQ27220_REG_REMAINING_CAP     0x10
#define BQ27220_REG_FULL_CHARGE_CAP   0x12
#define BQ27220_REG_AVERAGE_CURRENT   0x14
#define BQ27220_REG_TIME_TO_EMPTY     0x16
#define BQ27220_REG_TIME_TO_FULL      0x18
#define BQ27220_REG_CYCLE_COUNT       0x2A
#define BQ27220_REG_SOC               0x2C
#define BQ27220_REG_SOH               0x2E
#define BQ27220_REG_OPERATION_STATUS  0x3A

static bool bq27220ReadWord(
    uint8_t reg,
    uint16_t& value) {

  Wire.beginTransmission(BQ27220_I2C_ADDR);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(
          (int)BQ27220_I2C_ADDR,
          2,
          (int)true) != 2) {
    return false;
  }

  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();

  value = ((uint16_t)hi << 8) | lo;

  return true;
}

static bool bq27220ReadSigned(
    uint8_t reg,
    int16_t& value) {

  uint16_t raw = 0;

  if (!bq27220ReadWord(reg, raw)) {
    return false;
  }

  value = (int16_t)raw;

  return true;
}

static bool batteryGaugeTest() {
  uint16_t temperatureRaw = 0;
  uint16_t voltage = 0;
  uint16_t status = 0;
  int16_t current = 0;
  uint16_t remaining = 0;
  uint16_t fullCharge = 0;
  int16_t averageCurrent = 0;
  uint16_t timeToEmpty = 0;
  uint16_t timeToFull = 0;
  uint16_t cycleCount = 0;
  uint16_t soc = 0;
  uint16_t soh = 0;
  uint16_t operationStatus = 0;

  if (!bq27220ReadWord(
          BQ27220_REG_TEMPERATURE,
          temperatureRaw)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_VOLTAGE,
          voltage)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_STATUS,
          status)) {
    return false;
  }

  if (!bq27220ReadSigned(
          BQ27220_REG_CURRENT,
          current)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_REMAINING_CAP,
          remaining)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_FULL_CHARGE_CAP,
          fullCharge)) {
    return false;
  }

  if (!bq27220ReadSigned(
          BQ27220_REG_AVERAGE_CURRENT,
          averageCurrent)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_TIME_TO_EMPTY,
          timeToEmpty)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_TIME_TO_FULL,
          timeToFull)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_CYCLE_COUNT,
          cycleCount)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_SOC,
          soc)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_SOH,
          soh)) {
    return false;
  }

  if (!bq27220ReadWord(
          BQ27220_REG_OPERATION_STATUS,
          operationStatus)) {
    return false;
  }

  float temperatureC =
      ((float)temperatureRaw / 10.0f) - 273.15f;

  Serial.println("[BATTERY] BQ27220:");

  Serial.printf(
      "  voltage        = %u mV (%.3f V)\r\n",
      voltage,
      voltage / 1000.0f);

  Serial.printf(
      "  current        = %d mA\r\n",
      current);

  Serial.printf(
      "  avg current    = %d mA\r\n",
      averageCurrent);

  Serial.printf(
      "  temperature    = %.1f C\r\n",
      temperatureC);

  Serial.printf(
      "  state of charge= %u %%\r\n",
      soc);

  Serial.printf(
      "  state of health= %u %%\r\n",
      soh);

  Serial.printf(
      "  remaining cap  = %u mAh\r\n",
      remaining);

  Serial.printf(
      "  full charge    = %u mAh\r\n",
      fullCharge);

  Serial.printf(
      "  time to empty  = %u min\r\n",
      timeToEmpty);

  Serial.printf(
      "  time to full   = %u min\r\n",
      timeToFull);

  Serial.printf(
      "  cycle count    = %u\r\n",
      cycleCount);

  Serial.printf(
      "  battery status = 0x%04X\r\n",
      status);

  Serial.printf(
      "  operation      = 0x%04X\r\n",
      operationStatus);

  return true;
}

// =============================================================================
// BQ25896 charger
// =============================================================================
//
// Read-only charger status test.
//
// REG0B:
//   bits 7:5 = VBUS_STAT
//   bits 4:3 = CHRG_STAT
//   bit  2   = PG_STAT
//   bit  0   = VSYS_STAT
//
// REG0C:
//   fault status.
// =============================================================================

#define BQ25896_REG_STATUS  0x0B
#define BQ25896_REG_FAULT   0x0C

static bool bq25896ReadRegister(
    uint8_t reg,
    uint8_t& value) {

  return i2cRead8(
      BQ25896_I2C_ADDR,
      reg,
      value);
}

static bool chargerTest() {
  uint8_t status = 0;
  uint8_t fault = 0;

  if (!bq25896ReadRegister(
          BQ25896_REG_STATUS,
          status)) {

    Serial.println(
        "[CHARGER] BQ25896 status read FAIL");

    return false;
  }

  if (!bq25896ReadRegister(
          BQ25896_REG_FAULT,
          fault)) {

    Serial.println(
        "[CHARGER] BQ25896 fault read FAIL");

    return false;
  }

  uint8_t vbusStat =
      (status >> 5) & 0x07;

  uint8_t chargeStat =
      (status >> 3) & 0x03;

  bool powerGood =
      (status & 0x04) != 0;

  bool vsysRegulation =
      (status & 0x01) != 0;

  Serial.println("[CHARGER] BQ25896:");

  Serial.printf(
      "  status         = 0x%02X\r\n",
      status);

  Serial.printf(
      "  VBUS_STAT      = %u\r\n",
      vbusStat);

  Serial.printf(
      "  CHRG_STAT      = %u\r\n",
      chargeStat);

  Serial.printf(
      "  power good     = %s\r\n",
      powerGood ? "YES" : "NO");

  Serial.printf(
      "  VSYS regulation= %s\r\n",
      vsysRegulation ? "YES" : "NO");

  Serial.printf(
      "  fault          = 0x%02X\r\n",
      fault);

  return true;
}

// =============================================================================
// Battery combined test
// =============================================================================

static bool batteryBeginSmoke() {
  Serial.println("[BATTERY] Starting battery test...");

  bool gaugeOk = batteryGaugeTest();
  bool chargerOk = chargerTest();

  Serial.printf(
      "[BATTERY] gauge=%s charger=%s\r\n",
      gaugeOk ? "OK" : "FAIL",
      chargerOk ? "OK" : "FAIL");

  return gaugeOk && chargerOk;
}

// =============================================================================
// GPS
// =============================================================================

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

static bool getField(
    const char* line,
    int idx,
    char* out,
    size_t outSz) {

  int f = 0;
  const char* p = line;

  while (*p && f < idx) {
    if (*p++ == ',') {
      f++;
    }
  }

  if (f != idx) {
    return false;
  }

  size_t n = 0;

  while (*p &&
         *p != ',' &&
         *p != '*' &&
         n + 1 < outSz) {

    out[n++] = *p++;
  }

  out[n] = '\0';

  return true;
}

static void gpsHandleLine(
    const char* line) {

  if (!strncmp(line, "$GNRMC", 6) ||
      !strncmp(line, "$GPRMC", 6)) {

    char st[4] = "";

    getField(
        line,
        1,
        gpsTime,
        sizeof(gpsTime));

    getField(
        line,
        2,
        st,
        sizeof(st));

    getField(
        line,
        3,
        gpsLat,
        sizeof(gpsLat));

    getField(
        line,
        4,
        gpsNs,
        sizeof(gpsNs));

    getField(
        line,
        5,
        gpsLon,
        sizeof(gpsLon));

    getField(
        line,
        6,
        gpsEw,
        sizeof(gpsEw));

    gpsFixValid = (st[0] == 'A');
  }

  if (!strncmp(line, "$GNGGA", 6) ||
      !strncmp(line, "$GPGGA", 6)) {

    char sats[8] = "";

    if (getField(
            line,
            7,
            sats,
            sizeof(sats)) &&
        sats[0]) {

      gpsSats = atoi(sats);
    }
  }
}

static void gpsBegin() {
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);

  delay(200);

  GPSSerial.begin(
      GPS_BAUD,
      SERIAL_8N1,
      GPS_RX,
      GPS_TX);

  Serial.printf(
      "[GPS] UART %d baud RX=%d TX=%d EN=%d\r\n",
      GPS_BAUD,
      GPS_RX,
      GPS_TX,
      GPS_EN);
}

static void gpsPoll() {
  while (GPSSerial.available() > 0) {

    char c =
        (char)GPSSerial.read();

    gpsBytes++;

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {

      if (gpsLineLen) {

        gpsLine[gpsLineLen] = '\0';

        gpsHandleLine(gpsLine);

        gpsLineLen = 0;
      }

      continue;
    }

    if (gpsLineLen + 1 <
        sizeof(gpsLine)) {

      gpsLine[gpsLineLen++] = c;

    } else {

      gpsLineLen = 0;
    }
  }
}

static void gpsPrintSummary(
    const char* tag) {

  Serial.printf(
      "[GPS] %s bytes=%lu fix=%s sats=%d time=%s lat=%s%s lon=%s%s\r\n",
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

// =============================================================================
// Setup
// =============================================================================

void setup() {
  Serial.begin(115200);

  delay(800);

  Serial.println();

  Serial.println(
      "=================================");

  Serial.printf(
      "  %s (battery + SPI + haptic test)\r\n",
      DEVICE_NAME);

  Serial.println(
      "=================================");

  // ---------------------------------------------------------------------------
  // Pro peripheral power
  // ---------------------------------------------------------------------------

  Power::enablePeripherals();

  // ---------------------------------------------------------------------------
  // I2C
  // ---------------------------------------------------------------------------

  Wire.begin(
      I2C_SDA,
      I2C_SCL);

  Wire.setClock(
      I2C_FREQUENCY);

  Wire.setTimeOut(50);

  scanI2C();

  // ---------------------------------------------------------------------------
  // CST328 touch
  // ---------------------------------------------------------------------------

  cst328BeginSmoke();

  // ---------------------------------------------------------------------------
  // Keyboard
  // ---------------------------------------------------------------------------

  keyboard.begin();
  keyboard.backlightOn();

  // ---------------------------------------------------------------------------
  // SPI
  // ---------------------------------------------------------------------------

  SPI.begin(
      SPI_SCK,
      SPI_MISO,
      SPI_MOSI);

  // ---------------------------------------------------------------------------
  // LoRa
  // ---------------------------------------------------------------------------

  bool loraBefore =
      loraBeginSmoke();

  // ---------------------------------------------------------------------------
  // SD
  // ---------------------------------------------------------------------------

  bool sdOk =
      sdBeginSmoke();

  // ---------------------------------------------------------------------------
  // LoRa result
  // ---------------------------------------------------------------------------

  Serial.printf(
      "[SPI] shared-bus smoke test: %s\r\n",
      (loraBefore && sdOk) ? "PASS" : "FAIL");

  // ---------------------------------------------------------------------------
  // Haptic
  // ---------------------------------------------------------------------------

  hapticSmokeTest();

  // ---------------------------------------------------------------------------
  // Battery
  // ---------------------------------------------------------------------------

  batteryBeginSmoke();

  // ---------------------------------------------------------------------------
  // GPS
  // ---------------------------------------------------------------------------

  gpsBegin();

  uint32_t gpsStart = millis();

  while (millis() - gpsStart < 2500) {
    gpsPoll();
    delay(1);
  }

  gpsPrintSummary("boot");
  
  // ---------------------------------------------------------------------------
  // Eink
  // ---------------------------------------------------------------------------

  tdeck_pro::eink::begin();
  tdeck_pro::eink::showTestScreen();

  Serial.println(
      "[BOOT] ready - keyboard, GPS, SPI, haptic and battery checks active");
}

// =============================================================================
// Loop
// =============================================================================

void loop() {
  // ---------------------------------------------------------------------------
  // Keyboard
  // ---------------------------------------------------------------------------

  keyboard.update();

  if (keyboard.hasEvent()) {

    const KeyEvent& e =
        keyboard.getEvent();

    if (e.enter) {

      Serial.println(
          "[KEY] ENTER");

    } else if (e.del) {

      Serial.println(
          "[KEY] DEL");

    } else if (e.space) {

      Serial.println(
          "[KEY] SPACE");

    } else if (e.character) {

      Serial.printf(
          "[KEY] '%c'\r\n",
          e.character);

    } else {

      Serial.println(
          "[KEY] (modifier/other)");
    }
  }

  // ---------------------------------------------------------------------------
  // Touch
  // ---------------------------------------------------------------------------

  cst328Poll();

  // ---------------------------------------------------------------------------
  // GPS
  // ---------------------------------------------------------------------------

  gpsPoll();

  // ---------------------------------------------------------------------------
  // Periodic heartbeat / battery
  // ---------------------------------------------------------------------------

  static uint32_t lastLed = 0;
  static uint32_t lastGps = 0;
  static uint32_t lastBattery = 0;

  static bool ledOn = false;

  uint32_t now = millis();

  if (now - lastLed >= 1000) {

    lastLed = now;

    ledOn = !ledOn;

    heartbeatLed(ledOn);
  }

  if (now - lastBattery >= 5000UL) {

    lastBattery = now;

    Serial.println();
    Serial.println(
        "[BATTERY] periodic status");

    batteryBeginSmoke();
  }

  if (now - lastGps >= 60000UL) {

    lastGps = now;

    gpsPrintSummary("1min");
  }
}