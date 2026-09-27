#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include "config/BoardConfig.h"
#include "hal/Power.h"
#include "hal/Keyboard.h"

// =============================================================================
// Keyboard
// =============================================================================

static Keyboard keyboard;

// =============================================================================
// Haptic / DRV2605
// =============================================================================

static constexpr uint8_t DRV2605_ADDR = 0x5A;

// DRV2605 registers
static constexpr uint8_t DRV_REG_STATUS = 0x00;
static constexpr uint8_t DRV_REG_MODE   = 0x01;
static constexpr uint8_t DRV_REG_RTPIN  = 0x02;

// DRV2605 modes
static constexpr uint8_t DRV_MODE_REALTIME = 0x05;

// Test parameters
static constexpr uint8_t HAPTIC_PULSE_COUNT = 5;
static constexpr uint8_t HAPTIC_AMPLITUDE   = 0x7F;
static constexpr uint32_t HAPTIC_INTERVAL_MS = 1000;
static constexpr uint32_t HAPTIC_DURATION_MS = 200;

static bool hapticPresent = false;
static bool hapticPlaying = false;

static uint8_t hapticPulseCount = 0;

static uint32_t hapticNextPulseMs = 0;
static uint32_t hapticStopMs = 0;

// =============================================================================
// LED
// =============================================================================

static void heartbeatLed(bool on)
{
  pinMode(BOARD_KEYBOARD_LED, OUTPUT);
  digitalWrite(BOARD_KEYBOARD_LED, on ? HIGH : LOW);
}

// =============================================================================
// I2C scan
// =============================================================================

static void scanI2C()
{
  Serial.println("[I2C] Scanning...");

  uint8_t found = 0;

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);

    if (Wire.endTransmission() == 0) {
      Serial.printf(
        "[I2C] device at 0x%02X\r\n",
        addr
      );

      found++;
    }
  }

  Serial.printf(
    "[I2C] %u device(s)\r\n",
    found
  );
}

// =============================================================================
// DRV2605 I2C helpers
// =============================================================================

static bool drvWrite(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(DRV2605_ADDR);

  Wire.write(reg);
  Wire.write(value);

  return Wire.endTransmission() == 0;
}

static bool drvRead(uint8_t reg, uint8_t& value)
{
  Wire.beginTransmission(DRV2605_ADDR);

  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(
        DRV2605_ADDR,
        (uint8_t)1
      ) != 1) {
    return false;
  }

  value = Wire.read();

  return true;
}

// =============================================================================
// Haptic motor
//
// T-Deck Pro V1.1 uses GPIO 2 as the motor enable signal.
// The official LilyGO factory definitions identify:
//
//   BOARD_MOTOR_PIN 2
//
// The DRV2605 itself is at I2C address 0x5A.
//
// We intentionally use real-time playback instead of assuming a particular
// waveform-library configuration. This gives us a simple electrical smoke
// test first.
// =============================================================================

static bool hapticBegin()
{
  Serial.println("[HAPTIC] enabling motor power...");

  pinMode(BOARD_MOTOR_PIN, OUTPUT);

  // T-Deck Pro V1.1 motor enable.
  digitalWrite(BOARD_MOTOR_PIN, HIGH);

  delay(100);

  Serial.printf(
    "[HAPTIC] MOTOR_EN GPIO=%d state=%s\r\n",
    BOARD_MOTOR_PIN,
    digitalRead(BOARD_MOTOR_PIN) ? "HIGH" : "LOW"
  );

  uint8_t status = 0;

  if (!drvRead(DRV_REG_STATUS, status)) {
    Serial.println(
      "[HAPTIC] FAIL - DRV2605 not responding at 0x5A"
    );

    return false;
  }

  Serial.printf(
    "[HAPTIC] DRV2605 status=0x%02X\r\n",
    status
  );

  // Make sure no RTP output is active.
  if (!drvWrite(DRV_REG_RTPIN, 0x00)) {
    Serial.println(
      "[HAPTIC] FAIL - could not clear RTP output"
    );

    return false;
  }

  // Put DRV2605 into real-time playback mode.
  if (!drvWrite(
        DRV_REG_MODE,
        DRV_MODE_REALTIME
      )) {

    Serial.println(
      "[HAPTIC] FAIL - could not enter real-time mode"
    );

    return false;
  }

  delay(10);

  Serial.println(
    "[HAPTIC] controller ready; motor enable HIGH"
  );

  return true;
}

// =============================================================================
// Start haptic test
// =============================================================================

static void hapticStartTest()
{
  hapticPresent = hapticBegin();

  if (!hapticPresent) {
    Serial.println(
      "[HAPTIC] test unavailable"
    );

    return;
  }

  hapticPulseCount = 0;
  hapticPlaying = false;

  // Start immediately.
  hapticNextPulseMs = millis();

  hapticStopMs = 0;

  Serial.printf(
    "[HAPTIC] starting %u-pulse test\r\n",
    HAPTIC_PULSE_COUNT
  );
}

// =============================================================================
// Haptic polling
// =============================================================================

static void hapticPoll()
{
  if (!hapticPresent) {
    return;
  }

  uint32_t now = millis();

  // ---------------------------------------------------------------------------
  // Stop currently active pulse.
  // ---------------------------------------------------------------------------

  if (hapticPlaying &&
      (int32_t)(now - hapticStopMs) >= 0) {

    drvWrite(
      DRV_REG_RTPIN,
      0x00
    );

    hapticPlaying = false;

    // If all pulses have been completed, finish the test.
    if (hapticPulseCount >= HAPTIC_PULSE_COUNT) {

      Serial.println(
        "[HAPTIC] test complete"
      );

      // Leave motor enable active, but make absolutely sure
      // the DRV2605 output is stopped.
      drvWrite(
        DRV_REG_RTPIN,
        0x00
      );

      return;
    }
  }

  // ---------------------------------------------------------------------------
  // Start next pulse.
  // ---------------------------------------------------------------------------

  if (!hapticPlaying &&
      hapticPulseCount < HAPTIC_PULSE_COUNT &&
      (int32_t)(now - hapticNextPulseMs) >= 0) {

    if (!drvWrite(
          DRV_REG_RTPIN,
          HAPTIC_AMPLITUDE
        )) {

      Serial.println(
        "[HAPTIC] FAIL - RTP write failed"
      );

      hapticPresent = false;

      return;
    }

    hapticPulseCount++;
    hapticPlaying = true;

    hapticStopMs =
      now + HAPTIC_DURATION_MS;

    hapticNextPulseMs =
      now + HAPTIC_INTERVAL_MS;

    Serial.printf(
      "[HAPTIC] pulse %u/%u amplitude=0x%02X\r\n",
      hapticPulseCount,
      HAPTIC_PULSE_COUNT,
      HAPTIC_AMPLITUDE
    );
  }
}

// =============================================================================
// LoRa SPI smoke test
// =============================================================================

static void waitBusy(uint32_t timeoutMs = 1000)
{
  uint32_t start = millis();

  while (digitalRead(LORA_BUSY) == HIGH) {

    if (millis() - start > timeoutMs) {
      Serial.println(
        "[LORA] BUSY timeout"
      );

      return;
    }

    delay(1);
  }
}

static uint8_t loraReadReg(uint16_t addr)
{
  waitBusy();

  digitalWrite(
    LORA_CS,
    LOW
  );

  SPI.transfer(0x1D);

  SPI.transfer(
    (addr >> 8) & 0xFF
  );

  SPI.transfer(
    addr & 0xFF
  );

  SPI.transfer(0x00);

  uint8_t val =
    SPI.transfer(0x00);

  digitalWrite(
    LORA_CS,
    HIGH
  );

  waitBusy();

  return val;
}

static bool loraBeginSmoke()
{
  pinMode(
    BOARD_LORA_EN,
    OUTPUT
  );

  digitalWrite(
    BOARD_LORA_EN,
    HIGH
  );

  delay(10);

  pinMode(
    LORA_CS,
    OUTPUT
  );

  digitalWrite(
    LORA_CS,
    HIGH
  );

  pinMode(
    EPD_CS,
    OUTPUT
  );

  digitalWrite(
    EPD_CS,
    HIGH
  );

  pinMode(
    SD_CS,
    OUTPUT
  );

  digitalWrite(
    SD_CS,
    HIGH
  );

  pinMode(
    LORA_RST,
    OUTPUT
  );

  pinMode(
    LORA_BUSY,
    INPUT
  );

  pinMode(
    LORA_IRQ,
    INPUT
  );

  digitalWrite(
    LORA_RST,
    LOW
  );

  delay(10);

  digitalWrite(
    LORA_RST,
    HIGH
  );

  delay(20);

  waitBusy();

  SPI.beginTransaction(
    SPISettings(
      2000000,
      MSBFIRST,
      SPI_MODE0
    )
  );

  uint8_t msb =
    loraReadReg(0x0740);

  uint8_t lsb =
    loraReadReg(0x0741);

  SPI.endTransaction();

  bool ok =
    !(msb == 0xFF &&
      lsb == 0xFF);

  Serial.printf(
    "[LORA] syncword=0x%02X%02X %s\r\n",
    msb,
    lsb,
    ok ? "OK" : "FAIL"
  );

  return ok;
}

// =============================================================================
// SD shared-SPI test
// =============================================================================

static bool sdBeginSmoke()
{
  Serial.println(
    "[SD] Starting shared-SPI test..."
  );

  // Explicitly deselect all other SPI peripherals.
  digitalWrite(
    LORA_CS,
    HIGH
  );

  digitalWrite(
    EPD_CS,
    HIGH
  );

  digitalWrite(
    SD_CS,
    HIGH
  );

  delay(5);

  if (!SD.begin(
        SD_CS,
        SPI,
        4000000
      )) {

    Serial.println(
      "[SD] FAIL - mount failed"
    );

    return false;
  }

  uint8_t type =
    SD.cardType();

  const char* typeStr =
    "UNKNOWN";

  if (type == CARD_NONE) {
    typeStr = "NONE";
  }
  else if (type == CARD_MMC) {
    typeStr = "MMC";
  }
  else if (type == CARD_SD) {
    typeStr = "SD";
  }
  else if (type == CARD_SDHC) {
    typeStr = "SDHC";
  }

  uint64_t sizeMb =
    SD.cardSize() /
    (1024ULL * 1024ULL);

  Serial.printf(
    "[SD] mounted type=%s size=%llu MB\r\n",
    typeStr,
    (unsigned long long)sizeMb
  );

  File root =
    SD.open("/");

  if (!root) {
    Serial.println(
      "[SD] FAIL - could not open root"
    );

    return false;
  }

  Serial.println(
    "[SD] root entries:"
  );

  int n = 0;

  File f =
    root.openNextFile();

  while (f && n < 12) {

    Serial.printf(
      "  %s%s size=%u\r\n",
      f.name(),
      f.isDirectory() ? "/" : "",
      (unsigned)f.size()
    );

    f.close();

    n++;

    f =
      root.openNextFile();
  }

  root.close();

  // ---------------------------------------------------------------------------
  // Write/read test.
  // ---------------------------------------------------------------------------

  const char* testPath =
    "/ratspeak_spi_test.txt";

  const char* testText =
    "tdeck-pro shared SPI test";

  File w =
    SD.open(
      testPath,
      FILE_WRITE
    );

  if (!w) {
    Serial.println(
      "[SD] FAIL - test file open for write failed"
    );

    return false;
  }

  w.println(
    testText
  );

  w.close();

  File r =
    SD.open(
      testPath,
      FILE_READ
    );

  if (!r) {
    Serial.println(
      "[SD] FAIL - test file open for read failed"
    );

    return false;
  }

  char readback[64] = {};

  size_t bytesRead =
    r.readBytes(
      readback,
      sizeof(readback) - 1
    );

  r.close();

  bool contentsOk =
    bytesRead > 0 &&
    strstr(
      readback,
      testText
    ) != nullptr;

  Serial.printf(
    "[SD] readback %s: %s\r\n",
    contentsOk ? "OK" : "FAIL",
    readback
  );

  if (!contentsOk) {
    return false;
  }

  Serial.println(
    "[SD] filesystem test PASS"
  );

  return true;
}

// =============================================================================
// Verify LoRa after SD access
// =============================================================================

static bool loraCheckAfterSD()
{
  Serial.println(
    "[SPI] Rechecking LoRa after SD operations..."
  );

  // SD must be deselected.
  digitalWrite(
    SD_CS,
    HIGH
  );

  digitalWrite(
    EPD_CS,
    HIGH
  );

  digitalWrite(
    LORA_CS,
    HIGH
  );

  delay(2);

  SPI.beginTransaction(
    SPISettings(
      2000000,
      MSBFIRST,
      SPI_MODE0
    )
  );

  uint8_t msb =
    loraReadReg(0x0740);

  uint8_t lsb =
    loraReadReg(0x0741);

  SPI.endTransaction();

  bool ok =
    !(msb == 0xFF &&
      lsb == 0xFF);

  Serial.printf(
    "[SPI] post-SD LoRa syncword=0x%02X%02X %s\r\n",
    msb,
    lsb,
    ok ? "OK" : "FAIL"
  );

  return ok;
}

// =============================================================================
// GPS
// =============================================================================

static HardwareSerial GPSSerial(2);

static bool gpsOnline = false;

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

// =============================================================================
// GPS helpers
// =============================================================================

static bool getField(
  const char* line,
  int idx,
  char* out,
  size_t outSz
)
{
  int f = 0;

  const char* p =
    line;

  while (*p && f < idx) {

    if (*p == ',') {
      f++;
    }

    p++;
  }

  if (f != idx) {
    return false;
  }

  size_t n = 0;

  while (
    *p &&
    *p != ',' &&
    *p != '*' &&
    n + 1 < outSz
  ) {

    out[n++] =
      *p++;
  }

  out[n] = '\0';

  return true;
}

static void gpsHandleLine(
  const char* line
)
{
  if (
    !strncmp(
      line,
      "$GNRMC",
      6
    ) ||
    !strncmp(
      line,
      "$GPRMC",
      6
    )
  ) {

    char status[4] = "";

    getField(
      line,
      1,
      gpsTime,
      sizeof(gpsTime)
    );

    getField(
      line,
      2,
      status,
      sizeof(status)
    );

    getField(
      line,
      3,
      gpsLat,
      sizeof(gpsLat)
    );

    getField(
      line,
      4,
      gpsNs,
      sizeof(gpsNs)
    );

    getField(
      line,
      5,
      gpsLon,
      sizeof(gpsLon)
    );

    getField(
      line,
      6,
      gpsEw,
      sizeof(gpsEw)
    );

    gpsFixValid =
      status[0] == 'A';
  }

  if (
    !strncmp(
      line,
      "$GNGGA",
      6
    ) ||
    !strncmp(
      line,
      "$GPGGA",
      6
    )
  ) {

    char sats[8] = "";

    if (
      getField(
        line,
        7,
        sats,
        sizeof(sats)
      ) &&
      sats[0]
    ) {

      gpsSats =
        atoi(sats);
    }
  }
}

static void gpsBegin()
{
  pinMode(
    GPS_EN,
    OUTPUT
  );

  digitalWrite(
    GPS_EN,
    HIGH
  );

  delay(200);

  GPSSerial.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX,
    GPS_TX
  );

  GPSSerial.setTimeout(0);

  gpsOnline = true;

  gpsBytes = 0;
  gpsLineLen = 0;

  Serial.printf(
    "[GPS] UART %u baud RX=%d TX=%d EN=%d\r\n",
    (unsigned)GPS_BAUD,
    GPS_RX,
    GPS_TX,
    GPS_EN
  );
}

static void gpsPoll()
{
  if (!gpsOnline) {
    return;
  }

  while (
    GPSSerial.available() > 0
  ) {

    char c =
      (char)GPSSerial.read();

    gpsBytes++;

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {

      if (gpsLineLen > 0) {

        gpsLine[gpsLineLen] =
          '\0';

        gpsHandleLine(
          gpsLine
        );

        gpsLineLen = 0;
      }

      continue;
    }

    if (
      gpsLineLen + 1 <
      sizeof(gpsLine)
    ) {

      gpsLine[gpsLineLen++] =
        c;
    }
    else {
      gpsLineLen = 0;
    }
  }
}

static void gpsPrintSummary(
  const char* tag
)
{
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
    gpsEw
  );
}

// =============================================================================
// Setup
// =============================================================================

void setup()
{
  Serial.begin(115200);

  delay(800);

  Serial.println();

  Serial.println(
    "================================="
  );

  Serial.printf(
    "  %s (SPI + haptic test)\r\n",
    DEVICE_NAME
  );

  Serial.println(
    "================================="
  );

  // ---------------------------------------------------------------------------
  // Board power
  // ---------------------------------------------------------------------------

  Power::enablePeripherals();

  // ---------------------------------------------------------------------------
  // I2C
  // ---------------------------------------------------------------------------

  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );

  Wire.setClock(
    I2C_FREQUENCY
  );

  Wire.setTimeOut(50);

  scanI2C();

  // ---------------------------------------------------------------------------
  // Keyboard
  // ---------------------------------------------------------------------------

  if (!keyboard.begin()) {
    Serial.println(
      "[KB] init failed"
    );
  }
  else {
    keyboard.backlightOn();
  }

  // ---------------------------------------------------------------------------
  // Shared SPI
  // ---------------------------------------------------------------------------

  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI
  );

  bool loraBefore =
    loraBeginSmoke();

  bool sdOk =
    sdBeginSmoke();

  bool loraAfter =
    loraCheckAfterSD();

  Serial.printf(
    "[SPI] result: before-LoRa=%s SD=%s after-LoRa=%s\r\n",
    loraBefore ? "OK" : "FAIL",
    sdOk ? "OK" : "FAIL",
    loraAfter ? "OK" : "FAIL"
  );

  if (
    loraBefore &&
    sdOk &&
    loraAfter
  ) {

    Serial.println(
      "[SPI] shared-bus smoke test PASS"
    );
  }
  else {

    Serial.println(
      "[SPI] shared-bus smoke test FAIL - inspect results above"
    );
  }

  // ---------------------------------------------------------------------------
  // Haptic
  // ---------------------------------------------------------------------------

  hapticStartTest();

  // ---------------------------------------------------------------------------
  // GPS
  // ---------------------------------------------------------------------------

  gpsBegin();

  // Give GPS a short startup window.
  uint32_t t0 =
    millis();

  while (
    millis() - t0 <
    3000
  ) {

    keyboard.update();

    gpsPoll();

    hapticPoll();

    delay(1);
  }

  gpsPrintSummary(
    "boot"
  );

  Serial.println(
    "[BOOT] ready - keyboard, GPS, SPI checks and haptic test active"
  );
}

// =============================================================================
// Main loop
// =============================================================================

void loop()
{
  // ---------------------------------------------------------------------------
  // Keyboard
  // ---------------------------------------------------------------------------

  keyboard.update();

  if (keyboard.hasEvent()) {

    const KeyEvent& e =
      keyboard.getEvent();

    if (e.enter) {

      Serial.println(
        "[KEY] ENTER"
      );
    }
    else if (e.del) {

      Serial.println(
        "[KEY] DEL"
      );
    }
    else if (e.space) {

      Serial.println(
        "[KEY] SPACE"
      );
    }
    else if (e.character) {

      Serial.printf(
        "[KEY] '%c'\r\n",
        e.character
      );
    }
    else {

      Serial.println(
        "[KEY] (modifier/other)"
      );
    }
  }

  // ---------------------------------------------------------------------------
  // GPS
  // ---------------------------------------------------------------------------

  gpsPoll();

  // ---------------------------------------------------------------------------
  // Haptic
  // ---------------------------------------------------------------------------

  hapticPoll();

  // ---------------------------------------------------------------------------
  // LED heartbeat / GPS status
  // ---------------------------------------------------------------------------

  static uint32_t lastLed = 0;
  static uint32_t lastGps = 0;

  static bool ledOn = false;

  uint32_t now =
    millis();

  if (
    now - lastLed >=
    1000
  ) {

    lastLed = now;

    ledOn =
      !ledOn;

    heartbeatLed(
      ledOn
    );
  }

  if (
    now - lastGps >=
    60000UL
  ) {

    lastGps = now;

    gpsPrintSummary(
      "1min"
    );
  }
}