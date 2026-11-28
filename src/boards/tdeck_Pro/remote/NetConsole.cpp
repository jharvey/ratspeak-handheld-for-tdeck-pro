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
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <mbedtls/platform.h>
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
constexpr size_t kOut = 64 * 1024;          // PSRAM
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
    // While connected, a writer (not the console task itself) waits up to 300 ms for room,
    // so a burst (a log dump) goes out at network speed instead of overwriting itself.
    // Unconnected, the ring just keeps the latest output.
    if (g_live && n < kOut / 2 && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle() != g_task) {
        for (int i = 0; i < 150; i++) {
            portENTER_CRITICAL(&g_outMux);
            const size_t used = (g_outHead + kOut - g_outTail) % kOut;
            portEXIT_CRITICAL(&g_outMux);
            if (used + n < kOut - 1) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
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

// Let's Encrypt roots (ISRG Root X1, valid to 2035; ISRG Root X2, to 2040): the public relay
// address (Tailscale Funnel) serves Let's Encrypt certificates.
const char kRoots[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

char g_err[72] = "";                      // last connection failure, for the console

// TLS memory from PSRAM (internal RAM is short with WiFi on; the framework's mbedTLS
// allocates internal RAM by default). Same as ESP-IDF's "external memory" option.
void* psCalloc(size_t n, size_t sz) {
    return heap_caps_calloc_prefer(n, sz, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// One connection to the relay: a plain socket (home relay) or TLS (internet relay).
struct Conn {
    int s = -1;
    WiFiClientSecure* tls = nullptr;
    bool open() const { return s >= 0 || tls; }
    // >0 bytes sent, 0 = try again later, -1 = broken
    int send(const void* p, size_t n) {
        if (tls) {
            if (!tls->connected()) return -1;
            const size_t w = tls->write((const uint8_t*)p, n);
            return w ? (int)w : -1;
        }
        const int w = ::send(s, p, n, MSG_DONTWAIT);
        if (w >= 0) return w;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    // >0 bytes read, 0 = nothing yet, -1 = closed or broken
    int recv(void* p, size_t n) {
        if (tls) {
            const int a = tls->available();
            if (a > 0) {
                const int r = tls->read((uint8_t*)p, n < (size_t)a ? n : (size_t)a);
                return r > 0 ? r : 0;
            }
            return tls->connected() ? 0 : -1;
        }
        const int r = ::recv(s, p, n, MSG_DONTWAIT);
        if (r > 0) return r;
        if (r == 0) return -1;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    void close() {
        if (tls) { tls->stop(); delete tls; tls = nullptr; }
        if (s >= 0) { ::close(s); s = -1; }
    }
};

bool sendAll(Conn& c, const char* p, size_t n, uint32_t ms) {
    const uint32_t t0 = millis();
    while (n) {
        const int w = c.send(p, n);
        if (w > 0) { p += w; n -= w; continue; }
        if (w < 0 || millis() - t0 > ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// One text line (without the newline), waiting up to ms.
bool recvLine(Conn& c, char* out, size_t cap, uint32_t ms) {
    const uint32_t t0 = millis();
    size_t n = 0;
    while (millis() - t0 < ms) {
        char ch;
        const int r = c.recv(&ch, 1);
        if (r == 1) {
            if (ch == '\n') { out[n] = 0; if (n && out[n - 1] == '\r') out[n - 1] = 0; return true; }
            if (n + 1 < cap) out[n++] = ch;
            continue;
        }
        if (r < 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

// Home relay: plain TCP, short timeout (it either answers at once or is not on this network).
int dialPlain(const Cfg& c) {
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)c.port);
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(c.host, port, &hints, &res) != 0 || !res) { snprintf(g_err, sizeof(g_err), "home relay: host not found"); return -1; }
    const int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); snprintf(g_err, sizeof(g_err), "no socket"); return -1; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    const int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) { close(s); snprintf(g_err, sizeof(g_err), "home relay not reachable"); return -1; }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    timeval tv = {3, 0};
    int err = 0;
    socklen_t el = sizeof(err);
    if (select(s + 1, nullptr, &w, nullptr, &tv) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
        close(s);
        snprintf(g_err, sizeof(g_err), "home relay not reachable");
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

// Internet relay: TLS to "host[:port]", certificate checked against the Let's Encrypt roots.
bool dialTls(Conn& c, const char* pub) {
    static bool memSet = false;
    if (!memSet) { mbedtls_platform_set_calloc_free(psCalloc, free); memSet = true; }
    char host[64];
    strlcpy(host, pub, sizeof(host));
    uint16_t port = 443;
    char* colon = strrchr(host, ':');
    if (colon) { port = (uint16_t)atoi(colon + 1); *colon = 0; }
    auto* t = new WiFiClientSecure();
    t->setCACert(kRoots);
    t->setHandshakeTimeout(30);
    const uint32_t t0 = millis();
    if (!t->connect(host, port, 15000)) {
        char e[48] = "";
        t->lastError(e, sizeof(e));
        snprintf(g_err, sizeof(g_err), "internet relay: %s", e[0] ? e : "not reachable");
        t->stop();
        delete t;
        return false;
    }
    Serial.printf("[NETCON] TLS to %s:%u in %lu ms (internal RAM free %u)\n", host, (unsigned)port,
                  (unsigned long)(millis() - t0), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    c.tls = t;
    return true;
}

// Greeting and key check both ways. false = closed (g_err says why).
bool handshake(Conn& s, const Cfg& c) {
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
    if (!sendAll(s, line, strlen(line), 5000)) { snprintf(g_err, sizeof(g_err), "relay closed"); return false; }
    if (!recvLine(s, line, sizeof(line), 10000)) { snprintf(g_err, sizeof(g_err), "relay did not answer"); return false; }
    char nr[40] = "", mac[80] = "";
    if (sscanf(line, "AUTH %39s %79s", nr, mac) != 2) { snprintf(g_err, sizeof(g_err), "relay refused"); return false; }
    snprintf(msg, sizeof(msg), "relay|%s|%s", nu, nr);
    hmacHex(c.key, msg, want);
    if (strcmp(want, mac) != 0) { snprintf(g_err, sizeof(g_err), "wrong key at relay"); return false; }
    snprintf(msg, sizeof(msg), "unit|%s|%s", nr, nu);
    hmacHex(c.key, msg, want);
    snprintf(line, sizeof(line), "OK %s\n", want);
    if (!sendAll(s, line, strlen(line), 5000)) { snprintf(g_err, sizeof(g_err), "relay closed"); return false; }
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
        if (!c.host[0] && !c.pub[0]) { g_status = "no relay set"; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!c.key[0]) { g_status = "no key set"; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (!g_linkUp) { g_status = "waiting for WiFi"; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        Conn conn;
        bool home = false;
        if (c.host[0]) {
            g_status = "connecting (home)";
            conn.s = dialPlain(c);
            home = conn.s >= 0;
        }
        if (!conn.open() && c.pub[0]) {
            g_status = "connecting (internet)";
            dialTls(conn, c.pub);
        }
        if (conn.open() && handshake(conn, c)) {
            g_status = home ? "connected (home)" : "connected (internet)";
            g_connects++;
            g_lost = 0;
            g_live = true;
            backoff = 2000;
            Serial.printf("[NETCON] %s\n", g_status);
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
                    if (!sendAll(conn, note, n, 5000)) { g_status = "connection lost"; break; }
                }
                const size_t n = outPeek(buf, sizeof(buf));
                if (n) {
                    const int w = conn.send(buf, n);
                    if (w > 0) { outDrop(w); g_sentB += w; busy = true; }
                    else if (w < 0) { g_status = "connection lost"; break; }
                }
                const int r = conn.recv(buf, sizeof(buf));
                if (r > 0) { inPush(buf, r); busy = true; }
                else if (r < 0) { g_status = "connection lost"; break; }
                vTaskDelay(pdMS_TO_TICKS(busy ? 2 : 25));
            }
            g_live = false;
            conn.close();
            Serial.printf("[NETCON] %s\n", g_status);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        conn.close();
        g_status = "not connected";
        Serial.printf("[NETCON] %s, retry in %lu s\n", g_err, (unsigned long)(backoff / 1000));
        // wait, but react at once to a settings change
        for (uint32_t t = 0; t < backoff && g_cfgGen == gen; t += 250) vTaskDelay(pdMS_TO_TICKS(250));
        backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
    }
}

void startIfOn() {
    if (!g_cfg.on) return;
    teeInstall();
    if (g_task) return;
    // The TLS handshake needs a deep stack (6 KB overflowed). This framework rejects task
    // stacks in PSRAM (xTaskCreateStatic asserts), so 12 KB of internal RAM; the TLS buffers
    // themselves go to PSRAM (psCalloc).
    xTaskCreatePinnedToCore(task, "netcon", 12 * 1024, nullptr, 2, &g_task, 0);
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
        strlcpy(g_cfg.pub, p.getString("pub", "").c_str(), sizeof(g_cfg.pub));
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
        p.putString("pub", c.pub);
        p.end();
    }
    portENTER_CRITICAL(&g_cfgMux);
    g_cfg = c;
    g_cfgGen++;
    portEXIT_CRITICAL(&g_cfgMux);
    g_on = c.on;
    startIfOn();
    Serial.printf("[NETCON] %s, home relay %s:%u, internet relay %s, key %s\n", c.on ? "on" : "off",
                  c.host[0] ? c.host : "-", (unsigned)c.port, c.pub[0] ? c.pub : "-", c.key[0] ? "set" : "not set");
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
const char* lastError() { return g_err; }
uint32_t sentKb() { return g_sentB >> 10; }
uint32_t connects() { return g_connects; }

}  // namespace netcon
#endif
