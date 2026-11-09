// T-Deck Pro GPS check: a tiny standalone app that tells whether a unit's MIA-M10Q GPS
// works. LoRa is kept powered off (safe without an antenna). Every 10 s it prints what
// the receiver sends and a one-line verdict on the USB console:
//   VERDICT: GPS FAULT (CANNOT USE BBR) ...   receiver refuses to navigate
//   VERDICT: GPS OK ...                       NMEA arriving, satellites heard / fix
//   VERDICT: NO DATA ...                      nothing from the receiver at all
// Flash it at 0x10000 (app only; works with any partition table, uses no filesystem).
#include <Arduino.h>

namespace {
constexpr int kGpsEn = 39, kGpsRx = 44, kGpsTx = 43, kLoraEn = 46;
HardwareSerial gps(1);

uint32_t bytes = 0, nmea = 0, badSum = 0, ubxFrames = 0;
bool bbrFault = false, sawBanner = false, rmcValid = false;
int fixQ = 0, fixSats = 0;
String module = "?", firmware = "?";

// GSV: satellites in view / heard (SNR > 0) / best SNR, per constellation, last cycle.
struct Sky { char talker[3]; int inView, heard, best, heardAcc, bestAcc; uint32_t at; };
Sky sky[6] = {};

void ubxSend(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t n) {
    uint8_t h[6] = {0xB5, 0x62, cls, id, (uint8_t)n, (uint8_t)(n >> 8)};
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += h[i]; b += a; }
    for (uint16_t i = 0; i < n; i++) { a += p[i]; b += a; }
    gps.write(h, 6);
    if (n) gps.write(p, n);
    const uint8_t ck[2] = {a, b};
    gps.write(ck, 2);
}

Sky* skyFor(const char* t) {
    for (auto& k : sky) if (k.talker[0] == t[0] && k.talker[1] == t[1]) return &k;
    for (auto& k : sky) if (!k.talker[0]) { k.talker[0] = t[0]; k.talker[1] = t[1]; return &k; }
    return nullptr;
}

void handleUbx(uint8_t cls, uint8_t id, const uint8_t* p, uint16_t n) {
    ubxFrames++;
    if (cls == 0x0A && id == 0x04 && n >= 40) {           // MON-VER
        char sw[31] = {0};
        memcpy(sw, p, 30);
        firmware = sw;
        for (uint16_t o = 40; o + 30 <= n; o += 30) {
            char ext[31] = {0};
            memcpy(ext, p + o, 30);
            if (!strncmp(ext, "MOD=", 4)) module = ext + 4;
        }
    }
}

void handleSentence(char* s) {
    char* star = strchr(s, '*');
    if (!star) { badSum++; return; }
    uint8_t x = 0;
    for (char* q = s + 1; q < star; q++) x ^= (uint8_t)*q;
    if (x != (uint8_t)strtoul(star + 1, nullptr, 16)) { badSum++; return; }
    *star = 0;
    nmea++;
    if (!strncmp(s + 3, "TXT", 3)) {
        sawBanner = true;
        Serial.printf("[GPS] %s\n", s);
        if (strstr(s, "CANNOT USE BBR")) bbrFault = true;
        return;
    }
    char* f[24] = {};
    int n = 0;
    f[n++] = s;
    for (char* q = s; *q && n < 24; q++) if (*q == ',') { *q = 0; f[n++] = q + 1; }
    if (!strncmp(f[0] + 3, "GSV", 3) && n >= 4) {
        Sky* k = skyFor(f[0] + 1);
        if (!k) return;
        if (atoi(f[2]) == 1) { k->heardAcc = 0; k->bestAcc = 0; }
        for (int i = 7; i < n; i += 4) {
            const int snr = f[i][0] ? atoi(f[i]) : 0;
            if (snr > 0) { k->heardAcc++; if (snr > k->bestAcc) k->bestAcc = snr; }
        }
        if (atoi(f[2]) >= atoi(f[1])) {
            k->inView = atoi(f[3]); k->heard = k->heardAcc; k->best = k->bestAcc; k->at = millis();
        }
    } else if (!strncmp(f[0] + 3, "GGA", 3) && n > 7) {
        fixQ = f[6][0] ? atoi(f[6]) : 0;
        fixSats = f[7][0] ? atoi(f[7]) : 0;
    } else if (!strncmp(f[0] + 3, "RMC", 3) && n > 2) {
        rmcValid = f[2][0] == 'A';
    }
}

void pollGps() {
    static uint8_t st = 0, cls = 0, id = 0, ub[260];
    static uint16_t len = 0, pos = 0;
    static char line[120];
    static size_t ll = 0;
    while (gps.available()) {
        const uint8_t u = (uint8_t)gps.read();
        bytes++;
        if (st == 0 && u == 0xB5) { st = 1; continue; }
        if (st == 1) { if (u == 0x62) { st = 2; continue; } st = 0; }
        if (st >= 2) {
            switch (st) {
                case 2: cls = u; st = 3; break;
                case 3: id = u; st = 4; break;
                case 4: len = u; st = 5; break;
                case 5: len |= (uint16_t)(u << 8); pos = 0; st = 6; break;
                default:
                    if (pos < sizeof(ub)) ub[pos] = u;
                    if (++pos >= len + 2) { if (len + 2 <= (int)sizeof(ub)) handleUbx(cls, id, ub, len); st = 0; }
            }
            continue;
        }
        if (u == '\r') continue;
        if (u == '\n') { line[ll] = 0; if (ll > 6 && line[0] == '$') handleSentence(line); ll = 0; }
        else if (ll + 1 < sizeof(line)) line[ll++] = (char)u;
        else ll = 0;
    }
}

void report() {
    int inView = 0, heard = 0, best = 0;
    for (auto& k : sky)
        if (k.talker[0] && millis() - k.at < 5000) { inView += k.inView; heard += k.heard; if (k.best > best) best = k.best; }
    const uint64_t mac = ESP.getEfuseMac();
    Serial.printf("[CHECK] t=%lus  MAC=%02X:%02X:%02X:%02X:%02X:%02X  module=%s fw=%s\n",
                  (unsigned long)(millis() / 1000), (int)(mac & 0xFF), (int)(mac >> 8 & 0xFF), (int)(mac >> 16 & 0xFF),
                  (int)(mac >> 24 & 0xFF), (int)(mac >> 32 & 0xFF), (int)(mac >> 40 & 0xFF), module.c_str(), firmware.c_str());
    Serial.printf("[CHECK] bytes=%lu nmea=%lu badsum=%lu ubx=%lu  sky: %d in view, %d heard, best %d dB-Hz  fix=%d sats=%d\n",
                  (unsigned long)bytes, (unsigned long)nmea, (unsigned long)badSum, (unsigned long)ubxFrames, inView,
                  heard, best, fixQ, fixSats);
    if (bbrFault)
        Serial.println("VERDICT: GPS FAULT (CANNOT USE BBR) - receiver answers but refuses to navigate");
    else if (fixQ > 0 || rmcValid)
        Serial.printf("VERDICT: GPS OK - position fix with %d satellites\n", fixSats);
    else if (nmea > 0)
        Serial.printf("VERDICT: GPS OK - receiving NMEA, %d satellites heard (best %d dB-Hz), no fix yet\n", heard, best);
    else
        Serial.println("VERDICT: NO DATA - nothing from the receiver");
}
}  // namespace

void setup() {
    Serial.begin(115200);
    pinMode(kLoraEn, OUTPUT);
    digitalWrite(kLoraEn, LOW);                  // LoRa unpowered: nothing can transmit
    pinMode(kGpsEn, OUTPUT);
    digitalWrite(kGpsEn, LOW);                   // power-cycle the receiver for a fresh banner
    delay(1500);
    gps.setRxBufferSize(4096);
    gps.begin(38400, SERIAL_8N1, kGpsRx, kGpsTx);
    digitalWrite(kGpsEn, HIGH);
}

void loop() {
    pollGps();
    static uint32_t verAt = 3000, nextReport = 10000;
    if (verAt && millis() >= verAt) { verAt = 0; ubxSend(0x0A, 0x04, nullptr, 0); }   // MON-VER poll
    if (millis() >= nextReport) { nextReport += 10000; report(); }
    delay(2);
}
