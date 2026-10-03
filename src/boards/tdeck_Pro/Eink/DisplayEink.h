#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include "config/BoardConfig.h"

class DisplayEink {
public:
    DisplayEink();

    bool begin();
    void fillScreen(bool black);
    void setPixel(uint16_t x, uint16_t y, bool black);

    // Only update the 1-bit buffer (no panel I/O)
    void flush(const lv_area_t* area, lv_color_t* color_map);

    // Push buffer to panel once (full refresh)
    void fullRefresh();

    // Call after a batch of LVGL draws to refresh at most once
    void refreshIfDirty();
    void markDirty() { dirty_ = true; }

    uint16_t width()  const { return EPD_WIDTH; }
    uint16_t height() const { return EPD_HEIGHT; }
    uint8_t* framebuffer() { return framebuffer_; }

private:
    uint8_t* framebuffer_;
    bool     ready_;
    bool     dirty_;
};