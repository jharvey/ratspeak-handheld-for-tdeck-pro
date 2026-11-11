# Offline map tiles for the T-Deck Pro

The map screen (press **L** on any list screen) draws 256×256 Web-Mercator tiles from the
SD card. The e-ink panel is 1-bit, so tiles are converted on the PC to a raw 1-bit format;
the device needs no image decoder.

```
<SD card>/ratdeckpro/maps/<z>/<x>/<y>.rmt
```

Each `.rmt` file is `RMT1`, then width and height as uint16 little-endian (256, 256), then
8192 bytes: row-major, most significant bit first, bit 1 = black. About 8 KB per tile.

## Make tiles

```
pip install pillow          # only for real map images; --test needs nothing
python make_tiles.py --mbtiles city.mbtiles --center 43.6532,-79.3832 --radius-km 5 --zoom 12-16 --out sdcard
```

Copy `sdcard/ratdeckpro` to the root of a FAT32 microSD card.

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

The public OpenStreetMap tile server (`tile.openstreetmap.org`) forbids bulk downloading,
so the tool refuses it. Use an MBTiles export, your own tile server, or a provider whose
terms allow offline use.

Rough sizes: a 5 km radius at zoom 12–16 is about 1,200 tiles (10 MB); zoom 17 roughly
quadruples the last level.

## On the device

| Key | Action |
|---|---|
| L | Open the map (centred on the GPS fix, else the saved position) |
| W A S D | Pan half a screen |
| Z / X | Zoom in / out |
| C or Enter | Back to your position (follow mode) |
| Shift (^) | Close |

The header shows zoom, centre and the scale-bar length; `*` means the view is not
following your position. Missing tiles show a dotted grid. Console: `map`,
`map <lat> <lon> [zoom]`, `map close`.
