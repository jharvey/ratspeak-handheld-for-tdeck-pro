#pragma once
// Offline map view for the 1-bit e-ink panel.
//
// Tiles are 256x256 Web-Mercator (OSM "slippy map") tiles, pre-converted on a PC to
// 1 bit per pixel by tools/maptiles/make_tiles.py and copied to the SD card:
//     <SD>/ratdeckpro/maps/<z>/<x>/<y>.rmt
// File = 8-byte header "RMT1" + uint16 LE width (256) + uint16 LE height (256), then
// 8192 bytes, row-major, MSB first, bit 1 = black. No decoder needed on the device.
//
// The module draws through callbacks and reads tiles through a callback, so it has no
// dependency on the app shell (also compiled into the cooperative build and the simulator).

#include <stddef.h>
#include <stdint.h>

namespace mapview {

constexpr int kTileSize  = 256;
constexpr size_t kTileBytes = kTileSize * kTileSize / 8;   // 8192
constexpr size_t kHeaderBytes = 8;
constexpr int kMinZoom = 1, kMaxZoom = 18, kDefaultZoom = 14;

// Reads the whole file at `path` (relative, e.g. "/maps/14/4578/5978.rmt") into `buf`.
// Returns the number of bytes read, 0 if absent or unreadable.
using TileReader = size_t (*)(const char* path, uint8_t* buf, size_t capacity);
using PixelFn = void (*)(int x, int y, bool black);

void setReader(TileReader reader);

bool isOpen();
// Opens centred on (lat, lon); with havePos=false opens on the last centre (or 0,0).
// follow=false: an explicit place (console "map <lat> <lon>"), not the own position.
void open(double lat, double lon, bool havePos, bool follow = true);
void setZoom(int z);                 // clamped to kMinZoom..kMaxZoom, keeps the centre
void close();

// Own position (GPS or saved). Follow mode keeps it centred until the user pans.
void setPosition(bool valid, double lat, double lon);
// true when the view should be redrawn because the followed position moved >= 8 px.
bool positionNeedsRedraw();

// Draws the map into the rectangle (x0, y0, w, h). dark = invert (black paper theme).
void draw(PixelFn px, int x0, int y0, int w, int h, bool dark);
// One-line header: "z14 43.65321,-79.38320 200 m" / "no map tiles here (z14)".
void status(char* out, size_t n);

// Keys while the map is open: W/A/S/D pan, Z/+ zoom in, X/- zoom out, C or Enter = centre
// on own position (follow). Returns true if the key changed the view.
bool key(char c, bool enter);

// Cache stats for the console: tiles loaded / missing since open.
void stats(uint32_t& loaded, uint32_t& missing, uint32_t& hits);

}  // namespace mapview
