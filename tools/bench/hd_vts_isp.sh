#!/usr/bin/env bash
# Does an ISP block own HD's blanking floor? LENC, then the defect-pixel cancellers.
#
# THE FLOOR AS MEASURED. With HD's readout cut to 720 binned rows and a zero ISP offset, at
# 88 MHz and HTS 2156, VTS 738 and 740 count their predicted rate exactly, VTS 736 is MARGINAL
# (55.453 in one run, 33.613 and 36.566 in the next), 734 and 732 deliver exactly half, and 728
# delivers nothing at all. So the floor is 738 = 720 rows + 18 lines, not the 728 the driver's own
# binned rule (rows/2 + 8) and the datasheet's "1296x728 with dummy" both predict.
#
# The failure below the floor is NOT the split-frame trap applyHtsFloor warns about. The frames
# are complete and correct - a 1280x720 still at ratio 1.012 with the chart perfectly legible,
# looked at, not just gated - and simply arrive at half rate. That is the shape of a pipeline that
# needs more time per frame than the frame allows and drops every other one, which is why an ISP
# block is a reasonable suspect for those 18 lines.
#
# PREDICTION, stated before measuring: LENC WILL NOT MOVE THE FLOOR. Datasheet 5.2 describes it as
# a per-pixel gain computed from where the pixel sits, adapted to sensor gain - positional, so it
# needs the pixel's coordinates and not its neighbours, and a block with no row neighbourhood has
# no reason to hold row buffers. The blocks that plausibly do are colour interpolation, 0x5000
# bit 0, which is the demosaic and inherently reads a 3x3 or larger neighbourhood, and the two
# defect-pixel cancellers on bits 1 and 2, which do the same. The scaler is already EXCLUDED by
# measurement: turning it on and off moved nothing, both arms failing at 736 and passing at 738.
#
# Colour interpolation is not walked. Turning demosaic off does not produce a slightly worse
# picture, it produces something that is not a colour image at all, so a rate measured with it off
# would not describe any configuration this board could ship.
#
# Every arm is restored by rewriting 0x5000 to the value read at the start, and the walk stops
# descending once a rung fails, since the point is where the floor IS, not how far past it goes.
#
#   BOARD=<addr> bash tools/bench/hd_vts_isp.sh   [WALK="740 738 736 734 732"]
set -u
OUT=${OUT:-FPS_RECAL_stills/hdisp_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; OTHER=${OTHER:-10}; FPS_REQ=${FPS_REQ:-52}; Q=${Q:-10}
WALK=${WALK:-"740 738 736 734 732"}
HTS=2156; PCLK=88

CSV="$OUT/vtsisp.csv"
echo "arm,isp0x5000,vts,predFps,vsyncFps,stillBytes,ratio,hdiff,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
ISP0=""
restore() {
  log "== restoring =="
  [ -n "$ISP0" ] && { curl -s -m 25 "$B/control?camReg=0x5000,0x$ISP0" > /dev/null; sleep 1.5; }
  for kv in "camReg=0x380F,0xE8" "camReg=0x3807,0xAF" "camReg=0x3813,0x08" "camReg=0x5001,0x83" \
            "camReg=0x3108,0x26" "camReg=0x3036,0x78"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  curl -s -m 25 "$B/control?framesize=$OTHER" > /dev/null; sleep 7   # a REAL size change rewrites the window
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) 0x5000=$(regrd 0x5000)"
  log "          window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )) yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) )) 0x5001=$(regrd 0x5001)"
}
trap restore EXIT

vsync_or_fail() {  # two counts, second wins; a dry window logs a DIFFERENT line and must be seen
  ctl "xclkStat=1" > /dev/null; sleep 11
  ctl "xclkStat=1" > /dev/null; sleep 11
  ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1
}

rung() {  # rung <arm> <ispVal> <vts> ; echoes ok/bad so the caller can stop descending
  local arm=$1 isp=$2 vts=$3 V f fjpg bytes st ratio hdiff verdict pred
  ctl "camReg=0x380F,$(printf '0x%02X' $((vts & 0xFF)))" > /dev/null; sleep 3
  V=$(vsync_or_fail)
  if printf '%s' "$V" | grep -aq 'count failed'; then f=NOFRAMES; else
    f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/'); fi
  fjpg="$OUT/${arm}_vts${vts}.jpg"
  http_gap; curl -s -m 25 -o "$fjpg" "$B/control?still=1"
  bytes=$(stat -c %s "$fjpg" 2>/dev/null || echo 0)
  st=$(python "$HERE/still_color.py" "$fjpg" 2>/dev/null) || st=""
  if [ -n "$st" ]; then ratio=$(echo "$st" | awk '{print $7}'); hdiff=$(echo "$st" | awk '{print $10}')
  else ratio=-; hdiff=-; fi
  pred=$(python -c "print('%.2f' % (${PCLK}e6 / ($HTS * $vts)))")
  if [ "$f" = NOFRAMES ]; then verdict=NOFRAMES
  else verdict=$(python -c "
f, p = float('$f'), float('$pred')
d = abs(f / p - 1) * 100
print('ok' if d <= 2 else ('HALFRATE' if abs(f / (p / 2) - 1) * 100 <= 5 else 'RATE%+.0f%%' % (-d)))"); fi
  log "   $arm (0x5000=$isp) VTS $vts predicted $pred: VSYNC $f | still $bytes B ratio $ratio hdiff $hdiff | $verdict"
  echo "$arm,$isp,$vts,$pred,$f,$bytes,$ratio,$hdiff,$verdict" >> "$CSV"
  [ "$verdict" = ok ] && return 0 || return 1
}

arm() {  # arm <name> <0x5000 value>
  local name=$1 isp=$2 v
  ctl "camReg=0x5000,0x$isp" > /dev/null; sleep 3
  local got; got=$(regrd 0x5000)
  [ "$(echo "$got" | tr 'a-f' 'A-F')" = "$(echo "$isp" | tr 'a-f' 'A-F')" ] || { anomaly "$name: 0x5000 wrote $isp reads $got"; return; }
  pass
  log "-- $name: 0x5000 = 0x$isp --"
  ctl "camReg=0x380F,0xE8" > /dev/null; sleep 3   # back to a known-good VTS before descending
  for v in $WALK; do
    rung "$name" "$isp" "$v" || { log "      floor found above VTS $v - not descending further"; break; }
  done
  ctl "camReg=0x380F,0xE8" > /dev/null; sleep 3
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"

ctl "framesize=$OTHER" > /dev/null; sleep 7
ctl "framesize=$IDX" > /dev/null; sleep 7
ctl "quality=$Q" > /dev/null
ctl "fps=$FPS_REQ" > /dev/null; sleep 4
ISP0=$(regrd 0x5000)
log "== HD, 88 MHz, HTS 2156, 720 binned rows. ISP control 0x5000 starts at 0x$ISP0 =="
log "   bit7 LENC, bit5 raw gamma, bit2 black pixel cancel, bit1 white pixel cancel, bit0 colour interpolation"
ctl "camReg=0x380D,0x6C" > /dev/null; sleep 1
ctl "camReg=0x3036,0x42" > /dev/null; sleep 2
ctl "camReg=0x3108,0x11" > /dev/null; sleep 3
ctl "camReg=0x5001,0xA3" > /dev/null; sleep 2   # scaler on, so the offset change has a legal
ctl "camReg=0x3813,0x00" > /dev/null; sleep 2   # intermediate, then the window matches again
ctl "camReg=0x3807,0x8F" > /dev/null; sleep 3
ctl "camReg=0x5001,0x83" > /dev/null; sleep 3   # scaler off: measured not to affect the floor

LENCOFF=$(python -c "print('%02X' % (int('$ISP0', 16) & ~0x80))")
DPCOFF=$(python -c "print('%02X' % (int('$ISP0', 16) & ~0x06))")
BOTHOFF=$(python -c "print('%02X' % (int('$ISP0', 16) & ~0x86))")

arm "baseline"  "$ISP0"
arm "lencOFF"   "$LENCOFF"
arm "dpcOFF"    "$DPCOFF"
arm "lenc+dpcOFF" "$BOTHOFF"

log "== result =="
column -s, -t < "$CSV"
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
floors = {}
for r in rows:
    if r["verdict"] == "ok":
        v = int(r["vts"])
        if r["arm"] not in floors or v < floors[r["arm"]][0]:
            floors[r["arm"]] = (v, r["vsyncFps"], r["isp0x5000"])
base = floors.get("baseline", (None,))[0]
for arm, (v, f, isp) in floors.items():
    delta = "" if base is None or arm == "baseline" else ("  %+d lines vs baseline" % (v - base))
    print("   %-12s 0x5000=%s  lowest VTS that holds its rate: %d at %s fps%s" % (arm, isp, v, f, delta))
if base is not None and all(v[0] >= base for v in floors.values()):
    print("\n   No ISP block tested moved the floor. The 18 lines are not LENC and not the defect-pixel\n"
          "   cancellers, and the scaler was already excluded by measurement.")
EOF
