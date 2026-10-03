#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include "config/BoardConfig.h"   // ← correct path

class DisplayEink {
public:
    DisplayEink();
    bool begin();
    void fullRefresh();
    void partialRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void setPixel(uint16_t x, uint16_t y, bool black);
    void fillScreen(bool black);
    void drawBitmap(uint16_t x, uint16_t y, const uint8_t* bmp, uint16_t w, uint16_t h);
    uint16_t width()  const { return EPD_WIDTH; }
    uint16_t height() const { return EPD_HEIGHT; }

    // Called by LVGL flush
    void flush(const lv_area_t* area, lv_color_t* color_map);

private:
    SPIClass* spi_;
    uint8_t*  framebuffer_;          // 320*240/8 = 9600 bytes
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t data);
    void waitBusy(uint32_t timeoutMs = 5000);
    void setWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
};