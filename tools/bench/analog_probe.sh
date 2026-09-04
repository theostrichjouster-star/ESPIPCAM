#!/usr/bin/env bash
# Binned-mode analog register probe (4 Sep 2026): does the row-time floor move with the
# sensor-control registers 0x3708 / 0x3709 / 0x370C?
#
# At every binned size this board runs 0x3708=0x21, 0x3709=0x12, 0x370C=0x02. The datasheet
# marks them debug and says nothing. The recollection under test is that OmniVision's
# reference tables carry 0x3709=0x52 / 0x370C=0x03 in the 2x2 modes and 0x12 / 0x00 at full
# resolution - ours would then be the full-resolution pair on a binned readout, a candidate
# for why our binned row floor sits near 24us where the reference 1280x960 mode runs 22.58.
#
# Instrument: VGA, not 1280X960. Same 2624x1952 window, same 2x2, same analog registers, but
# its 640-wide output never stretches, so the counted rate tracks HTS exactly and the image
# gates see the readout alone. HTS goes in through camRegGrp (both bytes at one frame
# boundary) - the plain two-byte write is what latched the magenta cast in the floor campaign.
#
# Predictions, stated first:
#   control walk (driver values):  1960 and 1900 clean, 1850 green highlights (gsat up), 1800 worse
#   reference values, if the recollection is right: clean to 1850 and 1800, edge near 1750-1700
#   reference values, if wrong: same edge as the control, or a broken image at 2060 itself
# Part 2 only if the VGA edge moved two steps: 1280X960 on route B (88 MHz) at HTS 2060 /
# 2000 / 1960, where the 1280-wide output stretch decides whether the rate follows.
#
# Restores the analog registers to the values read at the start, HTS 2060, the field config.
#   BOARD=<addr> bash tools/bench/analog_probe.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/analog}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
SETTLE=${SETTLE:-12}
REFSET=${REFSET:-"0x3709,0x52,0x370C,0x03"}   # stage A: the two that differ binned/full in the reference
WALK=${WALK:-"1960 1900 1850 1800 1750 1700"}
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/points.csv"
[ -f "$CSV" ] || echo "size,analog,hts,predFps,vsyncMax,vsyncMin,w,h,bytes,ratio,gsat,rsat,hdiff,vdiff,gateRate,gateImage" > "$CSV"
A08=""; A09=""; A0C=""

restore() {
  log "== restore: analog 0x3708=${A08:-?} 0x3709=${A09:-?} 0x370C=${A0C:-?}, HTS 2060, field config =="
  [ -n "$A08" ] && curl -s -m 25 "$B/control?camRegGrp=0x3708,0x$A08,0x3709,0x$A09,0x370C,0x$A0C" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?camRegGrp=0x380C,0x08,0x380D,0x0C" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT

regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
analog_now() { echo "0x$(regrd 0x3708)/0x$(regrd 0x3709)/0x$(regrd 0x370C)"; }

set_hts_grp() {  # atomic, both bytes at one frame boundary
  local want=$1
  ctl "camRegGrp=0x380C,$(printf '0x%02X' $(( (want >> 8) & 0xFF ))),0x380D,$(printf '0x%02X' $(( want & 0xFF )))" > /dev/null
  sleep 1
  local got; got=$(ramlog | grep -a "camRegGrp: 0x380D" | tail -1)
  printf '%s\n' "$got" | grep -a -q "group launched" || log "   WARN: HTS group write did not confirm: $got"
}

BASE_HDIFF=""; BASE_W=0; BASE_H=0
# sample <size> <analog label> <hts> <predicted fps> <clock MHz> : 3 VSYNC counts, still, gates.
# Returns 0 when the IMAGE is clean; the rate verdict is in G_RATE
sample() {
  local size=$1 analog=$2 hts=$3 pred=$4 rates="" i line r rate rmin tag stats verdict
  sleep "$SETTLE"; assert_alive
  for i in 1 2 3; do
    ctl "xclkStat=1" > /dev/null; sleep 8
    line=$(ramlog | grep -a "VSYNC counted" | tail -1)
    r=$(printf '%s\n' "$line" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/'); rates="$rates ${r:-0}"
  done
  rate=$(python -c "print(max(float(x) for x in '$rates'.split()))"); rmin=$(python -c "print(min(float(x) for x in '$rates'.split()))")
  tag="${size}_${analog}_hts${hts}"
  curl -s -m 30 -o "$OUT/$tag.jpg" "$B/control?still=1" || { log "ABORT: still fetch failed"; exit 7; }
  stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg") || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  [ -n "$BASE_HDIFF" ] || { BASE_HDIFF=$hdiff; BASE_W=$w; BASE_H=$h; }
  verdict=$(python - "$rate" "$pred" "$w" "$h" "$ratio" "$gsat" "$rsat" "$hdiff" "$BASE_HDIFF" "$BASE_W" "$BASE_H" "${RATIO_LO:-0.9}" <<'PY'
import sys
rate, pred, w, h, ratio, gsat, rsat, hdiff, base, bw, bh, rlo = [float(x) for x in sys.argv[1:13]]
gr = "OK" if pred and abs(rate - pred) / pred <= 0.015 else "FAIL"
# RATIO_LO relaxes the colour floor for an analog change that shifts the white balance
# without corrupting the readout (run 1: 0.880 at 2060 with the reference values, hdiff and
# saturation untouched). The latched cast sits at 0.40-0.45, confetti at 0.68 - far below
gi = "OK" if (rlo <= ratio <= 1.2 and gsat < 3.0 and rsat < 3.0 and hdiff <= max(3.0, 1.25 * base) and w == bw and h == bh) else "FAIL"
print(gr, gi)
PY
)
  read -r G_RATE G_IMAGE <<< "$verdict"
  log "   $tag: ${rate}fps max/3 (min $rmin, pred $pred) still ${bytes}B ratio $ratio gsat $gsat% rsat $rsat% hdiff $hdiff vdiff $vdiff -> rate:$G_RATE image:$G_IMAGE"
  echo "$size,$analog,$hts,$pred,$rate,$rmin,$w,$h,$bytes,$ratio,$gsat,$rsat,$hdiff,$vdiff,$G_RATE,$G_IMAGE" >> "$CSV"
  [ "$G_IMAGE" = "OK" ]
}

EDGE=""
walk() {  # walk <size> <analog label> <clock MHz> <vts> : HTS down the WALK, stop at the first
          # corrupt image. Result in EDGE (empty = clean through the whole walk); not echoed,
          # because log() writes to stdout too and a command substitution would capture it all
  # hw, not h: sample() reads the still's height into h, and bash's dynamic scoping let that
  # overwrite this loop's local on run 1 (the edge logged as "HTS 480")
  local size=$1 analog=$2 clk=$3 vts=$4 hw pred
  EDGE=""
  for hw in $WALK; do
    pred=$(python -c "print('%.2f'%($clk*1e6/($hw*$vts)))")
    set_hts_grp "$hw"
    if ! sample "$size" "$analog" "$hw" "$pred"; then EDGE=$hw; log "   edge for $analog: HTS $hw corrupt"; break; fi
    [ "$G_RATE" = "OK" ] || log "   (rate short at HTS $hw with a clean image)"
  done
  set_hts_grp 2060; sleep 2
}

log "== binned analog register probe =="
assert_campaign_config 10
preflight
set_size 10 10 39; sleep 4
A08=$(regrd 0x3708); A09=$(regrd 0x3709); A0C=$(regrd 0x370C)
log "VGA start: analog $(analog_now), HTS 0x$(regrd 0x380C)$(regrd 0x380D)"

# Part 1a: control walk on the driver's analog values
log "-- VGA control walk, driver analog values --"
sample VGA drv 2060 39.47 || { log "ABORT: VGA baseline image failed its own gates"; exit 8; }
[ "$G_RATE" = "OK" ] || { log "ABORT: VGA baseline rate off"; exit 8; }
walk VGA drv 80 984; edgeDrv=$EDGE
log "VGA driver-analog edge: ${edgeDrv:-none in the walk}"

# Part 1b: the reference analog values, then the same walk
log "-- VGA with reference analog values ($REFSET) --"
ctl "camRegGrp=$REFSET" > /dev/null; sleep 2
log "   analog now $(analog_now)"
if ! sample VGA ref 2060 39.47 || [ "$G_RATE" != "OK" ]; then
  log "RESULT: the reference analog values break VGA at HTS 2060 itself - restoring, no walk"
  exit 0
fi
walk VGA ref 80 984; edgeRef=$EDGE
log "VGA reference-analog edge: ${edgeRef:-none in the walk}"
python - "${edgeDrv:-0}" "${edgeRef:-0}" <<'PY'
import sys
d, r = int(sys.argv[1]), int(sys.argv[2])
print("RESULT: driver edge %s, reference edge %s -> %s" % (d or "beyond walk", r or "beyond walk",
      "the analog registers MOVE the floor" if (r == 0 and d) or (r and d and r < d - 50) else "no movement worth having"))
PY

# Part 2 only when the VGA edge moved at least two steps: 1280X960 on route B
if [ -z "$edgeRef" ] || { [ -n "$edgeDrv" ] && [ "$edgeRef" -le $(( edgeDrv - 100 )) ]; }; then
  log "-- 1280X960 @42 (route B, 88 MHz) with the reference analog values --"
  set_size 25 10 41; sleep 4
  ctl "camRegGrp=$REFSET" > /dev/null; sleep 2
  log "   analog now $(analog_now), 0x3108=$(regrd 0x3108), HTS 0x$(regrd 0x380C)$(regrd 0x380D)"
  BASE_HDIFF=""
  sample 1280X960 ref 2156 41.48 || { log "   1280X960 baseline broken with the reference values"; exit 0; }
  for h in 2060 2000 1960; do
    pred=$(python -c "print('%.2f'%(88e6/($h*984)))")
    set_hts_grp "$h"
    sample 1280X960 ref "$h" "$pred" || { log "   1280X960 edge with reference values: HTS $h"; break; }
    [ "$G_RATE" = "OK" ] || log "   (1280-wide stretch at HTS $h, image clean)"
  done
  set_hts_grp 2156
fi
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
