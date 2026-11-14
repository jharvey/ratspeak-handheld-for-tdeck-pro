#pragma once
// Offline map view for the 1-bit e-ink panel.
//
// Tiles are 256x256 Web-Mercator (OSM "slippy map") tiles, pre-converted on a PC to
// 1 bit per pixel by tools/maptiles/make_tiles.py and copied to the SD card:
//     <SD>/ratdeckpro/maps/<z>/<x>/<y>.rmt
// File = 8-byte header "RMT1" + uint16 LE width (256) + uint16 LE height (256), then
// 8192 bytes, row-major, MSB first, bit 1 = black. No decoder needed on the device.
// A file of exactly 8192 bytes is the same bits without the header (push_tiles.py strips
// it: on a card formatted with 4 KB clusters a tile then takes 8 KB, not 12).
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

// Reads up to `len` bytes at `offset` of the file at `path` (relative, e.g.
// "/maps/14/4578/5978.rmt" or "/maps/packs/14/71_93.rmp") into `buf`.
// Returns the number of bytes read, 0 if absent or unreadable.
using TileReader = size_t (*)(const char* path, uint32_t offset, uint8_t* buf, size_t len);
using PixelFn = void (*)(int x, int y, bool black);

void setReader(TileReader reader);

bool isOpen();
// Opens centred on (lat, lon); with havePos=false opens on the last centre (or 0,0).
// follow=false: an explicit place (console "map <lat> <lon>"), not the own position.
void open(double lat, double lon, bool havePos, bool follow = true);
void setZoom(int z);                 // clamped to kMinZoom..kMaxZoom, keeps the centre
int  zoom();
// No tile under the centre at this zoom: zoom out to the deepest level that has one
// (cards often hold a wide area at low zoom and a smaller one in detail). Returns
// true if the zoom changed. Stays put when no level has a tile there.
bool fitZoom();
void close();

// Own position (GPS or saved). Follow mode keeps it centred until the user pans.
void setPosition(bool valid, double lat, double lon);
// true when the view should be redrawn because the followed position moved >= 8 px.
bool positionNeedsRedraw();

// Draws the map into the rectangle (x0, y0, w, h). dark = invert (black paper theme).
void draw(PixelFn px, int x0, int y0, int w, int h, bool dark);
// One-line header: "z14 43.65321,-79.38320 200 m" / "no map tiles here (z14)".
void status(char* out, size_t n, int shorter = 0);   // 1 = no scale text, 2 = also 4 decimals
// How the header writes the centre (decimal / DMS / MGRS); null = decimal degrees.
using CoordFormatter = void (*)(double lat, double lon, char* out, size_t n);
void setCoordFormatter(CoordFormatter f);

// Keys while the map is open: W/A/S/D pan, Z/+ zoom in, X/- zoom out, C or Enter = centre
// on own position (follow). Returns true if the key changed the view.
bool key(char c, bool enter);

// Other positions (contacts' last shared locations): a tag with the contact's number
// (1-99) on a white centre, black ring (thick = fresh position, thin = old), white halo;
// selected = a second black ring. Markers that overlap on screen: only the lowest number is
// drawn, with a small boxed "+" (more here), shown selected if a hidden one is selected. Copied (at most
// kMaxMarkers). markerScreen() gives where marker i was drawn by the last draw(),
// relative to the draw rectangle; false when hidden or off the view.
constexpr int kMaxMarkers = 48;           // 16 contacts + 32 waypoints
struct Marker { double lat, lon; bool selected; uint8_t number; bool fresh; bool more;
                uint8_t kind; };              // kind 0 = contact tag, 1 = waypoint (diamond)
void setMarkers(const Marker* m, int n);
bool markerScreen(int i, int& x, int& y);
// Centre the view on a point without following the own position (zoom kept).
void centreOn(double lat, double lon);
void getCentre(double& lat, double& lon);   // view centre

// Breadcrumb track: lat,lon pairs (float), drawn as a dotted line. The array is not
// copied: keep it alive while the map can draw. n = 0: none.
void setTrack(const float* latlon, int n);
// Dashed guide line from the own position to a point (selected waypoint), or off.
void setGuide(bool on, double lat = 0, double lon = 0);

// Cache stats for the console: tiles loaded / missing since open.
void stats(uint32_t& loaded, uint32_t& missing, uint32_t& hits);
uint32_t packedLoaded();   // of the tiles loaded, how many came out of pack files
uint32_t packedMillis();   // ms spent reading + decoding them (incl. pack index reads)
uint32_t decodeMillis();   // of which decoding

}  // namespace mapview
