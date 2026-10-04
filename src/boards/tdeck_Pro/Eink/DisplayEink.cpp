#include "DisplayEink.h"
#include <string.h>
#include <GxEPD2_BW.h>
#include <gdeq/GxEPD2_310_GDEQ031T10.h>

static GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT>
    g_epd(GxEPD2_310_GDEQ031T10(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

DisplayEink::DisplayEink()
    : framebuffer_(nullptr), ready_(false), dirty_(false) {}

bool DisplayEink::begin() {
    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(50);

    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    const size_t fb_bytes = (size_t)EPD_WIDTH * EPD_HEIGHT / 8;
    framebuffer_ = (uint8_t*)ps_malloc(fb_bytes);
    if (!framebuffer_) {
        framebuffer_ = (uint8_t*)malloc(fb_bytes);
        if (framebuffer_) Serial.println("[EINK] using internal heap for FB");
    }
    if (!framebuffer_) {
        Serial.printf("[EINK] framebuffer alloc failed (%u bytes)\n", (unsigned)fb_bytes);
        return false;
    }
    // Guide: Initial FB memset 0x00 = white. bit 1 = black on panel.
    memset(framebuffer_, 0x00, fb_bytes);
    Serial.printf("[EINK] framebuffer %u bytes OK\n", (unsigned)fb_bytes);

    Serial.println("[EINK] init GxEPD2...");
    g_epd.init(115200, true, 50, false);
    g_epd.setRotation(0);
    ready_ = true;
    dirty_ = false;
    Serial.println("[EINK] DisplayEink ready (GxEPD2)");
    return true;
}

void DisplayEink::fillScreen(bool black) {
    if (!framebuffer_) return;
    memset(framebuffer_, black ? 0xFF : 0x00, (size_t)EPD_WIDTH * EPD_HEIGHT / 8);
    dirty_ = true;
}

void DisplayEink::setPixel(uint16_t x, uint16_t y, bool black) {
    if (!framebuffer_ || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    uint32_t idx = (y * EPD_WIDTH + x) / 8;
    uint8_t  mask = 0x80 >> (x % 8);
    if (black) framebuffer_[idx] |=  mask;
    else       framebuffer_[idx] &= ~mask;
}

void DisplayEink::flush(const lv_area_t* area, lv_color_t* color_map) {
    if (!framebuffer_) return;

    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            lv_color_t c = color_map[y * w + x];
            bool black = (c.full == 0);
            setPixel((uint16_t)(area->x1 + x), (uint16_t)(area->y1 + y), black);
        }
    }
    dirty_ = true;
}

void DisplayEink::fullRefresh() {
    if (!ready_ || !framebuffer_) return;

    Serial.println("[EINK] fullRefresh...");
    g_epd.setFullWindow();
    g_epd.firstPage();
    do {
        g_epd.fillScreen(GxEPD_WHITE);
        g_epd.drawBitmap(0, 0, framebuffer_, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
    } while (g_epd.nextPage());
    dirty_ = false;
    Serial.println("[EINK] fullRefresh done");
}

void DisplayEink::refreshIfDirty() {
    if (dirty_) fullRefresh();
}