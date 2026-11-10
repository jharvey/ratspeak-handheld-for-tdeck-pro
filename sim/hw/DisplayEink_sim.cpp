// Simulated GDEQ031T10 e-ink panel behind the firmware's DisplayEink API.
// Same framebuffer format and row-diff partial refresh as Eink/DisplayEink.cpp;
// each refresh publishes the frame to the window and (optionally) takes as long as
// the real panel (~650 ms partial, ~1.6 s full), calling the busy callback meanwhile
// so the firmware's keyboard-during-refresh path is exercised too.
#include "Eink/DisplayEink.h"
#include "hw/SimHw.h"

#include <cstring>

namespace {
constexpr int kRowBytes = EPD_WIDTH / 8;
constexpr size_t kFbBytes = (size_t)EPD_WIDTH * EPD_HEIGHT / 8;
void (*g_busyCb)(const void*) = nullptr;
const void* g_busyParam = nullptr;

void publish(const uint8_t* fb, bool full, uint32_t ms) {
    auto& h = sim::hw();
    std::lock_guard<std::mutex> l(h.mu);
    memcpy(h.panel.data(), fb, kFbBytes);
    h.panelVersion++;
    h.lastRefreshFull = full;
    h.lastRefreshMs = ms;
}

void panelBusy(uint32_t ms) {
    bool realistic;
    { std::lock_guard<std::mutex> l(sim::hw().mu); realistic = sim::hw().realisticPanel; }
    if (!realistic) return;
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        if (g_busyCb) g_busyCb(g_busyParam);
        else delay(1);
    }
}
}  // namespace

DisplayEink::DisplayEink()
    : framebuffer_(nullptr), shadow_(nullptr), lastRefreshMs_(0), ready_(false), dirty_(false), partialCount_(0) {}

void DisplayEink::parkSpiCs() {}

bool DisplayEink::begin() {
    framebuffer_ = (uint8_t*)calloc(1, kFbBytes);
    shadow_ = (uint8_t*)malloc(kFbBytes);
    if (!framebuffer_ || !shadow_) return false;
    memset(shadow_, 0xFF, kFbBytes);
    ready_ = true;
    Serial.println("[EINK] simulated GDEQ031T10 240x320 ready");
    return true;
}

void DisplayEink::fillScreen(bool black) {
    if (!framebuffer_) return;
    memset(framebuffer_, black ? 0xFF : 0x00, kFbBytes);
    dirty_ = true;
}

void DisplayEink::setPixel(uint16_t x, uint16_t y, bool black) {
    if (!framebuffer_ || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    const uint32_t idx = (y * EPD_WIDTH + x) / 8;
    const uint8_t mask = 0x80 >> (x % 8);
    if (black) framebuffer_[idx] |= mask;
    else       framebuffer_[idx] &= ~mask;
}

void DisplayEink::flush(const lv_area_t* area, lv_color_t* color_map) {
    if (!framebuffer_) return;
    const int32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
    const lv_color_t white = lv_color_white();
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++)
            setPixel((uint16_t)(area->x1 + x), (uint16_t)(area->y1 + y), color_map[y * w + x].full != white.full);
    dirty_ = true;
}

void DisplayEink::setBusyCallback(void (*cb)(const void*), const void* param) { g_busyCb = cb; g_busyParam = param; }
void DisplayEink::powerOff() {}
void DisplayEink::hibernate() {}

void DisplayEink::fullRefresh() {
    if (!ready_ || !framebuffer_) return;
    const uint32_t t0 = millis();
    panelBusy(1600);
    memcpy(shadow_, framebuffer_, kFbBytes);
    dirty_ = false;
    partialCount_ = 0;
    lastRefreshMs_ = millis() - t0;
    publish(framebuffer_, true, lastRefreshMs_);
    Serial.printf("[EINK] full %ums\n", (unsigned)lastRefreshMs_);
}

void DisplayEink::partialRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!ready_ || !framebuffer_) return;
    if (w == 0 || h == 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    int first = -1, last = -1;
    for (int row = y; row < y + h; row++) {
        const size_t off = (size_t)row * kRowBytes;
        if (memcmp(framebuffer_ + off, shadow_ + off, kRowBytes) != 0) {
            if (first < 0) first = row;
            last = row;
        }
    }
    dirty_ = false;
    if (first < 0) { lastRefreshMs_ = 0; return; }
    const uint32_t t0 = millis();
    panelBusy(650);
    const uint16_t rows = (uint16_t)(last - first + 1);
    memcpy(shadow_ + (size_t)first * kRowBytes, framebuffer_ + (size_t)first * kRowBytes, (size_t)rows * kRowBytes);
    partialCount_++;
    lastRefreshMs_ = millis() - t0;
    publish(shadow_, false, lastRefreshMs_);
    Serial.printf("[EINK] partial rows %d-%d %ums\n", first, last, (unsigned)lastRefreshMs_);
}

void DisplayEink::refreshIfDirty() {
    if (!dirty_) return;
    partialRefresh(0, 0, EPD_WIDTH, EPD_HEIGHT);
}
