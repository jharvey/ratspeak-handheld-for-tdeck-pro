#pragma once
// Bench event trace: a RAM ring of (millis, tag, value) marks that survives light sleep,
// for timelines the USB console cannot show (it drops while the CPU sleeps). Marks are
// a few instructions; tags must be string literals. Dump with the "ev" console command.

#include <Arduino.h>

namespace evtrace {

struct Event {
    uint32_t ms;
    const char* tag;
    int32_t value;
};

constexpr size_t Capacity = 256;

struct Ring {
    Event events[Capacity];
    uint32_t total = 0;      // marks ever made; the ring keeps the newest Capacity
};

inline Ring& ring() {
    static Ring r;
    return r;
}

inline void mark(const char* tag, int32_t value = 0) {
    Ring& r = ring();
    r.events[r.total % Capacity] = Event{(uint32_t)millis(), tag, value};
    ++r.total;
}

inline void clear() { ring().total = 0; }

}  // namespace evtrace
