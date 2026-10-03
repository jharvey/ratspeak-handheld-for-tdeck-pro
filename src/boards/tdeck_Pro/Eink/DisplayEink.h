#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include "config/BoardConfig.h"

// Thin wrapper around GxEPD2 GDEQ031T10 (UC8253).
// Panel was verified with the GxEPD2 test pattern.

class DisplayEink {
public:
    DisplayEink();

    bool begin();
    void fillScreen(bool black);          // true = black, false = white
    void setPixel(uint16_t x, uint16_t y, bool black);
    void fullRefresh();
    void flush(const lv_area_t* area, lv_color_t* color_map);

    uint16_t width()  const { return EPD_WIDTH; }
    uint16_t height() const { return EPD_HEIGHT; }

    // 1-bit framebuffer: 1 = white, 0 = black (GxEPD convention for mono)
    uint8_t* framebuffer() { return framebuffer_; }

private:
    uint8_t* framebuffer_;
    bool     ready_;
};