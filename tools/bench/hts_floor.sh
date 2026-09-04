#!/usr/bin/env bash
# Phase A of the HTS floor campaign: per binned size, find the shortest line the sensor will
# run with the AEC still seeing. HTS_FLOOR has been 2060 since August on a measurement that a
# 3 Sep walk contradicts - QVGA tracked the model exactly at 1800 and 1400, pinning only at
# 1300 - so the readout is not what binds at 2060. The AEC is.
#
# The instrument is NOT "gain pegged", which is only the symptom. It is the AEC's own 4x4 zone
# grid (0x5691-0x56A0) against the weighted aggregate YAVG (0x56A1), the same pair that
# diagnosed the full-resolution case on 27 Aug: when the statistics engine goes blind the
# ZONES keep tracking the scene while YAVG pins in its deadband and the AEC servos the pinned
# value. avgZones dumps both at LOG_INF, so the RTC ring carries it and no flash is needed.
#
# EVERY SIZE IS WALKED INDEPENDENTLY. QVGA, VGA and 1280X960 share a window, a frame length
# and a rate, but not an ISP path - they downscale 4x, 2x and not at all. Nothing establishes
# that the blindness follows the readout rather than the ISP in front of it, so no size
# inherits another's floor.
#
#   BOARD=<addr> LIGHT=lit bash tools/bench/hts_floor.sh
#   BOARD=<addr> LIGHT=dim bash tools/bench/hts_floor.sh
set -u
B=${BOARD:?set BOARD}
LIGHT=${LIGHT:?set LIGHT=lit or LIGHT=dim}
OUT=${OUT:-FPS_RECAL_stills/hts_floor_$LIGHT}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
CSV="$OUT/points.csv"
[ -f "$CSV" ] || echo "light,size,idx,hts,vts,predFps,vsyncFps,zoneMean,zoneMin,zoneMax,yavg,bandLo,bandHi,aecState,gain,gainCeil,stillBytes,stillDims,gateRate,gateAec,gateStill,verdict" > "$CSV"

# name:idx:vts:ceilingFps   VTS is the value in force at the ceiling request, held fixed for
# the whole walk so HTS is the only variable. SVGA and XGA are untuned spot checks - they get
# no tuned clock, so their rate column is informational, but applyHtsFloor writes them too and
# a lowered floor would reach them.
SIZES=${SIZES:-"QVGA:6:984:39 VGA:10:984:39 1280X960:25:984:39 HD:13:744:52 VGANARROW:28:504:77 QVGANARROW:29:264:147 SVGA:11:984:0 XGA:12:984:0"}
WALK=${WALK:-"1960 1900 1850 1800 1750 1700 1650 1600"}
BASE_HTS=2060
SETTLE=${SETTLE:-12}     # seconds for the AEC to converge after a line-length change

say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/campaign.log"; }
# every fetch aborts the run on failure. Treating an error as a pass is what wedged the board
# during the frame-window descend
get() {
  local r
  r=$(curl -s -m 25 "http://$B$1") || { say "ABORT: HTTP failed on $1"; restore_and_exit; }
  printf '%s' "$r"
}
ctl() { curl -s -m 25 "http://$B/control?$1" >/dev/null || { say "ABORT: control $1 failed"; restore_and_exit; }; }
ring() { curl -s -m 30 "http://$B/control?displayLog=1" 2>/dev/null | grep -a ''; }

# HTS low byte first when DECREASING, high byte first when increasing, so no transient is ever
# shorter than both the value we came from and the one we are going to
set_hts() {
  local want=$1 lo=$(( $1 & 0xFF )) hi=$(( ($1 >> 8) & 0xFF )) cur=${2:-$BASE_HTS}
  if [ "$want" -lt "$cur" ]; then ctl "camReg=0x380D,$lo"; sleep 0.4; ctl "camReg=0x380C,$hi"
  else ctl "camReg=0x380C,$hi"; sleep 0.4; ctl "camReg=0x380D,$lo"; fi
  sleep 0.4
}

restore_and_exit() {
  say "restoring HTS $BASE_HTS and the field config"
  curl -s -m 25 "http://$B/control?camReg=0x380C,0x08" >/dev/null 2>&1; sleep 0.4
  curl -s -m 25 "http://$B/control?camReg=0x380D,0x0C" >/dev/null 2>&1; sleep 2
  curl -s -m 25 "http://$B/control?framesize=13" >/dev/null 2>&1; sleep 8
  for k in quality=10 fps=30 idleFps=5 enableMotion=1 record=1; do
    curl -s -m 25 "http://$B/control?$k" >/dev/null 2>&1
  done
  exit 9
}

# one measurement at the current HTS. Echoes: vsync zoneMean zoneMin zoneMax yavg lo hi state gain bytes dims
sample() {
  local L rate zones gain gainCeil bytes dims tag=$1
  ctl "avgZones=1"; sleep 1
  ctl "camRegRd=0x350A"; sleep 0.6
  ctl "camRegRd=0x350B"; sleep 0.6
  ctl "camRegRd=0x3A18"; sleep 0.6
  ctl "camRegRd=0x3A19"; sleep 0.6
  L=$(ring)
  zones=$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")
  # Both are 10-bit PAIRS in real-gain format, value = 16x the multiplier. Reading only the low
  # byte of the ceiling would report 255 where the part actually allows 511, and the pegged-gain
  # gate would then trip at half the real ceiling and fail every healthy point
  local g0 g1 c0 c1
  g0=$(printf '%s\n' "$L" | grep -a "camRegRd: 0x350A" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/')
  g1=$(printf '%s\n' "$L" | grep -a "camRegRd: 0x350B" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/')
  c0=$(printf '%s\n' "$L" | grep -a "camRegRd: 0x3A18" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/')
  c1=$(printf '%s\n' "$L" | grep -a "camRegRd: 0x3A19" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/')
  gain=$(python -c "print(((0x${g0:-0} & 3) << 8) | 0x${g1:-0})" 2>/dev/null || echo 0)
  gainCeil=$(python -c "v=((0x${c0:-1} & 3) << 8) | 0x${c1:-FF}; print(v if v else 511)" 2>/dev/null || echo 511)
  # VSYNC off the pin is ground truth, never the computed figure
  ctl "xclkStat=1"; sleep 8
  rate=$(ring | grep -a "VSYNC counted" | tail -1 | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
  curl -s -m 30 -o "$OUT/${tag}.jpg" "http://$B/control?still=1" 2>/dev/null
  bytes=$(wc -c < "$OUT/${tag}.jpg" 2>/dev/null || echo 0)
  dims=$(python "$HERE/jpeg_dims.py" "$OUT/${tag}.jpg" 2>/dev/null | awk '{print $1}')
  echo "${rate:-0} $zones ${gain:-0} ${gainCeil:-511} ${bytes:-0} ${dims:-none}"
}

say "== HTS floor walk, $LIGHT arm =="
ctl "record=0"; ctl "enableMotion=0"; ctl "idleFps=0"; ctl "quality=10"; sleep 3

for spec in $SIZES; do
  IFS=: read -r name idx vts ceil <<< "$spec"
  say "-- $name (idx $idx, VTS $vts) --"
  ctl "framesize=$idx"; sleep 8
  [ "$ceil" != "0" ] && { ctl "fps=$ceil"; sleep 5; }
  # baseline at the current floor, in THIS lighting, for THIS size - every gate below is
  # relative to it, because YAVG is a weighted aggregate and no absolute threshold is valid
  set_hts $BASE_HTS $BASE_HTS
  sleep "$SETTLE"
  read -r bRate bMean bMin bMax bYavg bLo bHi bState bGain bCeil bBytes bDims <<< "$(sample "${name}_${LIGHT}_$BASE_HTS")"
  say "   baseline HTS $BASE_HTS: ${bRate}fps zoneMean $bMean YAVG $bYavg gain $bGain/$bCeil still $bBytes ($bDims)"
  echo "$LIGHT,$name,$idx,$BASE_HTS,$vts,$(python -c "print('%.2f'%(80e6/($BASE_HTS*$vts)))"),$bRate,$bMean,$bMin,$bMax,$bYavg,$bLo,$bHi,$bState,$bGain,$bCeil,$bBytes,$bDims,-,-,-,baseline" >> "$CSV"

  prev=$BASE_HTS
  for h in $WALK; do
    set_hts "$h" "$prev"; prev=$h
    sleep "$SETTLE"
    read -r rate mean zmin zmax yavg lo hi state gain gceil bytes dims <<< "$(sample "${name}_${LIGHT}_$h")"
    pred=$(python -c "print('%.2f'%(80e6/($h*$vts)))")
    read -r gRate gAec gStill verdict <<< "$(python - "$rate" "$pred" "$mean" "$bMean" "$gain" "$gceil" "$bytes" "$bBytes" "$dims" "$bDims" "$ceil" <<'PY'
import sys
rate,pred,mean,bmean,gain,gceil,by,bby,dims,bdims,ceil = sys.argv[1:12]
f=lambda x: float(x) if x not in ("","none") else 0.0
# rate is informational on the untuned spot-check sizes, whose clock is the driver's
gr = "OK" if (ceil == "0" or (f(pred) and abs(f(rate)-f(pred))/f(pred) <= 0.015)) else "FAIL"
# the AEC gate: the scene has not moved, so the zone mean must not, and the gain must not peg
drift = abs(f(mean)-f(bmean))/f(bmean) if f(bmean) else 1.0
ga = "OK" if (drift <= 0.25 and f(gceil) and f(gain) <= 0.9*f(gceil)) else "FAIL"
gs = "OK" if (f(by) >= 0.4*f(bby) and dims == bdims and dims != "none") else "FAIL"
print(gr, ga, gs, "pass" if gr=="OK" and ga=="OK" and gs=="OK" else "FAIL")
PY
)"
    say "   HTS $h: ${rate}fps (pred $pred) zoneMean $mean (base $bMean) YAVG $yavg gain $gain/$gceil still $bytes -> rate:$gRate aec:$gAec still:$gStill"
    echo "$LIGHT,$name,$idx,$h,$vts,$pred,$rate,$mean,$zmin,$zmax,$yavg,$lo,$hi,$state,$gain,$gceil,$bytes,$dims,$gRate,$gAec,$gStill,$verdict" >> "$CSV"
    if [ "$verdict" = "FAIL" ]; then
      say "   first failure at HTS $h - floor for $name is the step above. Restoring $BASE_HTS"
      break
    fi
  done
  # never leave a size below its floor, and never rely on a framesize change to undo it:
  # applyHtsFloor only ever LOWERS HTS, so a stale short value survives one
  set_hts $BASE_HTS "$prev"
  sleep 3
done

say "== restoring field config =="
ctl "framesize=13"; sleep 8
for k in quality=10 fps=30 idleFps=5 enableMotion=1 record=1; do ctl "$k"; done
say "== $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
