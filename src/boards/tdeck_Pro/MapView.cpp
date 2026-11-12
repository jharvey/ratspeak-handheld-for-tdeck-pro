#include "MapView.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <esp_timer.h>
#if defined(SIMULATOR)
#include "miniz.h"                    // sim/third_party/miniz (MIT)
#else
#include <esp_heap_caps.h>
#include <esp32s3/rom/miniz.h>        // the inflater in the ESP32-S3's ROM
#endif

namespace mapview {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Inflated tiles kept in PSRAM (8 KB each, ~1.3 MB): a view touches at most 4, the rest make
// panning back and forth and zooming in and out again free (no card read, no inflate).
constexpr int kCacheSlots = 160;
// Pack files (tools/maptiles/make_packs.py): one per 64x64 tiles of a zoom; their 32 KB
// index (and RMP2's coder table) is kept for the last few packs used.
constexpr int kPackShift = 6, kPackSide = 1 << kPackShift;
constexpr size_t kPackHeader = 16, kPackIndex = kPackSide * kPackSide * 8;
constexpr int kCtxCount = 1024;                       // RMP2 context coder (tools/maptiles/rmcodec.py)
constexpr size_t kPackPrior = kCtxCount * 2;
constexpr int kPackSlots = 4;

struct Tile {
    int z = -1, x = 0, y = 0;
    bool present = false;        // false = known missing (draw the "no tile" pattern)
    uint32_t used = 0;
    uint8_t* bits = nullptr;     // kTileBytes, allocated once
};

struct Pack {
    int z = -1;
    uint32_t bx = 0, by = 0;
    bool present = false;        // false = no pack file for this block (loose tiles only)
    bool ctx = false;            // RMP2: context-coded tiles; RMP1: raw deflate
    uint32_t used = 0;
    uint8_t* head = nullptr;     // kPackHeader + kPackPrior + kPackIndex, allocated once
    const uint8_t* prior() const { return head + kPackHeader; }
    const uint8_t* index() const { return head + kPackHeader + (ctx ? kPackPrior : 0); }
};

TileReader g_reader = nullptr;
Tile g_cache[kCacheSlots];
Pack g_packs[kPackSlots];
uint8_t* g_io = nullptr;         // header + tile read buffer
tinfl_decompressor* g_inflater = nullptr;
uint32_t g_clock = 0, g_loaded = 0, g_missing = 0, g_hits = 0, g_packed = 0;
uint64_t g_packUs = 0;            // time spent reading + decoding packed tiles
uint64_t g_decodeUs = 0;          // of which decoding

bool g_open = false;
int g_zoom = kDefaultZoom;
double g_cx = 0, g_cy = 0;       // view centre in world pixels at g_zoom
bool g_follow = true;
bool g_posValid = false;
double g_posLat = 0, g_posLon = 0;
double g_drawnPx = -1e9, g_drawnPy = -1e9;   // own position (world px) at the last draw
int g_viewW = 240, g_viewH = 224;
const float* g_track = nullptr;
int g_trackN = 0;
bool g_guideOn = false;
double g_guideLat = 0, g_guideLon = 0;
const uint8_t kDigit5x7[11][7] = {        // rows, 5 bits each (bit 4 = left)
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}};                       // [10] = '+' 
Marker g_markers[kMaxMarkers];
int g_markerCount = 0;
int g_markerX[kMaxMarkers], g_markerY[kMaxMarkers];
bool g_markerOn[kMaxMarkers];

uint8_t* bigAlloc(size_t n) {
#if !defined(SIMULATOR)
    if (void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return (uint8_t*)p;
#endif
    return (uint8_t*)malloc(n);
}

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void packPath(char* out, size_t n, int z, uint32_t bx, uint32_t by) {
    snprintf(out, n, "/maps/packs/%d/%lu_%lu.rmp", z, (unsigned long)bx, (unsigned long)by);
}

// The index of the pack holding block (bx, by) of zoom z, read on first use.
Pack* pack(int z, uint32_t bx, uint32_t by) {
    Pack* victim = &g_packs[0];
    for (auto& p : g_packs) {
        if (p.z == z && p.bx == bx && p.by == by) { p.used = ++g_clock; return &p; }
        if (p.used < victim->used) victim = &p;
    }
    if (!victim->head) victim->head = bigAlloc(kPackHeader + kPackPrior + kPackIndex);
    if (!victim->head) return nullptr;
    victim->z = z; victim->bx = bx; victim->by = by; victim->used = ++g_clock;
    victim->present = false;
    char path[48];
    packPath(path, sizeof(path), z, bx, by);
    uint8_t* h = victim->head;
    if (!g_reader || g_reader(path, 0, h, kPackHeader) != kPackHeader) return victim;
    const bool v1 = !memcmp(h, "RMP1", 4), v2 = !memcmp(h, "RMP2", 4);
    if ((!v1 && !v2) || h[4] != z || h[5] != kPackShift || rd32(h + 8) != bx || rd32(h + 12) != by)
        return victim;
    victim->ctx = v2;
    const size_t rest = (v2 ? kPackPrior : 0) + kPackIndex;
    victim->present = g_reader(path, kPackHeader, h + kPackHeader, rest) == rest;
    return victim;
}

// A packed tile: raw deflate of the 8192 bitmap bytes, or the bytes themselves (len 8192).
bool unpackTile(const uint8_t* src, size_t len, uint8_t* out) {
    if (len == kTileBytes) { memcpy(out, src, kTileBytes); return true; }
    if (!g_inflater) g_inflater = (tinfl_decompressor*)bigAlloc(sizeof(tinfl_decompressor));
    if (!g_inflater) return false;
    tinfl_init(g_inflater);
    size_t inLen = len, outLen = kTileBytes;
    const tinfl_status st = tinfl_decompress(g_inflater, (const mz_uint8*)src, &inLen, (mz_uint8*)out,
                                             (mz_uint8*)out, &outLen,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return st == TINFL_STATUS_DONE && outLen == kTileBytes;
}

// RMP2 tile: every pixel coded with an adaptive binary range coder (LZMA style, 12-bit
// probabilities, shift 5) under a 10-pixel context: row y-2 x-1..x+1 (bits 9..7), row y-1
// x-2..x+2 (bits 6..2), row y x-2 (bit 1) and x-1 (bit 0); outside the tile is white.
// The probabilities start from the pack's table. Must match tools/maptiles/rmcodec.py.
bool ctxDecode(const uint8_t* src, size_t len, const uint8_t* prior, uint8_t* out) {
    static uint16_t probs[kCtxCount];
    static uint8_t rows[3][260];
    if (len < 5) return false;
    for (int i = 0; i < kCtxCount; i++) probs[i] = prior[2 * i] | prior[2 * i + 1] << 8;
    memset(rows, 0, sizeof(rows));
    uint8_t *r2 = rows[0], *r1 = rows[1], *cur = rows[2];
    uint32_t code = (uint32_t)src[1] << 24 | src[2] << 16 | src[3] << 8 | src[4];
    uint32_t range = 0xFFFFFFFFu;
    size_t pos = 5;
    for (int y = 0; y < 256; y++) {
        uint32_t w1 = r1[0] << 3 | r1[1] << 2 | r1[2] << 1 | r1[3];
        uint32_t w2 = r2[1] << 1 | r2[2];
        uint32_t c = 0, byte = 0;
        uint8_t* o = out + y * 32;
        for (int x = 0; x < 256; x++) {
            w1 = ((w1 << 1) | r1[x + 4]) & 31;
            w2 = ((w2 << 1) | r2[x + 3]) & 7;
            const uint32_t ctx = c | w1 << 2 | w2 << 7;
            const uint32_t p = probs[ctx];
            const uint32_t bound = (range >> 12) * p;
            uint32_t px;
            if (code < bound) {
                range = bound;
                probs[ctx] = (uint16_t)(p + ((4096 - p) >> 5));
                px = 0;
            } else {
                code -= bound;
                range -= bound;
                probs[ctx] = (uint16_t)(p - (p >> 5));
                px = 1;
            }
            while (range < (1u << 24)) {
                range <<= 8;
                code = (code << 8) | (pos < len ? src[pos] : 0);
                pos++;
            }
            cur[x + 2] = (uint8_t)px;
            c = ((c << 1) | px) & 3;
            byte = (byte << 1) | px;
            if ((x & 7) == 7) { o[x >> 3] = (uint8_t)byte; byte = 0; }
        }
        uint8_t* t = r2; r2 = r1; r1 = cur; cur = t;
    }
    return pos <= len + 8;
}

bool readPacked(int z, int x, int y, uint8_t* out) {
    Pack* p = pack(z, (uint32_t)x >> kPackShift, (uint32_t)y >> kPackShift);
    if (!p || !p->present) return false;
    const uint8_t* e = p->index() + ((y & (kPackSide - 1)) * kPackSide + (x & (kPackSide - 1))) * 8;
    const uint32_t off = rd32(e), len = rd32(e + 4);
    if (!len || len > kTileBytes) return false;
    char path[48];
    packPath(path, sizeof(path), z, p->bx, p->by);
    if (g_reader(path, off, g_io, len) != len) return false;
    const int64_t t0 = esp_timer_get_time();
    const bool ok = p->ctx && len != kTileBytes ? ctxDecode(g_io, len, p->prior(), out)
                                                : unpackTile(g_io, len, out);
    g_decodeUs += (uint64_t)(esp_timer_get_time() - t0);
    return ok;
}

double worldSize(int z) { return (double)kTileSize * (double)(1u << z); }

void project(double lat, double lon, int z, double& px, double& py) {
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    const double s = worldSize(z);
    px = (lon + 180.0) / 360.0 * s;
    const double r = lat * kPi / 180.0;
    py = (1.0 - log(tan(r) + 1.0 / cos(r)) / kPi) / 2.0 * s;
}

void unproject(double px, double py, int z, double& lat, double& lon) {
    const double s = worldSize(z);
    lon = px / s * 360.0 - 180.0;
    const double n = kPi - 2.0 * kPi * py / s;
    lat = 180.0 / kPi * atan(0.5 * (exp(n) - exp(-n)));
}

void wrapCentre() {
    const double s = worldSize(g_zoom);
    while (g_cx < 0) g_cx += s;
    while (g_cx >= s) g_cx -= s;
    if (g_cy < 0) g_cy = 0;
    if (g_cy > s - 1) g_cy = s - 1;
}

Tile* tile(int z, int x, int y) {
    const int n = 1 << z;
    if (y < 0 || y >= n) return nullptr;
    x = ((x % n) + n) % n;
    Tile* victim = &g_cache[0];
    for (auto& t : g_cache) {
        if (t.z == z && t.x == x && t.y == y) { t.used = ++g_clock; ++g_hits; return &t; }
        if (t.used < victim->used) victim = &t;
    }
    if (!victim->bits) victim->bits = bigAlloc(kTileBytes);
    if (!g_io) g_io = bigAlloc(kHeaderBytes + kTileBytes);
    if (!victim->bits || !g_io) return nullptr;
    victim->z = z; victim->x = x; victim->y = y; victim->used = ++g_clock; victim->present = false;
    const int64_t t0 = esp_timer_get_time();
    const bool packed = readPacked(z, x, y, victim->bits);
    g_packUs += (uint64_t)(esp_timer_get_time() - t0);
    if (packed) {
        victim->present = true;
        ++g_loaded;
        ++g_packed;
        return victim;
    }
    char path[48];
    snprintf(path, sizeof(path), "/maps/%d/%d/%d.rmt", z, x, y);
    const size_t got = g_reader ? g_reader(path, 0, g_io, kHeaderBytes + kTileBytes) : 0;
    if (got == kHeaderBytes + kTileBytes && !memcmp(g_io, "RMT1", 4) &&
        (g_io[4] | g_io[5] << 8) == kTileSize && (g_io[6] | g_io[7] << 8) == kTileSize) {
        memcpy(victim->bits, g_io + kHeaderBytes, kTileBytes);
        victim->present = true;
        ++g_loaded;
    } else if (got == kTileBytes) {           // header-less tile: exactly one 8 KB cluster pair
        memcpy(victim->bits, g_io, kTileBytes);
        victim->present = true;
        ++g_loaded;
    } else {
        ++g_missing;
    }
    return victim;
}

// Nice scale bar: the largest 1/2/5 x 10^k metres that fits in ~70 px.
void scaleBar(double& metres, int& pixels) {
    double lat, lon;
    unproject(g_cx, g_cy, g_zoom, lat, lon);
    const double mpp = 156543.03392 * cos(lat * kPi / 180.0) / (double)(1u << g_zoom);
    const double target = mpp * 70.0;
    double m = 1;
    while (m * 10 <= target) m *= 10;
    if (m * 5 <= target) m *= 5;
    else if (m * 2 <= target) m *= 2;
    metres = m;
    pixels = (int)(m / mpp + 0.5);
}

}  // namespace

void setReader(TileReader reader) { g_reader = reader; }

CoordFormatter g_coordFmt = nullptr;
void setCoordFormatter(CoordFormatter f) { g_coordFmt = f; }

void setMarkers(const Marker* m, int n) {
    if (n > kMaxMarkers) n = kMaxMarkers;
    for (int i = 0; i < n; i++) { g_markers[i] = m[i]; g_markerOn[i] = false; }
    g_markerCount = n < 0 ? 0 : n;
}

bool markerScreen(int i, int& x, int& y) {
    if (i < 0 || i >= g_markerCount || !g_markerOn[i]) return false;
    x = g_markerX[i]; y = g_markerY[i];
    return true;
}

void setTrack(const float* latlon, int n) { g_track = latlon; g_trackN = n > 0 ? n : 0; }

void setGuide(bool on, double lat, double lon) { g_guideOn = on; g_guideLat = lat; g_guideLon = lon; }

void getCentre(double& lat, double& lon) { unproject(g_cx, g_cy, g_zoom, lat, lon); }

void centreOn(double lat, double lon) {
    g_follow = false;
    project(lat, lon, g_zoom, g_cx, g_cy);
    wrapCentre();
}
bool isOpen() { return g_open; }

void open(double lat, double lon, bool havePos, bool follow) {
    static bool everCentred = false;
    if (havePos) {
        project(lat, lon, g_zoom, g_cx, g_cy);
        g_follow = follow;
        everCentred = true;
    } else if (!everCentred) {
        project(0, 0, g_zoom, g_cx, g_cy);   // no position yet: 0,0 rather than the world's corner
        everCentred = true;
    }
    g_open = true;
    g_loaded = g_missing = g_hits = g_packed = 0;
    g_packUs = g_decodeUs = 0;
    // A card swapped since the last visit: forget "missing" tiles and the pack indexes.
    for (auto& t : g_cache) if (!t.present) t.z = -1;
    for (auto& p : g_packs) p.z = -1;
}

void close() { g_open = false; }

void setZoom(int z) {
    if (z < kMinZoom) z = kMinZoom;
    if (z > kMaxZoom) z = kMaxZoom;
    while (g_zoom < z) { g_zoom++; g_cx *= 2; g_cy *= 2; }
    while (g_zoom > z) { g_zoom--; g_cx /= 2; g_cy /= 2; }
    wrapCentre();
}

int zoom() { return g_zoom; }

bool fitZoom() {
    const int start = g_zoom;
    for (int z = g_zoom; z >= kMinZoom; z--) {
        setZoom(z);
        const Tile* t = tile(g_zoom, (int)floor(g_cx / kTileSize), (int)floor(g_cy / kTileSize));
        if (t && t->present) return g_zoom != start;
    }
    setZoom(start);
    return false;
}

void setPosition(bool valid, double lat, double lon) {
    g_posValid = valid;
    g_posLat = lat; g_posLon = lon;
    if (valid && g_follow) { project(lat, lon, g_zoom, g_cx, g_cy); wrapCentre(); }
}

bool positionNeedsRedraw() {
    if (!g_open || !g_posValid) return false;
    double px, py;
    project(g_posLat, g_posLon, g_zoom, px, py);
    return fabs(px - g_drawnPx) >= 8 || fabs(py - g_drawnPy) >= 8;
}

void draw(PixelFn px, int x0, int y0, int w, int h, bool dark) {
    g_viewW = w; g_viewH = h;
    const double left = g_cx - w / 2.0, top = g_cy - h / 2.0;
    // Walk the tiles the view overlaps, then copy their bits.
    const int tx0 = (int)floor(left / kTileSize), ty0 = (int)floor(top / kTileSize);
    const int tx1 = (int)floor((left + w - 1) / kTileSize), ty1 = (int)floor((top + h - 1) / kTileSize);
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            Tile* t = tile(g_zoom, tx, ty);
            const int ox = (int)floor(tx * (double)kTileSize - left);   // tile origin in view px
            const int oy = (int)floor(ty * (double)kTileSize - top);
            const int sx0 = ox < 0 ? -ox : 0, sy0 = oy < 0 ? -oy : 0;
            const int sx1 = (ox + kTileSize > w) ? w - ox : kTileSize;
            const int sy1 = (oy + kTileSize > h) ? h - oy : kTileSize;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t* row = (t && t->present) ? t->bits + (size_t)sy * (kTileSize / 8) : nullptr;
                for (int sx = sx0; sx < sx1; sx++) {
                    bool black;
                    if (row) black = (row[sx >> 3] & (0x80 >> (sx & 7))) != 0;
                    else black = ((sx & 15) == 0 && (sy & 15) == 0);   // sparse dots = no tile
                    px(x0 + ox + sx, y0 + oy + sy, black != dark);
                }
            }
        }
    }

    // View coordinates of a lat/lon (wrapped across the date line).
    auto toView = [&](double lat, double lon, int& vx, int& vy) {
        double ppx, ppy;
        project(lat, lon, g_zoom, ppx, ppy);
        double dx = ppx - left;
        const double s = worldSize(g_zoom);
        if (dx < -s / 2) dx += s;
        if (dx > s / 2) dx -= s;
        vx = (int)dx;
        vy = (int)(ppy - top);
    };
    // Line from a to b: pattern(step) true = black pixel; 1 px white either side for contrast.
    auto line = [&](int ax, int ay, int bx, int by, int on, int off) {
        const int dx = abs(bx - ax), dy = -abs(by - ay), sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
        int err = dx + dy, k = 0, guard = 4000;
        while (guard-- > 0) {
            if ((k % (on + off)) < on && ax >= 0 && ay >= 0 && ax < w && ay < h) px(x0 + ax, y0 + ay, !dark);
            k++;
            if (ax == bx && ay == by) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; ax += sx; }
            if (e2 <= dx) { err += dx; ay += sy; }
        }
    };
    // Breadcrumb track: dotted (2 on, 2 off), skipping segments far off the view.
    for (int i = 1; i < g_trackN; i++) {
        int ax, ay, bx, by;
        toView(g_track[2 * (i - 1)], g_track[2 * (i - 1) + 1], ax, ay);
        toView(g_track[2 * i], g_track[2 * i + 1], bx, by);
        if ((ax < 0 && bx < 0) || (ay < 0 && by < 0) || (ax >= w && bx >= w) || (ay >= h && by >= h)) continue;
        if (abs(ax - bx) > 2000 || abs(ay - by) > 2000) continue;
        line(ax, ay, bx, by, 2, 2);
    }
    // Guide line from the own position to the selected waypoint: dashed (5 on, 3 off).
    if (g_guideOn && g_posValid) {
        int ax, ay, bx, by;
        toView(g_posLat, g_posLon, ax, ay);
        toView(g_guideLat, g_guideLon, bx, by);
        if (abs(ax - bx) < 4000 && abs(ay - by) < 4000) line(ax, ay, bx, by, 5, 3);
    }

    // Waypoints: black diamond (radius 6) in a white halo; selected: hollow centre and an
    // outer black diamond ring.
    for (int i = 0; i < g_markerCount; i++) {
        if (g_markers[i].kind != 1) continue;
        int cx, cy;
        toView(g_markers[i].lat, g_markers[i].lon, cx, cy);
        g_markerX[i] = cx; g_markerY[i] = cy;
        g_markerOn[i] = cx >= 0 && cy >= 0 && cx < w && cy < h;
        const bool sel = g_markers[i].selected;
        const int rOut = sel ? 11 : 8;
        for (int yy = -rOut; yy <= rOut; yy++) {
            for (int xx = -rOut; xx <= rOut; xx++) {
                const int vx = cx + xx, vy = cy + yy, d = abs(xx) + abs(yy);
                if (vx < 0 || vy < 0 || vx >= w || vy >= h || d > rOut) continue;
                bool black;
                if (d <= 6) black = !(sel && d <= 2);              // diamond (hollow centre if selected)
                else if (sel && d == 7) black = false;
                else if (sel && d <= 9) black = true;              // selected: outer ring
                else black = false;                                // white halo
                px(x0 + vx, y0 + vy, black != dark);
            }
        }
    }

    // Own position: black ring with a white halo and a centre dot, visible on any tile.
    if (g_posValid) {
        double ppx, ppy;
        project(g_posLat, g_posLon, g_zoom, ppx, ppy);
        g_drawnPx = ppx; g_drawnPy = ppy;
        double dx = ppx - left;
        const double s = worldSize(g_zoom);
        if (dx < -s / 2) dx += s;
        if (dx > s / 2) dx -= s;
        const int cx = (int)dx, cy = (int)(ppy - top);
        for (int yy = -9; yy <= 9; yy++) {
            for (int xx = -9; xx <= 9; xx++) {
                const int r2 = xx * xx + yy * yy;
                const int vx = cx + xx, vy = cy + yy;
                if (vx < 0 || vy < 0 || vx >= w || vy >= h) continue;
                if (r2 <= 4 || (r2 >= 30 && r2 <= 48)) px(x0 + vx, y0 + vy, !dark);
                else if (r2 <= 80) px(x0 + vx, y0 + vy, dark);
            }
        }
    }
    if (!g_follow || !g_posValid) {
        // Centre cross when the view is not on the own position.
        const int cx = w / 2, cy = h / 2;
        for (int d = -6; d <= 6; d++) {
            if (d > -2 && d < 2) continue;
            px(x0 + cx + d, y0 + cy, !dark);
            px(x0 + cx, y0 + cy + d, !dark);
        }
    }

    // Markers (contacts), drawn last so nothing covers them: number in 2x 5x7 digits
    // (10x14 px) on a white centre, black ring (3 px fresh / 1 px old), white halo;
    // selected: a second black ring outside. Overlapping markers: the
    // lowest number is drawn, with a boxed "+" (more here); the rest hidden. A selected one under
    // it makes that tag show selected.
    auto radius = [](const Marker& mk) { return 12 + (mk.fresh ? 3 : 1) + 2; };   // overlap test
    auto before = [](const Marker& a, const Marker& b) { return a.number < b.number; };   // low number wins
    int order[kMaxMarkers];
    for (int i = 0; i < g_markerCount; i++) {
        double mpx, mpy;
        project(g_markers[i].lat, g_markers[i].lon, g_zoom, mpx, mpy);
        double dx = mpx - left;
        const double s = worldSize(g_zoom);
        if (dx < -s / 2) dx += s;
        if (dx > s / 2) dx -= s;
        g_markerX[i] = (int)dx;
        g_markerY[i] = (int)(mpy - top);
        g_markers[i].more = false;
        order[i] = i;
    }
    int nOrder = 0;                          // contacts only (waypoints are drawn above)
    for (int i = 0; i < g_markerCount; i++) if (g_markers[i].kind == 0) order[nOrder++] = i;
    for (int a = 1; a < nOrder; a++)
        for (int b = a; b > 0 && before(g_markers[order[b]], g_markers[order[b - 1]]); b--)
            std::swap(order[b], order[b - 1]);
    bool showSel[kMaxMarkers] = {};          // drawn selected: itself, or a hidden one under it
    for (int k = 0; k < nOrder; k++) {
        const int i = order[k];
        g_markerOn[i] = true;
        if (g_markers[i].selected) showSel[i] = true;
        for (int q = 0; q < k; q++) {
            const int j = order[q];
            if (!g_markerOn[j]) continue;
            const int ddx = g_markerX[i] - g_markerX[j], ddy = g_markerY[i] - g_markerY[j];
            const int lim = radius(g_markers[i]) + radius(g_markers[j]) - 4;
            if (ddx * ddx + ddy * ddy < lim * lim) {
                g_markerOn[i] = false;
                g_markers[j].more = true;
                if (g_markers[i].selected) showSel[j] = true;
                break;
            }
        }
    }
    for (int i = 0; i < g_markerCount; i++) {
        if (g_markers[i].kind != 0) continue;
        const int cx = g_markerX[i], cy = g_markerY[i];
        if (!g_markerOn[i]) continue;
        g_markerOn[i] = cx >= 0 && cy >= 0 && cx < w && cy < h;
        if (cx < -16 || cy < -16 || cx >= w + 16 || cy >= h + 16) continue;
        const Marker& mk = g_markers[i];
        char num[4];
        snprintf(num, sizeof(num), "%u", (unsigned)(mk.number % 100));
        const int nd = (int)strlen(num), tw = nd * 12 - 2;          // 10 px glyph + 2 px gap
        const bool sel = showSel[i];
        // Tag (user's sketch): white centre with the black number, a black ring, a white
        // halo outermost. Ring 3 px = fresh position (< 1 h), 1 px = older. Selected: the
        // centre turns black with a white number.
        const int rIn = nd > 1 ? 12 : 9;                         // white centre (digits fit)
        // Selected: a second black ring outside (1 px white gap), centre stays white.
        const int rRing = rIn + (mk.fresh ? 3 : 1);
        const int rGap = sel ? rRing + 1 : rRing, rSel = sel ? rGap + 2 : rRing, rOut = rSel + 3;   // 3 px halo
        for (int yy = -rOut; yy <= rOut; yy++) {
            for (int xx = -rOut; xx <= rOut; xx++) {
                const int vx = cx + xx, vy = cy + yy;
                if (vx < 0 || vy < 0 || vx >= w || vy >= h) continue;
                const int d2 = xx * xx + yy * yy;
                if (d2 > rOut * rOut + rOut) continue;
                bool black;
                if (d2 <= rIn * rIn + rIn) {
                    black = false;
                    const int gx = xx + tw / 2, gy = yy + 7;           // cell 0..tw-1, 0..13
                    if (gx >= 0 && gx < tw && gy >= 0 && gy < 14 && (gx % 12) < 10) {
                        const char ch = num[gx / 12];
                        const int c = ch == '+' ? 10 : ch - '0';
                        if (kDigit5x7[c][gy / 2] & (0x10 >> ((gx % 12) / 2))) black = true;
                    }
                } else if (d2 <= rRing * rRing + rRing) {
                    black = true;                                 // the black ring
                } else if (d2 <= rGap * rGap + rGap) {
                    black = false;                                // selected: gap
                } else if (d2 <= rSel * rSel + rSel) {
                    black = true;                                 // selected: outer ring
                } else {
                    black = false;                                // white halo (outermost)
                }
                px(x0 + vx, y0 + vy, black != dark);
            }
        }
        if (mk.more) {                       // "+" badge (user's mock-up): 13x13 white box,
            // 1 px black border, bold plus; its lower-left corner overlaps the ring, upper right.
            const int bx = cx + rIn + 2, by = cy - rIn - 2;
            for (int yy = -6; yy <= 6; yy++) {
                for (int xx = -6; xx <= 6; xx++) {
                    const int vx = bx + xx, vy = by + yy;
                    if (vx < 0 || vy < 0 || vx >= w || vy >= h) continue;
                    const bool border = xx == -6 || xx == 6 || yy == -6 || yy == 6;
                    const bool plus = (xx >= -4 && xx <= 4 && yy >= -1 && yy <= 1) ||
                                      (yy >= -4 && yy <= 4 && xx >= -1 && xx <= 1);
                    px(x0 + vx, y0 + vy, (border || plus) != dark);
                }
            }
        }
    }

    // Scale bar, bottom left, on a white strip.
    double metres; int bar;
    scaleBar(metres, bar);
    const int bx = 6, by = h - 8;
    for (int yy = by - 3; yy <= by + 3; yy++)
        for (int xx = bx - 3; xx <= bx + bar + 3; xx++)
            if (xx >= 0 && yy >= 0 && xx < w && yy < h) px(x0 + xx, y0 + yy, dark);
    for (int xx = bx; xx <= bx + bar; xx++) { px(x0 + xx, y0 + by, !dark); px(x0 + xx, y0 + by + 1, !dark); }
    for (int yy = by - 3; yy <= by + 1; yy++) { px(x0 + bx, y0 + yy, !dark); px(x0 + bx + bar, y0 + yy, !dark); }
}

void status(char* out, size_t n) {
    double lat, lon;
    unproject(g_cx, g_cy, g_zoom, lat, lon);
    double metres; int bar;
    scaleBar(metres, bar);
    char scale[16];
    if (metres >= 1000) snprintf(scale, sizeof(scale), "%g km", metres / 1000);
    else snprintf(scale, sizeof(scale), "%g m", metres);
    // The header is built before the tiles are drawn, so ask the cache about the centre tile.
    const Tile* centre = g_open ? tile(g_zoom, (int)floor(g_cx / kTileSize), (int)floor(g_cy / kTileSize)) : nullptr;
    char pos[40];
    if (g_coordFmt) g_coordFmt(lat, lon, pos, sizeof(pos));
    else snprintf(pos, sizeof(pos), "%.4f,%.4f", lat, lon);
    if (g_open && !(centre && centre->present))
        snprintf(out, n, "z%d no tiles  %s", g_zoom, pos);
    else
        snprintf(out, n, "z%d %s %s%s", g_zoom, pos, scale, g_follow && g_posValid ? "" : " *");
}

bool key(char c, bool enter) {
    if (enter || c == 'c' || c == 'C') {
        if (!g_posValid) return false;
        g_follow = true;
        project(g_posLat, g_posLon, g_zoom, g_cx, g_cy);
        wrapCentre();
        return true;
    }
    const double stepX = g_viewW / 2.0, stepY = g_viewH / 2.0;
    switch (c) {
        case 'w': case 'W': g_cy -= stepY; break;
        case 's': case 'S': g_cy += stepY; break;
        case 'a': case 'A': g_cx -= stepX; break;
        case 'd': case 'D': g_cx += stepX; break;
        case 'z': case 'Z': case '+':
            if (g_zoom >= kMaxZoom) return false;
            g_zoom++; g_cx *= 2; g_cy *= 2; wrapCentre(); return true;
        case 'x': case 'X': case '-':
            if (g_zoom <= kMinZoom) return false;
            g_zoom--; g_cx /= 2; g_cy /= 2; wrapCentre(); return true;
        default: return false;
    }
    g_follow = false;
    wrapCentre();
    return true;
}

void stats(uint32_t& loaded, uint32_t& missing, uint32_t& hits) {
    loaded = g_loaded; missing = g_missing; hits = g_hits;
}

uint32_t packedLoaded() { return g_packed; }
uint32_t packedMillis() { return (uint32_t)(g_packUs / 1000); }
uint32_t decodeMillis() { return (uint32_t)(g_decodeUs / 1000); }

}  // namespace mapview
