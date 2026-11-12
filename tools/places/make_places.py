#!/usr/bin/env python3
"""Build the place-name index for the T-Deck Pro's map search (map key F).

Source: USGS GNIS Domestic Names (public domain), the national text file:
  https://prd-tnm.s3.amazonaws.com/StagedProducts/GeographicNames/DomesticNames/DomesticNames_National_Text.zip

    python make_places.py --gnis DomesticNames_National_Text.zip --out sdcard --states ME,NH,VT,TN,NC
    python ../sdcard/push_files.py --port COM5 --dest /places sdcard/ratdeckpro/places/*.txt

Output: <out>/ratdeckpro/places/<c>.txt, one file per first letter (a-z, 0 = other),
lines "Name|lat|lon|K|ST" sorted by name (case-insensitive). K = kind:
P populated place, S summit, L lake / reservoir, I island, B bay, F falls, G gap,
C cape, H beach. All populated places in the US are included; the other kinds only for
--states (default: the states between Maine and Tennessee / North Carolina).
"""
import argparse
import io
import os
import zipfile

KINDS = {"Populated Place": "P", "Summit": "S", "Lake": "L", "Reservoir": "L", "Island": "I",
         "Bay": "B", "Falls": "F", "Gap": "G", "Cape": "C", "Beach": "H"}
ABBR = {"Alabama": "AL", "Alaska": "AK", "Arizona": "AZ", "Arkansas": "AR", "California": "CA", "Colorado": "CO",
        "Connecticut": "CT", "Delaware": "DE", "District of Columbia": "DC", "Florida": "FL", "Georgia": "GA",
        "Hawaii": "HI", "Idaho": "ID", "Illinois": "IL", "Indiana": "IN", "Iowa": "IA", "Kansas": "KS",
        "Kentucky": "KY", "Louisiana": "LA", "Maine": "ME", "Maryland": "MD", "Massachusetts": "MA",
        "Michigan": "MI", "Minnesota": "MN", "Mississippi": "MS", "Missouri": "MO", "Montana": "MT",
        "Nebraska": "NE", "Nevada": "NV", "New Hampshire": "NH", "New Jersey": "NJ", "New Mexico": "NM",
        "New York": "NY", "North Carolina": "NC", "North Dakota": "ND", "Ohio": "OH", "Oklahoma": "OK",
        "Oregon": "OR", "Pennsylvania": "PA", "Rhode Island": "RI", "South Carolina": "SC", "South Dakota": "SD",
        "Tennessee": "TN", "Texas": "TX", "Utah": "UT", "Vermont": "VT", "Virginia": "VA", "Washington": "WA",
        "West Virginia": "WV", "Wisconsin": "WI", "Wyoming": "WY", "Puerto Rico": "PR", "Guam": "GU",
        "Virgin Islands": "VI", "American Samoa": "AS", "Northern Mariana Islands": "MP"}
ROUTE = "ME,NH,VT,MA,CT,RI,NY,NJ,PA,DE,MD,WV,VA,TN,NC"


def ascii_name(s):
    import unicodedata
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return s.replace("|", "/").strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gnis", required=True)
    ap.add_argument("--out", default="sdcard")
    ap.add_argument("--states", default=ROUTE, help="all kinds for these states; populated places everywhere")
    a = ap.parse_args()
    full = set(x.strip().upper() for x in a.states.split(",") if x.strip())
    z = zipfile.ZipFile(a.gnis)
    member = [n for n in z.namelist() if n.lower().endswith(".txt")][0]
    f = io.TextIOWrapper(z.open(member), encoding="utf-8-sig", errors="replace")
    cols = f.readline().rstrip("\n").split("|")
    ix = {c: i for i, c in enumerate(cols)}
    rows = []
    for ln in f:
        p = ln.rstrip("\n").split("|")
        k = KINDS.get(p[ix["feature_class"]])
        if not k:
            continue
        st = ABBR.get(p[ix["state_name"]], p[ix["state_name"]][:2].upper())
        if k != "P" and st not in full:
            continue
        try:
            lat, lon = float(p[ix["prim_lat_dec"]]), float(p[ix["prim_long_dec"]])
        except ValueError:
            continue
        if lat == 0 and lon == 0:
            continue
        name = ascii_name(p[ix["feature_name"]])
        if name:
            rows.append((name.lower(), "%s|%.5f|%.5f|%s|%s" % (name, lat, lon, k, st)))
    rows.sort()
    root = os.path.join(a.out, "ratdeckpro", "places")
    os.makedirs(root, exist_ok=True)
    files = {}
    for key, line in rows:
        c = key[0] if "a" <= key[0] <= "z" else "0"
        files.setdefault(c, []).append(line)
    total = 0
    for c, lines in sorted(files.items()):
        with open(os.path.join(root, c + ".txt"), "w", newline="\n", encoding="ascii") as out:
            out.write("\n".join(lines) + "\n")
        total += os.path.getsize(os.path.join(root, c + ".txt"))
    print("%d places in %d files, %.1f MB, under %s" % (len(rows), len(files), total / 1e6, root))


if __name__ == "__main__":
    main()
