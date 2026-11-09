// Simulated WiFi station against the access points listed in sim::Hw::wifiNets.
// Timing mirrors a real join: scan ~1.5 s, join ~2 s, wrong password fails at ~3 s.
#include "WiFi.h"
#include "hw/SimHw.h"

#include <mutex>
#include <vector>

WiFiClass WiFi;
uint32_t WiFiScanClass::_scanTimeout = 10000;

namespace {
std::mutex g_mu;
wifi_mode_t g_mode = WIFI_OFF;
bool g_scanRunning = false;
uint32_t g_scanDone = 0;
std::vector<sim::WifiNet> g_results;
bool g_haveResults = false;

enum class Join { Idle, Joining, Connected, BadPass, NoSsid };
Join g_join = Join::Idle;
uint32_t g_joinAt = 0;
std::string g_ssid, g_pass;
int g_rssi = -127;

bool lookup(const std::string& ssid, sim::WifiNet& out) {
    std::lock_guard<std::mutex> l(sim::hw().mu);
    for (auto& n : sim::hw().wifiNets) if (n.ssid == ssid) { out = n; return true; }
    return false;
}

void step() {   // caller holds g_mu
    const uint32_t now = sim::nowMs();
    if (g_scanRunning && (int32_t)(now - g_scanDone) >= 0) {
        g_scanRunning = false;
        std::lock_guard<std::mutex> l(sim::hw().mu);
        g_results = sim::hw().wifiNets;
        g_haveResults = true;
    }
    if (g_join == Join::Joining) {
        sim::WifiNet n;
        const bool found = lookup(g_ssid, n);
        if (!found && now - g_joinAt > 2500) g_join = Join::NoSsid;
        else if (found && n.secure && n.password != g_pass && now - g_joinAt > 3000) g_join = Join::BadPass;
        else if (found && (!n.secure || n.password == g_pass) && now - g_joinAt > 2000) {
            g_join = Join::Connected;
            g_rssi = n.rssi;
        }
    } else if (g_join == Join::Connected) {
        sim::WifiNet n;
        if (!lookup(g_ssid, n)) g_join = Join::Idle;          // AP removed in the side panel
        else g_rssi = n.rssi + (int)(now / 1000 % 5) - 2;     // a little jitter
    }
}
}  // namespace

bool WiFiClass::mode(wifi_mode_t m) {
    std::lock_guard<std::mutex> l(g_mu);
    g_mode = m;
    if (m == WIFI_OFF || m == WIFI_AP) { g_join = Join::Idle; g_scanRunning = false; }
    return true;
}
wifi_mode_t WiFiClass::getMode() { std::lock_guard<std::mutex> l(g_mu); return g_mode; }

wl_status_t WiFiClass::begin(const char* ssid, const char* pass, int32_t, const uint8_t*, bool) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_mode == WIFI_OFF) g_mode = WIFI_STA;
    g_ssid = ssid ? ssid : "";
    g_pass = pass ? pass : "";
    g_join = Join::Joining;
    g_joinAt = sim::nowMs();
    return WL_DISCONNECTED;
}
bool WiFiClass::disconnect(bool wifiOff, bool) {
    std::lock_guard<std::mutex> l(g_mu);
    g_join = Join::Idle;
    if (wifiOff) g_mode = WIFI_OFF;
    return true;
}
bool WiFiClass::reconnect() {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_ssid.empty()) return false;
    g_join = Join::Joining; g_joinAt = sim::nowMs();
    return true;
}
wl_status_t WiFiClass::status() {
    std::lock_guard<std::mutex> l(g_mu);
    step();
    if (g_mode == WIFI_OFF) return WL_DISCONNECTED;
    switch (g_join) {
        case Join::Connected: return WL_CONNECTED;
        case Join::BadPass:   return WL_CONNECT_FAILED;
        case Join::NoSsid:    return WL_NO_SSID_AVAIL;
        case Join::Joining:   return WL_DISCONNECTED;
        default:              return WL_IDLE_STATUS;
    }
}
IPAddress WiFiClass::localIP() {
    std::lock_guard<std::mutex> l(g_mu);
    return g_join == Join::Connected ? IPAddress(192, 168, 1, 57) : IPAddress();
}
String WiFiClass::SSID() { std::lock_guard<std::mutex> l(g_mu); return g_join == Join::Connected ? String(g_ssid.c_str()) : String(); }
int8_t WiFiClass::RSSI() { std::lock_guard<std::mutex> l(g_mu); step(); return g_join == Join::Connected ? (int8_t)g_rssi : 0; }
bool WiFiClass::softAP(const char*, const char*, int, int, int) {
    std::lock_guard<std::mutex> l(g_mu);
    g_mode = g_mode == WIFI_STA ? WIFI_AP_STA : WIFI_AP;
    return true;
}
bool WiFiClass::softAPdisconnect(bool wifiOff) {
    std::lock_guard<std::mutex> l(g_mu);
    if (wifiOff) g_mode = WIFI_OFF;
    else if (g_mode == WIFI_AP_STA) g_mode = WIFI_STA;
    return true;
}

int16_t WiFiScanClass::scanNetworks(bool async, bool, bool, uint32_t, uint8_t, const char*, const uint8_t*) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_mode == WIFI_OFF) return WIFI_SCAN_FAILED;
        g_scanRunning = true;
        g_haveResults = false;
        g_scanDone = sim::nowMs() + 1500;
    }
    if (async) return WIFI_SCAN_RUNNING;
    delay(1500);
    return scanComplete();
}
int16_t WiFiScanClass::scanComplete() {
    std::lock_guard<std::mutex> l(g_mu);
    step();
    if (g_scanRunning) return WIFI_SCAN_RUNNING;
    return g_haveResults ? (int16_t)g_results.size() : WIFI_SCAN_FAILED;
}
void WiFiScanClass::scanDelete() {
    std::lock_guard<std::mutex> l(g_mu);
    g_results.clear();
    g_haveResults = false;
}
String WiFiScanClass::SSID(uint8_t i) {
    std::lock_guard<std::mutex> l(g_mu);
    return i < g_results.size() ? String(g_results[i].ssid.c_str()) : String();
}
int32_t WiFiScanClass::RSSI(uint8_t i) {
    std::lock_guard<std::mutex> l(g_mu);
    return i < g_results.size() ? g_results[i].rssi : -127;
}
wifi_auth_mode_t WiFiScanClass::encryptionType(uint8_t i) {
    std::lock_guard<std::mutex> l(g_mu);
    return i < g_results.size() && g_results[i].secure ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}
