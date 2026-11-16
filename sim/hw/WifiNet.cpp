// Simulated WiFi network. See WifiNet.h.
#include "hw/WifiNet.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket ::close
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#endif

namespace sim::wifinet {
namespace {
constexpr const char* kGroup = "239.255.42.98";
constexpr uint32_t kMagicData = 0x49465752;     // "RWFI"
constexpr uint32_t kMagicBeacon = 0x4E434252;   // "RBCN"

uint16_t netPort() {
    static const uint16_t port = [] {
        const char* v = getenv("RS_SIM_AIR_PORT");
        const long n = v ? strtol(v, nullptr, 10) : 0;
        return uint16_t((n > 1024 && n < 65535 ? n : 47999) + 1);
    }();
    return port;
}

#pragma pack(push, 1)
struct DataHdr { uint32_t magic, srcIp, dstIp; uint16_t srcPort, dstPort, len; };
struct BeaconPkt { uint32_t magic, ip; char ssid[33], pass[65]; };
#pragma pack(pop)

struct Datagram { uint32_t ip; uint16_t port; std::vector<uint8_t> data; };
struct Sock { uint16_t port = 0; std::deque<Datagram> rx; };
struct HeardAp { Ap ap; int64_t at; };

std::mutex g_mu;
std::once_flag g_once;
SOCKET g_sock = INVALID_SOCKET;
uint8_t g_host = 2;
uint32_t g_staIp = 0, g_apIp = 0;
Ap g_ap;
std::map<int, Sock> g_socks;
int g_nextSock = 1;
std::map<std::string, HeardAp> g_heard;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sendRaw(const void* p, size_t n) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(netPort());
    inet_pton(AF_INET, kGroup, &a.sin_addr);
    sendto(g_sock, (const char*)p, (int)n, 0, (sockaddr*)&a, sizeof(a));
}

void start() {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    std::random_device rd;
    g_host = (uint8_t)(2 + rd() % 200);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return;
    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(netPort()); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(g_sock, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(g_sock); g_sock = INVALID_SOCKET; return; }
    ip_mreq m{};
    inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof(m));
    unsigned char loop = 1, ttl = 1;
    setsockopt(g_sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    setsockopt(g_sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    std::thread([] {                                   // receive: data for my addresses, beacons
        std::vector<uint8_t> buf(2048);
        for (;;) {
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            const int n = recvfrom(g_sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
            if (n < 8) continue;
            uint32_t magic;
            memcpy(&magic, buf.data(), 4);
            std::lock_guard<std::mutex> l(g_mu);
            if (magic == kMagicBeacon && n == (int)sizeof(BeaconPkt)) {
                BeaconPkt b;
                memcpy(&b, buf.data(), sizeof(b));
                b.ssid[32] = 0; b.pass[64] = 0;
                if (b.ip == g_apIp) continue;          // my own
                g_heard[b.ssid] = {Ap{b.ssid, b.pass, b.ip}, nowMs()};
            } else if (magic == kMagicData && n >= (int)sizeof(DataHdr)) {
                DataHdr h;
                memcpy(&h, buf.data(), sizeof(h));
                if (n != (int)(sizeof(h) + h.len)) continue;
                if (!h.dstIp || (h.dstIp != g_staIp && h.dstIp != g_apIp)) continue;
                for (auto& kv : g_socks) {
                    if (kv.second.port != h.dstPort) continue;
                    if (kv.second.rx.size() > 400) kv.second.rx.pop_front();
                    kv.second.rx.push_back({h.srcIp, h.srcPort,
                                            std::vector<uint8_t>(buf.data() + sizeof(h), buf.data() + n)});
                }
            }
        }
    }).detach();
    std::thread([] {                                   // a hotspot's beacon, every second
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            BeaconPkt b{};
            {
                std::lock_guard<std::mutex> l(g_mu);
                if (!g_apIp) continue;
                b.magic = kMagicBeacon;
                b.ip = g_apIp;
                strncpy(b.ssid, g_ap.ssid.c_str(), 32);
                strncpy(b.pass, g_ap.pass.c_str(), 64);
            }
            sendRaw(&b, sizeof(b));
        }
    }).detach();
}

void ensure() { std::call_once(g_once, start); }
}  // namespace

uint8_t hostId() { ensure(); return g_host; }

void setStationIp(uint32_t ip) { ensure(); std::lock_guard<std::mutex> l(g_mu); g_staIp = ip; }

void startAp(const std::string& ssid, const std::string& pass) {
    ensure();
    std::random_device rd;
    std::lock_guard<std::mutex> l(g_mu);
    g_ap = Ap{ssid, pass, 0};
    g_apIp = (10u << 24) | (42u << 16) | ((1 + rd() % 250) << 8) | 1u;
    g_ap.ip = g_apIp;
}

void stopAp() { std::lock_guard<std::mutex> l(g_mu); g_apIp = 0; }

uint32_t apIp() { std::lock_guard<std::mutex> l(g_mu); return g_apIp; }

std::vector<Ap> heardAps() {
    ensure();
    std::vector<Ap> out;
    std::lock_guard<std::mutex> l(g_mu);
    const int64_t now = nowMs();
    for (auto& kv : g_heard)
        if (now - kv.second.at < 3000) out.push_back(kv.second.ap);
    return out;
}

int open() {
    ensure();
    std::lock_guard<std::mutex> l(g_mu);
    const int s = g_nextSock++;
    g_socks[s] = Sock{};
    return s;
}

bool bind(int s, uint16_t port) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) return false;
    it->second.port = port;
    return true;
}

int sendTo(int s, const void* data, size_t n, uint32_t ip, uint16_t port) {
    DataHdr h{};
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_socks.find(s);
        if (it == g_socks.end() || n > 1400) return -1;
        // from my address on the network the destination is on (hotspot or station)
        const bool viaAp = g_apIp && (ip >> 8) == (g_apIp >> 8);
        h.srcIp = viaAp ? g_apIp : g_staIp;
        if (!h.srcIp) return -1;                       // not on any network
        h.magic = kMagicData;
        h.dstIp = ip;
        h.srcPort = it->second.port ? it->second.port : 40000;
        h.dstPort = port;
        h.len = (uint16_t)n;
    }
    std::vector<uint8_t> pkt(sizeof(h) + n);
    memcpy(pkt.data(), &h, sizeof(h));
    memcpy(pkt.data() + sizeof(h), data, n);
    sendRaw(pkt.data(), pkt.size());
    return (int)n;
}

int recvFrom(int s, void* buf, size_t cap, uint32_t* ip, uint16_t* port) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end() || it->second.rx.empty()) return -1;
    Datagram d = std::move(it->second.rx.front());
    it->second.rx.pop_front();
    const size_t n = d.data.size() < cap ? d.data.size() : cap;
    memcpy(buf, d.data.data(), n);
    if (ip) *ip = d.ip;
    if (port) *port = d.port;
    return (int)n;
}

void close(int s) { std::lock_guard<std::mutex> l(g_mu); g_socks.erase(s); }

}  // namespace sim::wifinet
