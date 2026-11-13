"""Context-model coder for 1-bit 256x256 map tiles (pack format RMP2; decoder in MapView.cpp).

Each pixel is coded with an adaptive binary range coder (the LZMA one, 12-bit probabilities,
adaptation shift 5) whose probability is picked by a 10-pixel context:

    row y-2:        x-1 x x+1          -> context bits 7..9  (x-1 = bit 9)
    row y-1:  x-2 x-1 x x+1 x+2        -> context bits 2..6  (x-2 = bit 6)
    row y  :  x-2 x-1 [x]              -> bit 1 = x-2, bit 0 = x-1
Pixels outside the tile are white (0). Bit 1 = black, rows top to bottom, MSB = left pixel.
The 1024 probabilities start from a table trained on the whole pack (stored once per pack).
USGS topo tiles code to ~1.2 KB (8 KB raw; deflate ~2.3 KB).
"""

NCTX = 1024
SHIFT = 5
PMIN, PMAX = 31, 4065


def _rows(bits):
    """8192 bytes -> 256 rows of 260 ints (2 white pixels of padding each side)."""
    rows = []
    for y in range(256):
        r = [0, 0]
        for b in bits[y * 32:(y + 1) * 32]:
            r.extend(((b >> 7) & 1, (b >> 6) & 1, (b >> 5) & 1, (b >> 4) & 1,
                      (b >> 3) & 1, (b >> 2) & 1, (b >> 1) & 1, b & 1))
        r += [0, 0]
        rows.append(r)
    return rows


def contexts(bits):
    """Yields (context, pixel) for every pixel in coding order."""
    white = [0] * 260
    rows = _rows(bits)
    for y in range(256):
        cur = rows[y]
        r1 = rows[y - 1] if y >= 1 else white
        r2 = rows[y - 2] if y >= 2 else white
        w1 = (r1[0] << 3) | (r1[1] << 2) | (r1[2] << 1) | r1[3]      # x-2..x+1 for x=0
        w2 = (r2[1] << 1) | r2[2]                                    # x-1..x  for x=0
        c = 0
        for x in range(256):
            w1 = ((w1 << 1) | r1[x + 4]) & 31
            w2 = ((w2 << 1) | r2[x + 3]) & 7
            px = cur[x + 2]
            yield c | (w1 << 2) | (w2 << 7), px
            c = ((c << 1) | px) & 3
    # c: bit0 = x-1, bit1 = x-2


def train(tiles):
    """Starting probabilities (of a white pixel, /4096) for a set of tiles."""
    n0 = [1] * NCTX
    n1 = [1] * NCTX
    for t in tiles:
        for ctx, px in contexts(t):
            if px:
                n1[ctx] += 1
            else:
                n0[ctx] += 1
    return [min(PMAX, max(PMIN, (4096 * n0[i]) // (n0[i] + n1[i]))) for i in range(NCTX)]


def encode(bits, prior):
    probs = list(prior)
    out = bytearray()
    low, rng, cache, csize = 0, 0xFFFFFFFF, 0, 1

    def shift_low():
        nonlocal low, cache, csize
        if low < 0xFF000000 or low >= 1 << 32:
            carry = low >> 32
            temp = cache
            while True:
                out.append((temp + carry) & 0xFF)
                temp = 0xFF
                csize -= 1
                if not csize:
                    break
            cache = (low >> 24) & 0xFF
        csize += 1
        low = (low & 0x00FFFFFF) << 8

    for ctx, px in contexts(bits):
        p = probs[ctx]
        bound = (rng >> 12) * p
        if px:
            low += bound
            rng -= bound
            probs[ctx] = p - (p >> SHIFT)
        else:
            rng = bound
            probs[ctx] = p + ((4096 - p) >> SHIFT)
        while rng < 1 << 24:
            rng = (rng << 8) & 0xFFFFFFFF
            shift_low()
    for _ in range(5):
        shift_low()
    return bytes(out)


def decode(data, prior):
    """Reference decoder (the firmware's is the same in C)."""
    probs = list(prior)
    pos = 5
    code = int.from_bytes(data[1:5], "big")
    rng = 0xFFFFFFFF
    out = bytearray(8192)
    white = [0] * 260
    rows = []
    for y in range(256):
        r1 = rows[y - 1] if y >= 1 else white
        r2 = rows[y - 2] if y >= 2 else white
        cur = [0] * 260
        w1 = (r1[0] << 3) | (r1[1] << 2) | (r1[2] << 1) | r1[3]
        w2 = (r2[1] << 1) | r2[2]
        c = 0
        for x in range(256):
            w1 = ((w1 << 1) | r1[x + 4]) & 31
            w2 = ((w2 << 1) | r2[x + 3]) & 7
            ctx = c | (w1 << 2) | (w2 << 7)
            p = probs[ctx]
            bound = (rng >> 12) * p
            if code < bound:
                rng = bound
                probs[ctx] = p + ((4096 - p) >> SHIFT)
                px = 0
            else:
                code -= bound
                rng -= bound
                probs[ctx] = p - (p >> SHIFT)
                px = 1
                out[y * 32 + (x >> 3)] |= 0x80 >> (x & 7)
            while rng < 1 << 24:
                rng = (rng << 8) & 0xFFFFFFFF
                code = ((code << 8) | (data[pos] if pos < len(data) else 0)) & 0xFFFFFFFF
                pos += 1
            cur[x + 2] = px
            c = ((c << 1) | px) & 3
        rows.append(cur)
    return bytes(out)
