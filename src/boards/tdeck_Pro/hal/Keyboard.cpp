#include "Keyboard.h"
#include <ctype.h>

int Keyboard::_debugCount = 0;

// Special markers in kMap (not printable ASCII)
static constexpr char K_LSHIFT = 1;
static constexpr char K_RSHIFT = 2;
static constexpr char K_ALT   = 3;
static constexpr char K_SYM   = 4;
static constexpr char K_MIC   = 5;

// Empty during calibration — filled after you send the [CAL] log
static char kMap[4][10] = {
    { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P' },             
    { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '\b' },            
    { K_ALT, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '$', '\n' },        
    { K_LSHIFT, K_MIC, ' ', K_SYM, K_RSHIFT, 0, 0, 0, 0, 0 } 
};

bool Keyboard::tcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(KB_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

uint8_t Keyboard::tcaRead(uint8_t reg) {
  Wire.beginTransmission(KB_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)KB_I2C_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

bool Keyboard::begin() {
  _mode = InputMode::Navigation;
  _hasEvent = false;
  _shiftHeld = _altHeld = _symHeld = false;
  _debugCount = 0;

  pinMode(KB_INT, INPUT_PULLUP);
  pinMode(KB_LED, OUTPUT);
  digitalWrite(KB_LED, LOW);
  _backlightLit = false;

  Wire.beginTransmission(KB_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.printf("[KEYBOARD] TCA8418 not found at 0x%02X\r\n", KB_I2C_ADDR);
    return false;
  }

  tcaWrite(0x01, 0x11);
  tcaWrite(0x1D, 0x0F);
  tcaWrite(0x1E, 0xFF);
  tcaWrite(0x1F, 0x03);
  tcaWrite(0x02, 0x00);
  while (tcaRead(0x03) & 0x0F) {
    (void)tcaRead(0x04);
  }

  Serial.println("[KEYBOARD] TCA8418 ready — CALIBRATION MODE");
  Serial.println("[KEYBOARD] Press L->R top to bottom:");
  Serial.println("[KEYBOARD]  QWERTYUIOP");
  Serial.println("[KEYBOARD]  ASDFGHJKL BKSP");
  Serial.println("[KEYBOARD]  ALT ZXCVBNM $ ENTER");
  Serial.println("[KEYBOARD]  LSHIFT MIC SPACE SYM RSHIFT");
  return true;
}

void Keyboard::mapKey(uint8_t row, uint8_t col, bool pressed) {
  if (!pressed) return;
  if (row >= 4 || col >= 10) return;

  // --- CALIBRATION: only log position ---
  Serial.printf("[CAL1] row=%u col=%u\r\n", row, col);
  return;

  // --- Normal mapping (enabled after calibration) ---
  /*
  _event = {};
  char ch = kMap[row][col];
  if (ch == 0) return;

  if (ch == K_LSHIFT) {
    _shiftHeld = true;
    _event.shift = true;
    _hasEvent = true;
    return;
  }
  if (ch == K_RSHIFT) {
    _shiftHeld = true;
    _event.shift = true;
    _hasEvent = true;
    return;
  }
  if (ch == K_ALT) {
    _altHeld = true;
    _event.alt = true;
    _hasEvent = true;
    return;
  }
  if (ch == K_SYM) {
    _symHeld = true;
    _event.opt = true;  // reuse opt as sym for now
    _hasEvent = true;
    return;
  }
  if (ch == K_MIC) {
    // no character yet
    return;
  }
  if (ch == '\b') {
    _event.del = true;
    _event.character = 0x08;
  } else if (ch == '\n') {
    _event.enter = true;
    _event.character = '\n';
  } else if (ch == ' ') {
    _event.space = true;
    _event.character = ' ';
  } else if (ch >= 0x20 && ch <= 0x7E) {
    if (_shiftHeld && ch >= 'a' && ch <= 'z') {
      ch = (char)toupper((unsigned char)ch);
      _shiftHeld = false;
    }
    _event.character = ch;
  } else {
    return;
  }
  _hasEvent = true;
  */
}

void Keyboard::update() {
  _hasEvent = false;

  uint8_t pending = tcaRead(0x03) & 0x0F;
  if (!pending) return;

  uint8_t ev = tcaRead(0x04);
  bool pressed = (ev & 0x80) != 0;
  uint8_t code = ev & 0x7F;
  if (code == 0) return;

  uint8_t idx = code - 1;
  mapKey(idx / 10, idx % 10, pressed);
}

void Keyboard::discardPending() {
  while (tcaRead(0x03) & 0x0F) {
    (void)tcaRead(0x04);
  }
  _hasEvent = false;
}

void Keyboard::applyLedPwm(uint8_t duty) {
  if (duty == 0) {
    if (_ledcReady) ledcWrite(0, 0);
    pinMode(KB_LED, OUTPUT);
    digitalWrite(KB_LED, LOW);
    return;
  }
  if (duty >= 255) {
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
  _backlightDuty = (uint8_t)(16 + (uint16_t)(percent - 1) * 239 / 99);
  return true;
}

bool Keyboard::backlightOn() {
  if (_backlightDuty == 0) _backlightDuty = 200;
  applyLedPwm(_backlightDuty);
  _backlightLit = true;
  return true;
}

bool Keyboard::backlightOff() {
  applyLedPwm(0);
  _backlightLit = false;
  return true;
}