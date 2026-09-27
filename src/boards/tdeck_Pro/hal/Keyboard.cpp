#include "Keyboard.h"
#include <ctype.h>

int Keyboard::_debugCount = 0;

namespace {

// ============================================================================
// TCA8418 registers
// ============================================================================

constexpr uint8_t REG_CFG              = 0x01;
constexpr uint8_t REG_INT_STAT         = 0x02;
constexpr uint8_t REG_KEY_LCK_EC       = 0x03;
constexpr uint8_t REG_KEY_EVENT_A      = 0x04;

constexpr uint8_t REG_GPIO_INT_STAT1   = 0x11;
constexpr uint8_t REG_GPIO_INT_STAT2   = 0x12;
constexpr uint8_t REG_GPIO_INT_STAT3   = 0x13;

constexpr uint8_t REG_GPIO_INT_EN1     = 0x1A;
constexpr uint8_t REG_GPIO_INT_EN2     = 0x1B;
constexpr uint8_t REG_GPIO_INT_EN3     = 0x1C;

constexpr uint8_t REG_KP_GPIO1         = 0x1D;
constexpr uint8_t REG_KP_GPIO2         = 0x1E;
constexpr uint8_t REG_KP_GPIO3         = 0x1F;

constexpr uint8_t REG_GPI_EM1          = 0x20;
constexpr uint8_t REG_GPI_EM2          = 0x21;
constexpr uint8_t REG_GPI_EM3          = 0x22;

constexpr uint8_t REG_DEBOUNCE_DIS1    = 0x29;
constexpr uint8_t REG_DEBOUNCE_DIS2    = 0x2A;
constexpr uint8_t REG_DEBOUNCE_DIS3    = 0x2B;

// Key-event interrupt enabled.
// GPI interrupt is disabled.
constexpr uint8_t TCA8418_CFG = 0x19;

// ============================================================================
// Internal key markers
// ============================================================================

static constexpr char K_LSHIFT = 1;
static constexpr char K_RSHIFT = 2;
static constexpr char K_ALT    = 3;
static constexpr char K_SYM    = 4;
static constexpr char K_MIC    = 5;

// ============================================================================
// T-Deck Pro physical matrix
//
// TCA8418 event code:
//
//   index = event_code - 1
//   row   = index / 10
//   col   = index % 10
//
// Calibrated matrix:
//
//   row 0: col 9 Q W E R T Y U I O P col 0
//   row 1: col 9 A S D F G H J K L BKSP col 0
//   row 2: col 9 ALT Z X C V B N M $ ENTER col 0
//   row 3: col 4 LSHIFT, MIC, SPACE, SYM, RSHIFT
//
// The array is therefore stored in the reverse physical column order.
//
// IMPORTANT:
// Letters are LOWERCASE here.
// Shift is applied in mapKey().
// ============================================================================

static const char kMap[4][10] = {

    // row 0
    {
        'p', 'o', 'i', 'u', 'y',
        't', 'r', 'e', 'w', 'q'
    },

    // row 1
    {
        '\b', 'l', 'k', 'j', 'h',
        'g', 'f', 'd', 's', 'a'
    },

    // row 2
    {
        '\n', '$', 'm', 'n', 'b',
        'v', 'c', 'x', 'z', K_ALT
    },

    // row 3
    {
        K_RSHIFT, K_SYM, ' ', K_MIC, K_LSHIFT,
        0, 0, 0, 0, 0
    }
};

// ============================================================================
// Low-level I2C helpers
// ============================================================================

static bool writeRegister(
    uint8_t reg,
    uint8_t value)
{
  Wire.beginTransmission(KB_I2C_ADDR);

  Wire.write(reg);
  Wire.write(value);

  return Wire.endTransmission() == 0;
}

static uint8_t readRegister(
    uint8_t reg)
{
  Wire.beginTransmission(KB_I2C_ADDR);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return 0xFF;
  }

  if (Wire.requestFrom(
          (uint8_t)KB_I2C_ADDR,
          (uint8_t)1) != 1) {
    return 0xFF;
  }

  return Wire.read();
}

// ============================================================================
// Clear GPIO interrupt state
// ============================================================================

static void clearGpioInterruptState()
{
  (void)readRegister(REG_GPIO_INT_STAT1);
  (void)readRegister(REG_GPIO_INT_STAT2);
  (void)readRegister(REG_GPIO_INT_STAT3);
}

// ============================================================================
// Drain pending TCA8418 keypad events
// ============================================================================

static void drainKeyFifo()
{
  for (uint8_t i = 0; i < 32; ++i) {

    uint8_t count =
        readRegister(REG_KEY_LCK_EC) & 0x0F;

    if (count == 0) {
      break;
    }

    (void)readRegister(REG_KEY_EVENT_A);
  }
}

// ============================================================================
// Read one event
//
// Returns true when an event was consumed.
//
// Events 1..40 are valid positions in our 4x10 matrix.
// Events above 40 are non-matrix/GPI events and are consumed but ignored.
// ============================================================================

static bool readMatrixEvent(
    uint8_t& row,
    uint8_t& col,
    bool& pressed,
    uint8_t& rawEvent)
{
  uint8_t pending =
      readRegister(REG_KEY_LCK_EC) & 0x0F;

  if (pending == 0) {
    return false;
  }

  rawEvent =
      readRegister(REG_KEY_EVENT_A);

  pressed =
      (rawEvent & 0x80) != 0;

  uint8_t code =
      rawEvent & 0x7F;

  if (code == 0) {
    return true;
  }

  // Valid matrix positions are 1..40.
  //
  // The TCA8418 may produce GPI/non-matrix events such as 0x61/0x62.
  // Consume them without allowing them into our keyboard map.
  if (code > 40) {
    return true;
  }

  uint8_t index =
      code - 1;

  row =
      index / 10;

  col =
      index % 10;

  if (row >= 4 ||
      col >= 10) {
    return true;
  }

  return true;
}

// ============================================================================
// Symbol layer
//
// T-Deck Pro:
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
// SYM + MIC = 0
// ============================================================================

static char symbolFor(
    char base)
{
  switch (base) {

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

    case '$':
      return '$';

    default:
      return base;
  }
}

} // namespace

// ============================================================================
// TCA write
// ============================================================================

bool Keyboard::tcaWrite(
    uint8_t reg,
    uint8_t val)
{
  return writeRegister(
      reg,
      val);
}

// ============================================================================
// TCA read
// ============================================================================

uint8_t Keyboard::tcaRead(
    uint8_t reg)
{
  return readRegister(reg);
}

// ============================================================================
// begin
// ============================================================================

bool Keyboard::begin()
{
  _mode =
      InputMode::Navigation;

  _event =
      {};

  _hasEvent =
      false;

  _shiftHeld =
      false;

  _altHeld =
      false;

  _symHeld =
      false;

  _debugCount =
      0;

  // --------------------------------------------------------------------------
  // Keyboard interrupt.
  // --------------------------------------------------------------------------

  pinMode(
      KB_INT,
      INPUT_PULLUP);

  // --------------------------------------------------------------------------
  // Keyboard backlight.
  // --------------------------------------------------------------------------

  pinMode(
      KB_LED,
      OUTPUT);

  digitalWrite(
      KB_LED,
      LOW);

  _backlightLit =
      false;

  // --------------------------------------------------------------------------
  // Verify TCA8418.
  // --------------------------------------------------------------------------

  Wire.beginTransmission(
      KB_I2C_ADDR);

  if (Wire.endTransmission() != 0) {

    Serial.printf(
        "[KEYBOARD] TCA8418 not found at 0x%02X\r\n",
        KB_I2C_ADDR);

    return false;
  }

  // --------------------------------------------------------------------------
  // Disable GPIO interrupts.
  // --------------------------------------------------------------------------

  tcaWrite(
      REG_GPIO_INT_EN1,
      0x00);

  tcaWrite(
      REG_GPIO_INT_EN2,
      0x00);

  tcaWrite(
      REG_GPIO_INT_EN3,
      0x00);

  // --------------------------------------------------------------------------
  // Disable GPI event modes.
  // --------------------------------------------------------------------------

  tcaWrite(
      REG_GPI_EM1,
      0x00);

  tcaWrite(
      REG_GPI_EM2,
      0x00);

  tcaWrite(
      REG_GPI_EM3,
      0x00);

  // --------------------------------------------------------------------------
  // Configure 4x10 keypad.
  // --------------------------------------------------------------------------

  tcaWrite(
      REG_KP_GPIO1,
      0x0F);

  tcaWrite(
      REG_KP_GPIO2,
      0xFF);

  tcaWrite(
      REG_KP_GPIO3,
      0x03);

  // --------------------------------------------------------------------------
  // Enable integrated debounce.
  // --------------------------------------------------------------------------

  tcaWrite(
      REG_DEBOUNCE_DIS1,
      0x00);

  tcaWrite(
      REG_DEBOUNCE_DIS2,
      0x00);

  tcaWrite(
      REG_DEBOUNCE_DIS3,
      0x00);

  // --------------------------------------------------------------------------
  // Clear stale GPIO state.
  // --------------------------------------------------------------------------

  clearGpioInterruptState();

  // --------------------------------------------------------------------------
  // Enable keypad events.
  // --------------------------------------------------------------------------

  tcaWrite(
      REG_CFG,
      TCA8418_CFG);

  // --------------------------------------------------------------------------
  // Throw away anything generated during initialization.
  // --------------------------------------------------------------------------

  drainKeyFifo();

  clearGpioInterruptState();

  (void)readRegister(
      REG_INT_STAT);

  Serial.println(
      "[KEYBOARD] TCA8418 keyboard ready");

  return true;
}

// ============================================================================
// mapKey
// ============================================================================

void Keyboard::mapKey(
    uint8_t row,
    uint8_t col,
    bool pressed)
{
  if (row >= 4 ||
      col >= 10) {
    return;
  }

  char ch =
      kMap[row][col];

  // ==========================================================================
  // RELEASE
  //
  // Modifier releases update state but DO NOT create a KeyEvent.
  // Normal key releases also produce no KeyEvent.
  // ==========================================================================

  if (!pressed) {

    switch (ch) {

      case K_LSHIFT:
      case K_RSHIFT:
        _shiftHeld = false;
        break;

      case K_ALT:
        _altHeld = false;
        break;

      case K_SYM:
        _symHeld = false;
        break;

      default:
        break;
    }

    return;
  }

  // ==========================================================================
  // PRESS
  //
  // Start with no event.
  // ==========================================================================

  _event =
      {};

  _hasEvent =
      false;

  // ==========================================================================
  // SHIFT
  //
  // IMPORTANT:
  // Shift is state only.
  //
  // We do NOT create a KeyEvent for Shift.
  // ==========================================================================

  if (ch == K_LSHIFT ||
      ch == K_RSHIFT) {

    _shiftHeld =
        true;

    return;
  }

  // ==========================================================================
  // ALT
  // ==========================================================================

  if (ch == K_ALT) {

    _altHeld =
        true;

    return;
  }

  // ==========================================================================
  // SYM
  // ==========================================================================

  if (ch == K_SYM) {

    _symHeld =
        true;

    return;
  }

  // ==========================================================================
  // MIC
  //
  // MIC alone does nothing.
  //
  // SYM + MIC = 0.
  // ==========================================================================

  if (ch == K_MIC) {

    if (_symHeld) {

      _event.character =
          '0';

      _event.opt =
          true;

      _hasEvent =
          true;
    }

    return;
  }

  // ==========================================================================
  // BACKSPACE
  // ==========================================================================

  if (ch == '\b') {

    _event.del =
        true;

    _event.character =
        0x08;

    if (_shiftHeld) {
      _event.shift = true;
    }

    if (_altHeld) {
      _event.alt = true;
    }

    if (_symHeld) {
      _event.opt = true;
    }

    _hasEvent =
        true;

    return;
  }

  // ==========================================================================
  // ENTER
  // ==========================================================================

  if (ch == '\n') {

    _event.enter =
        true;

    _event.character =
        '\n';

    if (_shiftHeld) {
      _event.shift = true;
    }

    if (_altHeld) {
      _event.alt = true;
    }

    if (_symHeld) {
      _event.opt = true;
    }

    _hasEvent =
        true;

    return;
  }

  // ==========================================================================
  // SPACE
  // ==========================================================================

  if (ch == ' ') {

    _event.space =
        true;

    _event.character =
        ' ';

    if (_shiftHeld) {
      _event.shift = true;
    }

    if (_altHeld) {
      _event.alt = true;
    }

    if (_symHeld) {
      _event.opt = true;
    }

    _hasEvent =
        true;

    return;
  }

  // ==========================================================================
  // Printable character
  // ==========================================================================

  if (ch < 0x20 ||
      ch > 0x7E) {
    return;
  }

  char output =
      ch;

  // --------------------------------------------------------------------------
  // SYM has priority.
  //
  // Example:
  //
  //   SYM + Q = #
  //
  // not:
  //
  //   SYM + Q = Q
  // --------------------------------------------------------------------------

  if (_symHeld) {

    output =
        symbolFor(ch);

  } else if (_shiftHeld &&
             ch >= 'a' &&
             ch <= 'z') {

    // Normal:
    //
    //   q -> q
    //
    // Shift:
    //
    //   q -> Q

    output =
        (char)toupper(
            (unsigned char)ch);
  }

  _event.character =
      output;

  // Preserve modifier state in the KeyEvent for the actual character.
  if (_shiftHeld) {
    _event.shift =
        true;
  }

  if (_altHeld) {
    _event.alt =
        true;
  }

  if (_symHeld) {
    _event.opt =
        true;
  }

  _hasEvent =
      true;
}

// ============================================================================
// update
// ============================================================================

void Keyboard::update()
{
  // Every update starts with NO event.
  //
  // This is important because getEvent() returns the previous event object.
  // We never want a previous character to remain pending after the next
  // update() call.
  _hasEvent =
      false;

  _event =
      {};

  uint8_t row = 0;
  uint8_t col = 0;
  bool pressed = false;
  uint8_t rawEvent = 0;

  if (!readMatrixEvent(
          row,
          col,
          pressed,
          rawEvent)) {
    return;
  }

  uint8_t code =
      rawEvent & 0x7F;

  // Invalid/non-matrix event.
  if (code == 0 ||
      code > 40) {
    return;
  }

  if (row >= 4 ||
      col >= 10) {
    return;
  }

  // Translate this one physical event.
  mapKey(
      row,
      col,
      pressed);
}

// ============================================================================
// discardPending
// ============================================================================

void Keyboard::discardPending()
{
  drainKeyFifo();

  clearGpioInterruptState();

  (void)readRegister(
      REG_INT_STAT);

  _hasEvent =
      false;

  _event =
      {};

  _shiftHeld =
      false;

  _altHeld =
      false;

  _symHeld =
      false;
}

// ============================================================================
// Backlight PWM
// ============================================================================

void Keyboard::applyLedPwm(
    uint8_t duty)
{
  // Completely off.
  if (duty == 0) {

    if (_ledcReady) {
      ledcWrite(
          0,
          0);
    }

    pinMode(
        KB_LED,
        OUTPUT);

    digitalWrite(
        KB_LED,
        LOW);

    return;
  }

  // Completely on.
  if (duty >= 255) {

    pinMode(
        KB_LED,
        OUTPUT);

    digitalWrite(
        KB_LED,
        HIGH);

    return;
  }

  // PWM.
  if (!_ledcReady) {

    ledcSetup(
        0,
        5000,
        8);

    ledcAttachPin(
        KB_LED,
        0);

    _ledcReady =
        true;
  }

  ledcWrite(
      0,
      duty);
}

// ============================================================================
// setBacklightBrightness
// ============================================================================

bool Keyboard::setBacklightBrightness(
    uint8_t percent)
{
  percent =
      constrain(
          percent,
          0,
          100);

  if (percent == 0) {

    _backlightDuty =
        0;

    return true;
  }

  // Map 1..100% to a useful 8-bit PWM range.
  _backlightDuty =
      (uint8_t)(
          16 +
          (uint16_t)(
              percent - 1) *
          239 /
          99);

  return true;
}

// ============================================================================
// backlightOn
// ============================================================================

bool Keyboard::backlightOn()
{
  if (_backlightDuty == 0) {
    _backlightDuty =
        200;
  }

  applyLedPwm(
      _backlightDuty);

  _backlightLit =
      true;

  return true;
}

// ============================================================================
// backlightOff
// ============================================================================

bool Keyboard::backlightOff()
{
  applyLedPwm(0);

  _backlightLit =
      false;

  return true;
}