#pragma once
// Simulator: the few lwIP socket calls the walkie-talkie uses (UDP), on the simulated WiFi
// network (sim/hw/WifiNet.h) instead of the PC's real network. Only included by
// src/boards/tdeck_Pro/voice/Walkie.cpp.
#include <stddef.h>
#include <stdint.h>

#include "hw/WifiNet.h"

#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define INADDR_ANY 0u
#define MSG_DONTWAIT 0x40

typedef uint32_t socklen_t;
struct in_addr { uint32_t s_addr; };                                  // network order
struct sockaddr { uint16_t sa_family; char sa_data[14]; };
struct sockaddr_in { uint16_t sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };

inline uint16_t sim_bswap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
inline uint32_t sim_bswap32(uint32_t v) { return __builtin_bswap32(v); }
#define htons(v) sim_bswap16((uint16_t)(v))
#define ntohs(v) sim_bswap16((uint16_t)(v))
#define htonl(v) sim_bswap32((uint32_t)(v))
#define ntohl(v) sim_bswap32((uint32_t)(v))

inline int sim_socket(int, int, int) { return sim::wifinet::open(); }
inline int sim_bind(int s, const sockaddr* a, socklen_t) {
    const sockaddr_in* i = (const sockaddr_in*)a;
    return sim::wifinet::bind(s, ntohs(i->sin_port)) ? 0 : -1;
}
inline int sim_sendto(int s, const void* d, size_t n, int, const sockaddr* a, socklen_t) {
    const sockaddr_in* i = (const sockaddr_in*)a;
    return sim::wifinet::sendTo(s, d, n, ntohl(i->sin_addr.s_addr), ntohs(i->sin_port));
}
inline int sim_recvfrom(int s, void* b, size_t n, int, sockaddr* a, socklen_t* l) {
    uint32_t ip = 0;
    uint16_t port = 0;
    const int r = sim::wifinet::recvFrom(s, b, n, &ip, &port);
    if (r >= 0 && a) {
        sockaddr_in* i = (sockaddr_in*)a;
        i->sin_family = AF_INET;
        i->sin_port = htons(port);
        i->sin_addr.s_addr = htonl(ip);
        if (l) *l = sizeof(sockaddr_in);
    }
    return r;
}
inline int sim_close(int s) { sim::wifinet::close(s); return 0; }

#define socket sim_socket
#define bind sim_bind
#define sendto sim_sendto
#define recvfrom sim_recvfrom
#define close(s) sim_close(s)
