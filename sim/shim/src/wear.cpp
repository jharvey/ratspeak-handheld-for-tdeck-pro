// Flash wear counters (simulator only): every write to the LittleFS partition and every
// changed NVS (Preferences) value is counted per path pattern / per namespace:key, and
// the totals are written to <data>/wear.txt at most every 10 s. tools/stress/wear.py
// turns them into erase cycles per day and years to wear-out.
//
// Path patterns: runs of hex digits / digits in a file name become '#', so all message
// records of all conversations count as one row.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace sim {
std::string dataRoot();
uint32_t nowMs();
}

namespace {
struct Row {
    uint64_t ops = 0;      // writes (flash: open-for-write sessions; nvs: changed puts)
    uint64_t bytes = 0;    // bytes written (nvs: data bytes)
    uint64_t entries = 0;  // nvs: 32-byte entries written
};
std::mutex g_mu;
std::map<std::string, Row> g_flash, g_nvs, g_meta;
uint32_t g_lastDump = 0;


std::string pattern(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (size_t i = 0; i < path.size();) {
        if (isxdigit((unsigned char)path[i])) {
            size_t j = i;
            bool digit = false;
            while (j < path.size() && isxdigit((unsigned char)path[j])) { digit |= isdigit((unsigned char)path[j]) != 0; j++; }
            // Hex runs of 6+ chars or runs with a digit are ids; short letter runs ("bak") stay.
            if (j - i >= 6 || digit) out += '#';
            else out.append(path, i, j - i);
            i = j;
            continue;
        }
        out += path[i++];
    }
    return out;
}

void dumpLocked(bool force) {
    const uint32_t now = sim::nowMs();
    if (!force && now - g_lastDump < 10000) return;
    g_lastDump = now;
    const std::string root = sim::dataRoot();
    if (root.empty()) return;
    const std::string path = (std::filesystem::path(root) / "wear.txt").string();
    std::ofstream out(path + ".tmp", std::ios::trunc);
    out << "uptime_s\t" << now / 1000 << "\n";
    for (auto& [k, r] : g_flash) out << "flash\t" << k << "\t" << r.ops << "\t" << r.bytes << "\n";
    for (auto& [k, r] : g_meta) out << "meta\t" << k << "\t" << r.ops << "\t0\n";
    for (auto& [k, r] : g_nvs) out << "nvs\t" << k << "\t" << r.ops << "\t" << r.bytes << "\t" << r.entries << "\n";
    out.close();
    std::error_code ec;
    std::filesystem::rename(path + ".tmp", path, ec);
}
}  // namespace

namespace sim {
// A LittleFS file was written (one open..close session): total bytes.
void wearFlashWrite(const std::string& vpath, uint64_t bytes) {
    std::lock_guard<std::mutex> l(g_mu);
    Row& r = g_flash[pattern(vpath)];
    r.ops++; r.bytes += bytes;
    dumpLocked(false);
}
// A LittleFS metadata change: "remove", "rename", "mkdir".
void wearFlashMeta(const char* what, const std::string& vpath) {
    std::lock_guard<std::mutex> l(g_mu);
    g_meta[std::string(what) + " " + pattern(vpath)].ops++;
    dumpLocked(false);
}
// An NVS value changed (ESP-IDF skips writing an unchanged value). NVS stores a value in
// 32-byte entries: numbers take 1, strings / blobs 1 header + ceil(n/32) data entries.
void wearNvs(const std::string& ns, const std::string& key, size_t bytes, bool blob) {
    std::lock_guard<std::mutex> l(g_mu);
    Row& r = g_nvs[ns + ":" + key];
    r.ops++; r.bytes += bytes;
    r.entries += blob ? 1 + (bytes + 31) / 32 : 1;
    dumpLocked(false);
}
void wearDump() {
    std::lock_guard<std::mutex> l(g_mu);
    dumpLocked(true);
}
}  // namespace sim
