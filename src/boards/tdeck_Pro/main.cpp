#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

#include "config/BoardConfig.h"
#include "hal/Keyboard.h"
#include "hal/EinkDisplay.h"

// Protocol core
#include "protocol/ProtocolRuntime.h"
#include "reticulum/IdentityManager.h"
#include "storage/FlashStore.h"
#include "storage/MessageStore.h"
#include "reticulum/AnnounceManager.h"

// =============================================================================
// T-Deck Pro — Cooperative / Headless + Protocol Step A
// =============================================================================

static Keyboard keyboard;
static FlashStore flash;
static IdentityManager identityMgr;
static MessageStore messageStore;
static AnnounceManager* announceMgr = nullptr;   // optional for now
static ProtocolRuntime protocolRuntime;
static bool protocolReady = false;

// -----------------------------------------------------------------------------
// LED
// -----------------------------------------------------------------------------

static void heartbeatLed(bool on) {
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    digitalWrite(BOARD_KEYBOARD_LED, on ? HIGH : LOW);
}

// -----------------------------------------------------------------------------
// Power gates
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Battery (BQ27220)
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Protocol init (matches real signatures)
// -----------------------------------------------------------------------------

static void initProtocol() {
    Serial.println("[PROTO] Initializing...");

    if (!flash.begin()) {
        Serial.println("[PROTO] FlashStore begin failed");
        return;
    }

    // IdentityManager::begin(FlashStore*, SDStore* = nullptr)
    if (!identityMgr.begin(&flash, nullptr)) {
        Serial.println("[PROTO] IdentityManager begin failed");
        return;
    }

    // MessageStore is required by ProtocolRuntime::begin
    // (signature may accept null in some paths, but we pass a real one)
    // We keep it minimal for now.

    // ProtocolRuntime::begin(
    //   FlashStore*, SDStore*, IdentityManager*, MessageStore*,
    //   AnnounceManager*, int32_t profile, uint32_t nodeHeapCaps)
    //
    // profile: RS_HANDHELD_PROFILE_SMALL for tdeck-class boards
    // nodeHeapCaps: MALLOC_CAP_SPIRAM for SMALL profile

    const int32_t profile = RS_HANDHELD_PROFILE_SMALL;
    // Prefer internal RAM for headless cooperative bring-up.
    // SPIRAM allocation of the ~200 KB node is failing on this boot path.
    const uint32_t nodeHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    if (!protocolRuntime.begin(&flash, nullptr, &identityMgr, &messageStore,
                               announceMgr, profile, nodeHeapCaps)) {
        Serial.println("[PROTO] ProtocolRuntime begin failed");
        return;
    }

    protocolReady = protocolRuntime.protocolReady();
    Serial.printf("[PROTO] Ready = %s\r\n", protocolReady ? "yes" : "no");
}

// -----------------------------------------------------------------------------
// Serial command interface
// -----------------------------------------------------------------------------

static String serialLine;

static void printHelp() {
    Serial.println("Commands:");
    Serial.println("  help      - this help");
    Serial.println("  status    - firmware + uptime + heap");
    Serial.println("  battery   - voltage and SOC");
    Serial.println("  identity  - local destination hash");
    Serial.println("  peers     - (next milestone)");
    Serial.println("  announce  - (next milestone)");
}

static void handleCommand(const String& cmd) {
    String c = cmd;
    c.trim();
    c.toLowerCase();
    if (c.length() == 0) return;

    if (c == "help" || c == "?") {
        printHelp();
    }
    else if (c == "status") {
        Serial.printf("Firmware : %s\r\n", FIRMWARE_VERSION);
        Serial.printf("Uptime   : %lu s\r\n", millis() / 1000UL);
        Serial.printf("Free heap: %u bytes\r\n", ESP.getFreeHeap());
        Serial.printf("Protocol : %s\r\n", protocolReady ? "ready" : "not ready");
        Serial.println("Mode     : Cooperative / Headless");
    }
    else if (c == "battery") {
        float v = 0;
        int pct = -1;
        if (readBattery(v, pct)) {
            Serial.printf("Battery: %.2f V  %d%%\r\n", v, pct);
        } else {
            Serial.println("Battery read failed");
        }
    }
    else if (c == "identity") {
        if (!protocolReady) {
            Serial.println("Protocol not ready");
            return;
        }
        // Use the methods that actually exist on ProtocolRuntime
        Serial.printf("Dest hash : %s\r\n", protocolRuntime.destinationHashHex().c_str());
        Serial.printf("Identity  : %s\r\n", protocolRuntime.identityHashHex().c_str());
    }
    else if (c == "peers" || c == "announce") {
        Serial.println("(coming in next milestone)");
    }
    else {
        Serial.printf("Unknown command: %s\r\n", c.c_str());
        Serial.println("Type 'help' for list");
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
        } else if (serialLine.length() < 120) {
            serialLine += ch;
        }
    }
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  RATSPEAK  ·  T-Deck Pro");
    Serial.println("  HEADLESS / COOPERATIVE");
    Serial.printf ("  Firmware : %s\r\n", FIRMWARE_VERSION);
    Serial.println("========================================");
    Serial.println();

    powerGatesOn();
    delay(50);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    if (keyboard.begin()) {
        Serial.println("[KEY] TCA8418 OK");
    } else {
        Serial.println("[KEY] TCA8418 init failed");
    }

    for (int i = 0; i < 3; i++) {
        heartbeatLed(true);
        delay(70);
        heartbeatLed(false);
        delay(70);
    }

    tdeck_pro::eink::begin();
    tdeck_pro::eink::showBootScreen();

    float v = 0;
    int pct = -1;
    if (readBattery(v, pct)) {
        Serial.printf("[BAT] %.2f V  %d%%\r\n", v, pct);
    }

    initProtocol();

    Serial.println();
    Serial.println("[BOOT] ready — type 'help' for commands");
    printHelp();
    Serial.println();
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    // Cooperative protocol work
    if (protocolReady) {
        protocolRuntime.loop();          // correct method name
        protocolRuntime.pollReceive();
    }

    keyboard.update();
    if (keyboard.hasEvent()) {
        const KeyEvent& e = keyboard.getEvent();
        if (e.enter) {
            Serial.println("[KEY] ENTER");
        } else if (e.del) {
            Serial.println("[KEY] DEL");
        } else if (e.space) {
            Serial.println("[KEY] SPACE");
        } else if (e.character) {
            Serial.printf("[KEY] '%c'\r\n", e.character);
        }
    }

    pollSerial();

    static uint32_t lastLed = 0;
    static bool ledOn = false;
    uint32_t now = millis();
    if (now - lastLed >= 1000) {
        lastLed = now;
        ledOn = !ledOn;
        heartbeatLed(ledOn);
    }

    static uint32_t lastBat = 0;
    if (now - lastBat >= 30000UL) {
        lastBat = now;
        float v = 0;
        int pct = -1;
        if (readBattery(v, pct)) {
            Serial.printf("[BAT] %.2f V  %d%%\r\n", v, pct);
        }
    }

    delay(5);
}