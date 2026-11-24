#!/usr/bin/env python3
"""Build the RatSpeak ATAK bridge app on VM100 and copy the APK into the repo.

    python tools/android/build_apk.py [--version-code N --version-name X.Y]

Copies tools/android/atak-bridge to ~/atak-bridge on the VM (host RS_VM_HOST, default
debian-build), runs build.sh there (JDK 17 + Android SDK in ~/android-sdk, no Gradle) and
writes firmware/RatSpeak-ATAK-bridge.apk. The signing key stays on the VM
(~/.android-keys, backup on the NAS in M:/ratspeak-keys): every update must be signed with it.
"""
import argparse, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "tools", "android", "atak-bridge")
HOST = os.environ.get("RS_VM_HOST", "debian-build")


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version-code", default="1")
    ap.add_argument("--version-name", default="0.1")
    a = ap.parse_args()
    run(["ssh", HOST, "rm -rf ~/atak-bridge && mkdir -p ~/atak-bridge"])
    # relative paths from inside the app folder: scp reads "M:\..." as a host named M
    print("+ scp (from %s)" % APP, flush=True)
    subprocess.run(["scp", "-q", "-r", "AndroidManifest.xml", "build.sh", "src", HOST + ":atak-bridge/"],
                   cwd=APP, check=True)
    run(["ssh", HOST, "cd ~/atak-bridge && VC=%s VN=%s sh build.sh" % (a.version_code, a.version_name)])
    out = os.path.join(ROOT, "firmware", "RatSpeak-ATAK-bridge.apk")
    run(["scp", "-q", HOST + ":atak-bridge/build/RatSpeak-ATAK-bridge.apk", out])
    print("APK:", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    sys.exit(main())
