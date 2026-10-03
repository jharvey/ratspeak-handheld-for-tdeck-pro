#pragma once

#include <Arduino.h>

// Power gates (must be high before radio or panel)
#define BOARD_1V8_EN_PIN   38
#define LORA_EN_PIN        46

// Shared SPI (LoRa + e-ink)
#define SPI_SCK_PIN        36
#define SPI_MOSI_PIN       33
#define SPI_MISO_PIN       35   // confirm on your schematic if needed

// E-ink GDEQ031T10
#define EINK_CS_PIN        34
#define EINK_DC_PIN        35
#define EINK_BUSY_PIN      37
#define EINK_RST_PIN       -1   // no dedicated reset

// Keyboard TCA8418
#define I2C_SDA_PIN        18
#define I2C_SCL_PIN        8
#define KBD_I2C_ADDR       0x34
#define KBD_BL_PIN         42

// Touch CST328
#define TOUCH_I2C_ADDR     0x1A

// Display geometry
#define EINK_WIDTH         320
#define EINK_HEIGHT        240

namespace Board {
    void powerOn();
    void powerOff();
}