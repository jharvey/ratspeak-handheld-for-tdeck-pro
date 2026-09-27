#pragma once

#include <Arduino.h>

namespace tdeck_pro {
namespace eink {

bool begin();

void clear();
void showBootScreen();
void showTestScreen();

void sleep();
void wake();

bool isReady();

constexpr int WIDTH = 240;
constexpr int HEIGHT = 320;

}  // namespace eink
}  // namespace tdeck_pro