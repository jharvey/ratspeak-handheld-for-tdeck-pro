#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config/BoardConfig.h"
#include "input/KeyEvent.h"

// T-Deck Pro: TCA8418 @ 0x34 + GPIO keyboard backlight (BOARD_KEYBOARD_LED).
class Keyboard {
public:
  bool begin();
  void update();
  void discardPending();

  InputMode getMode() const { return _mode; }
  void setMode(InputMode mode) { _mode = mode; }

  bool hasEvent() const { return _hasEvent; }
  const KeyEvent& getEvent() const { return _event; }

  // percent 0..100 — drives GPIO 42 PWM/digital (not Plus ESP32-C3 I2C cmds)
  bool setBacklightBrightness(uint8_t percent);
  bool backlightOn();
  bool backlightOff();
  bool backlightIsLit() const { return _backlightLit; }

private:
  void applyLedPwm(uint8_t duty /*0..255*/);

  InputMode _mode = InputMode::Navigation;
  KeyEvent _event = {};
  bool _hasEvent = false;

  uint8_t _backlightDuty = 0;   // 0..255
  bool _backlightLit = false;
  bool _ledcReady = false;

  static int _debugCount;
};