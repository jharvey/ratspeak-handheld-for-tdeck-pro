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
    // Atomic slot claim: the storage worker task marks too, so two tasks never share a slot.
    const uint32_t slot = __atomic_fetch_add(&r.total, 1u, __ATOMIC_RELAXED);
    r.events[slot % Capacity] = Event{(uint32_t)millis(), tag, value};
}

// Like mark(), but a run of the same tag folds into one event: its time becomes the
// latest mark's and its value the sum. Keeps timer wakes (every few tens of ms while
// sleeping) from pushing everything else out of the ring. Main loop only.
inline void markRun(const char* tag, int32_t value = 0) {
    Ring& r = ring();
    const uint32_t total = __atomic_load_n(&r.total, __ATOMIC_RELAXED);
    if (total) {
        Event& last = r.events[(total - 1) % Capacity];
        if (last.tag == tag) { last.ms = (uint32_t)millis(); last.value += value; return; }
    }
    mark(tag, value);
}

inline void clear() { ring().total = 0; }

}  // namespace evtrace
