#pragma once
// Simulated WiFi station: scans and joins the access points listed in sim::Hw.
// AP mode and TCP servers are inert (no clients ever connect).
#include "Arduino.h"
#include "WiFiClient.h"
#include "WiFiServer.h"

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
               WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_PSK } wifi_auth_mode_t;
typedef enum { WL_NO_SHIELD = 255, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2,
               WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5,
               WL_DISCONNECTED = 6 } wl_status_t;
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class WiFiScanClass {
public:
    int16_t scanNetworks(bool async = false, bool showHidden = false, bool passive = false,
                         uint32_t maxMsPerChan = 300, uint8_t channel = 0, const char* ssid = nullptr,
                         const uint8_t* bssid = nullptr);
    int16_t scanComplete();
    void scanDelete();
    String SSID(uint8_t i);
    int32_t RSSI(uint8_t i);
    wifi_auth_mode_t encryptionType(uint8_t i);
    int32_t channel(uint8_t) { return 6; }
protected:
    static uint32_t _scanTimeout;
};

class WiFiClass : public WiFiScanClass {
public:
    bool mode(wifi_mode_t m);
    wifi_mode_t getMode();
    void persistent(bool) {}
    bool setHostname(const char*) { return true; }
    const char* getHostname() { return "ratspeak-sim"; }
    bool setAutoReconnect(bool v) { autoReconnect_ = v; return true; }
    bool setSleep(bool) { return true; }
    bool setTxPower(int) { return true; }
    bool enableIpV6() { return true; }
    wl_status_t begin(const char* ssid, const char* pass = nullptr, int32_t channel = 0,
                      const uint8_t* bssid = nullptr, bool connect = true);
    bool disconnect(bool wifiOff = false, bool eraseAp = false);
    bool reconnect();
    wl_status_t status();
    bool isConnected() { return status() == WL_CONNECTED; }
    IPAddress localIP();
    IPAddress localIPv6() { return IPAddress(); }
    IPAddress gatewayIP() { return IPAddress(192, 168, 1, 1); }
    IPAddress subnetMask() { return IPAddress(255, 255, 255, 0); }
    IPAddress dnsIP(uint8_t = 0) { return IPAddress(192, 168, 1, 1); }
    String SSID();
    using WiFiScanClass::SSID;
    int8_t RSSI();
    using WiFiScanClass::RSSI;
    String macAddress() { return "24:0A:C4:00:51:4D"; }
    bool softAP(const char* ssid, const char* pass = nullptr, int channel = 1, int hidden = 0,
                int maxConn = 4);
    bool softAPdisconnect(bool wifiOff = false);
    IPAddress softAPIP() { return IPAddress(192, 168, 4, 1); }
    uint8_t softAPgetStationNum() { return 0; }
private:
    bool autoReconnect_ = true;
};

extern WiFiClass WiFi;
