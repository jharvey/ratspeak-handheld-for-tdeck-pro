// Remote console over WiFi: see NetConsole.h.
#include "NetConsole.h"

#if !defined(SIMULATOR)
#include <Arduino.h>
#include <HWCDC.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/md.h>
#include <string.h>

namespace netcon {
namespace {

// ---- settings, shared with the task -------------------------------------------------------
Cfg g_cfg;
portMUX_TYPE g_cfgMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_cfgGen = 1;          // bumped on every change: the task reconnects
volatile bool g_on = false;              // copy of g_cfg.on for the output path
volatile bool g_linkUp = false;
char g_name[32] = "?";
char g_dest[40] = "?";

// ---- output ring (oldest bytes overwritten) -----------------------------------------------
constexpr size_t kOut = 24 * 1024;
uint8_t* g_out = nullptr;
size_t g_outHead = 0, g_outTail = 0;     // head = next write, tail = next send; equal = empty
volatile uint32_t g_lost = 0;            // bytes overwritten before they were sent (while connected)
volatile bool g_live = false;            // an authenticated connection is up
portMUX_TYPE g_outMux = portMUX_INITIALIZER_UNLOCKED;

// ---- input ring ---------------------------------------------------------------------------
constexpr size_t kIn = 2048;
uint8_t g_in[kIn];
size_t g_inHead = 0, g_inTail = 0;
portMUX_TYPE g_inMux = portMUX_INITIALIZER_UNLOCKED;

const char* volatile g_status = "off";
volatile uint32_t g_sentB = 0, g_connects = 0;
TaskHandle_t g_task = nullptr;
bool g_teeOn = false;

void outPush(const uint8_t* b, size_t n) {
    if (!g_on || !g_out || !b || !n || xPortInIsrContext()) return;
    portENTER_CRITICAL(&g_outMux);
    for (size_t i = 0; i < n; i++) {
        g_out[g_outHead] = b[i];
        g_outHead = (g_outHead + 1) % kOut;
        if (g_outHead == g_outTail) {        // full: drop the oldest byte
            g_outTail = (g_outTail + 1) % kOut;
            if (g_live) g_lost++;
        }
    }
    portEXIT_CRITICAL(&g_outMux);
}

// Copies up to n queued output bytes without removing them.
size_t outPeek(uint8_t* dst, size_t n) {
    portENTER_CRITICAL(&g_outMux);
    size_t got = 0, t = g_outTail;
    while (got < n && t != g_outHead) { dst[got++] = g_out[t]; t = (t + 1) % kOut; }
    portEXIT_CRITICAL(&g_outMux);
    return got;
}

// Removes n sent bytes (fewer if the writer overwrote some meanwhile).
void outDrop(size_t n) {
    portENTER_CRITICAL(&g_outMux);
    while (n-- && g_outTail != g_outHead) g_outTail = (g_outTail + 1) % kOut;
    portEXIT_CRITICAL(&g_outMux);
}

void inPush(const uint8_t* b, size_t n) {
    portENTER_CRITICAL(&g_inMux);
    for (size_t i = 0; i < n; i++) {
        const size_t nx = (g_inHead + 1) % kIn;
        if (nx == g_inTail) break;           // full: the main loop is behind, drop the rest
        g_in[g_inHead] = b[i];
        g_inHead = nx;
    }
    portEXIT_CRITICAL(&g_inMux);
}

// The console's output goes through Serial (HWCDC): Print's printf / print / println reach
// write() through the object's table of virtual functions, so Serial gets this class's
// table (as hal/UsbDrive.cpp does for drive + COM port mode). The USB port still gets
// everything; the copy goes to the ring.
class NetTee : public HWCDC {
public:
    size_t write(const uint8_t* b, size_t n) override {
        const size_t r = HWCDC::write(b, n);
        outPush(b, n);
        return r;
    }
    size_t write(uint8_t ch) override { return write(&ch, 1); }
};

void teeInstall() {
    if (g_teeOn) return;
    if (!g_out) g_out = (uint8_t*)heap_caps_malloc(kOut, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_out) g_out = (uint8_t*)malloc(kOut / 4 * 4);
    if (!g_out) return;
    static NetTee tee;
    memcpy(static_cast<void*>(&Serial), static_cast<void*>(&tee), sizeof(void*));
    g_teeOn = true;
}

void hexOf(const uint8_t* b, size_t n, char* out) {
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = 0;
}

void hmacHex(const char* key, const char* msg, char out[65]) {
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)key, strlen(key),
                    (const uint8_t*)msg, strlen(msg), mac);
    hexOf(mac, 32, out);
}

bool sendAll(int s, const char* p, size_t n, uint32_t ms) {
    const uint32_t t0 = millis();
    while (n) {
        const int w = send(s, p, n, MSG_DONTWAIT);
        if (w > 0) { p += w; n -= w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (millis() - t0 > ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// One text line (without the newline), waiting up to ms.
bool recvLine(int s, char* out, size_t cap, uint32_t ms) {
    const uint32_t t0 = millis();
    size_t n = 0;
    while (millis() - t0 < ms) {
        char c;
        const int r = recv(s, &c, 1, MSG_DONTWAIT);
        if (r == 1) {
            if (c == '\n') { out[n] = 0; if (n && out[n - 1] == '\r') out[n - 1] = 0; return true; }
            if (n + 1 < cap) out[n++] = c;
            continue;
        }
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

int dial(const Cfg& c) {
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)c.port);
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(c.host, port, &hints, &res) != 0 || !res) { g_status = "host not found"; return -1; }
    const int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); g_status = "no socket"; return -1; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    const int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) { close(s); g_status = "relay not reachable"; return -1; }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    timeval tv = {6, 0};
    int err = 0;
    socklen_t el = sizeof(err);
    if (select(s + 1, nullptr, &w, nullptr, &tv) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
        close(s);
        g_status = "relay not reachable";
        return -1;
    }
    int one = 1, idle = 20, intvl = 5, cnt = 3;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

// Greeting and key check both ways. false = closed (g_status says why).
bool handshake(int s, const Cfg& c) {
    uint8_t rnd[8];
    esp_fill_random(rnd, sizeof(rnd));
    char nu[17], line[200], msg[120], want[65];
    hexOf(rnd, sizeof(rnd), nu);
    char name[32], dest[40];
    portENTER_CRITICAL(&g_cfgMux);
    strlcpy(name, g_name, sizeof(name));
    strlcpy(dest, g_dest, sizeof(dest));
    portEXIT_CRITICAL(&g_cfgMux);
    for (char* p = name; *p; p++) if (*p == ' ') *p = '_';
    snprintf(line, sizeof(line), "RSCON1 %s %s %s\n", name, dest, nu);
    if (!sendAll(s, line, strlen(line), 3000)) { g_status = "relay closed"; return false; }
    if (!recvLine(s, line, sizeof(line), 6000)) { g_status = "relay did not answer"; return false; }
    char nr[40] = "", mac[80] = "";
    if (sscanf(line, "AUTH %39s %79s", nr, mac) != 2) { g_status = "relay refused"; return false; }
    snprintf(msg, sizeof(msg), "relay|%s|%s", nu, nr);
    hmacHex(c.key, msg, want);
    if (strcmp(want, mac) != 0) { g_status = "wrong key at relay"; return false; }
    snprintf(msg, sizeof(msg), "unit|%s|%s", nr, nu);
    hmacHex(c.key, msg, want);
    snprintf(line, sizeof(line), "OK %s\n", want);
    if (!sendAll(s, line, strlen(line), 3000)) { g_status = "relay closed"; return false; }
    return true;
}

void task(void*) {
    uint32_t backoff = 2000;
    for (;;) {
        Cfg c;
        portENTER_CRITICAL(&g_cfgMux);
        c = g_cfg;
        const uint32_t gen = g_cfgGen;
        portEXIT_CRITICAL(&g_cfgMux);
        if (!c.on) { g_status = "off"; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!c.host[0]) { g_status = "no relay host set"; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!c.key[0]) { g_status = "no key set"; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!g_linkUp) { g_status = "waiting for WiFi"; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        g_status = "connecting";
        const int s = dial(c);
        if (s >= 0 && handshake(s, c)) {
            g_status = "connected";
            g_connects++;
            g_lost = 0;
            g_live = true;
            backoff = 2000;
            Serial.printf("[NETCON] connected to %s:%u\n", c.host, (unsigned)c.port);
            uint8_t buf[1024];
            uint32_t lostSaid = 0;
            for (;;) {
                if (g_cfgGen != gen || !g_linkUp) { g_status = "reconnecting"; break; }
                bool busy = false;
                if (g_lost != lostSaid) {        // tell the relay bytes were lost (ring overrun)
                    char note[64];
                    lostSaid = g_lost;
                    const int n = snprintf(note, sizeof(note), "\n[NETCON] %lu bytes lost (ring full)\n",
                                           (unsigned long)lostSaid);
                    if (!sendAll(s, note, n, 2000)) { g_status = "connection lost"; break; }
                }
                const size_t n = outPeek(buf, sizeof(buf));
                if (n) {
                    const int w = send(s, buf, n, MSG_DONTWAIT);
                    if (w > 0) { outDrop(w); g_sentB += w; busy = true; }
                    else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { g_status = "connection lost"; break; }
                }
                const int r = recv(s, buf, sizeof(buf), MSG_DONTWAIT);
                if (r > 0) { inPush(buf, r); busy = true; }
                else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { g_status = "connection lost"; break; }
                vTaskDelay(pdMS_TO_TICKS(busy ? 2 : 25));
            }
            g_live = false;
            close(s);
            Serial.printf("[NETCON] %s\n", g_status);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s >= 0) close(s);
        Serial.printf("[NETCON] %s:%u %s, retry in %lu s\n", c.host, (unsigned)c.port, g_status,
                      (unsigned long)(backoff / 1000));
        // wait, but react at once to a settings change
        for (uint32_t t = 0; t < backoff && g_cfgGen == gen; t += 250) vTaskDelay(pdMS_TO_TICKS(250));
        backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
    }
}

void startIfOn() {
    if (!g_cfg.on) return;
    teeInstall();
    if (!g_task) xTaskCreatePinnedToCore(task, "netcon", 6144, nullptr, 2, &g_task, 0);
}

}  // namespace

void load() {
    Preferences p;
    if (p.begin("rs_ncon", true)) {
        g_cfg.on = p.getUChar("on", 0) != 0;
        strlcpy(g_cfg.host, p.getString("host", "").c_str(), sizeof(g_cfg.host));
        g_cfg.port = p.getUShort("port", kDefaultPort);
        if (!g_cfg.port) g_cfg.port = kDefaultPort;
        strlcpy(g_cfg.key, p.getString("key", "").c_str(), sizeof(g_cfg.key));
        p.end();
    }
    g_on = g_cfg.on;
    startIfOn();
}

Cfg cfg() {
    portENTER_CRITICAL(&g_cfgMux);
    Cfg c = g_cfg;
    portEXIT_CRITICAL(&g_cfgMux);
    return c;
}

void set(const Cfg& c) {
    Preferences p;
    if (p.begin("rs_ncon", false)) {
        p.putUChar("on", c.on ? 1 : 0);
        p.putString("host", c.host);
        p.putUShort("port", c.port);
        p.putString("key", c.key);
        p.end();
    }
    portENTER_CRITICAL(&g_cfgMux);
    g_cfg = c;
    g_cfgGen++;
    portEXIT_CRITICAL(&g_cfgMux);
    g_on = c.on;
    startIfOn();
    Serial.printf("[NETCON] %s, relay %s:%u, key %s\n", c.on ? "on" : "off", c.host[0] ? c.host : "-",
                  (unsigned)c.port, c.key[0] ? "set" : "not set");
}

void tick(bool wifiUp, const char* name, const char* dest) {
    g_linkUp = wifiUp;
    if (name && dest && (strcmp(name, g_name) || strcmp(dest, g_dest))) {
        portENTER_CRITICAL(&g_cfgMux);
        strlcpy(g_name, name, sizeof(g_name));
        strlcpy(g_dest, dest, sizeof(g_dest));
        portEXIT_CRITICAL(&g_cfgMux);
    }
}

int read() {
    int c = -1;
    portENTER_CRITICAL(&g_inMux);
    if (g_inTail != g_inHead) { c = g_in[g_inTail]; g_inTail = (g_inTail + 1) % kIn; }
    portEXIT_CRITICAL(&g_inMux);
    return c;
}

const char* status() { return g_status; }
uint32_t sentKb() { return g_sentB >> 10; }
uint32_t connects() { return g_connects; }

}  // namespace netcon
#endif
