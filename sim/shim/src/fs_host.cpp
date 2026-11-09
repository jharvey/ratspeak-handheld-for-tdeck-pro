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

class HostFileImpl final : public fs::FileImpl {
public:
    HostFileImpl(std::string vpath, std::string host, FILE* f)
        : vpath_(std::move(vpath)), host_(std::move(host)), f_(f) { setName(); }
    HostFileImpl(std::string vpath, std::string host, bool dir)
        : vpath_(std::move(vpath)), host_(std::move(host)), dir_(dir) {
        setName();
        std::error_code ec;
        for (auto& e : fsx::directory_iterator(host_, ec)) entries_.push_back(e.path().filename().string());
        std::sort(entries_.begin(), entries_.end());
    }
    ~HostFileImpl() override { close(); }

    size_t write(const uint8_t* b, size_t n) override { return f_ ? fwrite(b, 1, n, f_) : 0; }
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
    bool dir_ = false, open_ = true;
    std::vector<std::string> entries_;
    size_t pos_ = 0;
};

fs::FileImplPtr openHost(const std::string& vpath, const std::string& host, const char* mode, bool create) {
    std::error_code ec;
    if (fsx::is_directory(host, ec)) return std::make_shared<HostFileImpl>(vpath, host, true);
    const char m = mode && *mode ? mode[0] : 'r';
    if (m == 'r' && !fsx::exists(host, ec)) return {};
    if (m != 'r') {
        const auto parent = fsx::path(host).parent_path();
        if (!fsx::is_directory(parent, ec)) {
            if (!create) return {};
            fsx::create_directories(parent, ec);
        }
    }
    const char* cm = m == 'w' ? (strchr(mode, '+') ? "w+b" : "wb")
                   : m == 'a' ? (strchr(mode, '+') ? "a+b" : "ab")
                   : (strchr(mode, '+') ? "r+b" : "rb");
    FILE* f = fopen(host.c_str(), cm);
    if (!f) return {};
    return std::make_shared<HostFileImpl>(vpath, host, f);
}

fs::FileImplPtr HostFileImpl::openNextFile(const char* mode) {
    if (!dir_ || pos_ >= entries_.size()) return {};
    const std::string& n = entries_[pos_++];
    return openHost(joinV(n), (fsx::path(host_) / n).string(), mode, false);
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
    return File(openHost(v, hostPath(v.c_str()), mode, create));
}
bool FS::exists(const char* path) { std::error_code ec; return fsx::exists(hostPath(path), ec); }
bool FS::remove(const char* path) {
    std::error_code ec;
    const auto h = hostPath(path);
    return !fsx::is_directory(h, ec) && fsx::remove(h, ec);
}
bool FS::rename(const char* a, const char* b) {
    std::error_code ec;
    const auto ha = hostPath(a), hb = hostPath(b);
    if (!fsx::exists(ha, ec)) return false;
    fsx::rename(ha, hb, ec);       // replaces an existing target, like LittleFS
    return !ec;
}
bool FS::mkdir(const char* path) {
    std::error_code ec;
    const auto h = hostPath(path);
    if (fsx::is_directory(h, ec)) return true;
    return fsx::create_directory(h, ec);
}
bool FS::rmdir(const char* path) {
    std::error_code ec;
    const auto h = hostPath(path);
    return fsx::is_directory(h, ec) && fsx::is_empty(h, ec) && fsx::remove(h, ec);
}

bool LittleFSFS::begin(bool, const char*, uint8_t, const char*) {
    std::error_code ec;
    fsx::create_directories(root(), ec);
    return !ec;
}
bool LittleFSFS::format() {
    std::error_code ec;
    fsx::remove_all(root(), ec);
    fsx::create_directories(root(), ec);
    return !ec;
}
size_t LittleFSFS::usedBytes() {
    std::error_code ec;
    size_t n = 0;
    for (auto& e : fsx::recursive_directory_iterator(root(), ec))
        if (e.is_regular_file(ec)) n += (size_t)e.file_size(ec);
    return n;
}

bool SDFS::begin(uint8_t, SPIClass&, uint32_t, const char*, uint8_t, bool) {
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
