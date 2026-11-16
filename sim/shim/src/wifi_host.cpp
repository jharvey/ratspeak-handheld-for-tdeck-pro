// Simulated WiFi station against the access points listed in sim::Hw::wifiNets, plus the
// hotspots other simulated units run (sim/hw/WifiNet.h: they are heard in a scan and can be
// joined; each unit has its own address there, so the walkie-talkie works between them).
// Timing mirrors a real join: scan ~1.5 s, join ~2 s, wrong password fails at ~3 s.
#include "WiFi.h"
#include "hw/SimHw.h"
#include "hw/WifiNet.h"

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
uint32_t g_joinedApIp = 0;   // joined another unit's hotspot: its address (0 = a side-panel network)

// Another simulated unit's hotspot, as an access point entry.
std::vector<sim::WifiNet> hotspots() {
    std::vector<sim::WifiNet> out;
    for (auto& a : sim::wifinet::heardAps()) out.push_back({a.ssid, -45, !a.pass.empty(), a.pass});
    return out;
}

bool lookup(const std::string& ssid, sim::WifiNet& out) {
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        for (auto& n : sim::hw().wifiNets) if (n.ssid == ssid) { out = n; return true; }
    }
    for (auto& n : hotspots()) if (n.ssid == ssid) { out = n; return true; }
    return false;
}

uint32_t hotspotIp(const std::string& ssid) {
    for (auto& a : sim::wifinet::heardAps()) if (a.ssid == ssid) return a.ip;
    return 0;
}

// My address on the network just joined (host order).
void setJoinedIp() {
    const uint32_t ap = hotspotIp(g_ssid);
    g_joinedApIp = ap;
    const uint32_t ip = ap ? (ap & 0xFFFFFF00u) | sim::wifinet::hostId()
                           : (192u << 24 | 168u << 16 | 1u << 8 | sim::wifinet::hostId());
    sim::wifinet::setStationIp(ip);
}

void step() {   // caller holds g_mu
    const uint32_t now = sim::nowMs();
    if (g_scanRunning && (int32_t)(now - g_scanDone) >= 0) {
        g_scanRunning = false;
        {
            std::lock_guard<std::mutex> l(sim::hw().mu);
            g_results = sim::hw().wifiNets;
        }
        for (auto& n : hotspots()) g_results.push_back(n);
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
            setJoinedIp();
        }
    } else if (g_join == Join::Connected) {
        sim::WifiNet n;
        if (!lookup(g_ssid, n)) { g_join = Join::Idle; sim::wifinet::setStationIp(0); }   // AP gone
        else g_rssi = n.rssi + (int)(now / 1000 % 5) - 2;     // a little jitter
    }
}
}  // namespace

bool WiFiClass::mode(wifi_mode_t m) {
    std::lock_guard<std::mutex> l(g_mu);
    g_mode = m;
    if (m == WIFI_OFF || m == WIFI_AP) { g_join = Join::Idle; g_scanRunning = false; sim::wifinet::setStationIp(0); }
    if (m == WIFI_OFF || m == WIFI_STA) sim::wifinet::stopAp();
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
    sim::wifinet::setStationIp(0);
    if (wifiOff) { g_mode = WIFI_OFF; sim::wifinet::stopAp(); }
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
    if (g_join != Join::Connected) return IPAddress();
    const uint32_t ap = g_joinedApIp, h = sim::wifinet::hostId();
    return ap ? IPAddress(ap >> 24, ap >> 16 & 255, ap >> 8 & 255, h) : IPAddress(192, 168, 1, h);
}
IPAddress WiFiClass::softAPIP() {
    const uint32_t ip = sim::wifinet::apIp();
    return ip ? IPAddress(ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255) : IPAddress(192, 168, 4, 1);
}
String WiFiClass::SSID() { std::lock_guard<std::mutex> l(g_mu); return g_join == Join::Connected ? String(g_ssid.c_str()) : String(); }
int8_t WiFiClass::RSSI() { std::lock_guard<std::mutex> l(g_mu); step(); return g_join == Join::Connected ? (int8_t)g_rssi : 0; }
bool WiFiClass::softAP(const char* ssid, const char* pass, int, int, int) {
    std::lock_guard<std::mutex> l(g_mu);
    g_mode = g_mode == WIFI_STA ? WIFI_AP_STA : WIFI_AP;
    sim::wifinet::startAp(ssid ? ssid : "", pass ? pass : "");
    return true;
}
bool WiFiClass::softAPdisconnect(bool wifiOff) {
    std::lock_guard<std::mutex> l(g_mu);
    sim::wifinet::stopAp();
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
