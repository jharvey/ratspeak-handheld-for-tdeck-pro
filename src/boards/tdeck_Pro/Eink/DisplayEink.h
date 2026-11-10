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

    // Fast localized update (partial window). Only rows inside the window that
    // differ from what the panel already shows are sent; if none differ, the
    // panel is not touched. Never auto-escalates to full.
    void partialRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    // After LVGL batch: partial update of the changed rows. Full only via fullRefresh().
    void refreshIfDirty();
    void markDirty() { dirty_ = true; }

    // Switch the panel's charge pump off (the image stays). GxEPD2 leaves it on after a
    // refresh; the next refresh turns it back on (~50 ms). Called before light sleep.
    void powerOff();

    // Called repeatedly while the panel is busy (~0.7 s per partial refresh),
    // so the app can keep reading input. The callback must not draw.
    void setBusyCallback(void (*cb)(const void*), const void* param = nullptr);

    uint8_t partialCount() const { return partialCount_; }
    uint32_t lastRefreshMs() const { return lastRefreshMs_; }

    uint16_t width()  const { return EPD_WIDTH; }
    uint16_t height() const { return EPD_HEIGHT; }
    uint8_t* framebuffer() { return framebuffer_; }

private:
    void parkSpiCs();

    uint8_t* framebuffer_;
    uint8_t* shadow_;          // what the panel currently shows (1 = black)
    uint32_t lastRefreshMs_;
    bool     ready_;
    bool     dirty_;
    uint8_t  partialCount_;
    static constexpr uint8_t kMaxPartialsBeforeFull = 5;
};
