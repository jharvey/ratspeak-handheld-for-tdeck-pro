#include "DisplayEink.h"
#include <string.h>
#include <GxEPD2_BW.h>
#include <gdeq/GxEPD2_310_GDEQ031T10.h>

// Small page height: GxEPD2 keeps only this many rows of internal buffer.
// Full frame lives in our framebuffer_; we push it with writeImage().
// HEIGHT would cost another ~9600 bytes static and blows the post-proto heap.
static GxEPD2_BW<GxEPD2_310_GDEQ031T10, 16>
    g_epd(GxEPD2_310_GDEQ031T10(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

DisplayEink::DisplayEink()
    : framebuffer_(nullptr), ready_(false), dirty_(false) {}

bool DisplayEink::begin() {
    // Power gates and SPI are already on from main (radio path).
    // Only ensure CS lines idle so e-ink owns the bus for init.
    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif

    const size_t fb_bytes = (size_t)EPD_WIDTH * EPD_HEIGHT / 8;  // 9600
    framebuffer_ = (uint8_t*)ps_malloc(fb_bytes);
    if (!framebuffer_) {
        framebuffer_ = (uint8_t*)malloc(fb_bytes);
        if (framebuffer_) {
            Serial.println("[EINK] using internal heap for FB");
        }
    }
    if (!framebuffer_) {
        Serial.printf("[EINK] framebuffer alloc failed (%u bytes)\n",
                      (unsigned)fb_bytes);
        return false;
    }
    // bit 1 = black on panel (Adafruit_GFX / GxEPD2 drawBitmap)
    memset(framebuffer_, 0x00, fb_bytes);
    Serial.printf("[EINK] framebuffer %u bytes OK free_int=%u\n",
                  (unsigned)fb_bytes,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    Serial.println("[EINK] init GxEPD2...");
    // 0 = no debug serial from GxEPD2; 2 ms reset pulse (Waveshare-style)
    g_epd.init(0, true, 2, false);
    g_epd.setRotation(0);
    ready_ = true;
    dirty_ = false;
    Serial.printf("[EINK] DisplayEink ready free_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void DisplayEink::fillScreen(bool black) {
    if (!framebuffer_) return;
    memset(framebuffer_, black ? 0xFF : 0x00,
           (size_t)EPD_WIDTH * EPD_HEIGHT / 8);
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
            setPixel((uint16_t)(area->x1 + x),
                     (uint16_t)(area->y1 + y), black);
        }
    }
    dirty_ = true;
    // Do NOT fullRefresh here — caller batches then refreshIfDirty()
}

void DisplayEink::fullRefresh() {
    if (!ready_ || !framebuffer_) return;

    // Keep LoRa CS high while we drive the panel
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    digitalWrite(SD_CS, HIGH);
#endif

    Serial.println("[EINK] fullRefresh...");
    g_epd.setFullWindow();
    g_epd.writeImage(framebuffer_, 0, 0, EPD_WIDTH, EPD_HEIGHT);
    g_epd.refresh(false);  // false = full (not partial) refresh
    dirty_ = false;
    Serial.println("[EINK] fullRefresh done");
}

void DisplayEink::refreshIfDirty() {
    if (dirty_) fullRefresh();
}