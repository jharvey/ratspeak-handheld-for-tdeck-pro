#include "DisplayEink.h"
#include <string.h>

DisplayEink::DisplayEink() : spi_(nullptr), framebuffer_(nullptr) {}

bool DisplayEink::begin() {
    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(50);

    pinMode(EPD_CS, OUTPUT);
    pinMode(EPD_DC, OUTPUT);
    pinMode(EPD_BUSY, INPUT);
    if (EPD_RST >= 0) {
        pinMode(EPD_RST, OUTPUT);
        digitalWrite(EPD_RST, HIGH);
    }
    digitalWrite(EPD_CS, HIGH);

    spi_ = &SPI;

    framebuffer_ = (uint8_t*)ps_malloc(EPD_WIDTH * EPD_HEIGHT / 8);
    if (!framebuffer_) {
        Serial.println("[EINK] framebuffer alloc failed");
        return false;
    }
    memset(framebuffer_, 0xFF, EPD_WIDTH * EPD_HEIGHT / 8);

    if (EPD_RST >= 0) {
        digitalWrite(EPD_RST, LOW);
        delay(10);
        digitalWrite(EPD_RST, HIGH);
        delay(10);
    }

    sendCommand(0x04);
    waitBusy();
    sendCommand(0x00);
    sendData(0x1F);
    sendCommand(0x50);
    sendData(0x97);

    Serial.println("[EINK] DisplayEink ready");
    return true;
}

void DisplayEink::sendCommand(uint8_t cmd) {
    digitalWrite(EPD_DC, LOW);
    digitalWrite(EPD_CS, LOW);
    spi_->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    spi_->transfer(cmd);
    spi_->endTransaction();
    digitalWrite(EPD_CS, HIGH);
}

void DisplayEink::sendData(uint8_t data) {
    digitalWrite(EPD_DC, HIGH);
    digitalWrite(EPD_CS, LOW);
    spi_->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    spi_->transfer(data);
    spi_->endTransaction();
    digitalWrite(EPD_CS, HIGH);
}

void DisplayEink::waitBusy(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (digitalRead(EPD_BUSY) == HIGH) {
        if (millis() - start > timeoutMs) break;
        delay(1);
    }
}

void DisplayEink::setWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    sendCommand(0x44);
    sendData(x / 8);
    sendData((x + w - 1) / 8);
    sendCommand(0x45);
    sendData(y & 0xFF);
    sendData((y >> 8) & 0xFF);
    sendData((y + h - 1) & 0xFF);
    sendData(((y + h - 1) >> 8) & 0xFF);
    sendCommand(0x4E);
    sendData(x / 8);
    sendCommand(0x4F);
    sendData(y & 0xFF);
    sendData((y >> 8) & 0xFF);
}

void DisplayEink::fillScreen(bool black) {
    memset(framebuffer_, black ? 0x00 : 0xFF, EPD_WIDTH * EPD_HEIGHT / 8);
}

void DisplayEink::setPixel(uint16_t x, uint16_t y, bool black) {
    if (x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    uint32_t idx = (y * EPD_WIDTH + x) / 8;
    uint8_t  mask = 0x80 >> (x % 8);
    if (black) framebuffer_[idx] &= ~mask;
    else       framebuffer_[idx] |=  mask;
}

void DisplayEink::fullRefresh() {
    setWindow(0, 0, EPD_WIDTH, EPD_HEIGHT);
    sendCommand(0x24);
    digitalWrite(EPD_DC, HIGH);
    digitalWrite(EPD_CS, LOW);
    spi_->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < EPD_WIDTH * EPD_HEIGHT / 8; i++) {
        spi_->transfer(framebuffer_[i]);
    }
    spi_->endTransaction();
    digitalWrite(EPD_CS, HIGH);

    sendCommand(0x22);
    sendData(0xF7);
    sendCommand(0x20);
    waitBusy(8000);
}

void DisplayEink::partialRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    fullRefresh();
}

void DisplayEink::flush(const lv_area_t* area, lv_color_t* color_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            lv_color_t c = color_map[y * w + x];
            bool black = (c.full == 0);
            setPixel(area->x1 + x, area->y1 + y, black);
        }
    }

    if ((uint32_t)(w * h) > (EPD_WIDTH * EPD_HEIGHT / 4))
        fullRefresh();
    else
        partialRefresh(area->x1, area->y1, w, h);
}