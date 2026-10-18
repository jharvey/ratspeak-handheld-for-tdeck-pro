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

    // Push entire buffer to panel (full refresh — clears ghosting)
    void fullRefresh();

    // Fast localized update of a rectangle from the current framebuffer.
    // Uses GxEPD2 partial window. After ~5 consecutive partials a full
    // refresh is forced to clear ghosting (LilyGO / panel guidance).
    void partialRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    // After LVGL batch: partial full-window (no flash); forces full every N partials
    void refreshIfDirty();
    void markDirty() { dirty_ = true; }

    uint8_t partialCount() const { return partialCount_; }

    uint16_t width()  const { return EPD_WIDTH; }
    uint16_t height() const { return EPD_HEIGHT; }
    uint8_t* framebuffer() { return framebuffer_; }

private:
    void parkSpiCs();

    uint8_t* framebuffer_;
    bool     ready_;
    bool     dirty_;
    uint8_t  partialCount_;
    static constexpr uint8_t kMaxPartialsBeforeFull = 5;
};
