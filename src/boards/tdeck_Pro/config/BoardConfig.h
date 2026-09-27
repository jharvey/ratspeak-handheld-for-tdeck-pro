#pragma once

// =============================================================================
// rsDeck — LilyGo T-Deck Pro (V1.0) Pin Definitions
// =============================================================================
// Hardware reference:
//   - LilyGO T-Deck-Pro utilities.h (HD-V1-250326)
//   - wiki.lilygo.cc T-Deck Pro pin map
//
// V1.1 moves some pins (touch RST, EPD RST, etc.). Confirm hardware revision
// before changing this file. Do NOT copy Plus (T-Deck Plus) pin numbers here.
// =============================================================================

// --- Board Identity / Branding ---
#define BOARD_COMPONENT_ID       "tdeck_pro"
#define BOARD_BOOT_NAMESPACE     "ratdeckpro"
#define DEVICE_NAME              "rsDeckPro"
#define DEVICE_AP_PREFIX         "rsdeckpro"
#define BOARD_CONFIRM_INPUT_NAME "keyboard"
#define BOARD_DEFAULT_BRIGHTNESS 100
#define BOARD_BETA_LABEL         "T-Deck Pro port (headless)"

#include "config/FirmwareVersion.h"
#define BOARD_RELEASE_REPO       "ratspeak/ratspeak-handheld"
#define HAS_CONTACT_RENAME       true

// --- Feature Flags ---
// Display stays false until a GDEQ031T10 / e-ink HAL exists.
#define HAS_DISPLAY       false
#define HAS_KEYBOARD      true
#define HAS_TOUCH         true     // CST328 present; driver may still be stubbed
#define HAS_TRACKBALL     false    // Pro has no trackball
#define HAS_SCROLLWHEEL   false
#define HAS_LORA          true
#define HAS_WIFI          true
// TODO(BLE): same as Plus — revisit after Pro HAL is stable.
#define HAS_SD            true
#define HAS_AUDIO         false    // Pro audio path differs (PCM512A); defer
#define HAS_GPS           true     // MIA-M10Q UART GPS
// Pro uses BQ27220/BQ25896 over I2C, not a simple ADC divider model.
#define HAS_BATTERY_MODEL false

// --- Persisted Names ---
// Separate namespaces/paths so Pro data never collides with Plus installs.
#define NVS_NS_IDENTITY         "ratdeckpro_id"
#define NVS_NS_MSG              "ratdeckpro_msg"
#define SD_PATH_ROOT            "/ratdeckpro"
#define SD_PATH_CONFIG_DIR      "/ratdeckpro/config"
#define SD_PATH_USER_CONFIG     "/ratdeckpro/config/user.json"
#define SD_PATH_MESSAGES        "/ratdeckpro/messages"
#define SD_PATH_CONTACTS        "/ratdeckpro/contacts"
#define SD_PATH_IDENTITY_DIR    "/ratdeckpro/identity"
#define SD_PATH_IDENTITY        "/ratdeckpro/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/ratdeckpro/identity/import.identity"
#define SD_PATH_IMPORT_ID       "/ratdeckpro/identity/import.key"
#define SD_PATH_TRANSPORT       "/ratdeckpro/transport"

// --- Power Gates (drive HIGH early in boot) ---
// Pro does NOT use Plus BOARD_POWER_PIN (GPIO 10).
#define BOARD_POWER_PIN         -1    // unused on Pro; keep symbol for shared code
#define BOARD_GPS_EN            39    // GPS module enable
#define BOARD_1V8_EN            38    // 1.8 V rail (IMU etc.)
#define BOARD_LORA_EN           46    // SX1262 power enable
#define BOARD_6609_EN           41    // A7682E / modem path enable (optional)
#define BOARD_MOTOR_PIN          2    // haptic / motor

// Keyboard backlight (confirmed working on hardware)
#define BOARD_KEYBOARD_LED      42

// --- SX1262 LoRa Radio (shared SPI with EPD + SD) ---
#define LORA_CS                  3
#define LORA_IRQ                 5    // DIO1
#define LORA_RST                 4
#define LORA_BUSY                6
#define LORA_RXEN               -1    // not connected
#define LORA_TXEN               -1    // not connected

// --- SX1262 Radio Configuration ---
// Keep Plus-compatible RF defaults until Pro RF is characterized on the bench.
#define LORA_HAS_TCXO               true
#define LORA_DIO2_AS_RF_SWITCH      true
#define LORA_TCXO_VOLTAGE           0x02   // MODE_TCXO_1_8V_6X (verify on Pro)
#define LORA_USE_DCDC_REGULATOR     true
#define LORA_OCP_TUNED              0x38
#define LORA_DEFAULT_FREQ           915000000
#define LORA_DEFAULT_BW             250000   // Long Fast preset
#define LORA_DEFAULT_SF             11
#define LORA_DEFAULT_CR             5
#define LORA_DEFAULT_TX_POWER       22
#define LORA_DEFAULT_PREAMBLE       18

// --- Shared SPI Bus (EPD + LoRa + SD) ---
#define SPI_SCK                 36
#define SPI_MOSI                33
#define SPI_MISO                47

// --- E-paper display (GDEQ031T10) — defines kept for future driver ---
// Panel is 320x240 logical; some drivers report 240x320 native orientation.
#define EPD_CS                  34
#define EPD_DC                  35
#define EPD_BUSY                37
#define EPD_RST                 -1    // V1.0: often software reset only
#define EPD_WIDTH               320
#define EPD_HEIGHT              240

// Compatibility aliases so shared code that still expects TFT_* compiles.
// When HAS_DISPLAY is false these are unused; when e-ink lands, map them.
#define TFT_CS                  EPD_CS
#define TFT_DC                  EPD_DC
#define TFT_BL                  BOARD_KEYBOARD_LED  // not a TFT backlight on Pro
#define TFT_WIDTH               EPD_WIDTH
#define TFT_HEIGHT              EPD_HEIGHT
#define TFT_SPI_FREQ            4000000   // e-ink is slow; do not use 27 MHz
#define TFT_RST                 EPD_RST
#define TFT_ROTATION            0
#define TFT_BL_INVERT           false
#define TFT_PWM_FREQ            5000

// --- I2C Bus (keyboard, touch, IMU, fuel gauge, charger, …) ---
#define I2C_SDA                 13
#define I2C_SCL                 14
#define I2C_FREQUENCY           400000

// --- Keyboard (TCA8418) ---
#define KB_I2C_ADDR             0x34
#define KB_INT                  15
#define KB_LED                  BOARD_KEYBOARD_LED
#define KB_ROWS                 4
#define KB_COLS                 10

// --- Touch (CST328) ---
#define TOUCH_INT               12
#define TOUCH_RST               45    // V1.0; V1.1 may differ
#define TOUCH_I2C_ADDR          0x1A
// Keep dual-addr symbols if shared GT911-style code still references them.
#define TOUCH_I2C_ADDR_1        TOUCH_I2C_ADDR
#define TOUCH_I2C_ADDR_2        TOUCH_I2C_ADDR

// --- Trackball (not present) — stubs so optional includes compile ---
#define TBALL_UP                -1
#define TBALL_DOWN              -1
#define TBALL_LEFT              -1
#define TBALL_RIGHT             -1
#define TBALL_CLICK             -1

// --- SD Card (shared SPI) ---
#define SD_CS                   48

// --- GPS (MIA-M10Q UART) ---
#define GPS_TX                  43    // ESP TX -> GPS RX
#define GPS_RX                  44    // GPS TX -> ESP RX
#define GPS_PPS                  1
#define GPS_BAUD                38400
#define GPS_EN                  BOARD_GPS_EN

// --- Battery / PMU (I2C; no simple ADC model) ---
#define BAT_ADC_PIN             -1    // not used on Pro
#define BQ27220_I2C_ADDR        0x55
#define BQ25896_I2C_ADDR        0x6B

// --- Audio (deferred; Pro PCM512A path differs from Plus ES7210) ---
#define I2S_WS                  -1
#define I2S_DOUT                -1
#define I2S_BCK                 -1
#define I2S_DIN                 -1
#define I2S_SCK                 -1
#define I2S_MCLK                -1

// --- Boot ---
#define BOARD_BOOT_PIN           0

// --- Hardware Constants ---
#define MAX_PACKET_SIZE         255
#define SPI_FREQUENCY           8000000   // 8 MHz for SX1262 on shared bus