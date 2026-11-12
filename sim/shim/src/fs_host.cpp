// LittleFS / SD on host folders: <data>/littlefs and <data>/sd.
#include "FS.h"
#include "LittleFS.h"
#include "SD.h"
#include "hw/SimHw.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fsx = std::filesystem;

fs::LittleFSFS LittleFS;
fs::SDFS SD;

namespace sim {
std::string dataRoot() {
    std::lock_guard<std::mutex> l(hw().mu);
    return hw().dataDir;
}
// "/littlefs/a/b" -> <data>/littlefs/a/b ; "/sd/a" -> <data>/sd/a
std::string vfsHostPath(const char* path) {
    std::string p = path ? path : "";
    std::string sub = "vfs";
    for (const char* m : {"/littlefs", "/sd"}) {
        const size_t n = strlen(m);
        if (p.compare(0, n, m) == 0 && (p.size() == n || p[n] == '/')) { sub = m + 1; p = p.substr(n); break; }
    }
    return (fsx::path(dataRoot()) / sub / fsx::path(p).relative_path()).string();
}
}  // namespace sim

namespace {

// ---- flash space model: LittleFS allocates whole 4 KB blocks; a directory is a metadata pair.
constexpr uint64_t kBlock = 4096;
constexpr uint64_t kDirBlocks = 2;
uint64_t blocksFor(uint64_t size) { return (size + kBlock - 1) / kBlock; }
struct FlashSpace {
    std::mutex mu;
    bool scanned = false;
    uint64_t used = 0;  // blocks
} g_flash;
uint64_t flashCapacityBlocks() {
    std::lock_guard<std::mutex> l(sim::hw().mu);
    return sim::hw().flashBytes / kBlock;
}
void flashScanLocked(const std::string& root) {
    std::error_code ec;
    uint64_t n = kDirBlocks;
    for (auto& e : fsx::recursive_directory_iterator(root, ec)) {
        if (e.is_directory(ec)) n += kDirBlocks;
        else if (e.is_regular_file(ec)) n += blocksFor(e.file_size(ec));
    }
    g_flash.used = n;
    g_flash.scanned = true;
}
std::string flashRoot() { return (fsx::path(sim::dataRoot()) / "littlefs").string(); }
// Reserve (delta > 0) or release blocks; false = no space (nothing changed).
bool flashAdjust(int64_t delta) {
    std::lock_guard<std::mutex> l(g_flash.mu);
    if (!g_flash.scanned) flashScanLocked(flashRoot());
    if (delta > 0 && g_flash.used + (uint64_t)delta > flashCapacityBlocks()) return false;
    if (delta < 0 && (uint64_t)(-delta) > g_flash.used) g_flash.used = 0;
    else g_flash.used += delta;
    return true;
}

class HostFileImpl final : public fs::FileImpl {
public:
    HostFileImpl(std::string vpath, std::string host, FILE* f, bool flash)
        : vpath_(std::move(vpath)), host_(std::move(host)), f_(f), flash_(flash) { setName(); }
    HostFileImpl(std::string vpath, std::string host, bool dir, bool flash)
        : vpath_(std::move(vpath)), host_(std::move(host)), dir_(dir), flash_(flash) {
        setName();
        std::error_code ec;
        for (auto& e : fsx::directory_iterator(host_, ec)) entries_.push_back(e.path().filename().string());
        std::sort(entries_.begin(), entries_.end());
    }
    ~HostFileImpl() override { close(); }

    size_t write(const uint8_t* b, size_t n) override {
        if (!f_) return 0;
        if (flash_ && n) {  // a write that needs new blocks fails when flash is full (LFS_ERR_NOSPC)
            const uint64_t end = size(), pos = position();
            const uint64_t grown = std::max<uint64_t>(end, pos + n);
            const int64_t delta = (int64_t)blocksFor(grown) - (int64_t)blocksFor(end);
            if (delta > 0 && !flashAdjust(delta)) return 0;
        }
        return fwrite(b, 1, n, f_);
    }
    size_t read(uint8_t* b, size_t n) override { return f_ ? fread(b, 1, n, f_) : 0; }
    void flush() override { if (f_) fflush(f_); }
    bool seek(uint32_t pos, fs::SeekMode mode) override {
        if (!f_) return false;
        const int w = mode == fs::SeekSet ? SEEK_SET : mode == fs::SeekCur ? SEEK_CUR : SEEK_END;
        return fseek(f_, (long)pos, w) == 0;
    }
    size_t position() const override { return f_ ? (size_t)ftell(f_) : 0; }
    size_t size() const override {
        if (!f_) return 0;
        const long cur = ftell(f_);
        fseek(f_, 0, SEEK_END);
        const long end = ftell(f_);
        fseek(f_, cur, SEEK_SET);
        return end < 0 ? 0 : (size_t)end;
    }
    bool setBufferSize(size_t) override { return true; }
    void close() override { if (f_) { fclose(f_); f_ = nullptr; } open_ = false; }
    time_t getLastWrite() override { return 0; }
    const char* path() const override { return vpath_.c_str(); }
    const char* name() const override { return name_.c_str(); }
    boolean isDirectory(void) override { return dir_; }
    fs::FileImplPtr openNextFile(const char* mode) override;
    boolean seekDir(long position) override { pos_ = (size_t)std::max(0L, position); return true; }
    String getNextFileName(void) override { bool d; return getNextFileName(&d); }
    String getNextFileName(bool* isDir) override {
        if (!dir_ || pos_ >= entries_.size()) return String();
        const std::string& n = entries_[pos_++];
        std::error_code ec;
        if (isDir) *isDir = fsx::is_directory(fsx::path(host_) / n, ec);
        return String(joinV(n).c_str());
    }
    void rewindDirectory(void) override { pos_ = 0; }
    operator bool() override { return open_ && (dir_ || f_); }

private:
    void setName() {
        const size_t s = vpath_.find_last_of('/');
        name_ = s == std::string::npos ? vpath_ : vpath_.substr(s + 1);
    }
    std::string joinV(const std::string& n) const {
        return vpath_ == "/" ? "/" + n : vpath_ + "/" + n;
    }
    std::string vpath_, host_, name_;
    FILE* f_ = nullptr;
    bool dir_ = false, open_ = true, flash_ = false;
    std::vector<std::string> entries_;
    size_t pos_ = 0;
};

fs::FileImplPtr openHost(const std::string& vpath, const std::string& host, const char* mode, bool create,
                         bool flash) {
    std::error_code ec;
    if (fsx::is_directory(host, ec)) return std::make_shared<HostFileImpl>(vpath, host, true, flash);
    const char m = mode && *mode ? mode[0] : 'r';
    if (m == 'r' && !fsx::exists(host, ec)) return {};
    if (m != 'r') {
        const auto parent = fsx::path(host).parent_path();
        if (!fsx::is_directory(parent, ec)) {
            if (!create) return {};
            if (flash) {
                int64_t made = 0;
                for (auto p = parent; !p.empty() && !fsx::exists(p, ec); p = p.parent_path()) made += kDirBlocks;
                if (!flashAdjust(made)) return {};
            }
            fsx::create_directories(parent, ec);
        }
    }
    const char* cm = m == 'w' ? (strchr(mode, '+') ? "w+b" : "wb")
                   : m == 'a' ? (strchr(mode, '+') ? "a+b" : "ab")
                   : (strchr(mode, '+') ? "r+b" : "rb");
    if (flash && m == 'w' && fsx::is_regular_file(host, ec)) flashAdjust(-(int64_t)blocksFor(fsx::file_size(host, ec)));
    FILE* f = fopen(host.c_str(), cm);
    if (!f) return {};
    return std::make_shared<HostFileImpl>(vpath, host, f, flash);
}

fs::FileImplPtr HostFileImpl::openNextFile(const char* mode) {
    if (!dir_ || pos_ >= entries_.size()) return {};
    const std::string& n = entries_[pos_++];
    return openHost(joinV(n), (fsx::path(host_) / n).string(), mode, false, flash_);
}

std::string normV(const char* p) {
    std::string s = p ? p : "/";
    if (s.empty() || s[0] != '/') s = "/" + s;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

namespace fs {

std::string FS::root() const { return (fsx::path(sim::dataRoot()) / sub_).string(); }
std::string FS::hostPath(const char* path) const {
    return (fsx::path(root()) / fsx::path(normV(path)).relative_path()).string();
}
File FS::open(const char* path, const char* mode, const bool create) {
    const std::string v = normV(path);
    return File(openHost(v, hostPath(v.c_str()), mode, create, sub_ == "littlefs"));
}
bool FS::exists(const char* path) { std::error_code ec; return fsx::exists(hostPath(path), ec); }
bool FS::remove(const char* path) {
    std::error_code ec;
    const auto h = hostPath(path);
    if (fsx::is_directory(h, ec) || !fsx::exists(h, ec)) return false;
    const uint64_t size = fsx::file_size(h, ec);
    if (!fsx::remove(h, ec)) return false;
    if (sub_ == "littlefs") flashAdjust(-(int64_t)blocksFor(size));
    return true;
}
bool FS::rename(const char* a, const char* b) {
    std::error_code ec;
    const auto ha = hostPath(a), hb = hostPath(b);
    if (!fsx::exists(ha, ec)) return false;
    const bool hadTarget = fsx::is_regular_file(hb, ec);
    const uint64_t replaced = hadTarget ? fsx::file_size(hb, ec) : 0;
    fsx::rename(ha, hb, ec);       // replaces an existing target, like LittleFS
    if (!ec && hadTarget && sub_ == "littlefs") flashAdjust(-(int64_t)blocksFor(replaced));
    return !ec;
}
bool FS::mkdir(const char* path) {
    std::error_code ec;
    const auto h = hostPath(path);
    if (fsx::is_directory(h, ec)) return true;
    if (sub_ == "littlefs" && !flashAdjust(kDirBlocks)) return false;
    if (fsx::create_directory(h, ec)) return true;
    if (sub_ == "littlefs") flashAdjust(-(int64_t)kDirBlocks);
    return false;
}
bool FS::rmdir(const char* path) {
    std::error_code ec;
    const auto h = hostPath(path);
    if (!(fsx::is_directory(h, ec) && fsx::is_empty(h, ec) && fsx::remove(h, ec))) return false;
    if (sub_ == "littlefs") flashAdjust(-(int64_t)kDirBlocks);
    return true;
}

bool LittleFSFS::begin(bool, const char*, uint8_t, const char*) {
    std::error_code ec;
    fsx::create_directories(root(), ec);
    std::lock_guard<std::mutex> l(g_flash.mu);
    flashScanLocked(root());
    return !ec;
}
bool LittleFSFS::format() {
    std::error_code ec;
    fsx::remove_all(root(), ec);
    fsx::create_directories(root(), ec);
    std::lock_guard<std::mutex> l(g_flash.mu);
    flashScanLocked(root());
    return !ec;
}
size_t LittleFSFS::totalBytes() {
    std::lock_guard<std::mutex> l(sim::hw().mu);
    return (size_t)sim::hw().flashBytes;
}
size_t LittleFSFS::usedBytes() {  // whole blocks, like LittleFS
    std::lock_guard<std::mutex> l(g_flash.mu);
    if (!g_flash.scanned) flashScanLocked(root());
    return (size_t)(g_flash.used * kBlock);
}

bool SDFS::begin(uint8_t, SPIClass&, uint32_t, const char*, uint8_t, bool) {
    { std::lock_guard<std::mutex> l(sim::hw().mu); if (!sim::hw().sdPresent) { mounted_ = false; return false; } }
    std::error_code ec;
    fsx::create_directories(root(), ec);
    mounted_ = !ec;
    return mounted_;
}
void SDFS::end() { mounted_ = false; }
sdcard_type_t SDFS::cardType() { return mounted_ ? CARD_SDHC : CARD_NONE; }
uint64_t SDFS::usedBytes() {
    std::error_code ec;
    uint64_t n = 0;
    for (auto& e : fsx::recursive_directory_iterator(root(), ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}

}  // namespace fs
