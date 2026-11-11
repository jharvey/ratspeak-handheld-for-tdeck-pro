// INA226 battery-current logger for the T-Deck Pro board floor (guide §6.6).
//
// Runs on a separate small ESP32 (ESP32-C3 SuperMini by default). The INA226 sits in the
// T-Deck battery's + lead: IN+ = battery side, IN- = board side, so discharge reads positive
// and charging (USB attached) reads negative. Current comes straight from the shunt-voltage
// register (2.5 uV/LSB -> 25 uA/LSB with 0.1 ohm), so no calibration register is needed.
//
// Every conversion is summed; once per report period one line goes out:
//   D,<ms>,<n>,<avg_uA>,<min_uA>,<max_uA>,<bus_mV>,<uAh>
//     ms     device time at the end of the period      n     conversions in the period
//     avg/min/max  current over the period (uA)        bus   mean bus voltage (VBUS pin)
//     uAh    charge since boot / "c" (discharge positive)
// Lines starting with '#' are comments / replies. tools/inalogger/log.py reads this.
//
// Console (115200, one command per line):
//   s               status                 c        clear charge + time
//   p <ms>          report period (5..60000, default 100)
//   avg <0..7>      INA averaging 1,4,16,64,128,256,512,1024 (default 0 = 1)
//   ct <0..7>       shunt conversion time 140,204,332,588,1100,2116,4156,8244 us (default 4)
//   bt <0..7>       bus conversion time (same table, default 0 = 140 us)
//   r <mohm>        shunt resistance in milliohm (default 100)
//   z [n]           zero: average n conversions (default 2000) with NO current flowing
//                   (battery unplugged, or IN+ shorted to IN-) and store the offset
//   z0              clear the stored offset
//   q / g           quiet (no D lines) / go
// Settings are kept in NVS ("inalog").
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

#ifndef INA_SDA
#define INA_SDA 8
#endif
#ifndef INA_SCL
#define INA_SCL 9
#endif

namespace {
constexpr uint8_t kRegConfig = 0x00, kRegShunt = 0x01, kRegBus = 0x02, kRegMask = 0x06,
                  kRegManuf = 0xFE, kRegDie = 0xFF;
constexpr uint16_t kManufTI = 0x5449, kDieINA226 = 0x2260;
constexpr uint16_t kMaskCvrf = 0x0008;
constexpr uint16_t kCtUs[8] = {140, 204, 332, 588, 1100, 2116, 4156, 8244};
constexpr uint16_t kAvgN[8] = {1, 4, 16, 64, 128, 256, 512, 1024};

struct Settings {
    uint32_t periodMs = 100;
    uint8_t avg = 0, shuntCt = 4, busCt = 0;
    uint32_t shuntMohm = 100;
    float offsetRaw = 0.0f;   // shunt LSBs subtracted from every reading
} g_set;

Preferences g_prefs;
uint8_t g_addr = 0;          // 0 = no INA226 found
bool g_quiet = false;
uint32_t g_nextProbe = 0;

// Period accumulators.
int64_t g_sumRaw = 0, g_sumBus = 0;
int16_t g_minRaw = INT16_MAX, g_maxRaw = INT16_MIN;
uint32_t g_n = 0, g_periodStartUs = 0, g_i2cErrors = 0, g_errRun = 0;
// Since boot / "c".
double g_uAh = 0.0;
uint32_t g_clearMs = 0;

// Zeroing in progress.
uint32_t g_zeroLeft = 0, g_zeroTotal = 0;
int64_t g_zeroSum = 0;

bool readReg(uint8_t reg, uint16_t& out) {
    Wire.beginTransmission(g_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)g_addr, 2) != 2) return false;
    out = (uint16_t)(Wire.read() << 8);
    out |= (uint16_t)Wire.read();
    return true;
}

bool writeReg(uint8_t reg, uint16_t v) {
    Wire.beginTransmission(g_addr);
    Wire.write(reg);
    Wire.write((uint8_t)(v >> 8));
    Wire.write((uint8_t)(v & 0xFF));
    return Wire.endTransmission() == 0;
}

float rawToUa(float raw) {
    // 2.5 uV per LSB; I[uA] = V[uV] / R[ohm] = raw * 2.5 * 1000 / R[mohm].
    return (raw - g_set.offsetRaw) * 2500.0f / (float)g_set.shuntMohm;
}

uint32_t conversionUs() {
    return (uint32_t)kAvgN[g_set.avg] * (kCtUs[g_set.shuntCt] + kCtUs[g_set.busCt]);
}

void resetPeriod() {
    g_sumRaw = 0;
    g_sumBus = 0;
    g_minRaw = INT16_MAX;
    g_maxRaw = INT16_MIN;
    g_n = 0;
    g_periodStartUs = micros();
}

bool applyConfig() {
    if (!g_addr) return false;
    // Mode 7: shunt + bus, continuous.
    uint16_t cfg = (uint16_t)((g_set.avg & 7) << 9 | (g_set.busCt & 7) << 6 | (g_set.shuntCt & 7) << 3 | 7);
    bool ok = writeReg(kRegConfig, cfg);
    resetPeriod();
    return ok;
}

void saveSettings() {
    g_prefs.putUInt("per", g_set.periodMs);
    g_prefs.putUChar("avg", g_set.avg);
    g_prefs.putUChar("sct", g_set.shuntCt);
    g_prefs.putUChar("bct", g_set.busCt);
    g_prefs.putUInt("mohm", g_set.shuntMohm);
    g_prefs.putFloat("off", g_set.offsetRaw);
}

void loadSettings() {
    g_set.periodMs = g_prefs.getUInt("per", g_set.periodMs);
    g_set.avg = g_prefs.getUChar("avg", g_set.avg) & 7;
    g_set.shuntCt = g_prefs.getUChar("sct", g_set.shuntCt) & 7;
    g_set.busCt = g_prefs.getUChar("bct", g_set.busCt) & 7;
    g_set.shuntMohm = g_prefs.getUInt("mohm", g_set.shuntMohm);
    g_set.offsetRaw = g_prefs.getFloat("off", g_set.offsetRaw);
    if (g_set.periodMs < 5 || g_set.periodMs > 60000) g_set.periodMs = 100;
    if (g_set.shuntMohm == 0) g_set.shuntMohm = 100;
}

bool probe() {
    for (uint8_t a = 0x40; a <= 0x4F; ++a) {
        g_addr = a;
        uint16_t m = 0, d = 0;
        if (readReg(kRegManuf, m) && readReg(kRegDie, d) && m == kManufTI && (d & 0xFFF0) == kDieINA226) {
            writeReg(kRegConfig, 0x8000);   // reset
            delay(2);
            applyConfig();
            Serial.printf("# INA226 at 0x%02X (die 0x%04X)\n", a, d);
            return true;
        }
    }
    g_addr = 0;
    return false;
}

void status() {
    Serial.printf("# inalogger v1 sda=%d scl=%d ina=0x%02X shunt_mohm=%lu offset_raw=%.3f (%.1f uA)\n", INA_SDA, INA_SCL,
                  g_addr, (unsigned long)g_set.shuntMohm, g_set.offsetRaw,
                  g_set.offsetRaw * 2500.0f / (float)g_set.shuntMohm);
    Serial.printf("# avg=%u (x%u) shunt_ct=%u (%u us) bus_ct=%u (%u us) conv=%lu us period_ms=%lu\n", g_set.avg,
                  kAvgN[g_set.avg], g_set.shuntCt, kCtUs[g_set.shuntCt], g_set.busCt, kCtUs[g_set.busCt],
                  (unsigned long)conversionUs(), (unsigned long)g_set.periodMs);
    Serial.printf("# lsb_uA=%.2f range_mA=+-%.1f charge_uAh=%.3f since_ms=%lu i2c_errors=%lu quiet=%d\n",
                  2500.0f / (float)g_set.shuntMohm, 81.92f * 1000.0f / (float)g_set.shuntMohm, g_uAh,
                  (unsigned long)(millis() - g_clearMs), (unsigned long)g_i2cErrors, (int)g_quiet);
    Serial.println("# fields: D,ms,n,avg_uA,min_uA,max_uA,bus_mV,uAh");
}

bool parseCode(const String& arg, uint8_t& out) {
    long v = arg.toInt();
    if (arg.length() == 0 || v < 0 || v > 7) {
        Serial.println("# ERR code 0..7");
        return false;
    }
    out = (uint8_t)v;
    return true;
}

void handle(String line) {
    line.trim();
    if (line.length() == 0) return;
    int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp);
    String arg = sp < 0 ? String() : line.substring(sp + 1);
    arg.trim();
    if (cmd == "s") {
        status();
    } else if (cmd == "c") {
        g_uAh = 0.0;
        g_clearMs = millis();
        resetPeriod();
        Serial.println("# OK cleared");
    } else if (cmd == "p") {
        long v = arg.toInt();
        if (v < 5 || v > 60000) {
            Serial.println("# ERR p 5..60000");
            return;
        }
        g_set.periodMs = (uint32_t)v;
        saveSettings();
        resetPeriod();
        Serial.printf("# OK period_ms=%lu\n", (unsigned long)g_set.periodMs);
    } else if (cmd == "avg" || cmd == "ct" || cmd == "bt") {
        uint8_t code;
        if (!parseCode(arg, code)) return;
        (cmd == "avg" ? g_set.avg : cmd == "ct" ? g_set.shuntCt : g_set.busCt) = code;
        saveSettings();
        applyConfig();
        Serial.printf("# OK conv=%lu us\n", (unsigned long)conversionUs());
    } else if (cmd == "r") {
        long v = arg.toInt();
        if (v <= 0) {
            Serial.println("# ERR r <milliohm>");
            return;
        }
        g_set.shuntMohm = (uint32_t)v;
        saveSettings();
        Serial.printf("# OK shunt_mohm=%lu\n", (unsigned long)g_set.shuntMohm);
    } else if (cmd == "z0") {
        g_set.offsetRaw = 0.0f;
        saveSettings();
        Serial.println("# OK offset cleared");
    } else if (cmd == "z") {
        long n = arg.length() ? arg.toInt() : 2000;
        if (n < 10) n = 10;
        g_zeroLeft = g_zeroTotal = (uint32_t)n;
        g_zeroSum = 0;
        Serial.printf("# zeroing over %ld conversions (~%lu ms) - no current must flow\n", n,
                      (unsigned long)((uint64_t)n * conversionUs() / 1000));
    } else if (cmd == "q") {
        g_quiet = true;
        Serial.println("# OK quiet");
    } else if (cmd == "g") {
        g_quiet = false;
        Serial.println("# OK go");
    } else {
        Serial.println("# ERR commands: s c p avg ct bt r z z0 q g");
    }
}

void pollConsole() {
    static String buf;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            handle(buf);
            buf = "";
        } else if (buf.length() < 64) {
            buf += ch;
        }
    }
}

void sample() {
    uint16_t mask = 0;
    if (!readReg(kRegMask, mask)) {
        ++g_i2cErrors;
        ++g_errRun;
        return;
    }
    g_errRun = 0;
    if (!(mask & kMaskCvrf)) return;
    uint16_t sh = 0, bus = 0;
    if (!readReg(kRegShunt, sh) || !readReg(kRegBus, bus)) {
        ++g_i2cErrors;
        ++g_errRun;
        return;
    }
    int16_t raw = (int16_t)sh;
    if (g_zeroLeft) {
        g_zeroSum += raw;
        if (--g_zeroLeft == 0) {
            g_set.offsetRaw = (float)((double)g_zeroSum / (double)g_zeroTotal);
            saveSettings();
            Serial.printf("# OK offset_raw=%.3f (%.1f uA)\n", g_set.offsetRaw,
                          g_set.offsetRaw * 2500.0f / (float)g_set.shuntMohm);
        }
    }
    g_sumRaw += raw;
    g_sumBus += bus;
    if (raw < g_minRaw) g_minRaw = raw;
    if (raw > g_maxRaw) g_maxRaw = raw;
    ++g_n;
}

void report() {
    uint32_t now = micros();
    uint32_t elapsedUs = now - g_periodStartUs;
    if (elapsedUs < g_set.periodMs * 1000UL) return;
    if (g_n == 0) {
        if (!g_quiet) Serial.printf("# no conversions in %lu ms\n", (unsigned long)(elapsedUs / 1000));
        resetPeriod();
        return;
    }
    float avgUa = rawToUa((float)((double)g_sumRaw / (double)g_n));
    // Charge over the whole elapsed time, so gaps between conversions do not lose charge.
    g_uAh += (double)avgUa * (double)elapsedUs / 3.6e9;
    if (!g_quiet) {
        float busMv = (float)((double)g_sumBus / (double)g_n) * 1.25f;
        Serial.printf("D,%lu,%lu,%.1f,%.1f,%.1f,%.1f,%.3f\n", (unsigned long)millis(), (unsigned long)g_n, avgUa,
                      rawToUa((float)g_minRaw), rawToUa((float)g_maxRaw), busMv, g_uAh);
    }
    resetPeriod();
}
}  // namespace

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_MODE
    Serial.setTxTimeoutMs(0);   // never block when no PC is reading
#endif
    delay(300);
    g_prefs.begin("inalog", false);
    loadSettings();
    Wire.begin(INA_SDA, INA_SCL, 400000);
    g_clearMs = millis();
    if (!probe()) Serial.println("# ERR no INA226 on I2C 0x40-0x4F");
    status();
}

void loop() {
    pollConsole();
    if (!g_addr) {
        if ((int32_t)(millis() - g_nextProbe) >= 0) {
            g_nextProbe = millis() + 2000;
            if (!probe()) Serial.println("# ERR no INA226 on I2C 0x40-0x4F");
        }
        return;
    }
    sample();
    report();
    if (g_errRun > 50) {
        Serial.println("# ERR INA226 lost, probing again");
        g_addr = 0;
        g_errRun = 0;
    }
}
