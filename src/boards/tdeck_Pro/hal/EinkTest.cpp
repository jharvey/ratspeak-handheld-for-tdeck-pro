#include <Arduino.h>

#include "EinkDisplay.h"

void einkTest() {

  Serial.println();
  Serial.println("==============================");
  Serial.println("[EINK] TEST");
  Serial.println("==============================");

  if (!tdeck_pro::eink::begin()) {

    Serial.println("[EINK] initialization FAILED");

    return;
  }

  tdeck_pro::eink::showTestScreen();

  Serial.println("[EINK] TEST COMPLETE");
}