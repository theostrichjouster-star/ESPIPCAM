#!/usr/bin/env bash
# The tuner's programmed timing at every integer fps for a size, read off the BOARD.
#
# applyTunedTiming logs one LOG_INF line per retime - "Tuned timing <size>: PIXCLK x MHz, HTS h xN,
# VTS v -> sensor f fps for request r, max exposure e ms" - and LOG_INF reaches the RTC ring. So a
# whole table is a walk of `fps=` with a ring read after each, which is far cheaper than counting
# VSYNC at every rung and reports exactly what the sensor was told.
#
# It is a REGISTER table, not a delivery table. What it cannot tell you is whether the pipeline
# carries the rate, whether the frame is clean, or whether the sensor honours what it was given -
# for those, count VSYNC (xclkStat) and gate a still. This walks the arithmetic; the ceiling of
# each size gets a VSYNC count and a gated still at the end so the top of the table is anchored to
# something measured.
#
#   BOARD=<addr> SIZES="13:HD:56 25:1280X960:42" bash tools/bench/tuner_table.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/tuner_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

SIZES=${SIZES:-"13:HD:56 25:1280X960:42"}
Q=${Q:-10}

CSV="$OUT/tuner_table.csv"
echo "size,reqFps,pixClkMHz,hts,lineFactor,vts,sensorFps,maxExpMs,route" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
}
trap restore EXIT

# the retime line for the request just sent, retried while the capture task gets round to it
retimeLine() {  # retimeLine <sizeName> <reqFps>
  local name=$1 req=$2 i L
  for i in 1 2 3 4 5; do
    sleep 2
    L=$(ramlog | grep -a "Tuned timing $name:" | grep -a "for request $req," | tail -1)
    [ -n "$L" ] && { printf '%s' "$L"; return 0; }
  done
  return 1
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"

for sz in $SIZES; do
  IFS=: read -r idx name ceil <<< "$sz"
  log "== $name (index $idx), requests 1..$ceil =="
  ctl "framesize=$idx" > /dev/null; sleep 7
  ctl "quality=$Q" > /dev/null
  got=$(curl -s -m 25 "$B/control?updateFPS=1" | python "$HERE/jfield.py" fpsCeil)
  [ "$got" = "$ceil" ] || anomaly "$name fpsCeil reads $got, expected $ceil"
  for f in $(seq 1 "$ceil"); do
    ctl "fps=$f" > /dev/null
    L=$(retimeLine "$name" "$f") || { anomaly "$name request $f: no retime line"; continue; }
    pass
    python - "$name" "$f" "$L" "$CSV" <<'EOF' | tee -a "$LOGF"
import re, sys
name, req, line, csv = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
m = re.search(r"PIXCLK ([\d.]+)MHz, HTS (\d+) x(\d+), VTS (\d+) -> sensor ([\d.]+)fps "
              r"for request (\d+), max exposure (\d+)ms", line)
if not m:
    print("   %s %s: retime line did not parse" % (name, req)); sys.exit()
clk, hts, lf, vts, sf, _, exp = m.groups()
route = "B" if abs(float(clk) - 88.0) < 0.05 else "A"
print("   %-9s req %-3s PIXCLK %-6s HTS %s x%s  VTS %-5s -> sensor %-6s max exp %sms  route %s"
      % (name, req, clk, hts, lf, vts, sf, exp, route))
open(csv, "a").write("%s,%s,%s,%s,%s,%s,%s,%s,%s\n" % (name, req, clk, hts, lf, vts, sf, exp, route))
EOF
  done
  # anchor the top of the table to something measured rather than logged
  ctl "fps=$ceil" > /dev/null; sleep 4
  ctl "xclkStat=1" > /dev/null; sleep 11
  ctl "xclkStat=1" > /dev/null; sleep 11
  V=$(ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1)
  log "   $name at its ceiling: $V"
  http_gap; curl -s -m 25 -o "$OUT/${name}_ceiling.jpg" "$B/control?still=1"
  log "   $name ceiling still: $(python "$HERE/still_color.py" "$OUT/${name}_ceiling.jpg" 2>/dev/null | awk '{print "ratio "$7" gsat "$8" rsat "$9" hdiff "$10}') | bands $(python "$HERE/still_bands.py" "$OUT/${name}_ceiling.jpg" --terse 2>/dev/null)"
done

log "== done =="
column -s, -t < "$CSV"
