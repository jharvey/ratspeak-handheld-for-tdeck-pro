#!/usr/bin/env python3
"""Relay for the T-Deck Pro remote console over WiFi (firmware: remote/NetConsole.h).

Units dial out to this relay (Settings > WiFi > Remote console, or console
"netcon host <ip> 4404" / "netcon key <key>" / "netcon on"). The relay checks the shared
key both ways, logs every unit's console to <dir>/logs/<name>.log and lets local tools
talk to the units through a control socket (<dir>/ctl.sock, see rscon.py).

  python3 relay.py [--dir ~/rscon] [--port 4404]

The key is read from <dir>/key (one line, made by install.sh). Handshake:
  unit  -> relay   RSCON1 <name> <dest> <nonceU>
  relay -> unit    AUTH <nonceR> <hmac(key, "relay|" nonceU "|" nonceR)>
  unit  -> relay   OK <hmac(key, "unit|" nonceR "|" nonceU)>
then console text both ways.
"""
import argparse
import asyncio
import collections
import hashlib
import hmac
import os
import secrets
import time

LOG_MAX = 50 * 1024 * 1024


def ts():
    t = time.time()
    return time.strftime("%m-%d %H:%M:%S", time.localtime(t))


def mac(key, msg):
    return hmac.new(key.encode(), msg.encode(), hashlib.sha256).hexdigest()


class Unit:
    def __init__(self, name, dest, ip, writer, logdir):
        self.name, self.dest, self.ip, self.writer = name, dest, ip, writer
        self.since = time.time()
        self.rx = 0
        self.lines = collections.deque(maxlen=5000)
        self.followers = set()
        self.partial = b""
        self.path = os.path.join(logdir, "%s.log" % name)
        self.log = open(self.path, "a", encoding="utf-8", errors="replace")

    def write_log(self, line):
        if self.log.tell() > LOG_MAX:
            self.log.close()
            os.replace(self.path, self.path + ".old")
            self.log = open(self.path, "a", encoding="utf-8", errors="replace")
        self.log.write("%s %s\n" % (ts(), line))
        self.log.flush()

    def feed(self, data):
        self.rx += len(data)
        buf = self.partial + data
        *done, self.partial = buf.split(b"\n")
        for raw in done:
            line = raw.rstrip(b"\r").decode("utf-8", "replace")
            self.lines.append((time.time(), line))
            if not line.startswith("@FB "):            # screen mirror rows: not worth logging
                self.write_log(line)
            for q in list(self.followers):
                try:
                    q.put_nowait(line)
                except asyncio.QueueFull:
                    pass

    def close(self, why):
        self.write_log("### relay: %s" % why)
        self.log.close()
        for q in list(self.followers):
            q.put_nowait(None)


class Relay:
    def __init__(self, d):
        self.dir = d
        self.logdir = os.path.join(d, "logs")
        os.makedirs(self.logdir, exist_ok=True)
        self.key = open(os.path.join(d, "key")).read().strip()
        self.units = {}                                  # dest -> Unit

    def find(self, what):
        w = what.lower()
        hits = [u for u in self.units.values() if u.name.lower() == w or u.dest.lower().startswith(w)]
        if not hits:
            hits = [u for u in self.units.values() if u.name.lower().startswith(w)]
        return hits[0] if len(hits) == 1 else None

    async def unit_conn(self, reader, writer):
        ip = writer.get_extra_info("peername")[0]
        sock = writer.get_extra_info("socket")
        if sock is not None:                              # notice a unit that vanished (reset, out of range)
            import socket
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            for opt, v in (("TCP_KEEPIDLE", 20), ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
                if hasattr(socket, opt):
                    sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), v)
        unit = None
        try:
            hello = (await asyncio.wait_for(reader.readline(), 10)).decode(errors="replace").split()
            if len(hello) != 4 or hello[0] != "RSCON1":
                return
            _, name, dest, nu = hello
            nr = secrets.token_hex(8)
            writer.write(("AUTH %s %s\n" % (nr, mac(self.key, "relay|%s|%s" % (nu, nr)))).encode())
            await writer.drain()
            ok = (await asyncio.wait_for(reader.readline(), 10)).decode(errors="replace").split()
            if len(ok) != 2 or ok[0] != "OK" or not hmac.compare_digest(ok[1], mac(self.key, "unit|%s|%s" % (nr, nu))):
                print(ts(), "refused", ip, name, "(wrong key)", flush=True)
                return
            old = self.units.pop(dest, None)
            if old:
                old.writer.close()
                old.close("replaced by a new connection")
            unit = Unit(name, dest, ip, writer, self.logdir)
            self.units[dest] = unit
            unit.write_log("### relay: connected from %s" % ip)
            print(ts(), "connected", name, dest, ip, flush=True)
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                unit.feed(data)
        except (asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            if unit and self.units.get(unit.dest) is unit:
                del self.units[unit.dest]
                unit.close("disconnected")
                print(ts(), "disconnected", unit.name, flush=True)
            writer.close()

    async def ctl_conn(self, reader, writer):
        """One command per connection:
        list | send <unit> <text> | run <unit> <secs> <text> | follow <unit> | recent <unit> <n>"""
        try:
            parts = (await reader.readline()).decode(errors="replace").rstrip("\r\n").split(" ", 3)
            cmd = parts[0]
            out = lambda s: writer.write((s + "\n").encode())
            if cmd == "list":
                for u in self.units.values():
                    out("%-12s %s %-15s up %5d s  %d KB" % (u.name, u.dest, u.ip, time.time() - u.since, u.rx >> 10))
                if not self.units:
                    out("(no units connected)")
                await writer.drain()
                return
            u = self.find(parts[1]) if len(parts) > 1 else None
            if not u:
                out("ERR no single connected unit matches %r" % (parts[1] if len(parts) > 1 else ""))
                await writer.drain()
                return
            if cmd == "recent":
                n = int(parts[2]) if len(parts) > 2 else 100
                for t, line in list(u.lines)[-n:]:
                    out("%s %s" % (time.strftime("%H:%M:%S", time.localtime(t)), line))
                await writer.drain()
                return
            if cmd == "send":
                text = " ".join(parts[2:])
                u.writer.write((text + "\n").encode())
                await u.writer.drain()
                out("ok")
                await writer.drain()
                return
            if cmd in ("run", "follow"):
                q = asyncio.Queue(maxsize=20000)
                u.followers.add(q)
                try:
                    secs = 1e9
                    if cmd == "run":
                        secs = float(parts[2])
                        text = parts[3] if len(parts) > 3 else ""
                        u.writer.write((text + "\n").encode())
                        await u.writer.drain()
                    end = time.time() + secs
                    while time.time() < end:
                        try:
                            line = await asyncio.wait_for(q.get(), max(0.05, end - time.time()))
                        except asyncio.TimeoutError:
                            break
                        if line is None:
                            out("### unit disconnected")
                            break
                        out(line)
                        await writer.drain()
                finally:
                    u.followers.discard(q)
                await writer.drain()
                return
            out("ERR unknown command")
            await writer.drain()
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.expanduser("~/rscon"))
    ap.add_argument("--port", type=int, default=4404)
    a = ap.parse_args()
    r = Relay(a.dir)
    sock = os.path.join(a.dir, "ctl.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    s1 = await asyncio.start_server(r.unit_conn, "0.0.0.0", a.port)
    s2 = await asyncio.start_unix_server(r.ctl_conn, sock)
    os.chmod(sock, 0o600)
    print(ts(), "relay on port", a.port, "control", sock, flush=True)
    async with s1, s2:
        await asyncio.gather(s1.serve_forever(), s2.serve_forever())


if __name__ == "__main__":
    asyncio.run(main())
