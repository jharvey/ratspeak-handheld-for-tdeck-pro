#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

#include "config/BoardConfig.h"
#include "hal/Keyboard.h"
#include "hal/EinkDisplay.h"

// Protocol + radio
#include "protocol/ProtocolRuntime.h"
#include "reticulum/IdentityManager.h"
#include "storage/FlashStore.h"
#include "storage/MessageStore.h"
#include "reticulum/AnnounceManager.h"
#include "radio/BoardRadio.h"
#include "transport/LoRaInterface.h"

// =============================================================================
// T-Deck Pro — Cooperative / Headless + Protocol + LoRa
// =============================================================================

static Keyboard keyboard;
static FlashStore flash;
static IdentityManager identityMgr;
static MessageStore messageStore;
static AnnounceManager* announceMgr = nullptr;
static ProtocolRuntime protocolRuntime;
static bool protocolReady = false;

// Radio stack
static BoardRadio* boardRadio = nullptr;
static LoRaInterface* loraIface = nullptr;

// -----------------------------------------------------------------------------
// LED
// -----------------------------------------------------------------------------
//
// The keyboard LED was originally used as a one-second heartbeat during
// hardware bring-up.
//
// It is intentionally disabled now that the e-ink display provides the
// once-per-second activity indicator.
//
// Keeping this helper here makes it easy to restore later if ever needed,
// but setup() explicitly leaves the LED OFF and loop() never toggles it.
// -----------------------------------------------------------------------------

static void heartbeatLed(bool on) {
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    digitalWrite(
        BOARD_KEYBOARD_LED,
        on ? HIGH : LOW);
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

static bool readBattery(
    float& voltage,
    int& percent) {

    Wire.beginTransmission(
        BQ27220_I2C_ADDR);

    Wire.write(0x08);

    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(
            (int)BQ27220_I2C_ADDR,
            2) != 2) {

        return false;
    }

    uint16_t mv =
        Wire.read() |
        (Wire.read() << 8);

    voltage = mv / 1000.0f;

    Wire.beginTransmission(
        BQ27220_I2C_ADDR);

    Wire.write(0x2C);

    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(
            (int)BQ27220_I2C_ADDR,
            2) != 2) {

        return false;
    }

    percent =
        Wire.read() |
        (Wire.read() << 8);

    if (percent > 100) {
        percent = 100;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Radio bring-up
// -----------------------------------------------------------------------------

static bool initRadio() {
    Serial.println(
        "[RADIO] Starting SX1262...");

    // Ensure LoRa power gate is on
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(
        BOARD_LORA_EN,
        HIGH);

    delay(20);

    // Keep other SPI CS high
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);

    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);

    boardRadio = new BoardRadio(
        &SPI,
        LORA_CS,
        SPI_SCK,
        SPI_MOSI,
        SPI_MISO,
        LORA_RST,
        LORA_IRQ,
        LORA_BUSY,
        LORA_RXEN,
        LORA_HAS_TCXO,
        LORA_DIO2_AS_RF_SWITCH);

    if (!boardRadio->begin(
            LORA_DEFAULT_FREQ)) {

        Serial.println(
            "[RADIO] SX1262 begin failed");

        return false;
    }

    // Apply default Long Fast-style settings
    boardRadio->setSpreadingFactor(
        LORA_DEFAULT_SF);

    boardRadio->setSignalBandwidth(
        LORA_DEFAULT_BW);

    boardRadio->setCodingRate4(
        LORA_DEFAULT_CR);

    boardRadio->setTxPower(
        LORA_DEFAULT_TX_POWER);

    boardRadio->setPreambleLength(
        LORA_DEFAULT_PREAMBLE);

    boardRadio->enableCrc();

    loraIface =
        new LoRaInterface(
            boardRadio,
            "LoRa");

    if (!loraIface->start()) {
        Serial.println(
            "[RADIO] LoRaInterface start failed");

        return false;
    }

    Serial.println(
        "[RADIO] SX1262 + LoRaInterface online");

    return true;
}

// -----------------------------------------------------------------------------
// Protocol init
// -----------------------------------------------------------------------------

static void initProtocol() {
    Serial.println(
        "[PROTO] Initializing...");

    if (!flash.begin()) {
        Serial.println(
            "[PROTO] FlashStore begin failed");

        return;
    }

    if (!identityMgr.begin(
            &flash,
            nullptr)) {

        Serial.println(
            "[PROTO] IdentityManager begin failed");

        return;
    }

    // Prefer internal RAM — SPIRAM node alloc was failing earlier
    const int32_t profile =
        RS_HANDHELD_PROFILE_SMALL;

    const uint32_t nodeHeapCaps =
        MALLOC_CAP_INTERNAL |
        MALLOC_CAP_8BIT;

    if (!protocolRuntime.begin(
            &flash,
            nullptr,
            &identityMgr,
            &messageStore,
            announceMgr,
            profile,
            nodeHeapCaps)) {

        Serial.println(
            "[PROTO] ProtocolRuntime begin failed");

        return;
    }

    // Attach LoRa so announces have an interface
    if (loraIface) {
        protocolRuntime
            .pump()
            .attachLoRa(loraIface);

        Serial.println(
            "[PROTO] LoRa attached to pump");

    } else {
        Serial.println(
            "[PROTO] WARNING: no LoRa interface to attach");
    }

    protocolReady =
        protocolRuntime.protocolReady();

    Serial.printf(
        "[PROTO] Ready = %s\r\n",
        protocolReady ? "yes" : "no");
}

// -----------------------------------------------------------------------------
// EINK
// -----------------------------------------------------------------------------

static void updateStatusScreen() {
    char destShort[20] = {0};

    if (protocolReady) {
        String full =
            protocolRuntime.destinationHashHex();

        // Show first 12 hex chars in groups:
        // a78c:6388:7d9d
        if (full.length() >= 12) {
            snprintf(
                destShort,
                sizeof(destShort),
                "%c%c%c%c:%c%c%c%c:%c%c%c%c",
                full[0],
                full[1],
                full[2],
                full[3],
                full[4],
                full[5],
                full[6],
                full[7],
                full[8],
                full[9],
                full[10],
                full[11]);
        }
    }

    float v = 0;
    int pct = -1;

    readBattery(
        v,
        pct);

    bool loraOk =
        loraIface &&
        loraIface->isOnline();

    unsigned paths =
        protocolReady
            ? (unsigned)protocolRuntime.pathCount()
            : 0;

    tdeck_pro::eink::showStatusScreen(
        destShort[0]
            ? destShort
            : nullptr,
        pct,
        loraOk,
        paths,
        FIRMWARE_VERSION);
}

// -----------------------------------------------------------------------------
// Serial command interface
// -----------------------------------------------------------------------------

static String serialLine;

static void printHelp() {
    Serial.println("Commands:");
    Serial.println(
        "  help      - this help");
    Serial.println(
        "  status    - firmware + uptime + heap");
    Serial.println(
        "  battery   - voltage and SOC");
    Serial.println(
        "  identity  - local destination hash");
    Serial.println(
        "  peers     - path / link counts");
    Serial.println(
        "  announce  - send presence announce");
    Serial.println(
        "  radio     - radio online / RSSI");
}

static void handleCommand(
    const String& cmd) {

    String c = cmd;

    c.trim();
    c.toLowerCase();

    if (c.length() == 0) {
        return;
    }

    if (c == "help" || c == "?") {
        printHelp();
    }

    else if (c == "status") {
        Serial.printf(
            "Firmware : %s\r\n",
            FIRMWARE_VERSION);

        Serial.printf(
            "Uptime   : %lu s\r\n",
            millis() / 1000UL);

        Serial.printf(
            "Free heap: %u bytes\r\n",
            ESP.getFreeHeap());

        Serial.printf(
            "Protocol : %s\r\n",
            protocolReady
                ? "ready"
                : "not ready");

        Serial.printf(
            "LoRa     : %s\r\n",
            (loraIface &&
             loraIface->isOnline())
                ? "online"
                : "offline");

        Serial.println(
            "Mode     : Cooperative / Headless");
    }

    else if (c == "battery") {
        float v = 0;
        int pct = -1;

        if (readBattery(v, pct)) {
            Serial.printf(
                "Battery: %.2f V  %d%%\r\n",
                v,
                pct);
        } else {
            Serial.println(
                "Battery read failed");
        }
    }

    else if (c == "identity") {
        if (!protocolReady) {
            Serial.println(
                "Protocol not ready");

            return;
        }

        Serial.printf(
            "Dest hash : %s\r\n",
            protocolRuntime
                .destinationHashHex()
                .c_str());

        Serial.printf(
            "Identity  : %s\r\n",
            protocolRuntime
                .identityHashHex()
                .c_str());
    }

    else if (c == "announce") {
        if (!protocolReady) {
            Serial.println(
                "Protocol not ready");

            return;
        }

        auto result =
            protocolRuntime.announce(
                nullptr,
                0);

        Serial.printf(
            "Announce result: %d\r\n",
            (int)result);

        Serial.printf(
            "Paths known   : %u\r\n",
            (unsigned)
                protocolRuntime.pathCount());

        Serial.printf(
            "Links         : %u\r\n",
            (unsigned)
                protocolRuntime.linkCount());
    }

    else if (c == "peers") {
        if (!protocolReady) {
            Serial.println(
                "Protocol not ready");

            return;
        }

        Serial.printf(
            "Paths : %u\r\n",
            (unsigned)
                protocolRuntime.pathCount());

        Serial.printf(
            "Links : %u\r\n",
            (unsigned)
                protocolRuntime.linkCount());
    }

    else if (c == "radio") {
        if (!loraIface) {
            Serial.println(
                "LoRa interface not created");

            return;
        }

        Serial.printf(
            "Online : %s\r\n",
            loraIface->isOnline()
                ? "yes"
                : "no");

        Serial.printf(
            "RSSI   : %d\r\n",
            loraIface->lastRxRssi());

        Serial.printf(
            "SNR    : %.1f\r\n",
            loraIface->lastRxSnr());
    }

    else {
        Serial.printf(
            "Unknown command: %s\r\n",
            c.c_str());

        Serial.println(
            "Type 'help' for list");
    }
}

static void pollSerial() {
    while (Serial.available()) {
        char ch =
            (char)Serial.read();

        if (ch == '\n' ||
            ch == '\r') {

            if (serialLine.length() > 0) {
                handleCommand(
                    serialLine);

                serialLine = "";
            }

        } else if (
            serialLine.length() < 120) {

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

    Serial.println(
        "========================================");

    Serial.println(
        "  RATSPEAK  ·  T-Deck Pro");

    Serial.println(
        "  HEADLESS / COOPERATIVE");

    Serial.printf(
        "  Firmware : %s\r\n",
        FIRMWARE_VERSION);

    Serial.println(
        "========================================");

    Serial.println();

    powerGatesOn();

    delay(50);

    // -------------------------------------------------------------------------
    // Keyboard LED
    // -------------------------------------------------------------------------
    //
    // The old bring-up test flashed this LED three times during startup and
    // then toggled it once per second from loop().
    //
    // Disable both behaviors. Explicitly configure the LED and leave it LOW.
    // -------------------------------------------------------------------------

    heartbeatLed(false);

    Serial.println(
        "[LED] Keyboard LED disabled");

    Wire.begin(
        I2C_SDA,
        I2C_SCL);

    Wire.setClock(
        I2C_FREQUENCY);

    SPI.begin(
        SPI_SCK,
        SPI_MISO,
        SPI_MOSI);

    if (keyboard.begin()) {
        Serial.println(
            "[KEY] TCA8418 OK");
    } else {
        Serial.println(
            "[KEY] TCA8418 init failed");
    }

    // -------------------------------------------------------------------------
    // E-ink
    // -------------------------------------------------------------------------

    tdeck_pro::eink::begin();

    tdeck_pro::eink::showBootScreen();

    float v = 0;
    int pct = -1;

    if (readBattery(v, pct)) {
        Serial.printf(
            "[BAT] %.2f V  %d%%\r\n",
            v,
            pct);
    }

    // Radio first, then protocol
    // (so we can attach LoRa)
    if (!initRadio()) {
        Serial.println(
            "[BOOT] Radio init failed — continuing without LoRa");
    }

    initProtocol();

    // Now that we know real state,
    // draw the full status screen.
    updateStatusScreen();

    Serial.println();

    Serial.println(
        "[BOOT] ready — type 'help' for commands");

    printHelp();

    Serial.println();
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    if (protocolReady) {
        protocolRuntime.loop();
        protocolRuntime.pollReceive();
    }

    // Keep the LoRa driver ticking
    if (loraIface) {
        loraIface->loop();
    }

    // Keyboard
    keyboard.update();

    if (keyboard.hasEvent()) {
        const KeyEvent& e =
            keyboard.getEvent();

        if (e.enter) {
            Serial.println(
                "[KEY] ENTER");

        } else if (e.del) {
            Serial.println(
                "[KEY] DEL");

        } else if (e.space) {
            Serial.println(
                "[KEY] SPACE");

        } else if (e.character) {
            Serial.printf(
                "[KEY] '%c'\r\n",
                e.character);
        }
    }

    // Serial commands
    pollSerial();

    // -------------------------------------------------------------------------
    // Once-per-second e-ink uptime display
    // -------------------------------------------------------------------------
    //
    // This replaces the old keyboard LED heartbeat.
    //
    // Only a small partial window on the e-ink display is updated.
    // -------------------------------------------------------------------------

    static uint32_t lastUptimeDisplay = 0;

    uint32_t now = millis();

    if (now - lastUptimeDisplay >= 1000UL) {
        lastUptimeDisplay = now;

        tdeck_pro::eink::showUptime(
            now / 1000UL);
    }

    // -------------------------------------------------------------------------
    // Battery polling
    // -------------------------------------------------------------------------

    static uint32_t lastBat = 0;

    if (now - lastBat >= 30000UL) {
        lastBat = now;

        float v = 0;
        int pct = -1;

        if (readBattery(v, pct)) {
            Serial.printf(
                "[BAT] %.2f V  %d%%\r\n",
                v,
                pct);
        }
    }

    // -------------------------------------------------------------------------
    // Full status screen
    // -------------------------------------------------------------------------
    //
    // Keep the existing 60-second full-screen refresh.
    // The once-per-second clock is a separate partial update.
    // -------------------------------------------------------------------------

    static uint32_t lastScreen = 0;

    if (now - lastScreen >= 60000UL) {
        lastScreen = now;

        updateStatusScreen();
    }

    delay(5);
}