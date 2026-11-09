#pragma once
// Mirrors Arduino-ESP32 2.0.x FSImpl.h (the firmware subclasses fs::FileImpl).
#include <memory>
#include "Arduino.h"

namespace fs {

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class FileImpl;
using FileImplPtr = std::shared_ptr<FileImpl>;

class FileImpl {
public:
    virtual ~FileImpl() {}
    virtual size_t write(const uint8_t* buf, size_t size) = 0;
    virtual size_t read(uint8_t* buf, size_t size) = 0;
    virtual void flush() = 0;
    virtual bool seek(uint32_t pos, SeekMode mode) = 0;
    virtual size_t position() const = 0;
    virtual size_t size() const = 0;
    virtual bool setBufferSize(size_t size) = 0;
    virtual void close() = 0;
    virtual time_t getLastWrite() = 0;
    virtual const char* path() const = 0;
    virtual const char* name() const = 0;
    virtual boolean isDirectory(void) = 0;
    virtual FileImplPtr openNextFile(const char* mode) = 0;
    virtual boolean seekDir(long position) { (void)position; return false; }
    virtual String getNextFileName(void) { return String(); }
    virtual String getNextFileName(bool* isDir) { if (isDir) *isDir = false; return String(); }
    virtual void rewindDirectory(void) = 0;
    virtual operator bool() = 0;
};

}  // namespace fs
