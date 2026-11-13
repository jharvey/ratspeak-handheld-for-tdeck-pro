#!/usr/bin/env python3
"""Range-test CSV (from the T-Deck Pro, <SD>/ratdeckpro/rangetest/rt-*.csv) -> coverage map.

    python tools/rangetest/rangetest_map.py rt-20261113-1151.csv [more.csv ...] [--out NAME]

Writes, next to the first CSV (or NAME.*):
  NAME.kml   open in Google Earth / Google My Maps / QGIS: one point per event where the
             unit had a GPS fix, coloured by signal (green strong ... red weak), red X = lost
  NAME.svg   a plot with no map underneath: the same points by latitude / longitude, and
             RSSI over time below it (open in any web browser)
and prints a summary (pings, replies, lost %, RSSI / SNR range).

Columns: time_utc,event,seq,peer,rssi,snr,peer_rssi,peer_snr,lat,lon,fix
Events: ping (sent), pong (a reply: rssi/snr heard here, peer_rssi/peer_snr heard there),
ping-heard (the other unit's ping, heard here), lost (no reply to the last ping), stop.
No third-party packages needed.
"""
import argparse
import csv
import html
import os
import sys


def load(paths):
    rows = []
    for p in paths:
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                r["_file"] = os.path.basename(p)
                rows.append(r)
    return rows


def num(r, k, default=0.0):
    try:
        return float(r.get(k, "") or default)
    except ValueError:
        return default


def colour(rssi):
    """KML aabbggrr / SVG #rrggbb from RSSI: -60 dBm and up green, -120 and below red."""
    t = max(0.0, min(1.0, (rssi + 120.0) / 60.0))
    r, g = int(255 * (1 - t)), int(200 * t)
    return "ff00%02x%02x" % (g, r), "#%02x%02x00" % (r, g)


def summary(rows):
    sent = sum(1 for r in rows if r["event"] == "ping")
    pong = [r for r in rows if r["event"] == "pong"]
    heard = [r for r in rows if r["event"] == "ping-heard"]
    lost = sum(1 for r in rows if r["event"] == "lost")
    out = ["pings sent %d, replies %d, lost %d (%.0f %%), their pings heard %d" %
           (sent, len(pong), lost, 100.0 * lost / sent if sent else 0, len(heard))]
    rx = [num(r, "rssi") for r in pong + heard]
    snr = [num(r, "snr") for r in pong + heard]
    if rx:
        out.append("RSSI here %d .. %d dBm, SNR %.1f .. %.1f dB" % (min(rx), max(rx), min(snr), max(snr)))
    there = [num(r, "peer_rssi") for r in pong]
    if there:
        out.append("RSSI at the other unit %d .. %d dBm" % (min(there), max(there)))
    fixed = sum(1 for r in rows if r.get("fix") == "1")
    out.append("%d of %d events had a GPS fix" % (fixed, len(rows)))
    return out


def write_kml(rows, path):
    pts = []
    for r in rows:
        if r.get("fix") != "1" or r["event"] not in ("pong", "ping-heard", "lost"):
            continue
        la, lo = num(r, "lat"), num(r, "lon")
        if r["event"] == "lost":
            style, name = "lost", "lost #%s" % r["seq"]
            desc = "%s lost (no reply)" % r["time_utc"]
        else:
            rssi = num(r, "rssi")
            style, name = "s%d" % int(min(0, max(-130, rssi))), "%d dBm" % rssi
            desc = "%s %s seq %s: here %s dBm / SNR %s" % (r["time_utc"], r["event"], r["seq"], r["rssi"], r["snr"])
            if r["event"] == "pong":
                desc += "; there %s dBm / SNR %s" % (r["peer_rssi"], r["peer_snr"])
        pts.append((style, name, desc, la, lo, r.get("rssi")))
    styles = set(p[0] for p in pts)
    with open(path, "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<kml xmlns="http://www.opengis.net/kml/2.2"><Document>\n')
        f.write("<name>RatSpeak range test</name>\n")
        for s in sorted(styles):
            if s == "lost":
                f.write('<Style id="lost"><IconStyle><color>ff0000ff</color><scale>0.9</scale><Icon><href>'
                        'http://maps.google.com/mapfiles/kml/shapes/cross-hairs.png</href></Icon></IconStyle></Style>\n')
            else:
                kc, _ = colour(float(s[1:]))
                f.write('<Style id="%s"><IconStyle><color>%s</color><scale>0.7</scale><Icon><href>'
                        'http://maps.google.com/mapfiles/kml/shapes/shaded_dot.png</href></Icon></IconStyle></Style>\n'
                        % (s, kc))
        for style, name, desc, la, lo, _ in pts:
            f.write("<Placemark><name>%s</name><description>%s</description><styleUrl>#%s</styleUrl>"
                    "<Point><coordinates>%.6f,%.6f,0</coordinates></Point></Placemark>\n"
                    % (html.escape(name), html.escape(desc), style, lo, la))
        f.write("</Document></kml>\n")
    return len(pts)


def write_svg(rows, path, lines):
    W, H, M = 900, 900, 50
    pts = [r for r in rows if r.get("fix") == "1" and r["event"] in ("pong", "ping-heard", "lost")]
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" font-family="sans-serif" font-size="13">'
           % (W, H), '<rect width="100%" height="100%" fill="white"/>',
           '<text x="%d" y="24" font-size="16">RatSpeak range test</text>' % M]
    for i, t in enumerate(lines):
        svg.append('<text x="%d" y="%d">%s</text>' % (M, 44 + 16 * i, html.escape(t)))
    top, mapH = 120, 480
    if pts:
        las = [num(r, "lat") for r in pts]
        los = [num(r, "lon") for r in pts]
        la0, la1, lo0, lo1 = min(las), max(las), min(los), max(los)
        span = max(la1 - la0, (lo1 - lo0) * 0.72, 1e-4)
        def xy(la, lo):
            return (M + (lo - lo0) * 0.72 / span * (W - 2 * M), top + mapH - (la - la0) / span * mapH)
        svg.append('<rect x="%d" y="%d" width="%d" height="%d" fill="none" stroke="#999"/>' % (M, top, W - 2 * M, mapH))
        for r in pts:
            x, y = xy(num(r, "lat"), num(r, "lon"))
            if r["event"] == "lost":
                svg.append('<path d="M%.1f %.1f l8 8 m0 -8 l-8 8" stroke="red" stroke-width="2" transform="translate(-4 -4)"/>'
                           % (x, y))
            else:
                svg.append('<circle cx="%.1f" cy="%.1f" r="5" fill="%s"><title>%s dBm</title></circle>'
                           % (x, y, colour(num(r, "rssi"))[1], r["rssi"]))
        svg.append('<text x="%d" y="%d">%.5f,%.5f</text>' % (M, top + mapH + 16, la0, lo0))
        svg.append('<text x="%d" y="%d" text-anchor="end">%.5f,%.5f</text>' % (W - M, top - 4, la1, lo1))
    else:
        svg.append('<text x="%d" y="%d">no events with a GPS fix: no map</text>' % (M, top + 20))
    # RSSI over time
    sig = [r for r in rows if r["event"] in ("pong", "ping-heard")]
    gy, gh = top + mapH + 50, H - (top + mapH + 50) - 30
    svg.append('<rect x="%d" y="%d" width="%d" height="%d" fill="none" stroke="#999"/>' % (M, gy, W - 2 * M, gh))
    svg.append('<text x="%d" y="%d">RSSI over time (dBm, -40 top, -130 bottom); red = lost</text>' % (M, gy - 6))
    n = len(rows)
    for i, r in enumerate(rows):
        x = M + (W - 2 * M) * i / max(1, n - 1)
        if r["event"] == "lost":
            svg.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="red"/>' % (x, gy, x, gy + gh))
        elif r in sig:
            y = gy + gh * (-40 - num(r, "rssi")) / 90.0
            svg.append('<circle cx="%.1f" cy="%.1f" r="2.5" fill="%s"/>' % (x, y, colour(num(r, "rssi"))[1]))
    svg.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(svg))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--out", help="output name without extension (default: next to the first CSV)")
    a = ap.parse_args()
    rows = load(a.csv)
    if not rows:
        sys.exit("no rows")
    base = a.out or os.path.splitext(a.csv[0])[0]
    lines = summary(rows)
    for l in lines:
        print(l)
    n = write_kml(rows, base + ".kml")
    write_svg(rows, base + ".svg", lines)
    print("%s.kml (%d points), %s.svg" % (base, n, base))


if __name__ == "__main__":
    main()
