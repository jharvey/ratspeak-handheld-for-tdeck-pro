// ATAK / TAK feed: see CotFeed.h.
#include "CotFeed.h"

#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace cot {
namespace {

int g_tx = -1;   // send socket (opened on first use)
int g_rx = -1;   // listen socket (bench)

void iso(uint32_t t, char* out, size_t n) {
    const time_t tt = (time_t)t;
    struct tm g;
    const struct tm* p = gmtime(&tt);
    if (!p) { snprintf(out, n, "1970-01-01T00:00:00Z"); return; }
    g = *p;
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
             g.tm_hour, g.tm_min, g.tm_sec);
}

// XML attribute / text escape into out (always zero-terminated).
void esc(const char* s, char* out, size_t n) {
    size_t o = 0;
    for (; s && *s && o + 7 < n; s++) {
        const char* r = nullptr;
        switch (*s) {
            case '&': r = "&amp;"; break;
            case '<': r = "&lt;"; break;
            case '>': r = "&gt;"; break;
            case '"': r = "&quot;"; break;
            case '\'': r = "&apos;"; break;
            default: break;
        }
        if (r) { const size_t l = strlen(r); memcpy(out + o, r, l); o += l; }
        else if ((unsigned char)*s >= 32) out[o++] = *s;
    }
    out[o] = 0;
}

}  // namespace

size_t format(const Item& it, char* out, size_t cap) {
    char t[24], st[24], cs[64], rm[96], uid[64];
    iso(it.at, t, sizeof(t));
    iso(it.stale, st, sizeof(st));
    esc(it.callsign, cs, sizeof(cs));
    esc(it.uid, uid, sizeof(uid));
    rm[0] = 0;
    if (it.remarks) esc(it.remarks, rm, sizeof(rm));
    char bat[32] = "", col[32] = "", rem[128] = "";
    if (it.battery >= 0) snprintf(bat, sizeof(bat), "<status battery=\"%d\"/>", it.battery);
    if (it.argb) snprintf(col, sizeof(col), "<color argb=\"%ld\"/>", (long)it.argb);
    if (rm[0]) snprintf(rem, sizeof(rem), "<remarks>%s</remarks>", rm);
    const bool unit = !strncmp(it.type, "a-", 2);
    const int n = snprintf(out, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<event version=\"2.0\" uid=\"%s\" type=\"%s\" how=\"%s\" time=\"%s\" start=\"%s\" stale=\"%s\">"
        "<point lat=\"%.6f\" lon=\"%.6f\" hae=\"9999999.0\" ce=\"%s\" le=\"9999999.0\"/>"
        "<detail><contact callsign=\"%s\"/>%s%s%s%s<precisionlocation geopointsrc=\"%s\"/></detail></event>",
        uid, it.type, unit ? "m-g" : "h-g-i-g-o", t, t, st, it.lat, it.lon, unit ? "15.0" : "9999999.0", cs,
        unit ? "<__group name=\"Cyan\" role=\"Team Member\"/>" : "", bat, col, rem, unit ? "GPS" : "USER");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

bool send(const char* data, size_t n, uint32_t ip, uint16_t port) {
    if (g_tx < 0) g_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_tx < 0) return false;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return sendto(g_tx, data, n, 0, (const sockaddr*)&a, sizeof(a)) == (int)n;
}

bool listen(uint16_t port, uint32_t group) {
    if (g_rx >= 0) { close(g_rx); g_rx = -1; }
    if (!port) return true;
    g_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_rx < 0) return false;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_rx, (sockaddr*)&a, sizeof(a)) < 0) { close(g_rx); g_rx = -1; return false; }
#ifdef IP_ADD_MEMBERSHIP
    // Also take ATAK's SA multicast group (the simulator's WiFi has no multicast: unicast only).
    if (group) {
        struct ip_mreq m;
        memset(&m, 0, sizeof(m));
        m.imr_multiaddr.s_addr = htonl(group);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(g_rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));
    }
#else
    (void)group;
#endif
    return true;
}

int poll(char* buf, size_t cap, uint32_t* fromIp) {
    if (g_rx < 0 || cap < 2) return -1;
    sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int r = recvfrom(g_rx, buf, cap - 1, MSG_DONTWAIT, (sockaddr*)&from, &fl);
    if (r < 0) return -1;
    buf[r] = 0;
    if (fromIp) *fromIp = ntohl(from.sin_addr.s_addr);
    return r;
}

void stop() {
    if (g_tx >= 0) { close(g_tx); g_tx = -1; }
    listen(0);
}

}  // namespace cot
