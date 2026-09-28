#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

#include "config/BoardConfig.h"
#include "hal/Keyboard.h"
#include "hal/EinkDisplay.h"

// =============================================================================
// T-Deck Pro — Cooperative / Headless
// =============================================================================

static Keyboard keyboard;

// -----------------------------------------------------------------------------
// LED heartbeat
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
    digitalWrite(BOARD_MOTOR_PIN, LOW);   // haptic off
}

// -----------------------------------------------------------------------------
// Battery (BQ27220)
// -----------------------------------------------------------------------------

static bool readBattery(float& voltage, int& percent) {
    // Voltage 0x08 (mV)
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x08);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    uint16_t mv = Wire.read() | (Wire.read() << 8);
    voltage = mv / 1000.0f;

    // Relative SOC 0x2C (%)
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x2C);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    percent = Wire.read() | (Wire.read() << 8);
    if (percent > 100) percent = 100;
    return true;
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
    Serial.println("  identity  - (protocol placeholder)");
    Serial.println("  peers     - (protocol placeholder)");
    Serial.println("  announce  - (protocol placeholder)");
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
    else if (c == "identity" || c == "peers" || c == "announce") {
        Serial.println("(protocol core not wired yet — next milestone)");
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

    // Short LED blink to show we are alive
    for (int i = 0; i < 3; i++) {
        heartbeatLed(true);
        delay(70);
        heartbeatLed(false);
        delay(70);
    }

    // E-ink status screen
    tdeck_pro::eink::begin();
    tdeck_pro::eink::showBootScreen();

    // Battery info
    float v = 0;
    int pct = -1;
    if (readBattery(v, pct)) {
        Serial.printf("[BAT] %.2f V  %d%%\r\n", v, pct);
    }

    Serial.println();
    Serial.println("[BOOT] ready — type 'help' for commands");
    printHelp();
    Serial.println();
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    // Keyboard echo
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

    // Serial commands
    pollSerial();

    // 1 Hz LED heartbeat
    static uint32_t lastLed = 0;
    static bool ledOn = false;
    uint32_t now = millis();
    if (now - lastLed >= 1000) {
        lastLed = now;
        ledOn = !ledOn;
        heartbeatLed(ledOn);
    }

    // Optional periodic battery (every 30 s)
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