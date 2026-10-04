#pragma once

// =============================================================================
// rsDeck — LilyGo T-Deck Pro (v1.1)
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
#define HAS_DISPLAY       true
#define HAS_KEYBOARD      true
#define HAS_TOUCH         true
#define HAS_TRACKBALL     false
#define HAS_SCROLLWHEEL   false
#define HAS_LORA          true
#define HAS_WIFI          true
#define HAS_SD            true
#define HAS_AUDIO         true
#define HAS_GPS           true

#define HAS_BATTERY_MODEL false

// --- Persisted Names ---
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

// --- Power Gates ---
#define BOARD_POWER_PIN         -1
#define BOARD_GPS_EN            39
#define BOARD_1V8_EN            38
#define BOARD_LORA_EN           46
#define BOARD_6609_EN           41

#define BOARD_MOTOR_PIN         2
#define BOARD_KEYBOARD_LED      42

// --- SX1262 LoRa Radio ---
#define LORA_CS                  3
#define LORA_IRQ                 5
#define LORA_RST                 4
#define LORA_BUSY                6
#define LORA_RXEN               -1
#define LORA_TXEN               -1

#define LORA_HAS_TCXO               true
#define LORA_DIO2_AS_RF_SWITCH      true
#define LORA_TCXO_VOLTAGE           0x02
#define LORA_USE_DCDC_REGULATOR     true
#define LORA_OCP_TUNED              0x38

#define LORA_DEFAULT_FREQ           915000000
#define LORA_DEFAULT_BW             250000
#define LORA_DEFAULT_SF             11
#define LORA_DEFAULT_CR             5
#define LORA_DEFAULT_TX_POWER       22
#define LORA_DEFAULT_PREAMBLE       18

// --- Shared SPI Bus ---
#define SPI_SCK                 36
#define SPI_MOSI                33
#define SPI_MISO                47

// --- E-paper display ---
#define EPD_CS                  34
#define EPD_DC                  35
#define EPD_BUSY                37
#define EPD_RST                 16

#define EPD_WIDTH               240
#define EPD_HEIGHT              320

// Compatibility aliases used by some code
#define EINK_WIDTH              EPD_WIDTH
#define EINK_HEIGHT             EPD_HEIGHT
#define TDECK_PRO_EPD_CS        EPD_CS
#define TDECK_PRO_EPD_DC        EPD_DC
#define TDECK_PRO_EPD_RST       EPD_RST
#define TDECK_PRO_EPD_BUSY      EPD_BUSY

#define TFT_CS                  EPD_CS
#define TFT_DC                  EPD_DC
#define TFT_BL                  BOARD_KEYBOARD_LED
#define TFT_WIDTH               EPD_WIDTH
#define TFT_HEIGHT              EPD_HEIGHT
#define TFT_SPI_FREQ            4000000
#define TFT_RST                 EPD_RST
#define TFT_ROTATION            0
#define TFT_BL_INVERT           false
#define TFT_PWM_FREQ            5000

// --- I2C Bus ---
#define I2C_SDA                 13
#define I2C_SCL                 14
#define I2C_FREQUENCY           400000

// --- Keyboard ---
#define KB_I2C_ADDR             0x34
#define KB_INT                  15
#define KB_LED                  BOARD_KEYBOARD_LED
#define KB_ROWS                 4
#define KB_COLS                 10

// --- Touch ---
#define TOUCH_INT               12
#define TOUCH_RST               45
#define TOUCH_I2C_ADDR          0x1A
#define TOUCH_I2C_ADDR_1        TOUCH_I2C_ADDR
#define TOUCH_I2C_ADDR_2        TOUCH_I2C_ADDR

// --- Trackball stubs ---
#define TBALL_UP                -1
#define TBALL_DOWN              -1
#define TBALL_LEFT              -1
#define TBALL_RIGHT             -1
#define TBALL_CLICK             -1

// --- SD Card ---
#define SD_CS                   48

// --- GPS ---
#define GPS_TX                  43
#define GPS_RX                  44
#define GPS_PPS                  1
#define GPS_BAUD                38400
#define GPS_EN                  BOARD_GPS_EN

// --- Battery / PMU ---
#define BAT_ADC_PIN             -1
#define BQ27220_I2C_ADDR        0x55
#define BQ25896_I2C_ADDR        0x6B

// --- Audio (PCM5102A Voice variant) ---
#define I2S_BCK                 7
#define I2S_DOUT                8
#define I2S_WS                  9
#define I2S_DIN                 -1
#define I2S_SCK                 -1
#define I2S_MCLK                -1

#define MIC_DATA                17
#define MIC_CLOCK               18

// --- Boot ---
#define BOARD_BOOT_PIN           0

// --- Hardware Constants ---
#define MAX_PACKET_SIZE         255
#define SPI_FREQUENCY            8000000