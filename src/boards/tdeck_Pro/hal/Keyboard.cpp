#include "Keyboard.h"

#include <Wire.h>

int Keyboard::_debugCount = 0;

// TCA8418
static constexpr uint8_t TCA8418_ADDR = 0x34;

// Registers
static constexpr uint8_t REG_CFG             = 0x01;
static constexpr uint8_t REG_INT_STAT        = 0x02;
static constexpr uint8_t REG_KEY_LCK_EC      = 0x03;
static constexpr uint8_t REG_KEY_EVENT_A     = 0x04;

static constexpr uint8_t REG_KP_GPIO1        = 0x1D;
static constexpr uint8_t REG_KP_GPIO2        = 0x1E;
static constexpr uint8_t REG_KP_GPIO3        = 0x1F;

static constexpr uint8_t REG_GPI_INT_EN1     = 0x1B;
static constexpr uint8_t REG_GPI_INT_EN2     = 0x1C;

static constexpr uint8_t REG_GPI_EM1         = 0x2A;
static constexpr uint8_t REG_GPI_EM2         = 0x2B;
static constexpr uint8_t REG_GPI_EM3         = 0x2C;

static constexpr uint8_t REG_GPI_INT_STAT1   = 0x3D;
static constexpr uint8_t REG_GPI_INT_STAT2   = 0x3E;
static constexpr uint8_t REG_GPI_INT_STAT3   = 0x3F;

static constexpr uint8_t REG_DEBOUNCE_DIS1   = 0x29;
static constexpr uint8_t REG_DEBOUNCE_DIS2   = 0x2A;
static constexpr uint8_t REG_DEBOUNCE_DIS3   = 0x2B;

// Keyboard special values.
static constexpr char K_ALT    = 0x80;
static constexpr char K_SYM    = 0x81;
static constexpr char K_MIC    = 0x82;
static constexpr char K_LSHIFT = 0x83;
static constexpr char K_RSHIFT = 0x84;

// -----------------------------------------------------------------------------
// Matrix map.
//
// The TCA8418 event codes are decoded as:
//
//   index = event_code - 1
//   row   = index / 10
//   col   = index % 10
//
// Calibration showed:
//
// row 0: col9..0 = Q W E R T Y U I O P
// row 1: col9..0 = A S D F G H J K L BKSP
// row 2: col9..0 = ALT Z X C V B N M $ ENTER
// row 3: col4..0 = LSHIFT MIC SPACE SYM RSHIFT
//
// Therefore the array is stored col0..9.
// -----------------------------------------------------------------------------

static const char kMap[4][10] = {
  {
    'p', 'o', 'i', 'u', 'y', 't', 'r', 'e', 'w', 'q'
  },
  {
    '\b', 'l', 'k', 'j', 'h', 'g', 'f', 'd', 's', 'a'
  },
  {
    '\n', '$', 'm', 'n', 'b', 'v', 'c', 'x', 'z', K_ALT
  },
  {
    K_RSHIFT, K_SYM, ' ', K_MIC, K_LSHIFT,
    0, 0, 0, 0, 0
  }
};

// -----------------------------------------------------------------------------
// Symbol layer.
//
// These correspond to the secondary legends printed on the T-Deck Pro:
//
// Q #
// W 1
// E 2
// R 3
// T (
// Y )
// U -
// I _
// O +
// P @
//
// A *
// S 4
// D 5
// F 6
// G /
// H :
// J ;
// K '
// L "
//
// Z 7
// X 8
// C 9
// V ?
// B !
// N '
// M `
// $ $
//
// MIC + SYM = 0
// -----------------------------------------------------------------------------

static char symbolFor(char c)
{
  switch (c) {
    case 'q': return '#';
    case 'w': return '1';
    case 'e': return '2';
    case 'r': return '3';
    case 't': return '(';
    case 'y': return ')';
    case 'u': return '-';
    case 'i': return '_';
    case 'o': return '+';
    case 'p': return '@';

    case 'a': return '*';
    case 's': return '4';
    case 'd': return '5';
    case 'f': return '6';
    case 'g': return '/';
    case 'h': return ':';
    case 'j': return ';';
    case 'k': return '\'';
    case 'l': return '"';

    case 'z': return '7';
    case 'x': return '8';
    case 'c': return '9';
    case 'v': return '?';
    case 'b': return '!';
    case 'n': return '\'';
    case 'm': return '`';

    case '$': return '$';

    default:
      return c;
  }
}

// -----------------------------------------------------------------------------
// I2C helpers
// -----------------------------------------------------------------------------

bool Keyboard::tcaWrite(uint8_t reg, uint8_t val)
{
  Wire.beginTransmission(TCA8418_ADDR);
  Wire.write(reg);
  Wire.write(val);

  return Wire.endTransmission() == 0;
}

uint8_t Keyboard::tcaRead(uint8_t reg)
{
  Wire.beginTransmission(TCA8418_ADDR);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return 0;
  }

  if (Wire.requestFrom(TCA8418_ADDR, (uint8_t)1) != 1) {
    return 0;
  }

  return Wire.read();
}

// -----------------------------------------------------------------------------
// LED / backlight
// -----------------------------------------------------------------------------

void Keyboard::applyLedPwm(uint8_t duty)
{
  // Keep the existing board-specific LED/backlight behavior.
  // The actual LEDC setup is retained here so keyboard functionality
  // remains independent of the keyboard matrix.
  if (!_ledcReady) {
    return;
  }

  ledcWrite(0, duty);
}

// -----------------------------------------------------------------------------
// Begin
// -----------------------------------------------------------------------------

bool Keyboard::begin()
{
  _hasEvent = false;
  _event = {};

  _shiftHeld = false;
  _altHeld = false;
  _symHeld = false;

  // Start in Caps Lock / uppercase mode.
  _capsLock = true;

  _lastShiftPressMs = 0;

  _debugCount = 0;

  Wire.begin();

  // Confirm TCA8418 is present.
  Wire.beginTransmission(TCA8418_ADDR);

  if (Wire.endTransmission() != 0) {
    Serial.println("[KEYBOARD] TCA8418 not found");
    return false;
  }

  // ---------------------------------------------------------------------------
  // TCA8418 keypad matrix configuration.
  //
  // 4 rows x 10 columns.
  // ---------------------------------------------------------------------------

  // Keypad GPIO configuration.
  tcaWrite(REG_KP_GPIO1, 0x0F);
  tcaWrite(REG_KP_GPIO2, 0xFF);
  tcaWrite(REG_KP_GPIO3, 0x03);

  // Disable GPIO/GPI interrupt sources.
  tcaWrite(REG_GPI_INT_EN1, 0x00);
  tcaWrite(REG_GPI_INT_EN2, 0x00);

  // Disable GPI event modes.
  tcaWrite(REG_GPI_EM1, 0x00);
  tcaWrite(REG_GPI_EM2, 0x00);
  tcaWrite(REG_GPI_EM3, 0x00);

  // Enable keypad functionality and event FIFO.
  tcaWrite(REG_CFG, 0x19);

  // Enable debounce for keypad keys.
  tcaWrite(REG_DEBOUNCE_DIS1, 0x00);
  tcaWrite(REG_DEBOUNCE_DIS2, 0x00);
  tcaWrite(REG_DEBOUNCE_DIS3, 0x00);

  // Read/clear any pending GPIO interrupt state.
  (void)tcaRead(REG_GPI_INT_STAT1);
  (void)tcaRead(REG_GPI_INT_STAT2);
  (void)tcaRead(REG_GPI_INT_STAT3);

  // Flush any stale keyboard FIFO events.
  discardPending();

  Serial.println("[KEYBOARD] TCA8418 keyboard ready");

  return true;
}

// -----------------------------------------------------------------------------
// Convert a matrix key into a Ratspeak KeyEvent.
// -----------------------------------------------------------------------------

void Keyboard::mapKey(uint8_t row, uint8_t col, bool pressed)
{
  _event = {};
  _hasEvent = false;

  if (row >= 4 || col >= 10) {
    return;
  }

  char key = kMap[row][col];

  if (key == 0) {
    return;
  }

  // ---------------------------------------------------------------------------
  // SHIFT
  //
  // Shift itself is never passed to Ratspeak as a KeyEvent.
  //
  // A second Shift press within SHIFT_DOUBLE_TAP_MS toggles Caps Lock.
  // ---------------------------------------------------------------------------

  if (key == K_LSHIFT || key == K_RSHIFT) {

    if (pressed) {
      unsigned long now = millis();

      if (_lastShiftPressMs != 0 &&
          (now - _lastShiftPressMs) <= SHIFT_DOUBLE_TAP_MS) {

        // Double-tap Shift = toggle Caps Lock.
        _capsLock = !_capsLock;

        // The second tap becomes the beginning of a new sequence,
        // rather than allowing three rapid taps to immediately toggle again.
        _lastShiftPressMs = 0;

      } else {
        _lastShiftPressMs = now;
      }

      _shiftHeld = true;
    }
    else {
      _shiftHeld = false;
    }

    return;
  }

  // ---------------------------------------------------------------------------
  // ALT
  // ---------------------------------------------------------------------------

  if (key == K_ALT) {
    _altHeld = pressed;
    return;
  }

  // ---------------------------------------------------------------------------
  // SYM
  // ---------------------------------------------------------------------------

  if (key == K_SYM) {
    _symHeld = pressed;
    return;
  }

  // ---------------------------------------------------------------------------
  // MIC
  //
  // MIC alone is not an emitted key.
  //
  // MIC + SYM produces 0.
  // ---------------------------------------------------------------------------

  if (key == K_MIC) {
    if (pressed && _symHeld) {
      _event.character = '0';
      _event.shift = false;
      _event.alt = _altHeld;
      _event.opt = true;
      _event.del = false;
      _event.enter = false;
      _event.space = false;
      _hasEvent = true;
    }

    return;
  }

  // Only generate character/action events on key-down.
  if (!pressed) {
    return;
  }

  // ---------------------------------------------------------------------------
  // Backspace
  // ---------------------------------------------------------------------------

  if (key == '\b') {
    _event.del = true;
    _event.shift = _shiftHeld;
    _event.alt = _altHeld;
    _event.opt = _symHeld;
    _event.enter = false;
    _event.space = false;
    _event.character = 0;

    _hasEvent = true;
    return;
  }

  // ---------------------------------------------------------------------------
  // Enter
  // ---------------------------------------------------------------------------

  if (key == '\n') {
    _event.enter = true;
    _event.shift = _shiftHeld;
    _event.alt = _altHeld;
    _event.opt = _symHeld;
    _event.del = false;
    _event.space = false;
    _event.character = 0;

    _hasEvent = true;
    return;
  }

  // ---------------------------------------------------------------------------
  // Space
  // ---------------------------------------------------------------------------

  if (key == ' ') {
    _event.space = true;
    _event.shift = _shiftHeld;
    _event.alt = _altHeld;
    _event.opt = _symHeld;
    _event.del = false;
    _event.enter = false;
    _event.character = 0;

    _hasEvent = true;
    return;
  }

  // ---------------------------------------------------------------------------
  // Printable character
  // ---------------------------------------------------------------------------

  char output = key;

  if (_symHeld) {
    output = symbolFor(key);
  }
  else if (key >= 'a' && key <= 'z') {

    bool uppercase = _capsLock;

    // Conventional keyboard behavior:
    // Shift reverses Caps Lock for alphabetic characters.
    if (_shiftHeld) {
      uppercase = !uppercase;
    }

    if (uppercase) {
      output = key - ('a' - 'A');
    }
  }

  _event.character = output;
  _event.shift = _shiftHeld;
  _event.alt = _altHeld;
  _event.opt = _symHeld;
  _event.del = false;
  _event.enter = false;
  _event.space = false;

  _hasEvent = true;
}

// -----------------------------------------------------------------------------
// Update
// -----------------------------------------------------------------------------

void Keyboard::update()
{
  _hasEvent = false;
  _event = {};

  uint8_t eventCount = tcaRead(REG_KEY_LCK_EC) & 0x0F;

  if (eventCount == 0) {
    return;
  }

  // Read one FIFO event per update.
  uint8_t raw = tcaRead(REG_KEY_EVENT_A);

  // TCA8418 event code:
  //
  //   bit 7 = 1 -> key down
  //   bit 7 = 0 -> key up
  //
  //   bits 6..0 = event code
  //
  bool pressed = (raw & 0x80) != 0;
  uint8_t code = raw & 0x7F;

  // Valid keypad matrix event codes for 4x10 are 1..40.
  //
  // Codes above 40 are GPI events and should not become keyboard keys.
  if (code < 1 || code > 40) {
    return;
  }

  uint8_t idx = code - 1;
  uint8_t row = idx / 10;
  uint8_t col = idx % 10;

  mapKey(row, col, pressed);
}

// -----------------------------------------------------------------------------
// Flush pending TCA8418 keyboard events.
// -----------------------------------------------------------------------------

void Keyboard::discardPending()
{
  for (int i = 0; i < 32; ++i) {

    uint8_t count = tcaRead(REG_KEY_LCK_EC) & 0x0F;

    if (count == 0) {
      break;
    }

    (void)tcaRead(REG_KEY_EVENT_A);
  }

  // Clear/read interrupt state after flushing.
  (void)tcaRead(REG_GPI_INT_STAT1);
  (void)tcaRead(REG_GPI_INT_STAT2);
  (void)tcaRead(REG_GPI_INT_STAT3);
}

// -----------------------------------------------------------------------------
// Backlight
// -----------------------------------------------------------------------------

bool Keyboard::setBacklightBrightness(uint8_t percent)
{
  if (percent > 100) {
    percent = 100;
  }

  _backlightDuty = (uint16_t)percent * 255 / 100;

  if (_backlightLit) {
    applyLedPwm(_backlightDuty);
  }

  return true;
}

bool Keyboard::backlightOn()
{
  _backlightLit = true;
  applyLedPwm(_backlightDuty);
  return true;
}

bool Keyboard::backlightOff()
{
  _backlightLit = false;
  applyLedPwm(0);
  return true;
}