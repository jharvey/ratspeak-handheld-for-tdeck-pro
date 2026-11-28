#!/usr/bin/env python3
"""Screen catalog: one picture of every screen, named by the keys that reach it, an index page and a
walkthrough video (2026-11-28). Runs on the simulator; no radio needed.

  python tools/howto/make_catalog.py --sim C:/Users/MyPC/simrun/simb/ratspeak-sim.exe
         --seed M:/ratspeak-video/howto_seed [--out docs/screens] [--video docs/videos/Screen_catalog.mp4]

1. a fresh simulated radio (folder <sim dir>/simdata/catalog): GPS in San Antonio, the "atlas" and
   "library" SD seeds, a few contacts / messages / a group / a marker added through its console;
2. the screen walk below (SCREENS) as a simulator script: keys, then "shot <n>.png";
3. <out>/<n>_<name>.png (240x320 doubled), <out>/index.html and index.md (title, keys, picture),
   and a video with each screen for 3 s under its title (ffmpeg on PATH or $FFMPEG).
To add a screen: add a line to SCREENS (title, keys to reach it from the Home screen).
"""
import argparse, html, os, shutil, subprocess, sys, time

# (title, keys from Home). Keys: "M S 1" = M then S then 1. Two-digit settings rows: "1 0" (the
# script waits after the first digit). "@cmd" = a console line (serial), "~ms" = wait.
SCREENS = [
    ("Home", ""),
    ("Menu", "M"),
    ("Inbox", "I"),
    ("Contacts", "C"),
    ("Contact page", "C V ENTER"),
    ("Chat", "C V ENTER 1"),
    ("Chat: share menu (MIC)", "C V ENTER 1 MIC"),
    ("Peers", "M P"),
    ("Settings", "M S"),
    ("S.1 Mode", "M S 1 ~1500"),
    ("S.1.5 Typical features", "M S 1 ~1500 5"),
    ("S.2 Time & Location", "M S 2"),
    ("S.2.1 Set RTC", "M S 2 1 ~1500"),
    ("S.2.2 Time zone", "M S 2 2"),
    ("S.2.4 Manual location", "M S 2 4"),
    ("S.2.9 Waypoints", "M S 2 9"),
    ("S.2.10 Track", "M S 2 1 0"),
    ("S.3 WiFi", "M S 3"),
    ("S.3.3 Scan networks", "M S 3 3 ~3000"),
    ("S.3.4 Saved networks", "M S 3 4"),
    ("S.3.6 TCP hub", "M S 3 6"),
    ("S.3.7 Home Assistant voice", "M S 3 7"),
    ("S.3.12 Remote console", "M S 3 1 2"),
    ("S.4 Screen & I/O", "M S 4"),
    ("S.4.2 Idle splash", "M S 4 2"),
    ("S.4.2.9 If-found details", "M S 4 2 9"),
    ("S.5 Power", "M S 5"),
    ("S.5.7 Check-in", "M S 5 7"),
    ("S.5.8 Custom", "M S 5 8"),
    ("S.5.9 Energy monitor", "M S 5 9"),
    ("S.5.13 Switch off", "M S 5 1 3"),
    ("S.6 Battery", "M S 6"),
    ("S.6.8 Battery log", "M S 6 8"),
    ("S.6.12 Low-battery notice", "M S 6 1 2"),
    ("S.7 Identity", "M S 7"),
    ("S.7.5 Identities", "M S 7 5"),
    ("S.8 LoRa", "M S 8"),
    ("S.9 Alerts", "M S 9"),
    ("S.9.5 Arrival alerts", "M S 9 5"),
    ("S.10 Diagnostics", "M S 1 0"),
    ("S.10.9 Ready for a trip?", "M S 1 0 9 ~2500"),
    ("S.10.10 GPS signals", "M S 1 0 1 0 ~3000"),
    ("S.11 Storage", "M S 1 1"),
    ("S.11.5 SD card", "M S 1 1 5"),
    ("S.11.5.9 Restore backup (no backups on the card: stays on S.11.5)", "M S 1 1 5 9"),
    ("Atlas", "A ~3000"),
    ("Atlas: keys (K)", "A ~3000 K"),
    ("Atlas: map layers (O)", "A ~3000 O"),
    ("Atlas: markers (J)", "A ~3000 J"),
    ("Atlas: new marker (Alt+J)", "A ~3000 ALT+J"),
    ("Atlas: find a place (L)", "A ~3000 L"),
    ("Library", "M R"),
    ("Library: a help file", "M R 1 ~1500"),
    ("Library: reader keys (K)", "M R 1 ~1500 K"),
    ("Idle screen: splash", "@idlescr 0 @idle now ~3000"),
    ("Idle screen: map", "@idlescr 1 @idle now ~4000"),
    ("Idle screen: library page", "@idlescr 2 @idle now ~3000"),
    ("Idle screen: if found", "@idlescr 3 @idle now ~3000"),
]

PREP = """expect 60000 [BOOT] ready
serial wiz skip
wait 2000
serial name ROBIN
wait 1000
serial opmode 2
wait 800
serial contact 1cd73a4b54411ae8aa90efdb1cd73a4b SAM
wait 400
serial contact 3bb4b09700e7c1d5a7c50111caf10175 SAGE
wait 400
serial contact 6032c8bc2cb0ba47aeeddb7cdb2b0121 JILL
wait 400
serial grp new TRAIL CREW
wait 600
serial grp add 1 1cd73a4b54411ae8aa90efdb1cd73a4b
wait 400
serial grp add 1 3bb4b09700e7c1d5a7c50111caf10175
wait 400
serial maplayers 0
wait 400
serial idlescr 0
wait 1500
"""


def step_lines(keys):
    out = []
    toks = keys.split()
    for i, k in enumerate(toks):
        if k.startswith("~"):
            out.append("wait %s" % k[1:]); continue
        if k.startswith("@"):
            # console line: "@idle now" -> words up to the next token starting with @ or ~
            continue
        out.append("key %s" % k)
        nxt = toks[i + 1] if i + 1 < len(toks) else ""
        # a digit that could start a two-digit settings row waits for the second one
        out.append("wait %d" % (1600 if k.isdigit() and not nxt.isdigit() else 450 if k.isdigit() else 900))
    return out


def console_lines(keys):
    out, cur = [], None
    for k in keys.split():
        if k.startswith("@"):
            if cur: out.append(cur)
            cur = k[1:]
        elif k.startswith("~"):
            if cur: out.append(cur); cur = None
            out.append("~" + k[1:])
        elif cur is not None:
            cur += " " + k
    if cur: out.append(cur)
    res = []
    for c in out:
        res += ["wait %s" % c[1:]] if c.startswith("~") else ["serial %s" % c, "wait 800"]
    return res


def script(screens):
    s = [PREP]
    for i, (title, keys) in enumerate(screens):
        s.append("# %03d %s" % (i, title))
        # back to Home first: leave menus / pages / the map / the library
        s += ["key BACK", "wait 300", "key BACK", "wait 300", "key BACK", "wait 300", "key BACK", "wait 300",
              "key H", "wait 900"]
        if keys.startswith("@"):
            s += console_lines(keys)
        else:
            s += step_lines(keys)
        s += ["wait 1200", "shot %03d.png" % i]
        if keys.startswith("@"):
            s += ["key SPACE", "wait 1500"]               # wake from the idle screen
    s += ["quit"]
    return "\n".join(s) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--out", default="docs/screens")
    ap.add_argument("--video", default="docs/videos/Screen_catalog.mp4")
    ap.add_argument("--only-script", action="store_true")
    ap.add_argument("--no-sim", action="store_true", help="reuse the pictures of the last run")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = os.path.join(root, a.out) if not os.path.isabs(a.out) else a.out
    simdir = os.path.dirname(a.sim)
    dev = os.path.join(simdir, "simdata", "catalog")
    if a.no_sim:
        pass
    elif os.path.isdir(dev):
        shutil.rmtree(dev)
    if not a.no_sim:
        os.makedirs(os.path.join(dev, "sd"))
        open(os.path.join(dev, "sim.ini"), "w").write("uptime = 1d2h\ngps = 29.42600,-98.48610\n")
    for sd in (() if a.no_sim else ("atlas", "library")):
        shutil.copytree(os.path.join(a.seed, sd), os.path.join(dev, "sd"), dirs_exist_ok=True)
    sc = os.path.join(simdir, "catalog_walk.txt")
    open(sc, "w").write(script(SCREENS))
    if a.only_script:
        print(sc); return
    r = None
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", ""), "Fonts"))
    t0 = time.time()
    r = None if a.no_sim else subprocess.run([a.sim, "--name", "catalog", "--fast", "--script", sc, "--quit"], env=env,
                       capture_output=True, text=True, errors="replace", timeout=3600)
    if r is not None:
        print("simulator: exit %d, %.0f s" % (r.returncode, time.time() - t0))
    if r is not None and r.returncode:
        print(r.stdout[-2000:]); print(r.stderr[-1000:])
    shots = os.path.join(dev, "shots")
    os.makedirs(out, exist_ok=True)
    rows = []
    for i, (title, keys) in enumerate(SCREENS):
        src = os.path.join(shots, "%03d.png" % i)
        if not os.path.exists(src):
            print("missing:", title); continue
        safe = "".join(c if c.isalnum() or c in ".-" else "_" for c in title).strip("_")
        fn = "%03d_%s.png" % (i, safe)
        shutil.copyfile(src, os.path.join(out, fn))
        rows.append((i, title, keys, fn))
    kd = lambda k: " ".join(x for x in k.split() if not x.startswith("~")) or "(start)"
    md = ["# Screen catalog", "", "Every screen, as of %s, from the simulator (`tools/howto/make_catalog.py`). "
          "Keys are from the Home screen; `@...` is a console command." % time.strftime("%d %b %Y"), "",
          "| # | Screen | Keys | Picture |", "|---|---|---|---|"]
    md += ["| %d | %s | `%s` | <img src=\"%s\" width=\"120\"> |" % (i, t, kd(k), f) for i, t, k, f in rows]
    open(os.path.join(out, "index.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    cards = "\n".join('<figure><a href="%s"><img src="%s" loading="lazy"></a><figcaption><b>%s</b><br><code>%s</code></figcaption></figure>'
                      % (f, f, html.escape(t), html.escape(kd(k))) for i, t, k, f in rows)
    open(os.path.join(out, "index.html"), "w", encoding="utf-8").write(
        "<!doctype html><meta charset=utf-8><title>Screen catalog</title><style>body{font-family:sans-serif;margin:16px}"
        "main{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:14px}figure{margin:0}"
        "img{width:100%%;image-rendering:pixelated;border:1px solid #999}figcaption{font-size:13px;margin-top:4px}</style>"
        "<h1>Screen catalog</h1><p>%d screens, %s. Keys are from the Home screen.</p><main>%s</main>"
        % (len(rows), time.strftime("%d %b %Y"), cards))
    print("%d screens -> %s" % (len(rows), out))
    # walkthrough video: each screen 3 s with its title and keys above it
    ff = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if not ff or not rows:
        return
    from PIL import Image, ImageDraw, ImageFont
    tmp = os.path.join(simdir, "catalog_frames")
    shutil.rmtree(tmp, ignore_errors=True); os.makedirs(tmp)
    try:
        f1 = ImageFont.truetype("arialbd.ttf", 30); f2 = ImageFont.truetype("consola.ttf", 24)
    except OSError:
        f1 = f2 = ImageFont.load_default()
    for n, (i, t, k, f) in enumerate(rows):
        im = Image.open(os.path.join(out, f)).convert("RGB")
        c = Image.new("RGB", (720, 900), (236, 220, 182))
        d = ImageDraw.Draw(c)
        d.text((360, 40), t, font=f1, fill=(60, 40, 20), anchor="mm")
        d.text((360, 85), "keys: " + kd(k), font=f2, fill=(60, 40, 20), anchor="mm")
        im = im.resize((480, 640), Image.NEAREST)
        c.paste(im, (120, 130))
        c.save(os.path.join(tmp, "f%03d.png" % n))
    subprocess.run([ff, "-v", "error", "-y", "-framerate", "1/3", "-i", os.path.join(tmp, "f%03d.png"),
                    "-vf", "fps=25,format=yuv420p", "-c:v", "libx264", "-crf", "24",
                    os.path.join(root, a.video) if not os.path.isabs(a.video) else a.video], check=True)
    print("video ->", a.video)


if __name__ == "__main__":
    main()
