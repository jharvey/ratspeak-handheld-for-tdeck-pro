#include "DisplayEink.h"
#include <string.h>

DisplayEink::DisplayEink() : spi_(nullptr), framebuffer_(nullptr) {}

bool DisplayEink::begin() {
    pinMode(BOARD_1V8_EN_PIN, OUTPUT);
    pinMode(LORA_EN_PIN, OUTPUT);
    digitalWrite(BOARD_1V8_EN_PIN, HIGH);
    digitalWrite(LORA_EN_PIN, HIGH);
    delay(50);

    pinMode(EINK_CS_PIN, OUTPUT);
    pinMode(EINK_DC_PIN, OUTPUT);
    pinMode(EINK_BUSY_PIN, INPUT);
    digitalWrite(EINK_CS_PIN, HIGH);

    spi_ = new SPIClass(HSPI);
    spi_->begin(SPI_SCK_PIN, -1, SPI_MOSI_PIN, -1);

    framebuffer_ = (uint8_t*)ps_malloc(EINK_WIDTH * EINK_HEIGHT / 8);
    if (!framebuffer_) return false;
    memset(framebuffer_, 0xFF, EINK_WIDTH * EINK_HEIGHT / 8); // white

    // Basic init sequence for GDEQ031T10 / UC8253 family
    sendCommand(0x04); // Power on
    waitBusy();
    sendCommand(0x00); // Panel setting
    sendData(0x1F);
    sendCommand(0x50); // VCOM
    sendData(0x97);
    return true;
}

void DisplayEink::sendCommand(uint8_t cmd) {
    digitalWrite(EINK_DC_PIN, LOW);
    digitalWrite(EINK_CS_PIN, LOW);
    spi_->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    spi_->transfer(cmd);
    spi_->endTransaction();
    digitalWrite(EINK_CS_PIN, HIGH);
}

void DisplayEink::sendData(uint8_t data) {
    digitalWrite(EINK_DC_PIN, HIGH);
    digitalWrite(EINK_CS_PIN, LOW);
    spi_->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    spi_->transfer(data);
    spi_->endTransaction();
    digitalWrite(EINK_CS_PIN, HIGH);
}

void DisplayEink::waitBusy(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (digitalRead(EINK_BUSY_PIN) == HIGH) {
        if (millis() - start > timeoutMs) break;
        delay(1);
    }
}

void DisplayEink::setWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    sendCommand(0x44); // X
    sendData(x / 8);
    sendData((x + w - 1) / 8);
    sendCommand(0x45); // Y
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
    memset(framebuffer_, black ? 0x00 : 0xFF, EINK_WIDTH * EINK_HEIGHT / 8);
}

void DisplayEink::setPixel(uint16_t x, uint16_t y, bool black) {
    if (x >= EINK_WIDTH || y >= EINK_HEIGHT) return;
    uint32_t idx = (y * EINK_WIDTH + x) / 8;
    uint8_t mask = 0x80 >> (x % 8);
    if (black) framebuffer_[idx] &= ~mask;
    else       framebuffer_[idx] |=  mask;
}

void DisplayEink::fullRefresh() {
    setWindow(0, 0, EINK_WIDTH, EINK_HEIGHT);
    sendCommand(0x24); // write RAM
    digitalWrite(EINK_DC_PIN, HIGH);
    digitalWrite(EINK_CS_PIN, LOW);
    spi_->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < EINK_WIDTH * EINK_HEIGHT / 8; i++) {
        spi_->transfer(framebuffer_[i]);
    }
    spi_->endTransaction();
    digitalWrite(EINK_CS_PIN, HIGH);

    sendCommand(0x22); // display update
    sendData(0xF7);
    sendCommand(0x20);
    waitBusy(8000);
}

void DisplayEink::partialRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    // For first bring-up we just do full; replace with true partial later
    fullRefresh();
}

void DisplayEink::flush(const lv_area_t* area, lv_color_t* color_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            lv_color_t c = color_map[y * w + x];
            bool black = (c.full == 0);   // 1-bit: 0 = black
            setPixel(area->x1 + x, area->y1 + y, black);
        }
    }
    // Decide full vs partial based on area size
    if (w * h > (EINK_WIDTH * EINK_HEIGHT / 4)) {
        fullRefresh();
    } else {
        partialRefresh(area->x1, area->y1, w, h);
    }
}