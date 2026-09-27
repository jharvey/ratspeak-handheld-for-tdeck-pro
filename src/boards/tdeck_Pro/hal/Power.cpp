#include "Power.h"
#include "config/BoardConfig.h"

// Board-local Power methods. Display dim/sleep lives in ui/lvgl/hal/DisplayPower.cpp
// and is only used when the full LVGL app is linked.

void Power::enablePeripherals() {
  // Pro has discrete enables — not Plus GPIO 10.
#if BOARD_POWER_PIN >= 0
  pinMode(BOARD_POWER_PIN, OUTPUT);
  digitalWrite(BOARD_POWER_PIN, HIGH);
#endif

  pinMode(BOARD_LORA_EN, OUTPUT);
  digitalWrite(BOARD_LORA_EN, HIGH);

  pinMode(BOARD_GPS_EN, OUTPUT);
  digitalWrite(BOARD_GPS_EN, HIGH);

  pinMode(BOARD_1V8_EN, OUTPUT);
  digitalWrite(BOARD_1V8_EN, HIGH);

  // Optional modem rail — leave HIGH only if your board has the module.
  // pinMode(BOARD_6609_EN, OUTPUT);
  // digitalWrite(BOARD_6609_EN, HIGH);

  pinMode(BOARD_MOTOR_PIN, OUTPUT);
  digitalWrite(BOARD_MOTOR_PIN, LOW);

  // Isolate shared SPI slaves before any bus traffic.
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);

  // Keyboard backlight off at boot
  pinMode(BOARD_KEYBOARD_LED, OUTPUT);
  digitalWrite(BOARD_KEYBOARD_LED, LOW);

  delay(20);
  Serial.println("[POWER] Pro rails enabled (LORA/GPS/1V8); SPI CS high");
}

void Power::begin() {
  _lastActivity = millis();
  _state = ACTIVE;

  // Pro has no simple BAT_ADC_PIN model (BQ27220 later).
#if BAT_ADC_PIN >= 0
  pinMode(BAT_ADC_PIN, INPUT);
  analogReadResolution(12);
#endif

  Serial.println("[POWER] Power manager initialized (Pro stub battery)");
}

float Power::batteryVoltage() const {
#if BAT_ADC_PIN >= 0
  int raw = analogRead(BAT_ADC_PIN);
  return (raw / 4095.0f) * 3.3f * 2.0f;
#else
  // Placeholder until BQ27220 is wired in.
  return 0.0f;
#endif
}

int Power::batteryPercent() const {
  float v = batteryVoltage();
  if (v <= 0.0f) return -1;  // unknown
  if (isCharging()) return 100;
  v = constrain(v, 3.0f, 4.2f);
  return (int)((v - 3.0f) / 1.2f * 100.0f);
}

void Power::setBatteryModel(uint8_t model) {
  _batteryModel = model;
}

bool Power::isCharging() const {
#if BAT_ADC_PIN >= 0
  return batteryVoltage() >= _chargeThreshold;
#else
  return false;
#endif
}

void Power::setChargeThreshold(float v) {
  _chargeThreshold = v;
}

void Power::setFullBatteryVoltage(float v) {
  _fullBatteryV = v;
}