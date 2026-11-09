// newlib/POSIX pieces for the firmware: strlcpy, a device clock that
// settimeofday() can set, POSIX TZ strings, and dirent/stat over the sim VFS.
#define SIM_LIBC_IMPL 1
#include "sim_libc.h"
#undef time
#undef gettimeofday
#undef settimeofday
#undef setenv
#undef tzset
#undef localtime_r
#undef localtime
#undef gmtime_r
#undef mktime
#undef timegm
#undef stat

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "dirent.h"
#undef opendir
#undef readdir
#undef closedir
#undef rewinddir

namespace fsx = std::filesystem;
namespace sim { std::string vfsHostPath(const char* path); }

extern "C" size_t strlcpy(char* dst, const char* src, size_t n) {
    const size_t len = strlen(src);
    if (n) { size_t c = len < n - 1 ? len : n - 1; memcpy(dst, src, c); dst[c] = 0; }
    return len;
}
extern "C" size_t strlcat(char* dst, const char* src, size_t n) {
    const size_t d = strnlen(dst, n);
    if (d == n) return n + strlen(src);
    return d + strlcpy(dst + d, src, n - d);
}

// ---- device clock -----------------------------------------------------------------
// The device boots at the host's current UTC (like an RTC that kept time);
// settimeofday() shifts it without touching the PC clock.
static std::atomic<int64_t> g_offsetUs{0};
static int64_t hostUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
time_t simHostUtc() { return (time_t)(hostUs() / 1000000); }

extern "C" time_t sim_time(time_t* t) {
    const time_t v = (time_t)((hostUs() + g_offsetUs.load()) / 1000000);
    if (t) *t = v;
    return v;
}
extern "C" int sim_gettimeofday(struct timeval* tv, void*) {
    const int64_t us = hostUs() + g_offsetUs.load();
    if (tv) { tv->tv_sec = (long)(us / 1000000); tv->tv_usec = (long)(us % 1000000); }
    return 0;
}
extern "C" int sim_settimeofday(const struct timeval* tv, const void*) {
    if (tv) g_offsetUs = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec - hostUs();
    return 0;
}

// ---- POSIX TZ: std offset [dst [offset] [,Mm.w.d[/time],Mm.w.d[/time]]] ----------------
struct Rule { int m = 0, w = 0, d = 0, secs = 7200; };
struct Tz {
    long stdOff = 0;          // seconds to ADD to UTC for local standard time
    bool hasDst = false;
    long dstOff = 0;
    Rule start, end;
};
static std::mutex g_tzMu;
static Tz g_tz;
static std::string g_tzEnv = "UTC0";

static const char* parseName(const char* p) {
    if (*p == '<') { while (*p && *p != '>') p++; return *p ? p + 1 : p; }
    while (*p && isalpha((unsigned char)*p)) p++;
    return p;
}
static const char* parseOffset(const char* p, long& secs) {   // POSIX: west positive
    int sign = 1;
    if (*p == '+') p++; else if (*p == '-') { sign = -1; p++; }
    long h = 0, m = 0, s = 0;
    while (isdigit((unsigned char)*p)) h = h * 10 + (*p++ - '0');
    if (*p == ':') { p++; while (isdigit((unsigned char)*p)) m = m * 10 + (*p++ - '0'); }
    if (*p == ':') { p++; while (isdigit((unsigned char)*p)) s = s * 10 + (*p++ - '0'); }
    secs = sign * (h * 3600 + m * 60 + s);
    return p;
}
static const char* parseRule(const char* p, Rule& r) {
    if (*p != 'M') return p;   // only the Mm.w.d form (all the firmware uses)
    p++;
    r.m = (int)strtol(p, (char**)&p, 10); if (*p == '.') p++;
    r.w = (int)strtol(p, (char**)&p, 10); if (*p == '.') p++;
    r.d = (int)strtol(p, (char**)&p, 10);
    if (*p == '/') { long s; p = parseOffset(p + 1, s); r.secs = (int)s; }
    return p;
}
static void parseTz(const std::string& s, Tz& tz) {
    tz = Tz();
    const char* p = s.c_str();
    p = parseName(p);
    long off = 0;
    p = parseOffset(p, off);
    tz.stdOff = -off;
    if (!*p) return;
    const char* q = parseName(p);
    if (q == p) return;
    tz.hasDst = true;
    p = q;
    tz.dstOff = tz.stdOff + 3600;
    if (*p && *p != ',') { p = parseOffset(p, off); tz.dstOff = -off; }
    if (*p == ',') p = parseRule(p + 1, tz.start); else { tz.start = {3, 2, 0, 7200}; }
    if (*p == ',') p = parseRule(p + 1, tz.end); else { tz.end = {11, 1, 0, 7200}; }
}

static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (m <= 2));
}
static int64_t utcFromTm(const struct tm* t) {
    int64_t y = t->tm_year + 1900;
    int64_t mon = t->tm_mon;
    y += mon / 12; mon %= 12; if (mon < 0) { mon += 12; y--; }
    return daysFromCivil(y, (unsigned)mon + 1, 1) * 86400 + (int64_t)(t->tm_mday - 1) * 86400 +
           (int64_t)t->tm_hour * 3600 + (int64_t)t->tm_min * 60 + t->tm_sec;
}
static void tmFromUtc(int64_t secs, struct tm* out) {
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int y, m, d;
    civilFromDays(days, y, m, d);
    memset(out, 0, sizeof(*out));
    out->tm_year = y - 1900; out->tm_mon = m - 1; out->tm_mday = d;
    out->tm_hour = (int)(rem / 3600); out->tm_min = (int)(rem % 3600 / 60); out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days + 4) % 7); if (out->tm_wday < 0) out->tm_wday += 7;
    out->tm_yday = (int)(days - daysFromCivil(y, 1, 1));
}
// Local wall-clock seconds at which rule r fires in year y.
static int64_t ruleLocal(const Rule& r, int y) {
    const int64_t first = daysFromCivil(y, (unsigned)r.m, 1);
    const int wdayFirst = (int)((first + 4) % 7 + 7) % 7;
    int day = 1 + (r.d - wdayFirst + 7) % 7 + (r.w - 1) * 7;
    const int dim = (int)(daysFromCivil(r.m == 12 ? y + 1 : y, r.m == 12 ? 1 : (unsigned)r.m + 1, 1) - first);
    while (day > dim) day -= 7;
    return (first + day - 1) * 86400 + r.secs;
}
static bool inDst(const Tz& tz, int64_t utc) {
    if (!tz.hasDst) return false;
    struct tm t; tmFromUtc(utc + tz.stdOff, &t);
    const int y = t.tm_year + 1900;
    const int64_t s = ruleLocal(tz.start, y) - tz.stdOff;     // UTC instants
    const int64_t e = ruleLocal(tz.end, y) - tz.dstOff;
    return s < e ? (utc >= s && utc < e) : !(utc >= e && utc < s);
}

extern "C" int sim_setenv(const char* name, const char* value, int) {
    if (name && strcmp(name, "TZ") == 0) {
        std::lock_guard<std::mutex> l(g_tzMu);
        g_tzEnv = value ? value : "UTC0";
        return 0;
    }
    std::string kv = std::string(name) + "=" + (value ? value : "");
    return _putenv(kv.c_str());
}
extern "C" void sim_tzset(void) {
    std::lock_guard<std::mutex> l(g_tzMu);
    parseTz(g_tzEnv, g_tz);
}
extern "C" struct tm* sim_gmtime_r(const time_t* t, struct tm* out) { tmFromUtc((int64_t)*t, out); return out; }
extern "C" struct tm* sim_localtime_r(const time_t* t, struct tm* out) {
    Tz tz; { std::lock_guard<std::mutex> l(g_tzMu); tz = g_tz; }
    const bool dst = inDst(tz, (int64_t)*t);
    tmFromUtc((int64_t)*t + (dst ? tz.dstOff : tz.stdOff), out);
    out->tm_isdst = dst ? 1 : 0;
    return out;
}
extern "C" struct tm* sim_localtime(const time_t* t) { static thread_local struct tm r; return sim_localtime_r(t, &r); }
extern "C" time_t sim_timegm(struct tm* t) {
    const int64_t v = utcFromTm(t);
    tmFromUtc(v, t);
    return (time_t)v;
}
extern "C" time_t sim_mktime(struct tm* t) {
    Tz tz; { std::lock_guard<std::mutex> l(g_tzMu); tz = g_tz; }
    const int64_t local = utcFromTm(t);
    int64_t utc = local - tz.stdOff;
    if (t->tm_isdst > 0 || (t->tm_isdst < 0 && inDst(tz, local - tz.dstOff))) utc = local - tz.dstOff;
    time_t r = (time_t)utc;
    sim_localtime_r(&r, t);
    return r;
}

// ---- dirent / stat over the VFS ------------------------------------------------------
struct sim_DIR {
    std::vector<std::pair<std::string, bool>> entries;   // name, isDir
    size_t pos = 0;
    struct dirent ent;
};
extern "C" DIR* sim_opendir(const char* path) {
    std::error_code ec;
    const fsx::path host = sim::vfsHostPath(path);
    if (!fsx::is_directory(host, ec)) { errno = ENOENT; return nullptr; }
    auto* d = new sim_DIR();
    for (auto& e : fsx::directory_iterator(host, ec))
        d->entries.emplace_back(e.path().filename().string(), e.is_directory(ec));
    return d;
}
extern "C" struct dirent* sim_readdir(DIR* d) {
    if (!d || d->pos >= d->entries.size()) return nullptr;
    const auto& e = d->entries[d->pos++];
    memset(&d->ent, 0, sizeof(d->ent));
    d->ent.d_type = e.second ? DT_DIR : DT_REG;
    strlcpy(d->ent.d_name, e.first.c_str(), sizeof(d->ent.d_name));
    return &d->ent;
}
extern "C" int sim_closedir(DIR* d) { delete d; return 0; }
extern "C" void sim_rewinddir(DIR* d) { if (d) d->pos = 0; }

extern "C" int sim_stat(const char* path, struct stat* st) {
    const std::string host = sim::vfsHostPath(path);
    return ::stat(host.c_str(), st);
}
