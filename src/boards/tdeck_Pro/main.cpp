#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

#include "config/BoardConfig.h"
#include "hal/Power.h"

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
      Serial.printf("[I2C]  device at 0x%02X\r\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("[I2C]  no devices (check SDA/SCL and power gates)");
  } else {
    Serial.printf("[I2C]  %u device(s)\r\n", found);
  }
}

// =============================================================================
// TCA8418 keyboard smoke test
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
  tcaWrite(0x1D, 0x0F); // ROW0-3 keypad
  tcaWrite(0x1E, 0xFF); // COL0-7 keypad
  tcaWrite(0x1F, 0x03); // COL8-9 keypad
  tcaWrite(0x01, 0x11);
  tcaWrite(0x02, 0x00);
  while (tcaRead(0x03) & 0x0F) {
    (void)tcaRead(0x04);
  }
  Serial.println("[KB] TCA8418 matrix 4x10 ready — press keys");
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
    uint8_t row = idx / 10;
    uint8_t col = idx % 10;
    Serial.printf("[KB] %s row=%u col=%u code=%u\r\n",
                  pressed ? "PRESS " : "RELEASE", row, col, code);
    pending = tcaRead(0x03) & 0x0F;
  }
}

// =============================================================================
// SX1262 LoRa SPI smoke test
// =============================================================================
static void waitBusy(uint32_t timeoutMs = 1000) {
  uint32_t start = millis();
  while (digitalRead(LORA_BUSY) == HIGH) {
    if (millis() - start > timeoutMs) {
      Serial.println("[LORA] BUSY timeout");
      return;
    }
  }
}

static void loraCs(bool select) {
  digitalWrite(LORA_CS, select ? LOW : HIGH);
}

static uint8_t loraReadReg(uint16_t addr) {
  waitBusy();
  loraCs(true);
  SPI.transfer(0x1D);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  SPI.transfer(0x00);
  uint8_t val = SPI.transfer(0x00);
  loraCs(false);
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

  Serial.printf("[LORA] syncword regs = 0x%02X%02X\r\n", msb, lsb);
  if (msb == 0xFF && lsb == 0xFF) {
    Serial.println("[LORA] FAIL — SPI dead or EN/CS/pins wrong");
    return false;
  }
  Serial.println("[LORA] OK — radio responds on SPI");
  return true;
}

// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=================================");
  Serial.printf("  %s  (headless Pro bring-up)\r\n", DEVICE_NAME);
  Serial.println("=================================");

  Power::enablePeripherals();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQUENCY);
  Wire.setTimeOut(50);
  scanI2C();

  if (!keyboardBegin()) {
    Serial.println("[KB] init failed");
  }

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  loraBeginSmoke();

  heartbeatLed(true);
  delay(200);
  heartbeatLed(false);

  Serial.println("[BOOT] Headless setup complete");
  Serial.println("[BOOT] LED blinks every 1s; press keys for [KB] lines");
  Serial.println();
}

void loop() {
  keyboardPoll();

  static uint32_t last = 0;
  static bool on = false;
  uint32_t now = millis();
  if (now - last >= 1000) {
    last = now;
    on = !on;
    heartbeatLed(on);
    //Serial.printf("[HB] t=%lu led=%s\r\n", (unsigned long)now, on ? "ON" : "OFF");
  }
}