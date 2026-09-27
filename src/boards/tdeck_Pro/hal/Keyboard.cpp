#include "Keyboard.h"

int Keyboard::_debugCount = 0;

bool Keyboard::begin() {
  _mode = InputMode::Navigation;
  _hasEvent = false;
  _debugCount = 0;

  pinMode(KB_INT, INPUT_PULLUP);
  pinMode(KB_LED, OUTPUT);
  digitalWrite(KB_LED, LOW);
  _backlightLit = false;
  _backlightDuty = 0;

  Wire.beginTransmission(KB_I2C_ADDR);
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    Serial.printf("[KEYBOARD] TCA8418 not found at 0x%02X (err=%d)\n", KB_I2C_ADDR, err);
    // Still usable for backlight-only tests
    return false;
  }

  // Minimal TCA8418 bring-up: 4x10 matrix (LilyGO factory uses same size).
  // Full key decoding can be expanded later; presence check is enough for now.
  Wire.beginTransmission(KB_I2C_ADDR);
  Wire.write(0x01);  // CFG
  Wire.write(0x01);  // AI enabled-ish / keep simple; refine with datasheet as needed
  Wire.endTransmission();

  Serial.println("[KEYBOARD] TCA8418 present; backlight on GPIO 42");
  return true;
}

void Keyboard::update() {
  // Stub: no key synthesis yet. Expand with TCA8418 event FIFO later.
  _hasEvent = false;
}

void Keyboard::discardPending() {
  _hasEvent = false;
}

void Keyboard::applyLedPwm(uint8_t duty) {
  // Prefer digital for full on/off; use LEDC for partial brightness.
  if (duty == 0) {
    if (_ledcReady) {
      ledcWrite(0, 0);
    }
    pinMode(KB_LED, OUTPUT);
    digitalWrite(KB_LED, LOW);
    return;
  }
  if (duty >= 255) {
    if (_ledcReady) {
      ledcWrite(0, 255);
    }
    pinMode(KB_LED, OUTPUT);
    digitalWrite(KB_LED, HIGH);
    return;
  }
  if (!_ledcReady) {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(KB_LED, 0);
    _ledcReady = true;
  }
  ledcWrite(0, duty);
}

bool Keyboard::setBacklightBrightness(uint8_t percent) {
  percent = constrain(percent, 0, 100);
  if (percent == 0) {
    _backlightDuty = 0;
    return true;
  }
  // Map 1..100 -> ~16..255
  _backlightDuty = (uint8_t)(16 + (uint16_t)(percent - 1) * 239 / 99);
  return true;
}

bool Keyboard::backlightOn() {
  if (_backlightDuty == 0) {
    _backlightDuty = 255;
  }
  applyLedPwm(_backlightDuty);
  _backlightLit = true;
  return true;
}

bool Keyboard::backlightOff() {
  applyLedPwm(0);
  _backlightLit = false;
  return true;
}