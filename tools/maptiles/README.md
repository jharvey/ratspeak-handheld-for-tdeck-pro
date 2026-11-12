# Offline map tiles for the T-Deck Pro

The map screen (**M → Map**, or **L** on any list screen) draws 256×256 Web-Mercator tiles from the
SD card. The e-ink panel is 1-bit, so tiles are converted on the PC to a raw 1-bit format;
the device needs no image decoder.

```
<SD card>/ratdeckpro/maps/<z>/<x>/<y>.rmt
```

Each `.rmt` file is `RMT1`, then width and height as uint16 little-endian (256, 256), then
8192 bytes: row-major, most significant bit first, bit 1 = black. About 8 KB per tile.

## Pack files (compressed; preferred)

Loose tiles waste space (each takes a whole 8 KB cluster, 32 KB on a stock card) and copy
slowly (one FAT write per tile). `make_packs.py` deflates the tiles into one file per 64×64
tiles of a zoom level:

```
<SD card>/ratdeckpro/maps/packs/<z>/<bx>_<by>.rmp        bx = x >> 6, by = y >> 6
python make_packs.py --out M:/ratspeak-maps/packed  M:/ratspeak-maps/mapsd  M:/ratspeak-maps/world
```

Sources are folders of loose tiles, the preferred first (detailed maps before the world
base map). Re-running rebuilds only packs older than their tiles. USGS topo tiles pack to
~2-3.5 KB (2.5-4x), world base-map tiles to 0.1-0.4 KB. The unit inflates a tile with the
inflater in the ESP32-S3's ROM (about a millisecond) and keeps 160 inflated tiles in PSRAM;
where a pack has no tile it still reads the loose `.rmt`, so both can be on one card.
Copy packs to a unit over USB with `../sdcard/push_files.py`:

```
python ../sdcard/push_files.py --port COM5 --root M:/ratspeak-maps/packed/ratdeckpro maps/packs/9 maps/packs/10
```

Format: `RMP1`, u8 z, u8 6, u16 0, u32 bx, u32 by, then 4096 × (u32 offset, u32 length)
indexed by `(y & 63) * 64 + (x & 63)` (length 0 = no tile), then the tiles: raw deflate of the
8192 bitmap bytes, or the 8192 bytes as they are when the length is 8192.

## Make tiles

```
pip install pillow          # only for real map images; --test needs nothing
python make_tiles.py --mbtiles city.mbtiles --center 43.6532,-79.3832 --radius-km 5 --zoom 12-16 --out sdcard
```

Copy `sdcard/ratdeckpro` to the root of a FAT32 microSD card, or send it over USB with
the card left in the unit (no card reader; the unit must be unlocked):

```
pip install pyserial
python push_tiles.py --port COM9 --src sdcard            # resumes; lowest zoom first
python push_tiles.py --port COM9 --src sdcard --zoom 13  # just one level
```

Card to card, both units on USB, nothing kept on the PC (about 4-5 tiles/s):

```
python copy_tiles.py --from COM9 --to COM6 --zoom 13
```

About 7 tiles/s (a 6,000-tile set takes 15 min).

**Card format.** Stock 32 GB cards come FAT32 with 32 KB clusters: every 8 KB tile then
takes 32 KB. `push_tiles.py` / `copy_tiles.py` send tiles without their 8-byte header
(exactly 8192 bytes; firmware from 2026-11-12 reads both), and the console command
`sdformat 8192 ERASE-ALL` reformats the card in the unit with 8 KB clusters (one tile =
one cluster; ~3.5 million tiles on a 29.5 GB card). It erases the card and restarts the
unit; back up anything else first (`sdls`, `sdget`). `sdinfo` shows the cluster size. Progress is kept per unit in
`sdcard/pushed_<dest>.txt`; it stops when the card has less than 200 MB free. On a
32 KB-cluster FAT32 card each 8 KB tile takes 32 KB. Console `sdls /ratdeckpro/maps`
lists what is on the card.

| Option | Meaning |
|---|---|
| `--mbtiles FILE` | Raster MBTiles (PNG/JPEG), e.g. exported from QGIS or MOBAC |
| `--url TEMPLATE` | `https://…/{z}/{x}/{y}.png` from a server whose terms allow bulk/offline use; needs `--user-agent`; `--delay` seconds between requests (default 1) |
| `--test` | Synthetic grid tiles labelled z/x/y, to try the screen without map data |
| `--bbox` / `--center` + `--radius-km` | Area |
| `--zoom 12-16` | Zoom range (inclusive) |
| `--threshold N` | Plain black/white at grey level N instead of dithering (cleaner for line maps) |
| `--invert` | Swap black and white |
| `--max-tiles` | Safety cap (default 5000 ≈ 40 MB) |

| `--home LAT,LON,ZOOM` | Start view when the device has no GPS fix or saved position (`maps/home.txt`). Without it the first run writes its area's centre at its lowest zoom; later runs keep it. `--home-only` writes just that file |

Note: write a negative bbox as `--bbox=-71.1,42.95,-66.9,47.47` (with `=`), or the
argument parser reads it as an option.

### Canada (NRCan, Open Government Licence - Canada)

USGS topo stops at the border. Natural Resources Canada's Canada Base Map - Transportation
(roads first) is free to reuse with credit ("Contains information licensed under the Open
Government Licence - Canada"). Roads and labels are two layers; `--overlay-url` draws the
labels on top, and `--threshold 200` keeps its light-grey roads:

```
set N=https://maps-cartes.services.geo.ca/server2_serveur2/rest/services/BaseMaps
python make_tiles.py --url %N%/CBMT_CBCT_GEOM_3857/MapServer/tile/{z}/{y}/{x} --overlay-url %N%/CBMT_TXT_3857/MapServer/tile/{z}/{y}/{x} --threshold 200 --user-agent RatSpeak-maptiles/1.0 --delay 0.2 --bbox=-83.5,41.7,-59.5,50.5 --zoom 9-12 --out sdcard
```

### World overview (Natural Earth, rendered here)

`render_world.py` draws zoom 0-7 itself from Natural Earth GeoJSON (public domain:
coastlines, lakes, country borders 2 px, state / province borders dotted, names with a
white halo, big cities). 21,845 tiles (175 MB; ~700 MB on a 32 KB-cluster card).

```
python render_world.py --ne <folder with the ne_*.geojson files> --zoom 0-7 --out world --home 43.64,-70.92,9
python push_tiles.py --port COM9 --src world
```

### Example: Maine + the lower 48 (USGS topo, public domain)

USGS National Map basemaps are U.S. public domain (credit: U.S. Geological Survey).
Plain black/white at 150 reads better on e-ink than dithering:

```
set U=https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}
python make_tiles.py --url %U% --user-agent RatSpeak-maptiles/1.0 --delay 0.2 --threshold 150 --bbox=-125,24.4,-66.9,49.4 --zoom 3-8 --out sdcard
python make_tiles.py --url %U% --user-agent RatSpeak-maptiles/1.0 --delay 0.2 --threshold 150 --bbox=-71.1,42.95,-66.9,47.47 --zoom 9-12 --max-tiles 6000 --out sdcard
python make_tiles.py --test --home=45.25,-69.2,6 --home-only --center 45,-69 --out sdcard
```

About 1,400 + 4,800 tiles (50 MB). Maine at zoom 13 adds 14,000 tiles (115 MB); for street
detail, add a small `--center <town> --radius-km 15 --zoom 13-15` area instead.

The public OpenStreetMap tile server (`tile.openstreetmap.org`) forbids bulk downloading,
so the tool refuses it. Use an MBTiles export, your own tile server, or a provider whose
terms allow offline use.

Rough sizes: a 5 km radius at zoom 12–16 is about 1,200 tiles (10 MB); zoom 17 roughly
quadruples the last level.

## On the device

| Key | Action |
|---|---|
| M → Map, or L | Open the map (centred on the GPS fix, else the saved position, else the card's `home.txt`; zooms out to the deepest level with a tile there) |
| W A S D | Pan half a screen |
| Z / X | Zoom in / out |
| C or Enter | Back to your position (follow mode) |
| Shift (^) | Close |

The header shows zoom, centre and the scale-bar length; `*` means the view is not
following your position. Missing tiles show a dotted grid. Console: `map`,
`map <lat> <lon> [zoom]`, `map close`.
