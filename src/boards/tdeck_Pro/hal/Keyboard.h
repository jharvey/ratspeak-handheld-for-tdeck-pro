#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "config/BoardConfig.h"
#include "input/KeyEvent.h"

class Keyboard {
public:
  bool begin();
  void update();
  void discardPending();

  InputMode getMode() const { return _mode; }
  void setMode(InputMode mode) { _mode = mode; }

  bool hasEvent() const { return _hasEvent; }
  const KeyEvent& getEvent() const { return _event; }

  bool setBacklightBrightness(uint8_t percent);
  bool backlightOn();
  bool backlightOff();
  bool backlightIsLit() const { return _backlightLit; }

private:
  bool tcaWrite(uint8_t reg, uint8_t val);
  uint8_t tcaRead(uint8_t reg);

  void applyLedPwm(uint8_t duty);
  void mapKey(uint8_t row, uint8_t col, bool pressed);

  InputMode _mode = InputMode::Navigation;

  KeyEvent _event = {};
  bool _hasEvent = false;

  uint8_t _backlightDuty = 128;
  bool _backlightLit = false;
  bool _ledcReady = false;

  // Physical modifier state.
  bool _shiftHeld = false;
  bool _altHeld = false;
  bool _symHeld = false;

  // Keyboard starts in uppercase/caps-lock mode.
  bool _capsLock = true;

  // Used for double-tap Shift detection.
  unsigned long _lastShiftPressMs = 0;

  static constexpr unsigned long SHIFT_DOUBLE_TAP_MS = 400;

  static int _debugCount;
};