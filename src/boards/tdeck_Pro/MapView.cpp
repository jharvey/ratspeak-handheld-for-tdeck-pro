#include "MapView.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#if !defined(SIMULATOR)
#include <esp_heap_caps.h>
#endif

namespace mapview {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kCacheSlots = 6;   // a 240x224 view touches at most 4 tiles; 2 spare for panning

struct Tile {
    int z = -1, x = 0, y = 0;
    bool present = false;        // false = known missing (draw the "no tile" pattern)
    uint32_t used = 0;
    uint8_t* bits = nullptr;     // kTileBytes, allocated once
};

TileReader g_reader = nullptr;
Tile g_cache[kCacheSlots];
uint8_t* g_io = nullptr;         // header + tile read buffer
uint32_t g_clock = 0, g_loaded = 0, g_missing = 0, g_hits = 0;

bool g_open = false;
int g_zoom = kDefaultZoom;
double g_cx = 0, g_cy = 0;       // view centre in world pixels at g_zoom
bool g_follow = true;
bool g_posValid = false;
double g_posLat = 0, g_posLon = 0;
double g_drawnPx = -1e9, g_drawnPy = -1e9;   // own position (world px) at the last draw
int g_viewW = 240, g_viewH = 224;
const uint8_t kDigit5x7[10][7] = {        // rows, 5 bits each (bit 4 = left)
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
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
    char path[48];
    snprintf(path, sizeof(path), "/maps/%d/%d/%d.rmt", z, x, y);
    const size_t got = g_reader ? g_reader(path, g_io, kHeaderBytes + kTileBytes) : 0;
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
    g_loaded = g_missing = g_hits = 0;
    // A card swapped since the last visit: forget "missing" tiles.
    for (auto& t : g_cache) if (!t.present) t.z = -1;
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
    // (10x14 px) on a black disc (white disc + black edge when the position is old);
    // selected: a black ring around the disc; a white halo outermost. Overlapping markers: the
    // selected one, else the lowest number, is drawn with a "+" badge; the rest hidden.
    auto radius = [](const Marker& mk) {
        const int rDisc = mk.number >= 10 ? 13 : 10;
        return mk.selected ? rDisc + 4 : rDisc + 2;
    };
    auto before = [](const Marker& a, const Marker& b) {    // priority: selected, low number
        if (a.selected != b.selected) return a.selected;
        return a.number < b.number;
    };
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
    for (int a = 1; a < g_markerCount; a++)
        for (int b = a; b > 0 && before(g_markers[order[b]], g_markers[order[b - 1]]); b--)
            std::swap(order[b], order[b - 1]);
    for (int k = 0; k < g_markerCount; k++) {
        const int i = order[k];
        g_markerOn[i] = true;
        for (int q = 0; q < k; q++) {
            const int j = order[q];
            if (!g_markerOn[j]) continue;
            const int ddx = g_markerX[i] - g_markerX[j], ddy = g_markerY[i] - g_markerY[j];
            const int lim = radius(g_markers[i]) + radius(g_markers[j]) - 4;
            if (ddx * ddx + ddy * ddy < lim * lim) { g_markerOn[i] = false; g_markers[j].more = true; break; }
        }
    }
    for (int i = 0; i < g_markerCount; i++) {
        const int cx = g_markerX[i], cy = g_markerY[i];
        if (!g_markerOn[i]) continue;
        g_markerOn[i] = cx >= 0 && cy >= 0 && cx < w && cy < h;
        if (cx < -16 || cy < -16 || cx >= w + 16 || cy >= h + 16) continue;
        const Marker& mk = g_markers[i];
        char num[4];
        snprintf(num, sizeof(num), "%u", (unsigned)(mk.number % 100));
        const int nd = (int)strlen(num), tw = nd * 12 - 2;          // 10 px glyph + 2 px gap
        // From the centre out: disc, (selected: black ring), white halo outermost.
        // Fresh (black) disc: a 1 px white gap first so the selection ring shows.
        const int rDisc = nd > 1 ? 13 : 10, rGap = (mk.selected && mk.fresh) ? rDisc + 1 : rDisc;
        const int rSel = mk.selected ? rGap + 2 : rDisc, rOut = rSel + 2;
        for (int yy = -rOut; yy <= rOut; yy++) {
            for (int xx = -rOut; xx <= rOut; xx++) {
                const int vx = cx + xx, vy = cy + yy;
                if (vx < 0 || vy < 0 || vx >= w || vy >= h) continue;
                const int d2 = xx * xx + yy * yy;
                if (d2 > rOut * rOut + rOut) continue;
                bool black;
                if (d2 <= rDisc * rDisc + rDisc) {
                    const bool edge = d2 > (rDisc - 1) * (rDisc - 1) + (rDisc - 1);
                    black = mk.fresh ? true : edge;
                    const int gx = xx + tw / 2, gy = yy + 7;           // cell 0..tw-1, 0..13
                    if (gx >= 0 && gx < tw && gy >= 0 && gy < 14 && (gx % 12) < 10) {
                        const int c = num[gx / 12] - '0';
                        if (kDigit5x7[c][gy / 2] & (0x10 >> ((gx % 12) / 2))) black = !mk.fresh;
                    }
                } else if (d2 <= rGap * rGap + rGap) {
                    black = false;                                // gap (fresh + selected)
                } else if (d2 <= rSel * rSel + rSel) {
                    black = true;                                 // selected: black ring
                } else {
                    black = false;                                // white halo (outermost)
                }
                px(x0 + vx, y0 + vy, black != dark);
            }
        }
        if (mk.more) {                       // "+" badge, upper right: 11x11 white box, black plus
            const int bx = cx + rDisc - 1, by = cy - rDisc + 1;
            for (int yy = -5; yy <= 5; yy++) {
                for (int xx = -5; xx <= 5; xx++) {
                    const int vx = bx + xx, vy = by + yy;
                    if (vx < 0 || vy < 0 || vx >= w || vy >= h) continue;
                    const bool border = xx == -5 || xx == 5 || yy == -5 || yy == 5;
                    const bool plus = (xx >= -3 && xx <= 3 && yy >= -1 && yy <= 1) ||
                                      (yy >= -3 && yy <= 3 && xx >= -1 && xx <= 1);
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
    if (g_open && !(centre && centre->present))
        snprintf(out, n, "z%d no tiles  %.4f,%.4f", g_zoom, lat, lon);
    else
        snprintf(out, n, "z%d %.4f,%.4f %s%s", g_zoom, lat, lon, scale, g_follow && g_posValid ? "" : " *");
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

}  // namespace mapview
