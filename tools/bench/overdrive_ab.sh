#!/usr/bin/env bash
# Overdrive A/B at HD: retime row, AEC settle point and a still per fps.
# The still matters as much as the numbers - changing the overdrive moves PIXCLK and VTS,
# which moves the AEC band steps (b50/b60) and the max exposure, and an unsatisfiable AEC
# config is what produced the vertical striping of 27 Aug 2026. Numbers cannot see that.
#   BOARD=<addr> bash tools/bench/overdrive_ab.sh <label>
set -u
B=${BOARD:?set BOARD}
LABEL=${1:?usage: overdrive_ab.sh <baseline|after>}
OUT=FPS_RECAL_stills/overdrive_ab/$LABEL
mkdir -p "$OUT"
CSV="$OUT/points.csv"
echo "fps,pixclkMHz,hts,lf,vts,sensorFps,maxExpMs,aecSnapFrom,aecSnapTo,bandLines,stillBytes" > "$CSV"

ctl() { curl -s -m 20 "http://$B/control?$1" >/dev/null; }
ring() { curl -s -m 30 "http://$B/control?displayLog=1"; }

echo "-- campaign config: RAM only, restored at the end --"
ctl "record=0"; ctl "enableMotion=0"; ctl "idleFps=0"; ctl "framesize=13"; ctl "quality=10"
sleep 5

for F in 11 15 20 30 50; do
  ctl "fps=$F"
  sleep 12   # the AEC never re-seeks from a stable point - give it time to converge
  R=$(ring)
  LINE=$(echo "$R" | grep -a "Tuned timing HD" | tail -1)
  SNAP=$(echo "$R" | grep -a "AEC exposure snapped" | tail -1)
  curl -s -m 30 "http://$B/control?still=1" > "$OUT/still_HD_${F}fps.jpg"
  SZ=$(wc -c < "$OUT/still_HD_${F}fps.jpg")
  echo "req $F: $LINE"
  echo "        $SNAP"
  python - "$F" "$LINE" "$SNAP" "$SZ" >> "$CSV" <<'PYEOF'
import re, sys
fps, line, snap, sz = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
m = re.search(r"PIXCLK ([\d.]+)MHz, HTS (\d+) x(\d), VTS (\d+) -> sensor ([\d.]+)fps for request (\d+), max exposure (\d+)ms", line)
s = re.search(r"snapped (\d+) -> (\d+) lines \((\d+) x (\d+) line band\)", snap)
g = m.groups() if m else ("",)*7
sn = (s.group(1), s.group(2), s.group(4)) if s else ("", "", "")
print(f"{fps},{g[0]},{g[1]},{g[2]},{g[3]},{g[4]},{g[6]},{sn[0]},{sn[1]},{sn[2]},{sz}")
PYEOF
done

echo "-- restoring field config --"
ctl "fps=30"; ctl "idleFps=5"; ctl "enableMotion=1"; ctl "record=1"
echo "== $CSV =="
cat "$CSV"
