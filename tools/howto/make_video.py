#!/usr/bin/env python3
"""Turn simulator recordings into a how-to video (MP4, H.264 + AAC).

The simulator script records with "rec <dir>" ... "rec off" (sim/README.md): <dir> holds
fNNNNN.png (whole device drawing, saved when it changes) and rec.txt:
  T0 <epoch ms>               start (lines up two devices recorded at the same time)
  F <ms> <file>               a frame
  K|D|U <ms> <key> x,y,w,h    key tap / key held down / released (cap rectangle in frame pixels)
  C <ms> <text>               caption above the device(s) from now on
  N <ms> <dur> <text>         a note box over the screen for dur ms
  P <ms> <dur> <x> <y>        a hand points (and wiggles) at x,y for dur ms
  E <ms>                      end
This script:
  * plays the frames at their real times, with long quiet gaps shortened,
  * draws pointing hands (tools/howto/hand.svg, turned ~40 degrees): a key tap gets one hand;
    a held SYM / Alt gets the other hand (the key further right gets the right hand),
  * a badge with the key's name ("1 (SYM + W)"), captions, note boxes,
  * click sounds: a full click per tap, a half click when a modifier goes down and up,
  * one device, or two side by side (--rec2, --labels Jack Jill: a coloured name tag above each),
  * encodes with ffmpeg (found on PATH, or set FFMPEG=...).

  python tools/howto/make_video.py <rec dir> <out.mp4> [--title "Send a message"]
         [--rec2 <rec dir>] [--labels Jack Jill]
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

from PIL import Image, ImageDraw, ImageFont, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
FPS = 25
SCALE = 4                 # hand drawn at 4x, then reduced (smooth edges)
HAND_ANGLE = 40           # degrees, counter-clockwise: finger points up and to the left
HAND_SIZE = 0.78          # frame pixels per SVG unit, for a 410 px wide device
QUIET_MAX_MS = 2200       # a gap with nothing happening longer than this ...
QUIET_KEEP_MS = 1300      # ... is shortened to this
READ_MS_PER_CHAR = 55     # but never while a caption is still being read (this long per character,
READ_MIN_MS = 1500        # at least this long), nor while a note box shows
READ_STEP_MS = 1000       # (time points this far apart keep the real speed)
HAND_BEFORE_MS = 220      # hand shows this long before a press
HAND_AFTER_MS = 650       # and stays this long after it
HOLD_BEFORE_MS = 250      # a held modifier's hand arrives this long before it goes down
HOLD_AFTER_MS = 350       # and leaves this long after it is released
GLIDE_MS = 160            # moves to the next key this fast when presses follow each other
STAY_MS = 900             # between two presses closer than this the hand stays on screen
ORANGE = (230, 120, 0)
CAP_BG = (236, 220, 182)       # caption banner: antique paper (parchment), dark brown bold text
CAP_TEXT, CAP_EDGE = (62, 40, 18), (128, 92, 48)
CAP_POP = (255, 250, 232)      # ... flashes to bright new paper when the caption changes, then ages back
CAP_POP_EDGE = (196, 150, 40)  # (its edge flashes gold; no red / blue anywhere, no person's colour)
CAP_POP_MS = 900               # how long the change flash takes to fade
CAP_GROW_MS = 220              # the banner grows in from 92 % over this time
NOTE_POP_MS = 200              # note boxes grow in from 85 % over this time


def mix(a, b, k):
    return tuple(int(round(x + (y - x) * k)) for x, y in zip(a, b))


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
            else:
                i += 1
        if cur:
            subs.append(cur)
        out.append((filled, subs))
    return out


def make_hand(px_per_unit):
    """Right hand, turned; returns (RGBA image, fingertip x, y in that image)."""
    svg = open(os.path.join(HERE, "hand.svg"), encoding="utf-8").read()
    k = px_per_unit * SCALE
    pad = 6
    w, h = int((84 + 2 * pad) * k), int((116 + 2 * pad) * k)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    lw = max(2, int(2.6 * k))
    to = lambda p: ((p[0] + pad) * k, (p[1] + pad) * k)
    for filled, subs in svg_paths(svg):
        for sub in subs:
            pts = [to(p) for p in sub]
            if filled:
                d.polygon(pts, fill=(255, 255, 255, 255))
            d.line(pts + ([pts[0]] if filled else []), fill=(0, 0, 0, 255), width=lw, joint="curve")
    tip = to((30, 1))
    cx, cy = w / 2, h / 2
    rot = img.rotate(HAND_ANGLE, resample=Image.BICUBIC, expand=True)
    a = math.radians(HAND_ANGLE)
    dx, dy = tip[0] - cx, tip[1] - cy
    rx = dx * math.cos(a) + dy * math.sin(a) + rot.width / 2
    ry = -dx * math.sin(a) + dy * math.cos(a) + rot.height / 2
    small = rot.resize((rot.width // SCALE, rot.height // SCALE), Image.LANCZOS)
    return small, rx / SCALE, ry / SCALE


KEY_NAMES = {"ENTER": "Enter", "RETURN": "Enter", "BKSP": "Backspace", "SPACE": "Space", "SHIFT": "^ (Shift)",
             "BACK": "^ (Shift)", "^": "^ (Shift)", "ALT": "Alt", "SYM": "SYM", "MIC": "MIC", "RSHIFT": "Shift"}
SYM_OF = {"Q": "#", "W": "1", "E": "2", "R": "3", "T": "(", "Y": ")", "U": "-", "I": "_", "O": "+", "P": "@",
          "A": "*", "S": "4", "D": "5", "F": "6", "G": "/", "H": ":", "J": ";", "K": "'", "L": '"', "Z": "7",
          "X": "8", "C": "9", "V": "?", "B": "!", "N": "'", "M": "`", "MIC": "0"}
LETTER_OF = {v: k for k, v in SYM_OF.items() if k not in ("N",)}


# Each person's colour (name tag over the device): 1 green, 2 orange, 3 light purple.
LABEL_COLOURS = [(40, 150, 70), (235, 125, 20), (155, 115, 215)]
NOTE_FILL, NOTE_EDGE = (255, 236, 130, 245), (190, 140, 0)   # note boxes: yellow, apart from the banner


def key_label(spec):
    if len(spec) == 1 and not spec.isalpha():
        if spec in LETTER_OF:
            return "%s  (SYM + %s)" % (spec, LETTER_OF[spec])
        return spec
    parts = [p.upper() for p in spec.split("+")]
    if len(parts) == 2 and parts[0] == "ALT" and len(parts[1]) == 1:
        return "%s  (Alt + %s)" % (parts[1].lower(), parts[1])
    if len(parts) == 2 and parts[0] == "SYM" and parts[1] in SYM_OF:
        return "%s  (SYM + %s)" % (SYM_OF[parts[1]], parts[1])
    return " + ".join(KEY_NAMES.get(p, p) for p in parts)


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


def click_wav(path, clicks, total_ms, rate=44100, audio=()):
    """clicks: (ms, kind) with kind 'full' or 'half'. audio: (ms, samples) with 16 kHz int16
    samples (what the simulated speaker played and microphone heard), mixed in at ms."""
    n = int(total_ms * rate / 1000) + rate // 2
    buf = [0.0] * n
    rnd = random.Random(7)
    def make(length, decay, f1, f2, gain):
        out = []
        for i in range(int(length * rate)):
            t = i / rate
            env = math.exp(-t * decay)
            out.append(gain * env * (0.55 * math.sin(2 * math.pi * f1 * t) + 0.25 * math.sin(2 * math.pi * f2 * t)
                                     + 0.35 * (rnd.random() * 2 - 1)))
        return out
    full = make(0.018, 420, 2900, 1300, 1.0)
    half = make(0.010, 650, 3600, 1900, 0.45)
    for tm, kind in clicks:
        s = int(tm * rate / 1000)
        for i, v in enumerate(full if kind == "full" else half):
            if 0 <= s + i < n:
                buf[s + i] += v
    for tm, pcm in audio:                       # 16 kHz -> rate, linear interpolation
        s = int(tm * rate / 1000)
        m = int(len(pcm) * rate / 16000)
        for j in range(m):
            if not 0 <= s + j < n:
                continue
            x = j * 16000 / rate
            k = int(x)
            a = pcm[k] if k < len(pcm) else 0
            b = pcm[k + 1] if k + 1 < len(pcm) else a
            buf[s + j] += (a + (b - a) * (x - k)) / 32768 * 1.4
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(v * 0.6 * 32767)))) for v in buf))


# ---- one recording ---------------------------------------------------------------------
class Rec:
    def __init__(self, d, crop_bottom=0, mic=False):
        self.dir = d
        self.frames, self.keys, self.caps, self.notes, self.points = [], [], [], [], []
        self.t0, end = None, 0
        for line in open(os.path.join(d, "rec.txt"), encoding="utf-8"):
            line = line.rstrip("\n")
            if not line:
                continue
            kind, _, rest = line.partition(" ")
            if kind == "T0":
                self.t0 = int(rest)
            elif kind == "F":
                t, f = rest.split(" ", 1)
                self.frames.append((int(t), f))
            elif kind in ("K", "D", "U"):
                parts = rest.split(" ")
                rects = [tuple(int(v) for v in p.split(",")) for p in parts[2:] if p.count(",") == 3]
                self.keys.append((int(parts[0]), kind, parts[1].upper(), rects))
            elif kind == "C":
                t, _, text = rest.partition(" ")
                self.caps.append((int(t), text))
            elif kind == "N":
                t, dur, text = (rest.split(" ", 2) + ["", ""])[:3]
                self.notes.append((int(t), int(dur), text))
            elif kind == "P":
                t, dur, x, y = rest.split(" ")[:4]
                self.points.append((int(t), int(dur), int(x), int(y)))
            elif kind == "E":
                end = int(rest)
        if not self.frames:
            sys.exit("no frames in " + d)
        # audio.pcm (simulator speaker + microphone): int64 wall ms, uint32 n, n x int16 16 kHz
        # mic.pcm: what the microphone heard (the clean voice), only with --mic
        self.audio = []
        for ap in [os.path.join(d, "audio.pcm")] + ([os.path.join(d, "mic.pcm")] if mic else []):
            if not (os.path.exists(ap) and self.t0):
                continue
            raw = open(ap, "rb").read()
            i = 0
            while i + 12 <= len(raw):
                wall, cnt = struct.unpack_from("<qI", raw, i)
                i += 12
                pcm = struct.unpack_from("<%dh" % cnt, raw, i)
                i += cnt * 2
                if any(pcm):                      # silence adds nothing
                    self.audio.append((wall - self.t0, pcm))
            end = max(end, max((t + len(p) // 16 for t, p in self.audio), default=0))
        self.end = max(end, self.frames[-1][0], self.keys[-1][0] + 1000 if self.keys else 0)

        first = Image.open(os.path.join(d, self.frames[0][1])).convert("RGB")
        # The device drawing sits on a black (formerly transparent) background: crop to it
        # and use the background as a mask, so the rounded corners show the page colour.
        dark = first.convert("L").point(lambda v: 255 if v > 3 else 0)
        bx0, by0, bx1, by1 = dark.getbbox()
        by1 -= crop_bottom
        self.crop = (bx0, by0, bx1, by1)
        self.mask = dark.crop(self.crop)
        self.w, self.h = bx1 - bx0, by1 - by0
        # the simulator's "panel partial 0 ms" line: painted over with the body colour
        self.foot = (int(self.w * 0.25), int(self.h * 0.925), int(self.w * 0.75), int(self.h * 0.965))
        self.body = first.getpixel((bx0 + int(self.w * 0.22), by0 + int(self.h * 0.945)))
        sh = lambda r: (r[0] - bx0, r[1] - by0, r[2], r[3])
        self.keys = [(t, k, s, [sh(r) for r in rs]) for t, k, s, rs in self.keys]
        self.points = [(t, dur, x - bx0, y - by0) for t, dur, x, y in self.points]
        self.cache = {}
        self.build_targets()

    def shift(self, off):
        """Move every time by off ms (two devices on one clock)."""
        self.frames = [(t + off, f) for t, f in self.frames]
        self.keys = [(t + off, k, s, r) for t, k, s, r in self.keys]
        self.caps = [(t + off, c) for t, c in self.caps]
        self.notes = [(t + off, d, n) for t, d, n in self.notes]
        self.points = [(t + off, d, x, y) for t, d, x, y in self.points]
        self.audio = [(t + off, p) for t, p in self.audio]
        self.end += off
        self.build_targets()

    def build_targets(self):
        """Per hand ('L', 'R'): what it points at and when.
        target = (show from, show to, rect, press from, press to, label, kind)"""
        holds = []                      # (down, up, name, rect)
        open_ = {}
        for t, kind, spec, rects in self.keys:
            if kind == "D" and rects:
                open_[spec] = (t, rects[-1])
            elif kind == "U" and spec in open_:
                d, r = open_.pop(spec)
                holds.append([d, t, spec, r, None])
        for spec, (d, r) in open_.items():
            holds.append([d, self.end, spec, r, None])
        cx = lambda r: r[0] + r[2] / 2
        taps = []
        for t, kind, spec, rects in self.keys:
            if kind != "K" or not rects:
                continue
            held = [h for h in holds if h[0] <= t <= h[1]]
            hand, label = "R", key_label(spec)
            if held:
                h = held[-1]
                label = key_label(h[2] + "+" + spec)
                hand = "R" if cx(rects[-1]) >= cx(h[3]) else "L"
                if h[4] is None:
                    h[4] = "L" if hand == "R" else "R"
            taps.append((t - HAND_BEFORE_MS, t + HAND_AFTER_MS, rects[-1], t, t + 140, label, "tap", rects[:-1], hand))
        self.targets = {"L": [], "R": []}
        for t0, t1, r, p0, p1, label, kind, mods, hand in taps:
            self.targets[hand].append((t0, t1, r, p0, p1, label, kind, mods))
        for d, u, name, r, hand in holds:
            if hand is None:
                hand = "L" if name == "ALT" else "R"
            self.targets[hand].append((d - HOLD_BEFORE_MS, u + HOLD_AFTER_MS, r, d, u, KEY_NAMES.get(name, name) + " (hold)",
                                       "hold", []))
        for k in self.targets:
            self.targets[k].sort(key=lambda x: x[0])
        self.clicks = [(t, "full" if k == "K" else "half") for t, k, s, r in self.keys if k in ("K", "D", "U")]

    def frame_at(self, t):
        lo, hi = 0, len(self.frames) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.frames[mid][0] <= t:
                lo = mid
            else:
                hi = mid - 1
        f = self.frames[lo][1]
        if f not in self.cache:
            if len(self.cache) > 8:
                self.cache.clear()
            im = Image.open(os.path.join(self.dir, f)).convert("RGB").crop(self.crop)
            ImageDraw.Draw(im).rectangle(self.foot, fill=self.body)
            self.cache[f] = im
        return self.cache[f]


def centre(r):
    return (r[0] + r[2] / 2 + r[2] * 0.12, r[1] + r[3] / 2 + r[3] * 0.18)


def hand_state(targets, t):
    """(position, target) of one hand at time t, or None."""
    i = -1
    for j, tg in enumerate(targets):
        if tg[0] <= t:
            i = j
    if i < 0:
        return None
    tg = targets[i]
    pos = centre(tg[2])
    if t <= tg[1]:
        # just arrived from the previous key: glide
        if i > 0 and targets[i - 1][1] + STAY_MS >= tg[0] and t - tg[0] < GLIDE_MS:
            a = (t - tg[0]) / GLIDE_MS
            a = a * a * (3 - 2 * a)
            p0 = centre(targets[i - 1][2])
            pos = (p0[0] + (pos[0] - p0[0]) * a, p0[1] + (pos[1] - p0[1]) * a)
        return pos, tg
    nxt = targets[i + 1] if i + 1 < len(targets) else None
    if nxt and nxt[0] - tg[1] < STAY_MS:
        return pos, None                  # waiting for the next key
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("out")
    ap.add_argument("--title", default="")
    ap.add_argument("--rec2", default="")
    ap.add_argument("--labels", nargs="*", default=[])
    ap.add_argument("--crop-bottom", type=int, default=0, help="pixels to cut off the device drawing's bottom")
    ap.add_argument("--mic", action="store_true", help="also the microphone's sound (the clean voice)")
    args = ap.parse_args()

    ffmpeg = os.environ.get("FFMPEG") or shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("ffmpeg not found (PATH or FFMPEG=...)")
    recs = [Rec(args.rec, args.crop_bottom, args.mic)] + ([Rec(args.rec2, args.crop_bottom, args.mic)] if args.rec2 else [])
    if len(recs) == 2 and recs[0].t0 and recs[1].t0:
        base = min(r.t0 for r in recs)
        for r in recs:
            r.shift(r.t0 - base)
    end = max(r.end for r in recs)
    pts = []
    for r in recs:
        pts += [t for t, _ in r.frames] + [c[0] for c in r.caps]
        for k in r.keys:
            pts += [k[0], k[0] + HAND_AFTER_MS, max(0, k[0] - HAND_BEFORE_MS)]
        for n in r.notes:                       # a note box keeps its whole time: no shortening under it
            pts += list(range(n[0], n[0] + n[1], READ_STEP_MS)) + [n[0] + n[1]]
        for i, c in enumerate(r.caps):          # a new caption gets time to be read before a quiet gap is cut
            stop = min(c[0] + max(READ_MIN_MS, READ_MS_PER_CHAR * len(c[1])),
                       r.caps[i + 1][0] if i + 1 < len(r.caps) else r.end)
            pts += list(range(c[0], stop, READ_STEP_MS)) + [stop]
        for p in r.points:
            pts += [p[0], p[0] + p[1]]
        for t, pcm in r.audio:                  # sound plays at its real speed: no shortening
            pts += list(range(t, t + len(pcm) // 16 + 1, 500)) + [t + len(pcm) // 16]
    # time map: recording ms -> video ms, quiet gaps shortened
    pts = sorted(set([0, end] + [p for p in pts if 0 <= p <= end]))
    segs, vend = [], 0
    for a, b in zip(pts, pts[1:]):
        out = b - a if b - a <= QUIET_MAX_MS else QUIET_KEEP_MS
        segs.append((a, b, vend, vend + out))
        vend += out
    def fwd(t):
        for a, b, va, vb in segs:
            if a <= t <= b:
                return va + (t - a) * (vb - va) / max(1, b - a)
        return vend
    def back(v):
        for a, b, va, vb in segs:
            if va <= v <= vb:
                return a + (v - va) * (b - a) / max(1, vb - va)
        return end
    vend += 600

    dw = recs[0].w
    gap = 40
    label_h = 62 if args.labels else 0
    cap_h = 178 + label_h
    W = (max(sum(r.w for r in recs) + gap * (len(recs) - 1) + 120, 600) + 1) // 2 * 2
    H = (max(r.h for r in recs) + cap_h + 16 + 1) // 2 * 2
    x = (W - (sum(r.w for r in recs) + gap * (len(recs) - 1))) // 2
    for r in recs:
        r.ox, r.oy = x, cap_h
        x += r.w + gap
    hand_r, tipx_r, tipy_r = make_hand(HAND_SIZE * dw / 410)
    hand_l = ImageOps.mirror(hand_r)
    tipx_l, tipy_l = hand_l.width - tipx_r, tipy_r
    f_title = find_font(max(16, 600 // 22), bold=True)
    f_cap = find_font(max(16, 600 // 25), bold=True)
    f_key = find_font(max(14, 600 // 24), bold=True)
    f_note = find_font(max(14, dw // 19), bold=True)
    f_label = find_font(34, bold=True)

    tmp = tempfile.mkdtemp(prefix="howto_")
    wav = os.path.join(tmp, "clicks.wav")
    click_wav(wav, [(fwd(t), k) for r in recs for t, k in r.clicks], vend,
              audio=[(fwd(t), p) for r in recs for t, p in r.audio])
    cmd = [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H),
           "-r", str(FPS), "-i", "-", "-i", wav, "-c:v", "libx264", "-preset", "slow", "-crf", "24",
           "-pix_fmt", "yuv420p", "-tune", "animation", "-c:a", "aac", "-b:a", "96k", "-shortest",
           "-movflags", "+faststart", args.out]
    enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    caps = sorted(c for r in recs for c in r.caps)
    presses = 0

    for i in range(int(vend * FPS / 1000)):
        v = i * 1000 / FPS
        t = back(v)
        img = Image.new("RGB", (W, H), (250, 250, 248))
        d = ImageDraw.Draw(img)
        y = 10
        if args.title:
            d.text((W // 2, y), args.title, font=f_title, fill=(20, 20, 20), anchor="mt")
            y += f_title.size + 10
        cap, cap_t = "", 0
        for ct, text in caps:
            if ct <= t:
                cap, cap_t = text, ct
        if cap:
            # A banner that stands out: antique paper with dark brown bold text. When the caption changes
            # it flashes to bright new paper with a gold edge and grows in, then ages back.
            lines = wrap(d, cap, f_cap, W - 90)[:3]
            lh = f_cap.size + 8
            age = v - fwd(cap_t)
            k = max(0.0, 1.0 - age / CAP_POP_MS) ** 1.5
            grow = min(1.0, age / CAP_GROW_MS)
            bw = (W - 40) * (0.92 + 0.08 * grow)
            bh = lh * len(lines) + 20
            cx = W / 2
            top = y + 2
            d.rounded_rectangle((cx - bw / 2 + 3, top + 4, cx + bw / 2 + 3, top + bh + 4), radius=14,
                                fill=(205, 205, 200))                       # soft shadow
            d.rounded_rectangle((cx - bw / 2, top, cx + bw / 2, top + bh), radius=14, fill=mix(CAP_BG, CAP_POP, k),
                                outline=mix(CAP_EDGE, CAP_POP_EDGE, k), width=3 + int(round(3 * k)))
            yy = top + 11
            for line in lines:
                d.text((cx, yy), line, font=f_cap, fill=CAP_TEXT, anchor="mt")
                yy += lh
        for n, r in enumerate(recs):
            ox, oy = r.ox, r.oy
            img.paste(r.frame_at(t), (ox, oy), r.mask)
            if n < len(args.labels):                     # name tag above each device: who's who
                tag, col = args.labels[n].upper(), LABEL_COLOURS[n % len(LABEL_COLOURS)]
                tw = d.textlength(tag, font=f_label)
                d.rounded_rectangle((ox + r.w / 2 - tw / 2 - 22, oy - 58, ox + r.w / 2 + tw / 2 + 22, oy - 8),
                                    radius=12, fill=col)
                d.text((ox + r.w // 2, oy - 33), tag, font=f_label, fill=(255, 255, 255), anchor="mm")
            badge = None
            for side in ("L", "R"):
                st = hand_state(r.targets[side], t)
                if not st:
                    continue
                pos, tg = st
                pressed = bool(tg) and tg[3] <= t <= tg[4]
                if tg:
                    for mr in tg[7]:                    # old-style combo: a ring on the modifier
                        cx, cy = ox + mr[0] + mr[2] / 2, oy + mr[1] + mr[3] / 2
                        rr = max(mr[2], mr[3]) * 0.62
                        d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), outline=ORANGE, width=3)
                    if pressed:
                        kr = tg[2]
                        d.rounded_rectangle((ox + kr[0] - 2, oy + kr[1] - 2, ox + kr[0] + kr[2] + 2, oy + kr[1] + kr[3] + 2),
                                            radius=6, outline=ORANGE, width=3)
                    if badge is None or tg[6] == "tap":
                        badge = tg[5]
                push = 3 if pressed else 0
                hand, tx, ty = (hand_r, tipx_r, tipy_r) if side == "R" else (hand_l, tipx_l, tipy_l)
                px = ox + pos[0] + (push if side == "R" else -push)
                img.paste(hand, (int(px - tx), int(oy + pos[1] - ty + push)), hand)
            for pt, dur, px, py in r.points:            # a hand points (wiggling) at something
                if pt <= t <= pt + dur:
                    wig = 7 * math.sin((t - pt) / 1000 * 2 * math.pi * 2.2)
                    rr = 24 + 4 * math.sin((t - pt) / 1000 * 2 * math.pi * 1.5)
                    d.ellipse((ox + px - rr, oy + py - rr, ox + px + rr, oy + py + rr), outline=ORANGE, width=4)
                    img.paste(hand_r, (int(ox + px + 6 + wig - tipx_r), int(oy + py + 6 + wig - tipy_r)), hand_r)
            if badge:
                tw = d.textlength(badge, font=f_key)
                bx, by = ox + r.w // 2, oy + int(r.h * 0.615)
                d.rounded_rectangle((bx - tw / 2 - 14, by - 4, bx + tw / 2 + 14, by + f_key.size + 12), radius=10,
                                    fill=(30, 30, 30))
                d.text((bx, by + 2), badge, font=f_key, fill=(255, 255, 255), anchor="mt")
            for nt, dur, text in r.notes:               # note box over the screen
                if nt <= t <= nt + dur and text:
                    sx0, sx1 = ox + r.w * 0.09, ox + r.w * 0.91
                    lines = wrap(d, text, f_note, sx1 - sx0 - 28)
                    lh = f_note.size + 7
                    bh = lh * len(lines) + 24
                    cy = oy + r.h * 0.30
                    g = 0.85 + 0.15 * min(1.0, (v - fwd(nt)) / NOTE_POP_MS)   # pops in
                    mx = (sx0 + sx1) / 2
                    bx0, bx1 = mx - (sx1 - sx0) / 2 * g, mx + (sx1 - sx0) / 2 * g
                    bhh = bh / 2 * g
                    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
                    od = ImageDraw.Draw(over)
                    od.rounded_rectangle((bx0 + 4, cy - bhh + 5, bx1 + 4, cy + bhh + 5), radius=14, fill=(0, 0, 0, 70))
                    od.rounded_rectangle((bx0, cy - bhh, bx1, cy + bhh), radius=14, fill=NOTE_FILL,
                                         outline=NOTE_EDGE, width=5)
                    img.paste(over, (0, 0), over)
                    d = ImageDraw.Draw(img)
                    yy = cy - bh / 2 + 12
                    for line in (lines if g >= 1.0 else []):    # text once the box is full size
                        d.text(((sx0 + sx1) / 2, yy), line, font=f_note, fill=(30, 30, 30), anchor="mt")
                        yy += lh
        enc.stdin.write(img.tobytes())
    enc.stdin.close()
    rc = enc.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    if rc:
        sys.exit("ffmpeg failed (%d)" % rc)
    presses = sum(1 for r in recs for k in r.keys if k[1] == "K")
    print("%s: %.1f s, %d presses" % (args.out, vend / 1000, presses))


if __name__ == "__main__":
    main()
