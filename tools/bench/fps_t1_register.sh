#!/bin/bash
# Phase 1 - register tier regression: every integer fps from each mainstay's ceiling down to 1.
# Per point: fps=<f>, 2.5 s, read the RTC ring, parse the retime line for that request, apply
# the three gates and compare with FPS_RECAL_stills/sweep.csv (28 Aug). Light-independent.
#   BOARD=<addr> bash tools/bench/fps_t1_register.sh [--from <idx>:<fps>]
source "$(dirname "$0")/bench_lib.sh"
CSV="$OUT/t1_register.csv"
FROM_IDX=""; FROM_FPS=""
if [ "${1:-}" = "--from" ]; then FROM_IDX=${2%%:*}; FROM_FPS=${2##*:}; fi
# FHD became FHDNARROW on 3 Sep 2026 when FHDMID and FHDFULL joined it - same size, same
# timing, new name (sweep.csv was renamed to match). The two wide variants are scaler regime:
# they keep the ISP scaler on, so their fps moves the PLL at a pinned VTS
SIZES=("QVGA:6:39:scaler" "VGA:10:39:scaler" "HD:13:52:vts" "1280X960:25:39:vts" "FHDNARROW:16:16:vts" "FHDMID:26:12:scaler" "FHDFULL:27:9:scaler" "QSXGA:23:7:vts")

preflight
[ -f "$CSV" ] || echo "idx,size,fps,regime,pixclk,hts,lf,vts,sensorFps,maxExpMs,gateRate,gate33,gate100,match28,flag" > "$CSV"
log "T1 register tier start -> $CSV"
total=0; good=0; skipping=${FROM_IDX:+1}
for entry in "${SIZES[@]}"; do
  IFS=: read -r name idx ceil regime <<< "$entry"
  if [ -n "$skipping" ] && [ "$idx" != "$FROM_IDX" ]; then continue; fi
  log "== $name (idx $idx) ceiling $ceil, $regime =="
  set_size "$idx" 10 "$ceil"
  for f in $(seq "$ceil" -1 1); do
    if [ -n "$skipping" ]; then [ "$f" -gt "$FROM_FPS" ] && continue; skipping=""; fi
    ctl "fps=$f" > /dev/null; sleep 2.5
    row=$(get_retime "$name" "$f" "$ceil" "$regime" "$OUT/t1_${idx}_${f}.log"); rc=$?
    echo "$idx,$row" >> "$CSV"; total=$((total + 1))
    case $rc in
      0) good=$((good + 1)); pass; echo "  $row" ;;
      2) anomaly "$name $f: no retime line for this request" ;;
      *) anomaly "$name $f: $row" ;;
    esac
    case "$row" in *RESCUE*) ctl quality=10 > /dev/null; log "quality re-asserted after a rescue" ;; esac
    [ $((f % 5)) -eq 0 ] && assert_alive
  done
done
assert_alive
log "T1 done: $good / $total points pass all gates and match 28 Aug"
