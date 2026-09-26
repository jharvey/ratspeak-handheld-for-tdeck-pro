#include <Arduino.h>

static constexpr int LED_PIN = 42;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== tdeck_pro keyboard LED test ===");
  Serial.println("Look UNDER the keys (keyboard backlight)");
  Serial.println("GPIO 42  |  HIGH should be ON");

  // Digital mode first (same as LilyGO factory)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

void loop() {
  // ---- Phase 1: plain digital (LilyGO style) ----
  digitalWrite(LED_PIN, HIGH);
  Serial.println("digital HIGH  (expect ON)");
  delay(1000);

  digitalWrite(LED_PIN, LOW);
  Serial.println("digital LOW   (expect OFF)");
  delay(1000);

  // ---- Phase 2: PWM (in case the line is filtered) ----
  // re-attach each cycle so we can switch modes cleanly
  ledcSetup(0, 5000, 8);
  ledcAttachPin(LED_PIN, 0);

  ledcWrite(0, 255);
  Serial.println("PWM 255      (expect ON)");
  delay(1000);

  ledcWrite(0, 0);
  Serial.println("PWM 0        (expect OFF)");
  delay(1000);

  // detach so next digitalWrite works again
  ledcDetachPin(LED_PIN);
  pinMode(LED_PIN, OUTPUT);
}