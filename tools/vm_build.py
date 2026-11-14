#!/usr/bin/env python3
"""Build the firmware on the build VM (VM100, ssh host "debian-build") instead of locally.

The repo lives on the NAS (M:), where PlatformIO spends most of a build reading files over the
network (full build ~38 min, small change ~9 min). The VM keeps a copy on its own disk
(~/ratspeak, no .git) and builds there.

Each run:
  1. sends the files that changed since the last run (tracked files plus new, untracked
     files under src/ lib/ include/ partitions/ tools/ protocol/), as they are in the working
     tree, uncommitted edits included; files deleted here are deleted there;
  2. runs `pio run -e <env>` on the VM (output streamed here);
  3. copies firmware.bin back to C:/Users/MyPC/pio-build/vm/<env>/firmware.bin.
Flash from this PC as before (the units are on its USB ports).

  python tools/vm_build.py                     # tdeck_pro_standalone
  python tools/vm_build.py -e <env> [--full]   # --full: resend every file
  python tools/vm_build.py --sync-only
"""
import argparse
import io
import json
import os
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.environ.get("RS_VM_HOST", "debian-build")
REMOTE = "ratspeak"                      # under the VM user's home
PIO = "~/.platformio/penv/bin/pio"
STATE = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "ratspeak_vm_sync.json")
OUT = os.environ.get("RS_VM_OUT", "C:/Users/MyPC/pio-build/vm")
UNTRACKED_DIRS = ("src/", "lib/", "include/", "partitions/", "tools/", "protocol/")


def git_lines(*args):
    r = subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True, check=True)
    return [l for l in r.stdout.splitlines() if l]


def ssh(cmd, **kw):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, cmd], **kw)


def sync(full):
    files = set(git_lines("ls-files"))
    files |= {f for f in git_lines("ls-files", "--others", "--exclude-standard") if f.startswith(UNTRACKED_DIRS)}
    files = {f for f in files if os.path.isfile(os.path.join(ROOT, f))}
    try:
        old = {} if full else json.load(open(STATE))
    except (OSError, ValueError):
        old = {}
    now = {}
    for f in files:
        st = os.stat(os.path.join(ROOT, f))
        now[f] = [int(st.st_mtime), st.st_size]
    changed = sorted(f for f in files if old.get(f) != now[f])
    gone = sorted(f for f in old if f not in files)
    if changed:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tar:
            for f in changed:
                tar.add(os.path.join(ROOT, f), arcname=f)
        r = ssh("mkdir -p ~/%s && tar -xf - -C ~/%s" % (REMOTE, REMOTE), input=buf.getvalue())
        if r.returncode:
            sys.exit("sync failed")
    if gone:
        r = ssh("cd ~/%s && rm -f -- %s" % (REMOTE, " ".join("'%s'" % g.replace("'", "") for g in gone)))
        if r.returncode:
            sys.exit("delete failed")
    json.dump(now, open(STATE, "w"))
    print("[vm] sent %d changed file(s), deleted %d" % (len(changed), len(gone)))
    for f in changed[:15]:
        print("     " + f)
    if len(changed) > 15:
        print("     ...")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-e", "--env", default="tdeck_pro_standalone")
    ap.add_argument("--full", action="store_true", help="resend every file")
    ap.add_argument("--sync-only", action="store_true")
    a = ap.parse_args()
    sync(a.full)
    if a.sync_only:
        return
    r = ssh("cd ~/%s && %s run -e %s" % (REMOTE, PIO, a.env))
    if r.returncode:
        sys.exit("[vm] build FAILED")
    dst = os.path.join(OUT, a.env)
    os.makedirs(dst, exist_ok=True)
    r = subprocess.run(["scp", "-q", "-o", "BatchMode=yes",
                        "%s:%s/.pio/build/%s/firmware.bin" % (HOST, REMOTE, a.env), dst + "/firmware.bin"])
    if r.returncode:
        sys.exit("[vm] copy back failed")
    print("[vm] firmware: %s/firmware.bin (%d bytes)" % (dst, os.path.getsize(dst + "/firmware.bin")))


if __name__ == "__main__":
    main()
