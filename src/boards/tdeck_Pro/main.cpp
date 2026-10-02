#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <vector>
#include <LittleFS.h>
#include <driver/i2s.h>

#include "config/BoardConfig.h"
#include "hal/Keyboard.h"
#include "hal/EinkDisplay.h"
#include "audio/AudioNotify.h"

#include "protocol/ProtocolRuntime.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"
#include "storage/FlashStore.h"
#include "storage/MessageStore.h"
#include "radio/BoardRadio.h"
#include "transport/LoRaInterface.h"

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// =============================================================================
// Display node host — closer to product boot order:
//   flash.begin → identityMgr.begin → messageStore.begin → protocolRuntime.begin
//   loop: protocolRuntime.loop + pollReceive + messageStore.poll + radio
// =============================================================================

static Keyboard keyboard;
static FlashStore flash;
static IdentityManager identityMgr;
static MessageStore messageStore;
static AnnounceManager announceMgr;   // stack object; bridge for validated announces
static ProtocolRuntime protocolRuntime;
static bool protocolReady = false;
static bool messageStoreReady = false;

static BoardRadio* boardRadio = nullptr;
static LoRaInterface* loraIface = nullptr;
static AudioNotify audio;

static char g_lastEvent[48] = "boot";
static volatile bool g_needHomeRedraw = false;

static void setLastEvent(const char* msg) {
    if (!msg) msg = "";
    snprintf(g_lastEvent, sizeof(g_lastEvent), "%s", msg);
    g_needHomeRedraw = true;
}

static void heartbeatLed(bool on) {
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    digitalWrite(BOARD_KEYBOARD_LED, on ? HIGH : LOW);
}

static void powerGatesOn() {
    pinMode(BOARD_GPS_EN, OUTPUT);
    digitalWrite(BOARD_GPS_EN, HIGH);
    pinMode(BOARD_1V8_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_LORA_EN, HIGH);
    pinMode(BOARD_6609_EN, OUTPUT);
    digitalWrite(BOARD_6609_EN, HIGH);
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    digitalWrite(BOARD_MOTOR_PIN, LOW);
}

static bool readBattery(float& voltage, int& percent) {
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x08);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    uint16_t mv = Wire.read() | (Wire.read() << 8);
    voltage = mv / 1000.0f;

    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x2C);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    percent = Wire.read() | (Wire.read() << 8);
    if (percent > 100) percent = 100;
    return true;
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parseDestHash(const String& in, uint8_t out[16]) {
    char hex[33];
    size_t n = 0;
    for (size_t i = 0; i < in.length() && n < 32; i++) {
        char c = in[i];
        if (c == ':' || c == ' ' || c == '-') continue;
        if (hexNibble(c) < 0) return false;
        hex[n++] = (char)tolower(c);
    }
    if (n != 32) return false;
    for (int i = 0; i < 16; i++) {
        int hi = hexNibble(hex[i * 2]);
        int lo = hexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static const char* rejectionName(handheld::outgoing::Rejection r) {
    switch (r) {
        case handheld::outgoing::Rejection::None:        return "None";
        case handheld::outgoing::Rejection::Busy:        return "Busy";
        case handheld::outgoing::Rejection::Invalid:     return "Invalid";
        case handheld::outgoing::Rejection::TooLarge:    return "TooLarge";
        case handheld::outgoing::Rejection::NoMemory:    return "NoMemory";
        case handheld::outgoing::Rejection::Unavailable: return "Unavailable";
        case handheld::outgoing::Rejection::Fenced:      return "Fenced";
        case handheld::outgoing::Rejection::Exhausted:   return "Exhausted";
        case handheld::outgoing::Rejection::Recovering:  return "Recovering";
        case handheld::outgoing::Rejection::Stopped:     return "Stopped";
        default:                                         return "Unknown";
    }
}

static void printHashHexString(const char* label, const String& hex) {
    String clean;
    clean.reserve(32);
    for (size_t i = 0; i < hex.length(); i++) {
        char c = hex[i];
        if (c == ':' || c == ' ' || c == '-') continue;
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        clean += c;
    }
    Serial.print(label);
    Serial.println(clean);
}

static void dumpKnownDestsFile() {
    // FlashStore mounts at /littlefs; also try root for older layouts
    const char* paths[] = {
        "/littlefs/transport/known_dests.bin",
        "/transport/known_dests.bin",
    };
    File f;
    const char* used = nullptr;
    for (const char* p : paths) {
        f = LittleFS.open(p, "r");
        if (f) {
            used = p;
            break;
        }
    }
    if (!f) {
        Serial.println("[DIAG] known_dests.bin not found");
        return;
    }
    size_t n = f.size();
    Serial.printf("[DIAG] known_dests (%s) size=%u\r\n", used, (unsigned)n);
    std::vector<uint8_t> buf(n);
    size_t got = f.read(buf.data(), n);
    f.close();
    if (got != n) {
        Serial.println("[DIAG] short read");
        return;
    }

    String local;
    if (protocolReady) {
        local = protocolRuntime.destinationHashHex();
        String tmp;
        for (size_t i = 0; i < local.length(); i++) {
            char c = local[i];
            if (c == ':' || c == ' ') continue;
            if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
            tmp += c;
        }
        local = tmp;
    }

    Serial.println("[DIAG] candidate dest hashes (16-byte windows — may include header noise):");
    int shown = 0;
    for (size_t off = 0; off + 16 <= n; off += 16) {
        bool zero = true;
        for (int i = 0; i < 16; i++) {
            if (buf[off + i]) {
                zero = false;
                break;
            }
        }
        if (zero) continue;
        char hex[33];
        for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", buf[off + i]);
        hex[32] = 0;
        bool isLocal = (local.length() == 32 && strcmp(hex, local.c_str()) == 0);
        Serial.printf("  [%u] %s%s\r\n", (unsigned)off, hex,
                      isLocal ? "  <-- LOCAL" : "");
        if (++shown >= 20) break;
    }
    if (!shown) Serial.println("  (none non-zero)");
}

static void cmdDiag() {
    Serial.println("======== DIAG ========");
    Serial.printf("FW      : %s\r\n", FIRMWARE_VERSION);
    Serial.printf("Uptime  : %lu s\r\n", millis() / 1000UL);
    Serial.printf("Heap    : %u\r\n", ESP.getFreeHeap());
    Serial.printf("Flash   : %s\r\n", flash.isReady() ? "ready" : "NOT ready");
    Serial.printf("MsgStore: %s  deferredIO=%s\r\n",
                  messageStoreReady ? "ready" : "NOT ready",
                  messageStore.deferredIO() ? "yes" : "no");
    Serial.printf("Protocol: %s\r\n", protocolReady ? "ready" : "NOT ready");
    Serial.printf("LoRa    : %s\r\n",
                  (loraIface && loraIface->isOnline()) ? "online" : "offline");

    if (protocolReady) {
        printHashHexString("LOCAL_DEST  ", protocolRuntime.destinationHashHex());
        printHashHexString("LOCAL_IDENT ", protocolRuntime.identityHashHex());
        Serial.printf("Paths    : %u\r\n", (unsigned)protocolRuntime.pathCount());
        Serial.printf("Links    : %u\r\n", (unsigned)protocolRuntime.linkCount());
        Serial.printf("LXMF Q   : %d\r\n", protocolRuntime.lxmfQueuedCount());
    }
    if (loraIface) {
        Serial.printf("LastRSSI : %d\r\n", loraIface->lastRxRssi());
        Serial.printf("LastSNR  : %.1f\r\n", loraIface->lastRxSnr());
    }
    Serial.printf("Event   : %s\r\n", g_lastEvent);
    dumpKnownDestsFile();
    Serial.println("======== END DIAG ========");
    Serial.println("LOCAL_DEST = your address (share with Plus as contact).");
    Serial.println("If LXMF Q stays 1 forever, recovery has not finished.");
}

static void cmdWhoami() {
    if (!protocolReady) {
        Serial.println("Protocol not ready");
        return;
    }
    printHashHexString("LOCAL_DEST  ", protocolRuntime.destinationHashHex());
    printHashHexString("LOCAL_IDENT ", protocolRuntime.identityHashHex());
}

static bool initRadio() {
    Serial.println("[RADIO] Starting SX1262...");
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(20);
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);

    boardRadio = new BoardRadio(
        &SPI, LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
        LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
        LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);

    if (!boardRadio->begin(LORA_DEFAULT_FREQ)) {
        Serial.println("[RADIO] SX1262 begin failed");
        return false;
    }
    boardRadio->setSpreadingFactor(LORA_DEFAULT_SF);
    boardRadio->setSignalBandwidth(LORA_DEFAULT_BW);
    boardRadio->setCodingRate4(LORA_DEFAULT_CR);
    boardRadio->setTxPower(LORA_DEFAULT_TX_POWER);
    boardRadio->setPreambleLength(LORA_DEFAULT_PREAMBLE);
    boardRadio->enableCrc();

    loraIface = new LoRaInterface(boardRadio, "LoRa");
    if (!loraIface->start()) {
        Serial.println("[RADIO] LoRaInterface start failed");
        return false;
    }
    Serial.println("[RADIO] SX1262 + LoRaInterface online");
    return true;
}

static void onLxmfMessage(const LXMFManager::CommittedMessage& msg) {
    (void)msg;
    Serial.println("[LXMF] inbound message committed");
    setLastEvent("LXMF in");
}

static void initProtocol() {
    Serial.println("[PROTO] Initializing (product-like host order)...");

    // 1) Flash
    if (!flash.begin()) {
        Serial.println("[PROTO] FlashStore begin failed");
        return;
    }

    // 2) Identity
    if (!identityMgr.begin(&flash, nullptr)) {
        Serial.println("[PROTO] IdentityManager begin failed");
        return;
    }

    // 3) Message store (required before ProtocolRuntime — recovery uses it)
    if (!messageStore.begin(&flash, nullptr, false)) {
        Serial.println("[PROTO] MessageStore begin failed — LXMF recovery may stick");
        messageStoreReady = false;
        // Continue anyway so announce/radio still work; msg may stay Recovering
    } else {
        messageStoreReady = true;
        Serial.println("[PROTO] MessageStore ready");
    }

    // 4) Announce bridge (contacts/names when announces arrive)
    announceMgr.setStorage(nullptr, &flash);
    protocolRuntime.setAnnounceManager(&announceMgr);

    // 5) Protocol runtime
    const int32_t profile = RS_HANDHELD_PROFILE_SMALL;
    const uint32_t nodeHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    if (!protocolRuntime.begin(
            &flash, nullptr, &identityMgr, &messageStore, &announceMgr,
            profile, nodeHeapCaps)) {
        Serial.println("[PROTO] ProtocolRuntime begin failed");
        return;
    }

    const char* nodeName = "rsDeckPro";
    protocolRuntime.seedAnnounceAppData(
        reinterpret_cast<const uint8_t*>(nodeName), strlen(nodeName));

    protocolRuntime.setMessageCallback(onLxmfMessage);

    if (loraIface) {
        protocolRuntime.pump().attachLoRa(loraIface);
        Serial.println("[PROTO] LoRa attached to pump");
    }

    protocolReady = protocolRuntime.protocolReady();
    Serial.printf("[PROTO] Ready = %s\r\n", protocolReady ? "yes" : "no");
}

static void updateNodeHome() {
    char destShort[20] = {0};
    if (protocolReady) {
        String full = protocolRuntime.destinationHashHex();
        if (full.length() >= 12) {
            snprintf(destShort, sizeof(destShort),
                     "%c%c%c%c:%c%c%c%c:%c%c%c%c",
                     full[0], full[1], full[2], full[3],
                     full[4], full[5], full[6], full[7],
                     full[8], full[9], full[10], full[11]);
        }
    }
    float v = 0;
    int pct = -1;
    readBattery(v, pct);
    bool loraOk = loraIface && loraIface->isOnline();
    unsigned paths = protocolReady ? (unsigned)protocolRuntime.pathCount() : 0;
    unsigned links = protocolReady ? (unsigned)protocolRuntime.linkCount() : 0;

    tdeck_pro::eink::showNodeHome(
        destShort[0] ? destShort : nullptr,
        pct, loraOk, paths, links, g_lastEvent, FIRMWARE_VERSION);
}

static void testSpeaker() {
    Serial.println("[AUDIO] Speaker test");
    if (BOARD_6609_EN >= 0) {
        pinMode(BOARD_6609_EN, OUTPUT);
        digitalWrite(BOARD_6609_EN, HIGH);
        delay(20);
    }
    audio.end();
    audio.begin();
    audio.setEnabled(true);
    audio.setVolume(90);
    if (!audio.isReady()) {
        Serial.println("[AUDIO] Speaker I2S not ready");
        return;
    }
    audio.playBoot();
    delay(100);
    audio.writeTone(1000, 400);
    audio.writeSilence(50);
    Serial.println("[AUDIO] Speaker test done");
}

static void testMicrophone(uint16_t durationMs = 1500) {
    Serial.println("[AUDIO] Microphone PDM test starting...");
    audio.end();
    if (MIC_DATA < 0 || MIC_CLOCK < 0) {
        Serial.println("[AUDIO] MIC pins not configured");
        return;
    }
    const i2s_port_t port = I2S_NUM_0;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    cfg.sample_rate = 16000;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = I2S_PIN_NO_CHANGE;
    pins.ws_io_num = MIC_CLOCK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = MIC_DATA;
    pins.mck_io_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(port, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[AUDIO] PDM install failed");
        return;
    }
    if (i2s_set_pin(port, &pins) != ESP_OK) {
        i2s_driver_uninstall(port);
        Serial.println("[AUDIO] PDM pin config failed");
        return;
    }

    const int bufSamples = 512;
    int16_t* buf = (int16_t*)malloc(bufSamples * sizeof(int16_t));
    if (!buf) {
        i2s_driver_uninstall(port);
        return;
    }
    int32_t peak = 0;
    double sumSq = 0.0;
    size_t totalSamples = 0;
    uint32_t start = millis();
    while (millis() - start < durationMs) {
        size_t bytesRead = 0;
        if (i2s_read(port, buf, bufSamples * sizeof(int16_t),
                     &bytesRead, pdMS_TO_TICKS(100)) != ESP_OK || bytesRead == 0) {
            continue;
        }
        int n = (int)(bytesRead / sizeof(int16_t));
        for (int i = 0; i < n; i++) {
            int32_t s = buf[i] < 0 ? -buf[i] : buf[i];
            if (s > peak) peak = s;
            sumSq += (double)buf[i] * (double)buf[i];
            totalSamples++;
        }
    }
    free(buf);
    i2s_driver_uninstall(port);
    double rms = totalSamples ? sqrt(sumSq / (double)totalSamples) : 0.0;
    Serial.printf("[AUDIO] Mic peak=%ld rms=%.1f samples=%u\r\n",
                  (long)peak, rms, (unsigned)totalSamples);
}

static void cmdMsg(const String& rest) {
    if (!protocolReady) {
        Serial.println("Protocol not ready");
        return;
    }
    if (!messageStoreReady) {
        Serial.println("[LXMF] MessageStore not ready — submit may stay Recovering");
    }
    String r = rest;
    r.trim();
    int sp = r.indexOf(' ');
    if (sp <= 0) {
        Serial.println("Usage: msg <32hex-dest> <text>");
        return;
    }
    String destStr = r.substring(0, sp);
    String text = r.substring(sp + 1);
    text.trim();
    if (text.length() == 0) {
        Serial.println("Empty message body");
        return;
    }

    uint8_t dest[16];
    if (!parseDestHash(destStr, dest)) {
        Serial.println("Bad dest hash (need 32 hex chars)");
        return;
    }

    String local = protocolRuntime.destinationHashHex();
    String localClean, destClean;
    for (size_t i = 0; i < local.length(); i++) {
        char c = local[i];
        if (c == ':' || c == ' ') continue;
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        localClean += c;
    }
    for (size_t i = 0; i < destStr.length(); i++) {
        char c = destStr[i];
        if (c == ':' || c == ' ' || c == '-') continue;
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        destClean += c;
    }
    if (destClean == localClean) {
        Serial.println("[LXMF] refusing: that is LOCAL_DEST");
        return;
    }

    Serial.printf("[LXMF] submit dest=%s len=%u\r\n",
                  destClean.c_str(), (unsigned)text.length());

    auto sub = protocolRuntime.lxmfSubmit(
        dest, nullptr, 0,
        reinterpret_cast<const uint8_t*>(text.c_str()),
        (size_t)text.length(), false);

    if (!sub.accepted()) {
        Serial.printf("[LXMF] submit rejected: %d (%s)\r\n",
                      (int)sub.rejection, rejectionName(sub.rejection));
        setLastEvent("msg reject");
        return;
    }

    handheld::outgoing::InitialResult ir{};
    uint32_t t0 = millis();
    while (millis() - t0 < 5000UL) {
        if (messageStoreReady) messageStore.poll();
        protocolRuntime.loop();
        protocolRuntime.pollReceive();
        auto p = protocolRuntime.lxmfPoll(sub.ticket, ir);
        if (p == handheld::outgoing::Poll::Ready) {
            protocolRuntime.lxmfAcknowledge(sub.ticket);
            Serial.printf("[LXMF] admitted outcome=%d rev=%u\r\n",
                          (int)ir.outcome, (unsigned)ir.revision);
            setLastEvent("msg sent");
            return;
        }
        if (p == handheld::outgoing::Poll::Invalid) {
            Serial.println("[LXMF] poll invalid");
            setLastEvent("msg fail");
            return;
        }
        delay(10);
    }
    Serial.println("[LXMF] poll timeout (may still TX in background)");
    setLastEvent("msg pending");
}

static String serialLine;

static void printHelp() {
    Serial.println("Commands:");
    Serial.println("  help / status / diag / whoami");
    Serial.println("  battery / identity / peers / announce / radio / home");
    Serial.println("  msg <32hex-dest> <text>");
    Serial.println("  speaker / mic / audio");
}

static void handleCommand(const String& cmd) {
    String c = cmd;
    c.trim();
    if (c.length() == 0) return;
    String lower = c;
    lower.toLowerCase();

    if (lower == "help" || lower == "?") {
        printHelp();
    } else if (lower == "status") {
        Serial.printf("Firmware : %s\r\n", FIRMWARE_VERSION);
        Serial.printf("Uptime   : %lu s\r\n", millis() / 1000UL);
        Serial.printf("Free heap: %u\r\n", ESP.getFreeHeap());
        Serial.printf("MsgStore : %s\r\n", messageStoreReady ? "ready" : "not ready");
        Serial.printf("Protocol : %s\r\n", protocolReady ? "ready" : "not ready");
        Serial.printf("LoRa     : %s\r\n",
                      (loraIface && loraIface->isOnline()) ? "online" : "offline");
        if (protocolReady) {
            Serial.printf("Paths    : %u\r\n", (unsigned)protocolRuntime.pathCount());
            Serial.printf("Links    : %u\r\n", (unsigned)protocolRuntime.linkCount());
            Serial.printf("LXMF Q   : %d\r\n", protocolRuntime.lxmfQueuedCount());
        }
    } else if (lower == "diag") {
        cmdDiag();
    } else if (lower == "whoami" || lower == "identity") {
        cmdWhoami();
    } else if (lower == "battery") {
        float v = 0;
        int pct = -1;
        if (readBattery(v, pct)) Serial.printf("Battery: %.2f V  %d%%\r\n", v, pct);
        else Serial.println("Battery read failed");
    } else if (lower == "announce") {
        if (!protocolReady) {
            Serial.println("Protocol not ready");
            return;
        }
        auto result = protocolRuntime.announce(nullptr, 0);
        Serial.printf("Announce result: %d\r\n", (int)result);
        Serial.printf("Paths : %u  Links : %u\r\n",
                      (unsigned)protocolRuntime.pathCount(),
                      (unsigned)protocolRuntime.linkCount());
        setLastEvent("announce");
    } else if (lower == "peers") {
        if (!protocolReady) {
            Serial.println("Protocol not ready");
            return;
        }
        Serial.printf("Paths : %u\r\n", (unsigned)protocolRuntime.pathCount());
        Serial.printf("Links : %u\r\n", (unsigned)protocolRuntime.linkCount());
    } else if (lower == "radio") {
        if (!loraIface) {
            Serial.println("No LoRa");
            return;
        }
        Serial.printf("Online : %s\r\n", loraIface->isOnline() ? "yes" : "no");
        Serial.printf("RSSI   : %d\r\n", loraIface->lastRxRssi());
        Serial.printf("SNR    : %.1f\r\n", loraIface->lastRxSnr());
    } else if (lower == "home") {
        updateNodeHome();
    } else if (lower.startsWith("msg ") || lower == "msg") {
        if (lower == "msg") {
            Serial.println("Usage: msg <32hex-dest> <text>");
            return;
        }
        cmdMsg(c.substring(4));
    } else if (lower == "speaker") {
        testSpeaker();
    } else if (lower == "mic") {
        testMicrophone();
    } else if (lower == "audio") {
        testSpeaker();
        delay(300);
        testMicrophone();
    } else {
        Serial.printf("Unknown command: %s\r\n", c.c_str());
        printHelp();
    }
}

static void pollSerial() {
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (serialLine.length() > 0) {
                handleCommand(serialLine);
                serialLine = "";
            }
        } else if (serialLine.length() < 200) {
            serialLine += ch;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("========================================");
    Serial.println("  RATSPEAK  ·  T-Deck Pro");
    Serial.println("  DISPLAY NODE / E-INK");
    Serial.printf("  Firmware : %s\r\n", FIRMWARE_VERSION);
    Serial.println("========================================");

    powerGatesOn();
    delay(50);
    heartbeatLed(false);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    if (keyboard.begin()) Serial.println("[KEY] TCA8418 OK");
    else Serial.println("[KEY] TCA8418 init failed");

    tdeck_pro::eink::begin();
    tdeck_pro::eink::showBootScreen();

    float v = 0;
    int pct = -1;
    if (readBattery(v, pct)) Serial.printf("[BAT] %.2f V  %d%%\r\n", v, pct);

#if HAS_AUDIO
    audio.begin();
    audio.setEnabled(true);
    audio.setVolume(80);
#endif

    if (!initRadio()) Serial.println("[BOOT] Radio init failed");
    initProtocol();
    setLastEvent("ready");
    updateNodeHome();

    Serial.println("[BOOT] ready — type 'help' or 'diag'");
    printHelp();
}

void loop() {
    // Product-like cadence: store I/O + protocol + radio every pass
    if (messageStoreReady) {
        messageStore.poll();
    }
    if (protocolReady) {
        protocolRuntime.loop();
        protocolRuntime.pollReceive();
    }
    if (loraIface) loraIface->loop();
#if HAS_AUDIO
    audio.loop();
#endif

    keyboard.update();
    if (keyboard.hasEvent()) {
        const KeyEvent& e = keyboard.getEvent();
        if (e.enter && protocolReady) {
            auto r = protocolRuntime.announce(nullptr, 0);
            Serial.printf("[KEY] announce result=%d\r\n", (int)r);
            setLastEvent("key announce");
        }
    }

    pollSerial();

    uint32_t now = millis();

    if (g_needHomeRedraw) {
        g_needHomeRedraw = false;
        updateNodeHome();
    }

    static uint32_t lastAnnounce = 0;
    if (protocolReady && (now - lastAnnounce >= 300000UL)) {
        lastAnnounce = now;
        protocolRuntime.announce(nullptr, 0);
        setLastEvent("auto-announce");
    }

    static uint32_t lastUptime = 0;
    if (now - lastUptime >= 1000UL) {
        lastUptime = now;
        tdeck_pro::eink::showUptime(now / 1000UL);
    }

    static uint32_t lastBat = 0;
    if (now - lastBat >= 30000UL) {
        lastBat = now;
        float bv = 0;
        int bp = -1;
        if (readBattery(bv, bp)) Serial.printf("[BAT] %.2f V  %d%%\r\n", bv, bp);
    }

    static uint32_t lastScreen = 0;
    if (now - lastScreen >= 60000UL) {
        lastScreen = now;
        updateNodeHome();
    }

    // Optional: log when recovery appears to clear (Q goes to 0)
    static int lastQ = -1;
    if (protocolReady) {
        int q = protocolRuntime.lxmfQueuedCount();
        if (q != lastQ) {
            Serial.printf("[LXMF] queue depth %d -> %d\r\n", lastQ, q);
            lastQ = q;
            if (q == 0) setLastEvent("lxmf ready");
        }
    }

    delay(5);
}