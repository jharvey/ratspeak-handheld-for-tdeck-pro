#pragma once
// Low-level pieces for "Update firmware from SD" and "Back up / restore" (S.11.5):
//
//  * NVS (Preferences) listing and raw writes, so a backup can carry every setting,
//    the identity key and the message index without knowing each namespace's layout.
//  * Writing a firmware image into the other app slot (app0 <-> app1), with the
//    bootloader's rollback: a new image that does not reach markValid() (crash or reset
//    in its first seconds) is dropped by the bootloader and the old one boots again.
//  * bootCheck(): after an SD update the unit runs from app1. A later USB flash with
//    esptool writes app0 (0x10000) only; bootCheck() notices that app0 changed since the
//    update and boots it, so USB flashing keeps working as before.
//
// The simulator has its own stand-in (sim/shim/src/sysimage_sim.cpp).
#include <stddef.h>
#include <stdint.h>

namespace sysimg {

// NVS value types (same codes as ESP-IDF nvs_type_t). Numbers are little-endian with the
// type's size; strings carry no terminating NUL.
enum : uint8_t {
    T_U8 = 0x01, T_I8 = 0x11, T_U16 = 0x02, T_I16 = 0x12, T_U32 = 0x04, T_I32 = 0x14,
    T_U64 = 0x08, T_I64 = 0x18, T_STR = 0x21, T_BLOB = 0x42
};

// Calls visit() for every entry in every namespace; returns the number visited, -1 on error.
// visit() returns false to stop.
using NvsVisit = bool (*)(const char* ns, const char* key, uint8_t type, const uint8_t* data,
                          size_t len, void* ctx);
int nvsForEach(NvsVisit visit, void* ctx);
bool nvsEraseNamespace(const char* ns);
bool nvsPut(const char* ns, const char* key, uint8_t type, const uint8_t* data, size_t len);

// Firmware image into the other app slot. otaCheck() says why it cannot be done (layout,
// size); begin/write/end stream the file; end() checks the image and sets it to boot next.
bool otaCheck(uint32_t size, char* why, size_t whyCap);
bool otaBegin(uint32_t size, char* why, size_t whyCap);
bool otaWrite(const uint8_t* data, size_t len);
bool otaEnd(char* why, size_t whyCap);
void otaAbort();
const char* runningSlot();          // "app0" / "app1"

void bootCheck();                   // early in boot: USB-flashed app0 wins over an older SD update
bool pendingVerify();               // this image is new and not yet confirmed
void markValid();                   // confirm the running image (cancels the rollback)

}  // namespace sysimg
