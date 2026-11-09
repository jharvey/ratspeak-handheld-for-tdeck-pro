#pragma once
#include "FS.h"
namespace fs {
class LittleFSFS : public FS {
public:
    LittleFSFS() : FS("littlefs") {}
    bool begin(bool formatOnFail = false, const char* basePath = "/littlefs", uint8_t maxOpen = 10,
               const char* label = "spiffs");
    bool format();
    size_t totalBytes() { return 6 * 1024 * 1024; }
    size_t usedBytes();
    void end() {}
};
}  // namespace fs
extern fs::LittleFSFS LittleFS;
