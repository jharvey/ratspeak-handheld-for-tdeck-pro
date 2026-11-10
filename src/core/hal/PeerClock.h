#pragma once
// Clock samples heard from other devices, for the T-Deck Pro check-in mode (devices that
// only listen a few minutes an hour must agree on the time to within a minute or so).
//
// Two sources, both free (no protocol change):
//   - Reticulum announces: random_hash = 5 random bytes || 5-byte big-endian wire time. A
//     sender whose clock is GPS/NTP synchronized puts its Unix time there (otherwise a small
//     logical counter, rejected by the plausibility checks of the consumer).
//   - LXMF messages: the sender's timestamp, set when the message is created.
// Writers run wherever frames are parsed; the app reads the newest sample with take().
#include <Arduino.h>
#include <stdint.h>

#include <atomic>

namespace peerclock {

enum Source : uint8_t { NONE = 0, ANNOUNCE = 1, MESSAGE = 2 };

struct Sample {
    uint32_t senderSecs = 0;   // sender's Unix time
    uint32_t atMs = 0;         // millis() when it was heard
    uint8_t source = NONE;
};

namespace detail {
inline std::atomic<uint32_t> seq{0};
inline volatile uint32_t secs = 0, at = 0;
inline volatile uint8_t src = NONE;
}

inline void note(uint32_t senderSecs, Source s) {
    if (senderSecs < 1700000000u) return;           // logical counter / unset clock
    detail::seq.fetch_add(1, std::memory_order_acq_rel);   // odd: writing
    detail::secs = senderSecs;
    detail::at = millis();
    detail::src = s;
    detail::seq.fetch_add(1, std::memory_order_acq_rel);   // even: stable
}

// A raw Reticulum packet as received from LoRa (RNode split header already removed).
// Only announces heard directly (hops == 0) count: a relayed one may be old.
inline void noteFrame(const uint8_t* p, size_t len) {
    if (!p || len < 2) return;
    const uint8_t flags = p[0];
    if ((flags & 0x03) != 0x01 || (flags & 0x80)) return;    // ANNOUNCE, no IFAC
    if (p[1] != 0) return;                                    // hops
    const size_t hdr = 2 + ((flags & 0x40) ? 16 : 0) + 16 + 1;   // [transport id] dest context
    const size_t off = hdr + 64 + 10 + 5;                     // pub, name_hash, rng half
    if (len < off + 5) return;
    uint64_t t = 0;
    for (int i = 0; i < 5; i++) t = (t << 8) | p[off + i];
    if (t < 0xFFFFFFFFull) note((uint32_t)t, ANNOUNCE);
}

// Newest sample if it changed since lastSeq (lastSeq is updated).
inline bool take(uint32_t& lastSeq, Sample& out) {
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t a = detail::seq.load(std::memory_order_acquire);
        if (a & 1) continue;
        if (a == lastSeq) return false;
        out.senderSecs = detail::secs;
        out.atMs = detail::at;
        out.source = detail::src;
        if (detail::seq.load(std::memory_order_acquire) == a) { lastSeq = a; return true; }
    }
    return false;
}

}  // namespace peerclock
