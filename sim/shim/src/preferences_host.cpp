// NVS (Preferences) shim: <data>/nvs/<namespace>.nvs, one "key<TAB>type<TAB>hex" per line.
#include "Preferences.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <chrono>
#include <mutex>
#include <thread>
#include <sstream>
#include <vector>

namespace fsx = std::filesystem;
namespace sim {
std::string dataRoot();
void wearNvs(const std::string& ns, const std::string& key, size_t bytes, bool blob);
}

namespace {
struct Entry { PreferenceType type; std::vector<uint8_t> data; };
using Ns = std::map<std::string, Entry>;
std::mutex g_mu;
std::map<std::string, Ns> g_cache;

std::string nsPath(const std::string& ns) { return (fsx::path(sim::dataRoot()) / "nvs" / (ns + ".nvs")).string(); }

Ns& load(const std::string& ns) {   // caller holds g_mu
    auto it = g_cache.find(ns);
    if (it != g_cache.end()) return it->second;
    Ns& m = g_cache[ns];
    std::ifstream in(nsPath(ns));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string key, hex; int t;
        if (!std::getline(ss, key, '\t') || !(ss >> t)) continue;
        ss.get();
        std::getline(ss, hex);
        Entry e{(PreferenceType)t, {}};
        for (size_t i = 0; i + 1 < hex.size(); i += 2) e.data.push_back((uint8_t)std::stoi(hex.substr(i, 2), nullptr, 16));
        m[key] = std::move(e);
    }
    return m;
}
void save(const std::string& ns) {   // caller holds g_mu
    std::error_code ec;
    fsx::create_directories(fsx::path(nsPath(ns)).parent_path(), ec);
    const std::string tmp = nsPath(ns) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        static const char* hx = "0123456789abcdef";
        for (auto& [k, e] : g_cache[ns]) {
            out << k << '\t' << (int)e.type << '\t';
            for (uint8_t b : e.data) out << hx[b >> 4] << hx[b & 15];
            out << '\n';
        }
    }
    // Windows: the rename fails while another program (indexer, antivirus) has the file
    // open for a moment; a lost save looked like settings going back after a restart.
    for (int i = 0; i < 50; i++) {
        ec.clear();
        fsx::rename(tmp, nsPath(ns), ec);
        if (!ec) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fprintf(stderr, "[SIM] NVS save failed: %s (%s)\n", nsPath(ns).c_str(), ec.message().c_str());
}
}  // namespace

bool Preferences::begin(const char* name, bool readOnly, const char*) {
    if (!name || strlen(name) > 15) return false;     // NVS namespace limit
    std::lock_guard<std::mutex> l(g_mu);
    ns_ = name; ro_ = readOnly; started_ = true;
    load(ns_);
    return true;
}
void Preferences::end() { started_ = false; }
bool Preferences::clear() {
    if (!started_ || ro_) return false;
    std::lock_guard<std::mutex> l(g_mu);
    load(ns_).clear(); save(ns_);
    return true;
}
bool Preferences::remove(const char* key) {
    if (!started_ || ro_) return false;
    std::lock_guard<std::mutex> l(g_mu);
    const bool had = load(ns_).erase(key) > 0;
    if (had) save(ns_);
    return had;
}
bool Preferences::isKey(const char* key) {
    if (!started_) return false;
    std::lock_guard<std::mutex> l(g_mu);
    return load(ns_).count(key) > 0;
}
PreferenceType Preferences::getType(const char* key) {
    if (!started_) return PT_INVALID;
    std::lock_guard<std::mutex> l(g_mu);
    auto& m = load(ns_);
    auto it = m.find(key);
    return it == m.end() ? PT_INVALID : it->second.type;
}

size_t Preferences::putNum(const char* k, PreferenceType t, int64_t v, size_t sz) {
    if (!started_ || ro_ || !k || strlen(k) > 15) return 0;   // NVS key limit
    std::lock_guard<std::mutex> l(g_mu);
    Entry e{t, std::vector<uint8_t>((const uint8_t*)&v, (const uint8_t*)&v + 8)};
    auto& m = load(ns_);
    auto it = m.find(k);
    if (it == m.end() || it->second.type != t || it->second.data != e.data) sim::wearNvs(ns_, k, sz, false);
    m[k] = e;
    save(ns_);
    return sz;
}
int64_t Preferences::getNum(const char* k, int64_t d) {
    if (!started_) return d;
    std::lock_guard<std::mutex> l(g_mu);
    auto& m = load(ns_);
    auto it = m.find(k);
    if (it == m.end() || it->second.type >= PT_STR || it->second.data.size() < 8) return d;
    int64_t v; memcpy(&v, it->second.data.data(), 8);
    return v;
}
size_t Preferences::putFloat(const char* k, float v) { return putBytes(k, &v, sizeof(v)) ? sizeof(v) : 0; }
size_t Preferences::putDouble(const char* k, double v) { return putBytes(k, &v, sizeof(v)) ? sizeof(v) : 0; }
float Preferences::getFloat(const char* k, float d) { float v; return getBytes(k, &v, sizeof(v)) == sizeof(v) ? v : d; }
double Preferences::getDouble(const char* k, double d) { double v; return getBytes(k, &v, sizeof(v)) == sizeof(v) ? v : d; }

size_t Preferences::putString(const char* k, const char* v) {
    if (!started_ || ro_ || !k || !v || strlen(k) > 15) return 0;
    std::lock_guard<std::mutex> l(g_mu);
    Entry e{PT_STR, std::vector<uint8_t>(v, v + strlen(v))};
    auto& m = load(ns_);
    auto it = m.find(k);
    if (it == m.end() || it->second.type != PT_STR || it->second.data != e.data) sim::wearNvs(ns_, k, strlen(v) + 1, true);
    m[k] = std::move(e);
    save(ns_);
    return strlen(v);
}
size_t Preferences::putBytes(const char* k, const void* v, size_t n) {
    if (!started_ || ro_ || !k || (!v && n) || strlen(k) > 15) return 0;
    std::lock_guard<std::mutex> l(g_mu);
    Entry e{PT_BLOB, std::vector<uint8_t>((const uint8_t*)v, (const uint8_t*)v + n)};
    auto& m = load(ns_);
    auto it = m.find(k);
    if (it == m.end() || it->second.type != PT_BLOB || it->second.data != e.data) sim::wearNvs(ns_, k, n, true);
    m[k] = std::move(e);
    save(ns_);
    return n;
}
size_t Preferences::getString(const char* k, char* out, size_t max) {
    String s = getString(k, String());
    if (!out || !max) return s.length() + 1;
    size_t n = std::min(max - 1, s.length());
    memcpy(out, s.c_str(), n); out[n] = 0;
    return n + 1;
}
String Preferences::getString(const char* k, const String& d) {
    if (!started_) return d;
    std::lock_guard<std::mutex> l(g_mu);
    auto& m = load(ns_);
    auto it = m.find(k);
    if (it == m.end() || it->second.type != PT_STR) return d;
    return String((const char*)it->second.data.data(), it->second.data.size());
}
size_t Preferences::getBytesLength(const char* k) {
    if (!started_) return 0;
    std::lock_guard<std::mutex> l(g_mu);
    auto& m = load(ns_);
    auto it = m.find(k);
    return it == m.end() || it->second.type != PT_BLOB ? 0 : it->second.data.size();
}
size_t Preferences::getBytes(const char* k, void* out, size_t max) {
    if (!started_) return 0;
    std::lock_guard<std::mutex> l(g_mu);
    auto& m = load(ns_);
    auto it = m.find(k);
    if (it == m.end() || it->second.type != PT_BLOB || it->second.data.size() > max) return 0;
    memcpy(out, it->second.data.data(), it->second.data.size());
    return it->second.data.size();
}

// ---- Raw access for hal/SysImage (backup / restore): every namespace on disk ------------
namespace sim {
namespace {
const uint8_t kIdfType[] = {0x11, 0x01, 0x12, 0x02, 0x14, 0x04, 0x18, 0x08, 0x21, 0x42};   // by PreferenceType
size_t numSize(PreferenceType t) { return t >= PT_STR ? 0 : (size_t)1 << (t / 2); }
}

int nvsForEach(bool (*visit)(const char*, const char*, uint8_t, const uint8_t*, size_t, void*), void* ctx) {
    std::vector<std::string> names;
    std::error_code ec;
    for (auto& e : fsx::directory_iterator(fsx::path(dataRoot()) / "nvs", ec))
        if (e.path().extension() == ".nvs") names.push_back(e.path().stem().string());
    int n = 0;
    for (auto& ns : names) {
        Ns copy;
        { std::lock_guard<std::mutex> l(g_mu); copy = load(ns); }
        for (auto& [k, e] : copy) {
            if (e.type >= PT_INVALID) continue;
            const size_t len = e.type >= PT_STR ? e.data.size() : numSize(e.type);
            n++;
            if (!visit(ns.c_str(), k.c_str(), kIdfType[e.type], e.data.data(), len, ctx)) return n;
        }
    }
    return n;
}

bool nvsEraseNamespace(const char* ns) {
    std::lock_guard<std::mutex> l(g_mu);
    load(ns).clear();
    save(ns);
    return true;
}

bool nvsPut(const char* ns, const char* key, uint8_t type, const uint8_t* d, size_t len) {
    int t = -1;
    for (int i = 0; i < (int)sizeof(kIdfType); i++) if (kIdfType[i] == type) t = i;
    if (t < 0 || !key || strlen(key) > 15) return false;
    Entry e{(PreferenceType)t, {}};
    if (t >= PT_STR) e.data.assign(d, d + len);
    else {
        if (len != numSize((PreferenceType)t)) return false;
        int64_t v = 0;
        memcpy(&v, d, len);
        const bool isSigned = (t % 2) == 0;
        if (isSigned && len < 8 && (d[len - 1] & 0x80)) v |= ~(((int64_t)1 << (len * 8)) - 1);
        e.data.assign((const uint8_t*)&v, (const uint8_t*)&v + 8);
    }
    std::lock_guard<std::mutex> l(g_mu);
    load(ns)[key] = std::move(e);
    save(ns);
    return true;
}
}  // namespace sim
