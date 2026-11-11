#pragma once
// Energy monitor: where the battery charge goes, hour by hour.
//
// The board has one current sensor (the BQ27220 gauge, total only), so the split between
// parts is an estimate: time each part spends in each state x its current (measured on the
// bench 2026-11-09/10), plus per-event costs (LoRa TX airtime, e-ink refresh time). The
// gauge's average current, integrated over the same time, is the measured total; "Other" is
// measured minus estimated, i.e. how good the estimate is.
//
// Records close every period (default 1 h) into a 48-entry ring in RTC memory (survives
// resets, not a power loss). No hardware access here; shared with the desktop simulator.
#include <stdint.h>

namespace energy {

enum Cat : uint8_t { E_CPU, E_BOARD, E_LORA_RX, E_LORA_TX, E_GPS, E_WIFI, E_DISPLAY, E_KEYS_TOUCH, E_AUDIO, E_SD, CAT_COUNT };
const char* catName(uint8_t cat);

// What is powered right now (sampled whenever time is accounted).
struct State {
    bool     awake;      // CPU running (else light sleep)
    uint16_t cpuMhz;
    bool     loraRx;     // receiver listening
    uint8_t  loraDcPct;  // RX duty cycle: % of time in a listen window (0 = continuous)
    bool     gps;        // receiver powered
    bool     wifi;       // radio on
    bool     keyLight;
    bool     touch;      // touch controller scanning (not held in reset)
    bool     audio;      // DAC / amplifier path powered
    bool     sd;         // card mounted
};

struct Record {
    uint32_t epoch;          // wall clock at the end of the period (0 = unknown)
    uint32_t lenS;           // period length
    uint32_t measuredS;      // seconds covered by a discharge reading (on battery)
    uint16_t measured10;     // measured mAh x10 over measuredS
    uint16_t est10[CAT_COUNT];   // estimated mAh x10 per category
    uint8_t  awakePct;
    uint8_t  battPct;
    uint8_t  pad[2];
};

void begin();                                   // keeps an RTC ring that survived a reset
void account(uint32_t dtMs, const State& s);    // time spent in state s
void addEvent(uint8_t cat, float mAs);          // one-off cost (mA x seconds)
void addMeasured(int16_t avgMa, uint32_t dtMs); // gauge average over dt, discharging only
void tick(uint32_t nowMs, uint32_t epoch, int battPct);   // closes the period when due

void setPeriod(uint32_t seconds);               // 60 .. 86400 (testing: shorter)
uint32_t period();
int count();                                    // closed records available
const Record& closed(int i);                    // 0 = newest closed
Record current(uint32_t nowMs, int battPct);    // the period in progress
void clear();
void dump();                                    // console

// Estimated current of a state, mA (also used for the live "now" figure).
float stateMa(const State& s);

}  // namespace energy
