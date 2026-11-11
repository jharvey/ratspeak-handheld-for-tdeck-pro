// Host implementation of the Arduino-ESP32 core used by the firmware.
#include "Arduino.h"
#include "esp_partition.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hw/SimHw.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace sim {
static const auto kStart = std::chrono::steady_clock::now();
uint32_t nowMs() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - kStart).count();
}
Hw& hw() { static Hw h; return h; }
// Firmware -> side panel: power mode, sleep share, CPU, radio (declared in StandaloneApp).
void setPowerInfo(const char* line) {
    std::lock_guard<std::mutex> l(hw().mu);
    hw().powerInfo = line ? line : "";
}
}  // namespace sim

extern "C" unsigned long millis() { return sim::nowMs(); }
unsigned long micros() {
    return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - sim::kStart).count();
}
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void delayMicroseconds(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void yield() { std::this_thread::yield(); }

// ---- GPIO: remembered levels; a few pins have meaning in the simulator ------------
static int g_pins[64];
void pinMode(int pin, int mode) {
    if (mode == INPUT_PULLUP && pin >= 0 && pin < 64) g_pins[pin] = HIGH;   // e.g. KB_INT idles high
}
void digitalWrite(int pin, int v) {
    if (pin < 0 || pin >= 64) return;
    g_pins[pin] = v;
    if (pin == 2 && v) {                       // BOARD_MOTOR_PIN: vibration motor
        std::lock_guard<std::mutex> l(sim::hw().mu);
        sim::hw().vibrateUntil = sim::nowMs() + 120;
    }
}
int digitalRead(int pin) {
    if (pin == 12) {                           // TOUCH_INT (active low)
        std::lock_guard<std::mutex> l(sim::hw().mu);
        return sim::hw().touchDown ? LOW : HIGH;
    }
    return (pin >= 0 && pin < 64) ? g_pins[pin] : 0;
}
int analogRead(int) { return 2048; }
void analogReadResolution(int) {}
void attachInterrupt(int, void (*)(), int) {}
void detachInterrupt(int) {}

double ledcSetup(uint8_t, double f, uint8_t) { return f; }
void ledcAttachPin(uint8_t, uint8_t) {}
void ledcDetachPin(uint8_t) {}
void ledcWrite(uint8_t ch, uint32_t duty) {
    if (ch != 0) return;                       // keyboard backlight channel
    std::lock_guard<std::mutex> l(sim::hw().mu);
    sim::hw().kbBacklight = duty > 0;
}

static std::mt19937& rng() { static std::mt19937 r{std::random_device{}()}; return r; }
long random(long mx) { return mx > 0 ? (long)(rng()() % (uint32_t)mx) : 0; }
long random(long mn, long mx) { return mx > mn ? mn + random(mx - mn) : mn; }
void randomSeed(unsigned long s) { rng().seed((uint32_t)s); }

void* ps_malloc(size_t n) { return malloc(n); }
void* ps_calloc(size_t n, size_t s) { return calloc(n, s); }
void* ps_realloc(void* p, size_t n) { return realloc(p, n); }

extern "C" {
void* heap_caps_malloc(size_t n, uint32_t) { return malloc(n); }
void* heap_caps_calloc(size_t n, size_t s, uint32_t) { return calloc(n, s); }
void* heap_caps_realloc(void* p, size_t n, uint32_t) { return realloc(p, n); }
void* heap_caps_aligned_alloc(size_t a, size_t n, uint32_t) {
    if (a < sizeof(void*)) a = sizeof(void*);
    return _aligned_malloc(n, a);
}
void heap_caps_free(void* p) { free(p); }
// Plausible ESP32-S3 numbers so the firmware's heap gates behave as on the device.
size_t heap_caps_get_free_size(uint32_t caps) {
    return (caps & MALLOC_CAP_SPIRAM) ? 7u * 1024 * 1024 : 180u * 1024;
}
size_t heap_caps_get_largest_free_block(uint32_t caps) {
    return (caps & MALLOC_CAP_SPIRAM) ? 4u * 1024 * 1024 : 110u * 1024;
}
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps); }
size_t heap_caps_get_total_size(uint32_t caps) {
    return (caps & MALLOC_CAP_SPIRAM) ? 8u * 1024 * 1024 : 320u * 1024;
}

esp_err_t esp_efuse_mac_get_default(uint8_t* mac) {
    // Stable per simulated device: derived from its name.
    std::string n;
    { std::lock_guard<std::mutex> l(sim::hw().mu); n = sim::hw().deviceName; }
    uint32_t h = 2166136261u;
    for (char c : n) { h ^= (uint8_t)c; h *= 16777619u; }
    const uint8_t m[6] = {0x24, 0x0A, 0xC4, (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
    memcpy(mac, m, 6);
    return ESP_OK;
}
esp_err_t esp_read_mac(uint8_t* mac, int) { return esp_efuse_mac_get_default(mac); }
void esp_fill_random(void* buf, size_t len) {
    auto* p = (uint8_t*)buf;
    for (size_t i = 0; i < len; i++) p[i] = (uint8_t)rng()();
}
uint32_t esp_random(void) { return rng()(); }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void esp_restart(void) { ESP.restart(); }
uint32_t esp_get_free_heap_size(void) { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
int64_t esp_timer_get_time(void) { return (int64_t)micros(); }
const char* esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

// ---- partitions: raw reads see erased flash -------------------------------------------
static esp_partition_t g_spiffs = {nullptr, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                   0x910000, 6 * 1024 * 1024, 4096, "spiffs", false};
static esp_partition_t g_app = {nullptr, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                0x10000, 4 * 1024 * 1024, 4096, "app0", false};
const esp_partition_t* esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t,
                                                const char*) {
    return t == ESP_PARTITION_TYPE_APP ? &g_app : &g_spiffs;
}
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void* dst, size_t n) {
    memset(dst, 0xFF, n);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t*, size_t, const void*, size_t) { return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t, size_t) { return ESP_OK; }
}  // extern "C"

uint32_t EspClass::getFreeHeap() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
uint64_t EspClass::getEfuseMac() {
    uint8_t m[6]; esp_efuse_mac_get_default(m);
    uint64_t v = 0; for (int i = 5; i >= 0; i--) v = (v << 8) | m[i];
    return v;
}
void EspClass::restart() {
    Serial.println("[SIM] ESP.restart() - quitting device thread");
    { std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().quit = true; }
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));   // runner tears down
}
EspClass ESP;

// ---- Serial (USB console) -------------------------------------------------------------
static std::mutex g_serialMu;
static std::string g_serialLine;

int HardwareSerial::available() {
    if (port_ == 1) {                          // GPS UART
        extern int simGpsAvailable();
        return simGpsAvailable();
    }
    if (port_ != 0) return 0;
    std::lock_guard<std::mutex> l(sim::hw().mu);
    return (int)sim::hw().serialIn.size();
}
int HardwareSerial::read() {
    if (port_ == 1) {
        extern int simGpsRead();
        return simGpsRead();
    }
    if (port_ != 0) return -1;
    std::lock_guard<std::mutex> l(sim::hw().mu);
    auto& q = sim::hw().serialIn;
    if (q.empty()) return -1;
    char c = q.front(); q.pop_front();
    return (uint8_t)c;
}
size_t HardwareSerial::write(uint8_t c) { return write(&c, 1); }
size_t HardwareSerial::write(const uint8_t* b, size_t n) {
    if (port_ != 0) return n;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> l(g_serialMu);
        for (size_t i = 0; i < n; i++) {
            if (b[i] == '\n') { lines.push_back(g_serialLine); g_serialLine.clear(); }
            else if (b[i] != '\r') g_serialLine += (char)b[i];
        }
    }
    if (!lines.empty()) {
        std::function<void(const std::string&)> out;
        { std::lock_guard<std::mutex> l(sim::hw().mu); out = sim::hw().serialOut; }
        for (auto& s : lines) {
            if (out) out(s);
            if (s.rfind("@FB ", 0) != 0) fprintf(stdout, "%s\n", s.c_str());   // no mirror frames on stdout
        }
        fflush(stdout);
    }
    return n;
}
void HardwareSerial::flush() {}

HardwareSerial Serial(0);
HardwareSerial Serial1(1);
HardwareSerial Serial2(2);

// ---- GPS: NMEA RMC + GGA once a second from sim::Hw ------------------------------------
static std::deque<char> g_nmea;
static uint32_t g_nmeaLast = 0;

static void nmeaAppend(const char* body) {
    uint8_t ck = 0;
    for (const char* p = body; *p; p++) ck ^= (uint8_t)*p;
    char line[160];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, ck);
    for (const char* p = line; *p; p++) g_nmea.push_back(*p);
}

static void nmeaCoord(double v, bool lat, char* out, size_t n, char& hemi) {
    hemi = lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W');
    v = fabs(v);
    int deg = (int)v;
    double minutes = (v - deg) * 60.0;
    snprintf(out, n, lat ? "%02d%07.4f" : "%03d%07.4f", deg, minutes);
}

static void gpsTick() {
    const uint32_t now = sim::nowMs();
    if (now - g_nmeaLast < 1000 && g_nmeaLast) return;
    g_nmeaLast = now;
    bool fix; double lat, lon; int sats;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        fix = sim::hw().gpsFix; lat = sim::hw().lat; lon = sim::hw().lon; sats = sim::hw().sats;
    }
    // GPS time is the host's real UTC (independent of the device clock).
    extern time_t simHostUtc();
    const time_t utc = simHostUtc();
    struct tm t;
    gmtime_s(&t, &utc);
    char hhmmss[16], ddmmyy[16], la[16], lo[16], ns, ew, body[128];
    snprintf(hhmmss, sizeof(hhmmss), "%02d%02d%02d.00", t.tm_hour, t.tm_min, t.tm_sec);
    snprintf(ddmmyy, sizeof(ddmmyy), "%02d%02d%02d", t.tm_mday, t.tm_mon + 1, t.tm_year % 100);
    nmeaCoord(lat, true, la, sizeof(la), ns);
    nmeaCoord(lon, false, lo, sizeof(lo), ew);
    if (fix) {
        snprintf(body, sizeof(body), "GPRMC,%s,A,%s,%c,%s,%c,0.0,0.0,%s,,,A", hhmmss, la, ns, lo, ew, ddmmyy);
        nmeaAppend(body);
        snprintf(body, sizeof(body), "GPGGA,%s,%s,%c,%s,%c,1,%02d,0.9,120.0,M,0.0,M,,", hhmmss, la, ns, lo, ew, sats);
        nmeaAppend(body);
    } else {
        snprintf(body, sizeof(body), "GPRMC,%s,V,,,,,,,%s,,,N", hhmmss, ddmmyy);
        nmeaAppend(body);
        snprintf(body, sizeof(body), "GPGGA,%s,,,,,0,00,99.9,,M,,M,,", hhmmss);
        nmeaAppend(body);
    }
    if (g_nmea.size() > 4096) g_nmea.erase(g_nmea.begin(), g_nmea.end() - 1024);
}
int simGpsAvailable() { gpsTick(); return (int)g_nmea.size(); }
int simGpsRead() {
    gpsTick();
    if (g_nmea.empty()) return -1;
    char c = g_nmea.front(); g_nmea.pop_front();
    return (uint8_t)c;
}

// ---- FreeRTOS (enough for task ownership checks and simple helpers) -------------------
static thread_local int g_taskTag = 0;
static std::atomic<int> g_nextTask{1};
TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    if (!g_taskTag) g_taskTag = g_nextTask++;
    return (TaskHandle_t)(intptr_t)g_taskTag;
}
void vTaskDelay(TickType_t t) { delay(t); }
TickType_t xTaskGetTickCount(void) { return (TickType_t)millis(); }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char*, uint32_t, void* arg, UBaseType_t,
                                   TaskHandle_t* out, BaseType_t) {
    int tag = g_nextTask++;
    std::thread([fn, arg, tag] { g_taskTag = tag; fn(arg); }).detach();
    if (out) *out = (TaskHandle_t)(intptr_t)tag;
    return pdPASS;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char* n, uint32_t s, void* a, UBaseType_t p, TaskHandle_t* o) {
    return xTaskCreatePinnedToCore(fn, n, s, a, p, o, 0);
}
void vTaskDelete(TaskHandle_t) {}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }

struct SimSem { std::recursive_timed_mutex m; std::mutex cm; std::condition_variable cv; int count = 0; bool binary = false; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return new SimSem(); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return new SimSem(); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { auto* s = new SimSem(); s->binary = true; return s; }
static BaseType_t semTake(SimSem* s, TickType_t t) {
    if (!s) return pdFALSE;
    if (s->binary) {
        std::unique_lock<std::mutex> l(s->cm);
        auto ok = [s] { return s->count > 0; };
        if (t == portMAX_DELAY) s->cv.wait(l, ok);
        else if (!s->cv.wait_for(l, std::chrono::milliseconds(t), ok)) return pdFALSE;
        s->count--;
        return pdTRUE;
    }
    if (t == portMAX_DELAY) { s->m.lock(); return pdTRUE; }
    return s->m.try_lock_for(std::chrono::milliseconds(t)) ? pdTRUE : pdFALSE;
}
static BaseType_t semGive(SimSem* s) {
    if (!s) return pdFALSE;
    if (s->binary) {
        { std::lock_guard<std::mutex> l(s->cm); s->count = 1; }
        s->cv.notify_one();
        return pdTRUE;
    }
    s->m.unlock();
    return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { return semTake((SimSem*)s, t); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { return semGive((SimSem*)s); }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { return semTake((SimSem*)s, t); }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return semGive((SimSem*)s); }
void vSemaphoreDelete(SemaphoreHandle_t s) { delete (SimSem*)s; }

struct SimQueue { std::mutex m; std::condition_variable cv; std::deque<std::vector<uint8_t>> q; size_t len, item; };
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item) {
    auto* q = new SimQueue(); q->len = len; q->item = item; return q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void* item, TickType_t) {
    auto* q = (SimQueue*)h;
    std::lock_guard<std::mutex> l(q->m);
    if (q->q.size() >= q->len) return pdFALSE;
    q->q.emplace_back((const uint8_t*)item, (const uint8_t*)item + q->item);
    q->cv.notify_one();
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t h, void* item, TickType_t t) {
    auto* q = (SimQueue*)h;
    std::unique_lock<std::mutex> l(q->m);
    auto ok = [q] { return !q->q.empty(); };
    if (t == portMAX_DELAY) q->cv.wait(l, ok);
    else if (!q->cv.wait_for(l, std::chrono::milliseconds(t), ok)) return pdFALSE;
    memcpy(item, q->q.front().data(), q->item);
    q->q.pop_front();
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t h) {
    auto* q = (SimQueue*)h; std::lock_guard<std::mutex> l(q->m); return (UBaseType_t)q->q.size();
}
void vQueueDelete(QueueHandle_t h) { delete (SimQueue*)h; }
