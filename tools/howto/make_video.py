#!/usr/bin/env python3
"""Turn a simulator recording into a how-to video (MP4, H.264 + AAC).

The simulator script records with "rec <dir>" ... "rec off" (sim/README.md): <dir> holds
fNNNNN.png (whole device drawing, saved when it changes) and rec.txt with the frame times,
key taps (cap rectangles) and captions. This script:
  * plays the frames at their real times, with long quiet gaps shortened,
  * draws a pointing hand (tools/howto/hand.svg, turned ~45 degrees) on each key pressed,
    a ring on held modifiers (Alt, SYM) and a badge with the key's name,
  * shows the current caption above the device,
  * adds a short click sound on every key press,
  * encodes with ffmpeg (found on PATH, or set FFMPEG=...).

  python tools/howto/make_video.py <rec dir> <out.mp4> [--title "Send a message"]
Needs Pillow.
"""
import argparse
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FPS = 25
SCALE = 4                 # hand drawn at 4x, then reduced (smooth edges)
HAND_ANGLE = 40           # degrees, counter-clockwise: finger points up and to the left
HAND_SIZE = 0.78          # hand height relative to its SVG units (116) -> frame pixels / SVG unit
QUIET_MAX_MS = 2200       # a gap with nothing happening longer than this ...
QUIET_KEEP_MS = 1300      # ... is shortened to this
HAND_BEFORE_MS = 220      # hand shows this long before a press
HAND_AFTER_MS = 650       # and stays this long after it
GLIDE_MS = 160            # moves to the next key this fast when presses follow each other


def find_font(size, bold=False):
    for name in (["segoeuib.ttf", "arialbd.ttf"] if bold else ["segoeui.ttf", "arial.ttf"]):
        for d in (os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts"), "/usr/share/fonts/truetype/dejavu"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


# ---- hand: parse the SVG paths (M L Q Z only) and draw them --------------------------
def svg_paths(svg_text):
    out = []
    for m in re.finditer(r'<path([^>]*)/>', svg_text, re.S):
        attrs = m.group(1)
        d = re.search(r'\bd="([^"]*)"', attrs, re.S).group(1)
        filled = 'fill="none"' not in attrs
        toks = re.findall(r'[MLQZ]|-?\d+(?:\.\d+)?', d)
        subs, cur, i, pos = [], [], 0, (0.0, 0.0)
        while i < len(toks):
            t = toks[i]
            if t == 'M':
                if cur:
                    subs.append(cur)
                pos = (float(toks[i + 1]), float(toks[i + 2]))
                cur = [pos]
                i += 3
            elif t == 'L':
                pos = (float(toks[i + 1]), float(toks[i + 2]))
                cur.append(pos)
                i += 3
            elif t == 'Q':
                c = (float(toks[i + 1]), float(toks[i + 2]))
                e = (float(toks[i + 3]), float(toks[i + 4]))
                for k in range(1, 11):
                    s = k / 10
                    cur.append(((1 - s) ** 2 * pos[0] + 2 * (1 - s) * s * c[0] + s * s * e[0],
                                (1 - s) ** 2 * pos[1] + 2 * (1 - s) * s * c[1] + s * s * e[1]))
                pos = e
                i += 5
            elif t == 'Z':
                i += 1
            else:
                i += 1
        if cur:
            subs.append(cur)
        out.append((filled, subs))
    return out


def make_hand(px_per_unit):
    """RGBA image of the hand, turned; returns (image, fingertip x, y in that image)."""
    svg = open(os.path.join(HERE, "hand.svg"), encoding="utf-8").read()
    paths = svg_paths(svg)
    k = px_per_unit * SCALE
    pad = 6
    w, h = int((84 + 2 * pad) * k), int((116 + 2 * pad) * k)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    lw = max(2, int(2.6 * k))
    to = lambda p: ((p[0] + pad) * k, (p[1] + pad) * k)
    for filled, subs in paths:
        for sub in subs:
            pts = [to(p) for p in sub]
            if filled:
                d.polygon(pts, fill=(255, 255, 255, 255))
            d.line(pts + ([pts[0]] if filled else []), fill=(0, 0, 0, 255), width=lw, joint="curve")
    tip = to((30, 1))
    # rotate around the image centre, then find where the tip went
    cx, cy = w / 2, h / 2
    rot = img.rotate(HAND_ANGLE, resample=Image.BICUBIC, expand=True)
    a = math.radians(HAND_ANGLE)
    dx, dy = tip[0] - cx, tip[1] - cy
    # PIL rotates counter-clockwise on screen (y down): x' = dx cos + dy sin, y' = -dx sin + dy cos
    rx = dx * math.cos(a) + dy * math.sin(a) + rot.width / 2
    ry = -dx * math.sin(a) + dy * math.cos(a) + rot.height / 2
    small = rot.resize((rot.width // SCALE, rot.height // SCALE), Image.LANCZOS)
    return small, rx / SCALE, ry / SCALE


# ---- recording -------------------------------------------------------------------------
def load_rec(d):
    frames, keys, caps, end = [], [], [], 0
    for line in open(os.path.join(d, "rec.txt"), encoding="utf-8"):
        line = line.rstrip("\n")
        if not line:
            continue
        kind, rest = line[0], line[2:]
        if kind == "F":
            t, f = rest.split(" ", 1)
            frames.append((int(t), f))
        elif kind in "KDU":
            parts = rest.split(" ")
            t, spec = int(parts[0]), parts[1]
            rects = [tuple(int(v) for v in p.split(",")) for p in parts[2:] if p.count(",") == 3]
            keys.append((int(t), kind, spec, rects))
        elif kind == "C":
            t, _, text = rest.partition(" ")
            caps.append((int(t), text))
        elif kind == "E":
            end = int(rest)
    end = max(end, frames[-1][0] if frames else 0, keys[-1][0] + 1000 if keys else 0)
    return frames, keys, caps, end


def time_map(points, end):
    """Piecewise map recording ms -> video ms, shortening quiet gaps."""
    pts = sorted(set([0] + points + [end]))
    segs, vt = [], 0
    for a, b in zip(pts, pts[1:]):
        gap = b - a
        out = gap if gap <= QUIET_MAX_MS else QUIET_KEEP_MS
        segs.append((a, b, vt, vt + out))
        vt += out
    def fwd(t):
        for a, b, va, vb in segs:
            if a <= t <= b:
                return va + (t - a) * (vb - va) / max(1, b - a)
        return vt
    def back(v):
        for a, b, va, vb in segs:
            if va <= v <= vb:
                return a + (v - va) * (b - a) / max(1, vb - va)
        return end
    return fwd, back, vt


KEY_NAMES = {"ENTER": "Enter", "RETURN": "Enter", "BKSP": "Backspace", "SPACE": "Space", "SHIFT": "^ (Shift)",
             "BACK": "^ (Shift)", "^": "^ (Shift)", "ALT": "Alt", "SYM": "SYM", "MIC": "MIC", "RSHIFT": "Shift"}
SYM_OF = {"Q": "#", "W": "1", "E": "2", "R": "3", "T": "(", "Y": ")", "U": "-", "I": "_", "O": "+", "P": "@",
          "A": "*", "S": "4", "D": "5", "F": "6", "G": "/", "H": ":", "J": ";", "K": "'", "L": '"', "Z": "7",
          "X": "8", "C": "9", "V": "?", "B": "!", "N": "'", "M": "`", "MIC": "0"}
LETTER_OF = {v: k for k, v in SYM_OF.items() if k not in ("N",)}


def key_label(spec):
    if len(spec) == 1 and not spec.isalpha():
        if spec in LETTER_OF:
            return "%s  (SYM + %s)" % (spec, LETTER_OF[spec].replace("MIC", "MIC"))
        return spec
    parts = spec.split("+")
    if len(parts) == 2 and parts[0].upper() == "ALT" and len(parts[1]) == 1:
        return "%s  (Alt + %s)" % (parts[1].lower(), parts[1])
    if len(parts) == 2 and parts[0].upper() == "SYM" and parts[1].upper() in SYM_OF:
        return "%s  (SYM + %s)" % (SYM_OF[parts[1].upper()], parts[1].upper())
    return " + ".join(KEY_NAMES.get(p.upper(), p) for p in parts)


def wrap(draw, text, font, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=font) <= width or not cur:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def click_wav(path, times_ms, total_ms, rate=44100):
    n = int(total_ms * rate / 1000) + rate // 2
    buf = [0.0] * n
    rnd = random.Random(7)
    click = []
    for i in range(int(0.018 * rate)):
        t = i / rate
        env = math.exp(-t * 420)
        click.append(env * (0.55 * math.sin(2 * math.pi * 2900 * t) + 0.25 * math.sin(2 * math.pi * 1300 * t)
                            + 0.35 * (rnd.random() * 2 - 1)))
    for tm in times_ms:
        s = int(tm * rate / 1000)
        for i, v in enumerate(click):
            if 0 <= s + i < n:
                buf[s + i] += v
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(v * 0.6 * 32767)))) for v in buf))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("out")
    ap.add_argument("--title", default="")
    ap.add_argument("--crop-bottom", type=int, default=0, help="pixels to cut off the device drawing's bottom")
    args = ap.parse_args()

    ffmpeg = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("ffmpeg not found (PATH or FFMPEG=...)")
    frames, keys, caps, end = load_rec(args.rec)
    if not frames:
        sys.exit("no frames in " + args.rec)
    taps = [k for k in keys if k[1] in "KD"]
    fwd, back, vend = time_map([t for t, _ in frames] + [k[0] for k in keys] +
                               [k[0] + HAND_AFTER_MS for k in keys] + [k[0] - HAND_BEFORE_MS for k in keys if k[0] > HAND_BEFORE_MS] +
                               [c[0] for c in caps], end)
    vend += 600

    first = Image.open(os.path.join(args.rec, frames[0][1])).convert("RGB")
    # The device drawing sits on a black (formerly transparent) background: crop to it and
    # use the background as a mask, so the rounded corners show the page colour.
    dark = first.convert("L").point(lambda v: 255 if v > 3 else 0)
    bx0, by0, bx1, by1 = dark.getbbox()
    by1 -= args.crop_bottom
    crop = (bx0, by0, bx1, by1)
    mask = dark.crop(crop)
    dw, dh = bx1 - bx0, by1 - by0
    # the simulator's "panel partial 0 ms" line near the bottom: paint it over with the body colour
    foot = (int(dw * 0.25), int(dh * 0.925), int(dw * 0.75), int(dh * 0.965))
    body = first.getpixel((bx0 + int(dw * 0.22), by0 + int(dh * 0.945)))
    cap_h = 150
    W = (max(dw + 120, 600) + 1) // 2 * 2
    H = (dh + cap_h + 16 + 1) // 2 * 2
    ox, oy = (W - dw) // 2, cap_h
    hand, tipx, tipy = make_hand(HAND_SIZE * dw / 410)
    f_title = find_font(max(16, W // 22), bold=True)
    f_cap = find_font(max(14, W // 26))
    f_key = find_font(max(14, W // 24), bold=True)

    tmp = tempfile.mkdtemp(prefix="howto_")
    wav = os.path.join(tmp, "clicks.wav")
    click_wav(wav, [fwd(k[0]) for k in taps], vend)
    cmd = [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H),
           "-r", str(FPS), "-i", "-", "-i", wav, "-c:v", "libx264", "-preset", "slow", "-crf", "24",
           "-pix_fmt", "yuv420p", "-tune", "animation", "-c:a", "aac", "-b:a", "96k", "-shortest",
           "-movflags", "+faststart", args.out]
    enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)

    cache = {}
    def frame_at(t):
        lo, hi = 0, len(frames) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if frames[mid][0] <= t:
                lo = mid
            else:
                hi = mid - 1
        f = frames[lo][1]
        if f not in cache:
            if len(cache) > 8:
                cache.clear()
            im = Image.open(os.path.join(args.rec, f)).convert("RGB").crop(crop)
            ImageDraw.Draw(im).rectangle(foot, fill=body)
            cache[f] = im
        return cache[f]

    # key rectangles are in full-frame pixels: move them by the crop
    taps[:] = [(t, kind, spec, [(r[0] - bx0, r[1] - by0, r[2], r[3]) for r in rects]) for t, kind, spec, rects in taps]

    def centre(r):
        return (ox + r[0] + r[2] / 2 + r[2] * 0.12, oy + r[1] + r[3] / 2 + r[3] * 0.18)

    nfr = int(vend * FPS / 1000)
    for i in range(nfr):
        vt = i * 1000 / FPS
        t = back(vt)
        img = Image.new("RGB", (W, H), (250, 250, 248))
        img.paste(frame_at(t), (ox, oy), mask)
        d = ImageDraw.Draw(img)
        # caption band
        y = 10
        if args.title:
            d.text((W // 2, y), args.title, font=f_title, fill=(20, 20, 20), anchor="mt")
            y += f_title.size + 10
        cap = ""
        for ct, text in caps:
            if ct <= t:
                cap = text
        for line in wrap(d, cap, f_cap, W - 30)[:3]:
            d.text((W // 2, y), line, font=f_cap, fill=(60, 60, 60), anchor="mt")
            y += f_cap.size + 6
        # hand: the press that is current (shown from HAND_BEFORE_MS before to HAND_AFTER_MS after)
        cur = None
        for j, k in enumerate(taps):
            if k[0] - HAND_BEFORE_MS <= t <= k[0] + HAND_AFTER_MS and k[3]:
                cur = j
        if cur is None:
            for j, k in enumerate(taps):          # between two close presses: stay
                nk = taps[j + 1] if j + 1 < len(taps) else None
                if nk and k[3] and nk[3] and k[0] <= t <= nk[0] and nk[0] - k[0] < 1400:
                    cur = j
        if cur is not None:
            k = taps[cur]
            pos = centre(k[3][-1])
            nk = taps[cur + 1] if cur + 1 < len(taps) else None
            if nk and nk[3] and nk[0] - k[0] < 1400 and nk[0] - GLIDE_MS < t < nk[0]:
                a = (t - (nk[0] - GLIDE_MS)) / GLIDE_MS
                a = max(0.0, min(1.0, a))
                a = a * a * (3 - 2 * a)
                p2 = centre(nk[3][-1])
                pos = (pos[0] + (p2[0] - pos[0]) * a, pos[1] + (p2[1] - pos[1]) * a)
                if a > 0.5:
                    k = nk
            pressed = 0 <= t - k[0] <= 140
            for r in k[3][:-1]:           # modifiers held: a ring
                cx, cy = ox + r[0] + r[2] / 2, oy + r[1] + r[3] / 2
                rr = max(r[2], r[3]) * 0.62
                d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), outline=(230, 120, 0), width=3)
            if pressed:
                r = k[3][-1]
                d.rounded_rectangle((ox + r[0] - 2, oy + r[1] - 2, ox + r[0] + r[2] + 2, oy + r[1] + r[3] + 2),
                                    radius=6, outline=(230, 120, 0), width=3)
            push = 3 if pressed else 0
            hx = int(pos[0] - tipx + push)
            hy = int(pos[1] - tipy + push)
            img.paste(hand, (hx, hy), hand)
            # key name badge: above the keyboard, on the screen's lower edge
            label = key_label(k[2])
            tw = d.textlength(label, font=f_key)
            bx, by = W // 2, oy + int(dh * 0.615)
            d.rounded_rectangle((bx - tw / 2 - 14, by - 4, bx + tw / 2 + 14, by + f_key.size + 12), radius=10,
                                fill=(30, 30, 30))
            d.text((bx, by + 2), label, font=f_key, fill=(255, 255, 255), anchor="mt")
        enc.stdin.write(img.tobytes())
    enc.stdin.close()
    rc = enc.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    if rc:
        sys.exit("ffmpeg failed (%d)" % rc)
    print("%s: %.1f s, %d presses" % (args.out, vend / 1000, len(taps)))


if __name__ == "__main__":
    main()
