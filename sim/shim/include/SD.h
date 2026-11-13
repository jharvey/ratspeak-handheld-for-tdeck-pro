#pragma once
#include "FS.h"
#include "SPI.h"
typedef enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC, CARD_UNKNOWN } sdcard_type_t;
namespace fs {
class SDFS : public FS {
public:
    SDFS() : FS("sd") {}
    bool begin(uint8_t ssPin = 0, SPIClass& spi = SPI, uint32_t freq = 4000000,
               const char* mount = "/sd", uint8_t maxFiles = 5, bool formatIfEmpty = false);
    void end();
    sdcard_type_t cardType();
    uint64_t cardSize();                  // the simulated card's size (--sd 32G)
    uint64_t totalBytes();
    uint64_t usedBytes();
private:
    bool mounted_ = false;
};
}  // namespace fs
extern fs::SDFS SD;
