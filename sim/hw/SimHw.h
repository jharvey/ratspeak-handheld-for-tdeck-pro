#pragma once
// Shared state between the simulated hardware (used by firmware code on the
// device thread) and the Qt front end (GUI thread). Everything is guarded by mu.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace sim {

struct KeyMatrixEvent { uint8_t row, col; bool pressed; };

struct WifiNet {
    std::string ssid;
    int rssi = -60;
    bool secure = true;
    std::string password;      // what the "access point" accepts
};

struct Hw {
    std::mutex mu;

    // ---- options
    std::string dataDir = "simdata/device1";   // LittleFS / SD / NVS live here
    bool realisticPanel = true;                // emulate e-ink refresh time
    std::string deviceName = "device1";

    // ---- display (1 bit per pixel, 1 = black, 240x320 row-major MSB first)
    std::vector<uint8_t> panel = std::vector<uint8_t>(240 * 320 / 8, 0);
    uint32_t panelVersion = 0;
    uint32_t lastRefreshMs = 0;
    bool lastRefreshFull = false;

    // ---- keyboard (TCA8418): matrix events waiting to enter the FIFO
    std::deque<KeyMatrixEvent> keyEvents;
    bool kbBacklight = false;

    // ---- touch (CST328)
    bool touchDown = false;
    int touchX = 0, touchY = 0;

    // ---- power (BQ27220 gauge + BQ25896 charger)
    int batteryMv = 3950;
    int batterySoc = 72;        // gauge-reported %
    int batteryMa = -85;        // avg current, + = charging
    bool usb = false;
    bool charging = false;
    bool chargeDone = false;
    int fccMah = 1400;

    // ---- GPS
    bool gpsFix = true;
    double lat = 43.6532, lon = -79.3832;
    int sats = 7;

    // ---- WiFi
    std::vector<WifiNet> wifiNets = {
        {"HomeNet", -48, true, "password123"},
        {"Coffee-Shop", -67, false, ""},
        {"Neighbour_5G", -79, true, "secret-pass"},
    };

    // ---- vibration / audio feedback for the UI
    uint32_t vibrateUntil = 0;
    uint32_t beepCount = 0;

    // ---- serial
    std::deque<char> serialIn;
    std::function<void(const std::string&)> serialOut;   // called with a line (no lock held)

    // Firmware-side flags
    bool quit = false;
    std::string powerInfo;      // one-line power state published by the firmware each second
};

Hw& hw();

// Host clock (ms since process start), shared with millis().
uint32_t nowMs();

}  // namespace sim
