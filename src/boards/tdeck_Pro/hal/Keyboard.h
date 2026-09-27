#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config/BoardConfig.h"
#include "input/KeyEvent.h"

// T-Deck Pro keyboard HAL.
//
// Hardware:
//   TCA8418 keyboard matrix controller
//   GPIO keyboard backlight
//
// The public interface intentionally follows the original T-Deck
// Keyboard HAL so the rest of Ratspeak does not need to know about
// the TCA8418.
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

  // Backlight
  uint8_t _backlightDuty = 128;
  bool _backlightLit = false;
  bool _ledcReady = false;

  // Physical modifier state.
  //
  // These are deliberately kept inside the HAL rather than generating
  // standalone KeyEvents for modifier keys.
  bool _shiftHeld = false;
  bool _altHeld = false;
  bool _symHeld = false;

  static int _debugCount;
};