#pragma once
// fs::FS / File backed by host directories:
//   LittleFS -> <data>/littlefs     SD -> <data>/sd
#include <string>
#include "Arduino.h"
#include "FSImpl.h"

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

namespace fs {

class File : public Stream {
public:
    File(FileImplPtr p = FileImplPtr()) : p_(std::move(p)) {}
    size_t write(uint8_t c) override { return p_ ? p_->write(&c, 1) : 0; }
    size_t write(const uint8_t* b, size_t n) override { return p_ ? p_->write(b, n) : 0; }
    using Print::write;
    int available() override { return p_ ? (int)(p_->size() - p_->position()) : 0; }
    int read() override { uint8_t c; return p_ && p_->read(&c, 1) == 1 ? c : -1; }
    int peek() override {
        if (!p_) return -1;
        size_t pos = p_->position(); int c = read(); p_->seek((uint32_t)pos, SeekSet); return c;
    }
    void flush() override { if (p_) p_->flush(); }
    size_t read(uint8_t* buf, size_t n) { return p_ ? p_->read(buf, n) : 0; }
    size_t readBytes(char* buf, size_t n) { return read((uint8_t*)buf, n); }
    size_t readBytes(uint8_t* buf, size_t n) { return read(buf, n); }
    bool seek(uint32_t pos, SeekMode mode) { return p_ && p_->seek(pos, mode); }
    bool seek(uint32_t pos) { return seek(pos, SeekSet); }
    size_t position() const { return p_ ? p_->position() : 0; }
    size_t size() const { return p_ ? p_->size() : 0; }
    bool setBufferSize(size_t n) { return p_ && p_->setBufferSize(n); }
    void close() { if (p_) { p_->close(); p_.reset(); } }
    operator bool() const { return p_ && (bool)*p_; }
    time_t getLastWrite() { return p_ ? p_->getLastWrite() : 0; }
    const char* path() const { return p_ ? p_->path() : nullptr; }
    const char* name() const { return p_ ? p_->name() : nullptr; }
    boolean isDirectory(void) { return p_ && p_->isDirectory(); }
    boolean seekDir(long pos) { return p_ && p_->seekDir(pos); }
    File openNextFile(const char* mode = FILE_READ) { return p_ ? File(p_->openNextFile(mode)) : File(); }
    String getNextFileName(void) { return p_ ? p_->getNextFileName() : String(); }
    String getNextFileName(bool* isDir) { return p_ ? p_->getNextFileName(isDir) : String(); }
    void rewindDirectory(void) { if (p_) p_->rewindDirectory(); }
private:
    FileImplPtr p_;
};

class FS {
public:
    explicit FS(const char* sub) : sub_(sub) {}
    File open(const char* path, const char* mode = FILE_READ, const bool create = false);
    File open(const String& path, const char* mode = FILE_READ, const bool create = false) {
        return open(path.c_str(), mode, create);
    }
    bool exists(const char* path);
    bool exists(const String& path) { return exists(path.c_str()); }
    bool remove(const char* path);
    bool remove(const String& path) { return remove(path.c_str()); }
    bool rename(const char* a, const char* b);
    bool rename(const String& a, const String& b) { return rename(a.c_str(), b.c_str()); }
    bool mkdir(const char* path);
    bool mkdir(const String& path) { return mkdir(path.c_str()); }
    bool rmdir(const char* path);
    bool rmdir(const String& path) { return rmdir(path.c_str()); }
    std::string hostPath(const char* path) const;   // sim helper
    std::string root() const;
protected:
    std::string sub_;
};

}  // namespace fs

using fs::File;
using fs::FS;
using fs::SeekMode;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;
