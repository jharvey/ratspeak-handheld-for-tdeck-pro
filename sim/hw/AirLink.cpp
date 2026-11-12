#include "hw/AirLink.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#define closesocket close
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#endif

namespace sim::air {
namespace {
constexpr const char* kGroup = "239.255.42.99";
constexpr uint16_t kDefaultPort = 47999;
// RS_SIM_AIR_PORT picks another "channel" (UDP port), so separate test pairs on one PC
// (e.g. a stress runner beside an interactive pair) do not hear each other.
uint16_t airPort() {
    static const uint16_t port = [] {
        const char* v = getenv("RS_SIM_AIR_PORT");
        const long n = v ? strtol(v, nullptr, 10) : 0;
        return n > 1024 && n < 65536 ? uint16_t(n) : kDefaultPort;
    }();
    return port;
}
char g_address[40] = "";
constexpr uint32_t kMagic = 0x4D495352;   // "RSIM"

#pragma pack(push, 1)
struct Header { uint32_t magic, sender, freq, bw; uint8_t sf, cr; int8_t txp; uint8_t len; };
#pragma pack(pop)

std::atomic<bool> g_running{false};
SOCKET g_sock = INVALID_SOCKET;
uint32_t g_self = 0;
}  // namespace

const char* address() {
    if (!g_address[0]) snprintf(g_address, sizeof(g_address), "udp://%s:%u", kGroup, (unsigned)airPort());
    return g_address;
}
uint32_t selfId() { return g_self; }

bool start(std::function<void(Frame&&)> onFrame) {
    if (g_running) return true;
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(airPort()); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(g_sock); g_sock = INVALID_SOCKET; return false; }
    ip_mreq m{};
    inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof(m));
    unsigned char loop = 1, ttl = 1;
    setsockopt(g_sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    setsockopt(g_sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    g_self = std::random_device{}();
    g_running = true;
    std::thread([onFrame] {
        uint8_t buf[700];
        while (g_running) {
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            const int n = recvfrom(g_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n < (int)sizeof(Header)) continue;
            Header h;
            memcpy(&h, buf, sizeof(h));
            if (h.magic != kMagic || h.sender == g_self || n != (int)(sizeof(Header) + h.len)) continue;
            Frame f;
            f.sender = h.sender; f.freq = h.freq; f.bw = h.bw; f.sf = h.sf; f.cr = h.cr; f.txp = h.txp;
            f.payload.assign(buf + sizeof(Header), buf + n);
            onFrame(std::move(f));
        }
    }).detach();
    return true;
}

void send(const Frame& f) {
    if (g_sock == INVALID_SOCKET || f.payload.size() > 255) return;
    std::vector<uint8_t> buf(sizeof(Header) + f.payload.size());
    Header h{kMagic, g_self, f.freq, f.bw, f.sf, f.cr, f.txp, (uint8_t)f.payload.size()};
    memcpy(buf.data(), &h, sizeof(h));
    memcpy(buf.data() + sizeof(h), f.payload.data(), f.payload.size());
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(airPort());
    inet_pton(AF_INET, kGroup, &a.sin_addr);
    sendto(g_sock, (const char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&a, sizeof(a));
}

}  // namespace sim::air
