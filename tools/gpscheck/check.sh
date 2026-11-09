#!/bin/bash
# Check one T-Deck Pro's GPS:   tools/gpscheck/check.sh COM10 [seconds]
# Flashes the gpscheck app (LoRa kept off), listens on the USB console and prints the
# receiver's start-up notices plus the last report and verdict.
PORT=${1:?usage: check.sh COMx [seconds]}
SECS=${2:-70}
DIR="$(cd "$(dirname "$0")" && pwd)"
PY=~/.platformio/penv/Scripts/python.exe
ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
BIN="$DIR/.pio/build/gpscheck/firmware.bin"
[ -f "$BIN" ] || (cd "$DIR" && ~/.platformio/penv/Scripts/pio run >/dev/null) || { echo "build failed"; exit 1; }

"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 115200 --before default_reset --after hard_reset \
    --no-stub write_flash 0x10000 "$BIN" 2>&1 | grep -E "MAC|verified|rror" || exit 1

LOG="$DIR/last_check_${PORT}.log"
PYTHONIOENCODING=utf-8 "$PY" -I - "$PORT" "$SECS" > "$LOG" <<'EOF'
import sys, time, serial
port, secs = sys.argv[1], int(sys.argv[2])
end = time.time() + secs
s = None
while time.time() < end:
    if s is None:
        try:
            s = serial.Serial(port, 115200, timeout=0.3); s.dtr = True; s.rts = False
        except Exception:
            time.sleep(0.5); continue
    try:
        data = s.read(4096)
    except Exception:
        s = None; time.sleep(0.5); continue
    if data:
        sys.stdout.write(data.decode('utf-8', 'replace')); sys.stdout.flush()
EOF
echo "--- receiver notices:"
grep -a "^\[GPS\]" "$LOG" | sort -u | head -20
echo "--- last report:"
grep -a "^\[CHECK\]" "$LOG" | tail -2
grep -a "^VERDICT" "$LOG" | tail -1
