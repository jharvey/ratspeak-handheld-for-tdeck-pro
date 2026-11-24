// ATAK feed over Bluetooth LE: see CotBle.h.
#include "CotBle.h"
#include "PsramBuf.h"

#include <string.h>

namespace cotble {
namespace {

constexpr size_t kRxCap = 3072;          // events from the phone, until a whole one is in
char* g_rx = nullptr;                     // kRxCap bytes in PSRAM, from the first begin()
size_t g_rxLen = 0;
volatile uint8_t g_ev[8];                // events for the screen (small ring)
volatile uint8_t g_evHead = 0, g_evTail = 0;
uint32_t g_tx = 0, g_rxN = 0;
uint32_t g_code = 0;

void pushEv(Event e) {
    const uint8_t n = (uint8_t)((g_evHead + 1) & 7);
    if (n == g_evTail) return;
    g_ev[g_evHead] = e;
    g_evHead = n;
}

// Bytes from the phone into g_rx (lock held by the caller). A NUL would hide everything after
// it from strstr, so it becomes a space; data that does not fit drops what was waiting (an
// event that long is broken anyway), so the next good event still gets through.
void append(const char* d, size_t n) {
    if (n > kRxCap - 1) { d += n - (kRxCap - 1); n = kRxCap - 1; }
    if (n > kRxCap - 1 - g_rxLen) g_rxLen = 0;
    for (size_t i = 0; i < n; i++) g_rx[g_rxLen + i] = d[i] ? d[i] : ' ';
    g_rxLen += n;
}

// One whole event out of g_rx ("...</event>"), with the lock held by the caller.
int takeEventXml(char* buf, size_t cap) {
    static const char kEnd[] = "</event>";
    g_rx[g_rxLen < kRxCap ? g_rxLen : kRxCap - 1] = 0;
    char* e = strstr(g_rx, kEnd);
    if (!e) {
        if (g_rxLen >= kRxCap - 1) g_rxLen = 0;      // too long without an end: drop it
        return -1;
    }
    const size_t len = (size_t)(e - g_rx) + sizeof(kEnd) - 1;
    char* s = strstr(g_rx, "<?xml");                  // start at the event's own start
    if (!s || s > e) s = strstr(g_rx, "<event");
    if (!s || s > e) s = g_rx;
    const size_t off = (size_t)(s - g_rx);
    int out = -1;
    if (len - off + 1 <= cap) {
        memcpy(buf, s, len - off);
        buf[len - off] = 0;
        out = (int)(len - off);
    }
    memmove(g_rx, g_rx + len, g_rxLen - len);
    g_rxLen -= len;
    if (out >= 0) g_rxN++;
    return out;
}

}  // namespace

uint32_t passkey() { return g_code; }
uint32_t txEvents() { return g_tx; }
uint32_t rxEvents() { return g_rxN; }

Event takeEvent() {
    if (g_evTail == g_evHead) return EV_NONE;
    const Event e = (Event)g_ev[g_evTail];
    g_evTail = (uint8_t)((g_evTail + 1) & 7);
    return e;
}

}  // namespace cotble

#ifndef SIMULATOR
// ---------------------------------------------------------------------------------------------
// The radio: NimBLE peripheral.
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <nvs.h>

// The Arduino core frees the Bluetooth controller's RAM at boot unless btInUse() is true (it
// runs after nvs_flash_init). Keep it only when the ATAK link is set to Bluetooth.
extern "C" bool btInUse() {
    nvs_handle_t h;
    if (nvs_open("rs_cot", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t cfg[16] = {0};
    size_t n = sizeof(cfg);
    const bool ok = nvs_get_blob(h, "cfg", cfg, &n) == ESP_OK && n >= 2;
    nvs_close(h);
    return ok && cfg[1] == kCotLinkBle;
}

namespace cotble {
namespace {

const char* const kSvc = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const char* const kRxU = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const char* const kTxU = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

bool g_on = false, g_reserved = false, g_reservedKnown = false;
volatile bool g_conn = false, g_secure = false, g_pairing = false;
volatile uint16_t g_handle = 0xFFFF;
NimBLEServer* g_srv = nullptr;
NimBLECharacteristic* g_txc = nullptr;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
        g_handle = d->conn_handle;
        g_conn = true;
        g_secure = d->sec_state.encrypted && d->sec_state.authenticated;
        if (!g_secure) NimBLEDevice::startSecurity(d->conn_handle);   // pair (or re-use the bond)
        // A slow connection interval (100-200 ms) is enough for one event every few seconds.
        s->updateConnParams(d->conn_handle, 80, 160, 0, 600);
    }
    void onDisconnect(NimBLEServer*, ble_gap_conn_desc*) override {
        const bool was = g_secure;
        g_conn = false; g_secure = false; g_pairing = false; g_handle = 0xFFFF;
        portENTER_CRITICAL(&g_mux); g_rxLen = 0; portEXIT_CRITICAL(&g_mux);
        if (was) pushEv(EV_DISCONNECTED);
    }
    uint32_t onPassKeyRequest() override {      // display-only: the code the phone must type
        g_pairing = true;
        pushEv(EV_PAIR_CODE);
        return g_code;
    }
    void onAuthenticationComplete(ble_gap_conn_desc* d) override {
        g_pairing = false;
        if (!d->sec_state.encrypted || !d->sec_state.authenticated) {
            pushEv(EV_PAIR_FAILED);
            if (g_srv) g_srv->disconnect(d->conn_handle);
            return;
        }
        g_secure = true;
        pushEv(EV_CONNECTED);
    }
};

class RxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!g_secure) return;
        const std::string v = c->getValue();
        portENTER_CRITICAL(&g_mux);
        append(v.data(), v.size());
        portEXIT_CRITICAL(&g_mux);
    }
};

ServerCb g_scb;
RxCb g_rcb;

}  // namespace

bool reserved() {
    if (!g_reservedKnown) { g_reserved = btInUse(); g_reservedKnown = true; }
    return g_reserved;
}

bool begin(const char* name) {
    if (g_on) return true;
    if (!reserved()) return false;
    if (!g_rx) g_rx = psram<char, kRxCap>();
    if (!g_code) g_code = 100000 + esp_random() % 900000;
    const size_t m0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    NimBLEDevice::init(name);
    const size_t m1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);                  // a phone at arm's length: low power
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setSecurityAuth(true, true, true);         // bond, MITM (passkey), secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    g_srv = NimBLEDevice::createServer();
    g_srv->setCallbacks(&g_scb, false);
    NimBLEService* svc = g_srv->createService(kSvc);
    NimBLECharacteristic* rx = svc->createCharacteristic(kRxU,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
    rx->setCallbacks(&g_rcb);
    g_txc = svc->createCharacteristic(kTxU, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                                NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
    svc->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData ad, sr;
    ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    ad.setCompleteServices(NimBLEUUID(kSvc));
    sr.setName(name);
    adv->setAdvertisementData(ad);
    adv->setScanResponseData(sr);
    adv->setMinInterval(800);                                // 500 ms: a phone finds it in a second or two
    adv->setMaxInterval(1000);
    adv->start();
    Serial.printf("[BLE] internal RAM: %u before, %u after init, %u advertising (psram %u)\n", (unsigned)m0, (unsigned)m1,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    g_on = true;
    return true;
}

void end() {
    if (!g_on) return;
    NimBLEDevice::deinit(true);
    g_on = false; g_conn = false; g_secure = false; g_pairing = false;
    g_srv = nullptr; g_txc = nullptr;
    portENTER_CRITICAL(&g_mux); g_rxLen = 0; portEXIT_CRITICAL(&g_mux);
}

bool active() { return g_on; }
bool connected() { return g_on && g_conn && g_secure; }

const char* state() {
    if (!g_on) return "off";
    if (g_conn && g_secure) return "connected";
    if (g_conn) return "pairing";
    return "waiting for phone";
}

bool send(const char* xml, size_t n) {
    if (!connected() || !g_txc) return false;
    size_t mtu = g_srv ? g_srv->getPeerMTU(g_handle) : 23;
    size_t chunk = mtu > 23 ? mtu - 3 : 20;
    if (chunk > 244) chunk = 244;
    for (size_t o = 0; o < n; o += chunk) {
        const size_t k = n - o < chunk ? n - o : chunk;
        g_txc->setValue((const uint8_t*)xml + o, k);
        g_txc->notify();
        if (!connected()) return false;
    }
    g_txc->setValue((const uint8_t*)"\n", 1);
    g_txc->notify();
    g_tx++;
    return true;
}

int poll(char* buf, size_t cap) {
    if (!g_on) return -1;
    portENTER_CRITICAL(&g_mux);
    const int r = takeEventXml(buf, cap);
    portEXIT_CRITICAL(&g_mux);
    return r;
}

void forget() { if (g_on) NimBLEDevice::deleteAllBonds(); }

}  // namespace cotble

#else
// ---------------------------------------------------------------------------------------------
// Simulator: a UDP stand-in for the BLE link (see CotBle.h), on the PC's own loopback (not the
// simulated WiFi of lwip/sockets.h), so a script can play the phone.
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET RsSock;
#define RS_BADSOCK INVALID_SOCKET
#define rs_closesock closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int RsSock;
#define RS_BADSOCK (-1)
#define rs_closesock ::close
#endif

namespace cotble {
namespace {
RsSock g_sock = RS_BADSOCK;
bool g_on = false, g_peer = false;
sockaddr_in g_to;
}  // namespace

bool reserved() { return true; }

bool begin(const char*) {
    if (g_on) return true;
#ifdef _WIN32
    static bool wsa = false;
    if (!wsa) { WSADATA w; wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
#endif
    const char* e = getenv("RS_SIM_BLE_PORT");
    const int port = e && atoi(e) > 0 ? atoi(e) : 4390;
    g_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == RS_BADSOCK) return false;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(g_sock, (sockaddr*)&a, sizeof(a)) != 0) { rs_closesock(g_sock); g_sock = RS_BADSOCK; return false; }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
#else
    fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (!g_rx) g_rx = psram<char, kRxCap>();
    if (!g_code) g_code = 123456;
    g_on = true;
    g_peer = false;
    return true;
}

void end() {
    if (g_sock != RS_BADSOCK) { rs_closesock(g_sock); g_sock = RS_BADSOCK; }
    if (g_peer) pushEv(EV_DISCONNECTED);
    g_on = false; g_peer = false; g_rxLen = 0;
}

bool active() { return g_on; }
bool connected() { return g_on && g_peer; }
const char* state() { return !g_on ? "off" : g_peer ? "connected" : "waiting for phone"; }

bool send(const char* xml, size_t n) {
    if (!connected()) return false;
    for (size_t o = 0; o < n; o += 244) {               // the same cut as a 247-byte MTU
        const size_t k = n - o < 244 ? n - o : 244;
        ::sendto(g_sock, xml + o, (int)k, 0, (const sockaddr*)&g_to, sizeof(g_to));
    }
    ::sendto(g_sock, "\n", 1, 0, (const sockaddr*)&g_to, sizeof(g_to));
    g_tx++;
    return true;
}

int poll(char* buf, size_t cap) {
    if (!g_on) return -1;
    char d[1500];
    for (int k = 0; k < 16; k++) {
        sockaddr_in from;
#ifdef _WIN32
        int fl = sizeof(from);
#else
        socklen_t fl = sizeof(from);
#endif
        const int r = (int)::recvfrom(g_sock, d, sizeof(d), 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        if (!g_peer) { g_peer = true; g_to = from; pushEv(EV_CONNECTED); }
        if (r == 3 && !memcmp(d, "BYE", 3)) { g_peer = false; g_rxLen = 0; pushEv(EV_DISCONNECTED); continue; }
        append(d, (size_t)r);
    }
    return takeEventXml(buf, cap);
}

void forget() {}

}  // namespace cotble
#endif
