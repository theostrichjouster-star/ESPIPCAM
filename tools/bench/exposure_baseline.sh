#!/usr/bin/env bash
# Max exposure per visible frame size, at 1 fps and at the ceiling, read off the BOARD.
#
# This is the map for "maximise available exposure time per frame size". Exposure at a given rate is
# 1964 x tROW capped by the frame, so it is decided by three things the tuner chooses per rung: which
# path the size is on (VTS-tuned or clock-tuned at a pinned VTS), the line length, and the clock. The
# arithmetic is knowable but the tuner has branches, so read what it actually programs.
#
#   BOARD=<addr> bash tools/bench/exposure_baseline.sh
# SIZES_LIST overrides the set as name:idx:ceiling.
set -u
OUT=${OUT:-FPS_RECAL_stills/exposure_baseline_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

# every size the camera panel offers for the OV5640, ceiling from frameData maxTunedFPS
if [ -n "${SIZES_LIST:-}" ]; then read -r -a SIZES <<< "$SIZES_LIST"
else SIZES=("QVGANARROW:29:147" "VGANARROW:28:77" "HD:13:52" "1280X960:25:41" "QVGA:6:39" "VGA:10:39" \
            "FHDNARROW:16:16" "FHDMID:26:12" "FHDFULL:27:9" "QHD:20:9" "QSXGA:23:7"); fi

CSV="$OUT/exposure_baseline.csv"
echo "size,idx,reqFps,path,pixClkMHz,hts,lf,vts,aecMaxLines,expMs,periodMs,expPct" > "$CSV"

S0_SIZE=$(status_field framesize); S0_FPS=$(status_field fps); S0_IDLE=$(status_field idleFps)
log "restore point: framesize=$S0_SIZE fps=$S0_FPS idleFps=$S0_IDLE"
restore() {
  curl -s -m 25 "$B/control?framesize=$S0_SIZE" > /dev/null; sleep 5
  curl -s -m 25 "$B/control?fps=$S0_FPS" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?idleFps=$S0_IDLE" > /dev/null; sleep 2
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) idleFps=$(status_field idleFps)"
}
trap restore EXIT

ctl idleFps=0 > /dev/null   # the throttle would retime underneath every reading
ctl record=0 > /dev/null
ctl enableMotion=0 > /dev/null
log "== max exposure per size, 1 fps and ceiling, ${#SIZES[@]} sizes =="

point() {  # point <name> <idx> <fps>
  local name=$1 idx=$2 fps=$3 line path clk hts lf vts aecMax expMs period pct
  set_size "$idx" 10 "$fps"; sleep 6
  # The tuner's own line for THIS retime. Two shapes, one per path, and which one appears IS the
  # answer to "how is this size tuned" - so the path is read rather than assumed.
  # Matched on "for request <fps>" as well as the size, because a size change retimes at whatever
  # rate is in force first: without that, tail -1 read the idle throttle's 5 fps line and reported
  # its exposure as the answer for 1 fps. A wrong line here looks entirely plausible
  line=$(ramlog | grep -a "Tuned timing $name:\|Scaler clock $name:" | grep -a "for request $fps\b" | tail -1)
  if [ -z "$line" ]; then  # one more settle - a slow size can take two frames to land the rate
    sleep 8
    line=$(ramlog | grep -a "Tuned timing $name:\|Scaler clock $name:" | grep -a "for request $fps\b" | tail -1)
  fi
  if printf '%s' "$line" | grep -q "Scaler clock"; then path="clock"; else path="vts"; fi
  clk=$(printf '%s' "$line" | sed -n 's/.*PIXCLK \([0-9.]*\)MHz.*/\1/p')
  hts=$(printf '%s' "$line" | sed -n 's/.*HTS \([0-9]*\) x[0-9]*.*/\1/p')
  lf=$(printf '%s' "$line" | sed -n 's/.*HTS [0-9]* x\([0-9]*\).*/\1/p')
  vts=$(printf '%s' "$line" | sed -n 's/.*VTS \([0-9]*\).*/\1/p')
  # The exposure ceiling in LINES. `updateFPS` reports it as aecMax, and that arrives in the HTTP
  # REPLY as JSON - it is not a log line and /status does not carry it either, which is what made
  # the first version of this script skip all 22 points in silence
  aecMax=$(ctl updateFPS=1 | python -c "import json,sys; print(json.load(sys.stdin).get('aecMax',''))" 2>/dev/null)
  # the VTS-tuned line already states the exposure in ms; the scaler-clock line carries no exposure
  # figure at all, which is why aecMax is fetched for both rather than parsed from the line
  if [ -z "$clk" ] || [ -z "$hts" ] || [ -z "$vts" ]; then
    log "   $name @$fps: no retime line matched - got '${line:0:90}'"; return
  fi
  if [ -z "$aecMax" ]; then log "   $name @$fps: updateFPS gave no aecMax"; return; fi
  read -r expMs period pct <<< "$(python -c "
tl = $hts * $lf / ($clk * 1e6) * 1000.0
e = $aecMax * tl
p = $vts * tl
print('%.1f %.1f %.0f' % (e, p, 100.0 * e / p if p else 0))")"
  log "   $name @$fps: $path path, PIXCLK $clk, HTS $hts x$lf, VTS $vts | exposure $aecMax lines = ${expMs}ms of a ${period}ms frame (${pct}%)"
  echo "$name,$idx,$fps,$path,$clk,$hts,$lf,$vts,$aecMax,$expMs,$period,$pct" >> "$CSV"
}

for s in "${SIZES[@]}"; do
  IFS=: read -r name idx ceil <<< "$s"
  point "$name" "$idx" 1
  point "$name" "$idx" "$ceil"
done

log "== done: $CSV =="
column -s, -t < "$CSV"
