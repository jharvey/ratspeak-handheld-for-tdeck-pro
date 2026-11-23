"""Bench test of the unit's ESPHome native API (Home Assistant voice), over USB serial.

The unit's console command "apitunnel" carries the API byte stream as hex lines ("@API <hex>"
out, "api <hex>" in), so this runs without WiFi. This script plays Home Assistant's side the
way aioesphomeapi does: Noise_NNpsk0_25519_ChaChaPoly_SHA256 handshake with the unit's key,
Hello, DeviceInfo, ListEntities, SubscribeVoiceAssistant, then a voice run (talk ... talkstop).

The crypto here is written from the specs (RFC 7748, RFC 8439, the Noise spec) in plain
Python, independent of the firmware's code, so the two are checked against each other.

  python tools/test/esphome_api_test.py COM6 <44-char key>       (key: console "hakey")
  python tools/test/esphome_api_test.py COM9 <key> 192.168.0.11  (over WiFi, TCP 6053)
  python tools/test/esphome_api_test.py COM6 <key> --wake        (the wake word cycle, below)
  key "auto": read it from the unit (console "hakey"). Opening the port can restart a unit:
  the script waits for "[BOOT] ready" first.

--wake plays Home Assistant with a wake word engine: the unit (console "hawake 2") opens a
run with the wake word flag and streams its mic; we answer, report the wake word, end the
speech, answer and end the run; the unit must open a new wake word run by itself; MIC
(console "talk") must end that run and start push-to-talk; "hawake 0" must end listening.
"""
import base64
import hashlib
import hmac
import os
import struct
import sys
import time

import serial

# ---- X25519 (RFC 7748) ---------------------------------------------------------------------
P = 2**255 - 19
A24 = 121665


def x25519(k: bytes, u: bytes) -> bytes:
    k = bytearray(k)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    scalar = int.from_bytes(k, "little")
    x1 = int.from_bytes(u, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in reversed(range(255)):
        kt = (scalar >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a, b = (x2 + z2) % P, (x2 - z2) % P
        aa, bb = a * a % P, b * b % P
        e = (aa - bb) % P
        c, d = (x3 + z3) % P, (x3 - z3) % P
        da, cb = d * a % P, c * b % P
        x3 = (da + cb) ** 2 % P
        z3 = x1 * (da - cb) ** 2 % P
        x2 = aa * bb % P
        z2 = e * (aa + A24 * e) % P
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, P - 2, P) % P).to_bytes(32, "little")


# ---- ChaCha20-Poly1305 (RFC 8439) -------------------------------------------------------------
def _rotl(v, c):
    return ((v << c) & 0xFFFFFFFF) | (v >> (32 - c))


def _chacha_block(key, counter, nonce):
    s = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574] + list(struct.unpack("<8I", key)) + [counter] + list(
        struct.unpack("<3I", nonce))
    x = s[:]

    def qr(a, b, c, d):
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rotl(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rotl(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rotl(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rotl(x[b] ^ x[c], 7)

    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack("<16I", *[(x[i] + s[i]) & 0xFFFFFFFF for i in range(16)])


def _chacha_xor(key, counter, nonce, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        ks = _chacha_block(key, counter + i // 64, nonce)
        out += bytes(a ^ b for a, b in zip(data[i:i + 64], ks))
    return bytes(out)


def _poly1305(key, msg):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    acc, p = 0, (1 << 130) - 5
    for i in range(0, len(msg), 16):
        n = int.from_bytes(msg[i:i + 16] + b"\x01", "little")
        acc = (acc + n) * r % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def _pad16(b):
    return b"\x00" * ((16 - len(b) % 16) % 16)


def aead_seal(key, nonce, ad, pt):
    ct = _chacha_xor(key, 1, nonce, pt)
    otk = _chacha_block(key, 0, nonce)[:32]
    mac = _poly1305(otk, ad + _pad16(ad) + ct + _pad16(ct) + struct.pack("<QQ", len(ad), len(ct)))
    return ct + mac


def aead_open(key, nonce, ad, data):
    ct, mac = data[:-16], data[-16:]
    otk = _chacha_block(key, 0, nonce)[:32]
    if not hmac.compare_digest(mac, _poly1305(otk, ad + _pad16(ad) + ct + _pad16(ct) + struct.pack("<QQ", len(ad), len(ct)))):
        raise ValueError("bad tag")
    return _chacha_xor(key, 1, nonce, ct)


def nonce(n):
    return b"\x00" * 4 + struct.pack("<Q", n)


# ---- Noise NNpsk0 initiator -----------------------------------------------------------------
def hkdf(ck, ikm, n):
    tk = hmac.new(ck, ikm, hashlib.sha256).digest()
    out, prev = [], b""
    for i in range(1, n + 1):
        prev = hmac.new(tk, prev + bytes([i]), hashlib.sha256).digest()
        out.append(prev)
    return out


class Initiator:
    def __init__(self, psk: bytes, prologue: bytes):
        name = b"Noise_NNpsk0_25519_ChaChaPoly_SHA256"
        self.h = hashlib.sha256(name).digest() if len(name) > 32 else name.ljust(32, b"\x00")
        self.ck = self.h
        self.k, self.n = None, 0
        self.mix_hash(prologue)
        self.psk = psk

    def mix_hash(self, d):
        self.h = hashlib.sha256(self.h + d).digest()

    def mix_key(self, ikm):
        self.ck, self.k = hkdf(self.ck, ikm, 2)
        self.n = 0

    def mix_key_and_hash(self, ikm):
        self.ck, th, self.k = hkdf(self.ck, ikm, 3)
        self.mix_hash(th)
        self.n = 0

    def encrypt_and_hash(self, pt):
        ct = aead_seal(self.k, nonce(self.n), self.h, pt)
        self.n += 1
        self.mix_hash(ct)
        return ct

    def decrypt_and_hash(self, ct):
        pt = aead_open(self.k, nonce(self.n), self.h, ct)
        self.n += 1
        self.mix_hash(ct)
        return pt

    def write_message1(self):
        self.mix_key_and_hash(self.psk)              # psk
        self.e = os.urandom(32)                      # e
        self.epub = x25519(self.e, (9).to_bytes(32, "little"))
        self.mix_hash(self.epub)
        self.mix_key(self.epub)
        return self.epub + self.encrypt_and_hash(b"")

    def read_message2(self, m):
        re = m[:32]                                  # e
        self.mix_hash(re)
        self.mix_key(re)
        self.mix_key(x25519(self.e, re))             # ee
        self.decrypt_and_hash(m[32:])
        k1, k2 = hkdf(self.ck, b"", 2)
        return k1, k2                                # initiator sends with k1, receives with k2


# ---- protobuf --------------------------------------------------------------------------------
def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def pb_fields(d):
    i, out = 0, []
    while i < len(d):
        k, s = 0, 0
        while True:
            b = d[i]; i += 1; k |= (b & 0x7F) << s; s += 7
            if not b & 0x80: break
        f, wt = k >> 3, k & 7
        if wt == 0:
            v, s = 0, 0
            while True:
                b = d[i]; i += 1; v |= (b & 0x7F) << s; s += 7
                if not b & 0x80: break
            out.append((f, v))
        elif wt == 2:
            l, s = 0, 0
            while True:
                b = d[i]; i += 1; l |= (b & 0x7F) << s; s += 7
                if not b & 0x80: break
            out.append((f, d[i:i + l])); i += l
        elif wt == 5:
            out.append((f, d[i:i + 4])); i += 4
        else:
            raise ValueError("wire type %d" % wt)
    return out


# ---- the tunnel ----------------------------------------------------------------------------------
class TcpLink:
    """The real thing: the API over WiFi (TCP 6053); the serial port only for console commands."""

    def __init__(self, port, host):
        import socket
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr, self.s.rts = True, False
        self.sock = socket.create_connection((host, 6053), timeout=5)
        self.buf = b""

    def cmd(self, c):
        if c.startswith("apitunnel"):
            return
        self.s.write((c + "\r\n").encode())
        time.sleep(0.05)

    def send(self, data: bytes):
        self.sock.sendall(data)

    def read_bytes(self, n, timeout=5.0):
        end = time.time() + timeout
        while len(self.buf) < n and time.time() < end:
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                d = self.sock.recv(65536)
            except OSError:
                continue
            if not d:
                raise TimeoutError("connection closed")
            self.buf += d
        if len(self.buf) < n:
            raise TimeoutError("waited for %d bytes, have %d" % (n, len(self.buf)))
        d, self.buf = self.buf[:n], self.buf[n:]
        return d

    def read_frame(self):
        hdr = self.read_bytes(3)
        assert hdr[0] == 0x01, "indicator %02x" % hdr[0]
        return self.read_bytes((hdr[1] << 8) | hdr[2])


class Tunnel:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr, self.s.rts = True, False
        self.buf = b""
        self.text = ""

    def cmd(self, c):
        self.s.write((c + "\r\n").encode())
        time.sleep(0.05)

    def send(self, data: bytes):
        h = data.hex()
        for i in range(0, len(h), 160):
            self.cmd("api " + h[i:i + 160])

    def read_bytes(self, n, timeout=5.0):
        end = time.time() + timeout
        while len(self.buf) < n and time.time() < end:
            self.text += self.s.read(8192).decode(errors="replace")
            *whole, self.text = self.text.split("\n")      # keep a partial last line for later
            for line in whole:
                if line.startswith("@API "):
                    tail = line[5:].strip()
                    if tail in ("closed",) or tail.startswith("ready"):
                        print("   unit:", line.strip())
                        continue
                    self.buf += bytes.fromhex(tail)
                elif line.startswith("[HAV]"):
                    print("   unit:", line.strip())
        if len(self.buf) < n:
            raise TimeoutError("waited for %d bytes, have %d" % (n, len(self.buf)))
        d, self.buf = self.buf[:n], self.buf[n:]
        return d

    def read_frame(self):
        hdr = self.read_bytes(3)
        assert hdr[0] == 0x01, "indicator %02x" % hdr[0]
        return self.read_bytes((hdr[1] << 8) | hdr[2])


def frame(payload):
    return bytes((1, len(payload) >> 8, len(payload) & 0xFF)) + payload


WAKE = "--wake" in sys.argv
if WAKE:
    sys.argv.remove("--wake")


def settle(ser):
    """Opening the port may restart the unit: wait until it is up (or quiet)."""
    buf, t0, last = b"", time.time(), time.time()
    while time.time() - t0 < 200:
        d = ser.read(4096)
        if d:
            buf += d
            last = time.time()
            if b"[BOOT] ready" in buf:
                time.sleep(8)
                ser.read(1 << 20)
                return
        elif time.time() - last > 5:
            return


def read_key(ser):
    """Ask until it answers: a unit with a full message store can take 100 s to start."""
    end, buf, asked = time.time() + 180, b"", 0
    while time.time() < end:
        if time.time() - asked > 15:
            ser.write(b"hakey\r\n")
            asked = time.time()
        buf += ser.read(4096)
        for line in buf.decode(errors="replace").splitlines():
            if line.startswith("[HAV] key ") and len(line.split()[2]) == 44:
                return line.split()[2]
    raise SystemExit("no key from the unit (turn Home Assistant voice on: console havoice 1)")


def main():
    port, key = sys.argv[1], sys.argv[2]
    t = TcpLink(port, sys.argv[3]) if len(sys.argv) > 3 else Tunnel(port)
    settle(t.s)
    if key == "auto":
        key = read_key(t.s)
    psk = base64.b64decode(key)
    assert len(psk) == 32
    t.cmd("apitunnel off")
    time.sleep(0.3)
    t.s.reset_input_buffer()
    t.cmd("apitunnel")
    time.sleep(0.5)

    noise = Initiator(psk, b"NoiseAPIInit\x00\x00")
    msg1 = noise.write_message1()
    t.send(frame(b"") + frame(b"\x00" + msg1))       # client hello + handshake, as aioesphomeapi
    hello = t.read_frame()
    assert hello[0] == 1, "server hello proto %r" % hello[:1]
    name = hello[1:hello.index(b"\x00", 1)].decode()
    print("server hello: node name %r" % name)
    hs = t.read_frame()
    if hs[0] != 0:
        print("FAIL handshake error:", hs[1:].decode(errors="replace"))
        return 1
    k_send, k_recv = noise.read_message2(hs[1:])
    print("handshake OK (Noise NNpsk0, the unit's tag verified)")
    n_send = n_recv = 0

    def send(mtype, data=b""):
        nonlocal n_send
        pt = struct.pack(">HH", mtype, len(data)) + data
        t.send(frame(aead_seal(k_send, nonce(n_send), b"", pt)))
        n_send += 1

    def recv(timeout=5.0):
        nonlocal n_recv
        f = t.read_frame() if timeout else None
        pt = aead_open(k_recv, nonce(n_recv), b"", f)
        n_recv += 1
        mtype, l = struct.unpack(">HH", pt[:4])
        return mtype, pt[4:4 + l]

    send(1, b"\x0a\x0dHome Assistant" + b"\x10\x01" + b"\x18\x0a")      # HelloRequest
    mt, d = recv()
    assert mt == 2, mt
    print("HelloResponse:", pb_fields(d))
    send(9)                                                           # DeviceInfoRequest
    mt, d = recv()
    assert mt == 10, mt
    info = dict(pb_fields(d))
    print("DeviceInfo: name %r mac %r model %r friendly %r va_flags %r encryption %r" % (
        info.get(2), info.get(3), info.get(6), info.get(13), info.get(17), info.get(19)))
    send(11)                                                          # ListEntitiesRequest
    mt, d = recv()
    assert mt == 19, mt
    print("ListEntitiesDone")
    send(7)                                                           # PingRequest
    mt, d = recv()
    assert mt == 8, mt
    print("Ping OK")
    send(89, b"\x08\x01\x10\x01")                                     # SubscribeVoiceAssistant(subscribe, API_AUDIO)
    time.sleep(0.5)

    def event(et, **data):
        body = varint(8) + varint(et) if et else b""
        for k, v in data.items():
            kv = b"\x0a" + varint(len(k)) + k.encode() + b"\x12" + varint(len(v)) + v.encode()
            body += b"\x12" + varint(len(kv)) + kv
        send(92, body)

    if WAKE:
        return wake_cycle(t, send, recv, event)

    # A voice run: the unit starts it (console "talk"), we answer port 0, take its audio,
    # then play the pipeline's events and a short reply tone back to it.
    t.cmd("talk")
    mt, d = recv()
    assert mt == 90, "expected VoiceAssistantRequest, got %d" % mt
    print("VoiceAssistantRequest:", pb_fields(d))
    send(91, b"")                                                     # VoiceAssistantResponse port 0
    got = 0
    end = time.time() + 3.0
    while time.time() < end:
        try:
            mt, d = recv(timeout=1.0)
        except TimeoutError:
            break
        if mt == 106:
            for f, v in pb_fields(d):
                if f == 1:
                    got += len(v)
    print("mic audio received: %d bytes (%.2f s at 16 kHz)" % (got, got / 32000))
    t.cmd("talkstop")
    if isinstance(t, TcpLink):                   # the unit's own count of what it sent
        time.sleep(0.5)
        for line in t.s.read(65536).decode(errors="replace").splitlines():
            if "mic stopped" in line:
                print("   unit:", line.strip())
    mt, d = recv()
    print("stop: message %d %r" % (mt, pb_fields(d)))

    event(1)                                                          # RUN_START
    event(4, text="what time is it")                                  # STT_END
    event(7, text="It is three o'clock")                              # TTS_START
    event(98)                                                         # TTS_STREAM_START
    import math
    pcm = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 16000))) for i in range(512))
    for _ in range(10):
        send(106, b"\x0a" + varint(len(pcm)) + pcm)                   # VoiceAssistantAudio
    event(99)                                                         # TTS_STREAM_END
    event(2)                                                          # RUN_END
    time.sleep(1.5)
    t.read_bytes(0, timeout=1.0)
    send(5)                                                           # DisconnectRequest
    try:
        mt, d = recv()
        print("disconnect reply:", mt)
    except Exception as e:
        print("disconnect:", e)
    t.cmd("apitunnel off")
    print("PASS" if got > 0 else "FAIL: no mic audio")
    return 0 if got > 0 else 1


def wake_cycle(t, send, recv, event):
    """The wake word cycle (see the top). Returns 0 = PASS."""
    audio = [0]
    fails = []

    def wait_msg(want, timeout):
        """Next message of type want (audio in between is counted). None on timeout."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                mt, d = recv(timeout=1.0)
            except TimeoutError:
                continue
            if mt == 106:
                audio[0] += sum(len(v) for f, v in pb_fields(d) if f == 1)
                continue
            if mt == want:
                return dict((f, v) for f, v in pb_fields(d))
            print("   (message %d)" % mt)
        return None

    def check(ok, what):
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            fails.append(what)

    t.cmd("havoice 1")
    t.cmd("hawake 2")
    req = wait_msg(90, 10)
    check(req is not None and req.get(1) == 1 and req.get(3, 0) & 2,
          "1 the unit opens a run with the wake word flag: %r" % req)
    send(91, b"")                                                     # VoiceAssistantResponse
    audio[0] = 0
    wait_msg(-1, 2.5)
    check(audio[0] > 16000, "2 the mic streams while it waits for the wake word: %d bytes" % audio[0])
    event(1)                                                          # RUN_START
    event(9)                                                          # WAKE_WORD_START
    event(10, text="hey jarvis")                                      # WAKE_WORD_END
    event(3)                                                          # STT_START
    audio[0] = 0
    wait_msg(-1, 1.5)
    check(audio[0] > 8000, "3 after the wake word the speech keeps streaming: %d bytes" % audio[0])
    event(12)                                                         # STT_VAD_END: the unit stops
    t0 = time.time()
    got_end = False
    while time.time() - t0 < 3 and not got_end:
        try:
            mt, d = recv(timeout=1.0)
        except TimeoutError:
            continue
        if mt == 106 and any(f == 2 and v for f, v in pb_fields(d)):
            got_end = True
    check(got_end, "4 end of speech: the unit sends the last audio with end=true")
    event(4, text="turn on the kitchen light")                        # STT_END
    event(7, text="Turned on the light")                              # TTS_START
    event(2)                                                          # RUN_END
    req = wait_msg(90, 8)
    check(req is not None and req.get(1) == 1 and req.get(3, 0) & 2,
          "5 after the answer the unit listens for the wake word again: %r" % req)
    send(91, b"")
    wait_msg(-1, 1.0)
    t.cmd("talk")                                                     # MIC while listening
    req = wait_msg(90, 5)
    check(req is not None and not req.get(1), "6 MIC ends the wake word run (start=false): %r" % req)
    event(2)                                                          # HA ends that run
    req = wait_msg(90, 5)
    check(req is not None and req.get(1) == 1 and not req.get(3, 0) & 2,
          "7 then push-to-talk starts (no wake word flag): %r" % req)
    send(91, b"")
    wait_msg(-1, 1.0)
    t.cmd("talkstop")
    event(2)                                                          # RUN_END
    req = wait_msg(90, 8)
    check(req is not None and req.get(3, 0) & 2, "8 back to the wake word afterwards: %r" % req)
    send(91, b"")
    wait_msg(-1, 0.5)
    t.cmd("hawake 0")
    req = wait_msg(90, 5)
    check(req is not None and not req.get(1), "9 wake word off ends the run (start=false): %r" % req)
    t.cmd("apitunnel off")
    print("PASS" if not fails else "FAIL: %d of 9" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
