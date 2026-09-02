#!/bin/bash
# Phase 0 - campaign config, then four one-variable proofs (prediction -> registers -> VSYNC
# count -> forced recording -> still for the eyeball gate) and one ceiling+1 probe per size.
# The probe uses the UNCLAMPED fps path as an instrument: it must run before the clamp fix ships.
#   BOARD=<addr> bash tools/bench/fps_t0_proofs.sh
source "$(dirname "$0")/bench_lib.sh"
assert_campaign_config 10
wait_settled 240
preflight
CSV="$OUT/t0_proofs.csv"
echo "proof,idx,fps,q,retimeRow,vsync,file,frames,actFps,avgBytes,storageMs,sdKBs,boost,busy,motion0,motion1,still,rescue,verdict" > "$CSV"

proof() {  # proof <name> <idx> <fps> <q> <dur> <expSclk> <expVts> <expExpMs> <vsync yes|no> <regime>
  local name=$1 idx=$2 fps=$3 q=$4 dur=$5 xs=$6 xv=$7 xe=$8 vs=$9 regime=${10} ceil=${11}
  log "-- proof $name $fps fps: expect SCLK $xs VTS $xv maxExp ${xe}ms, delivered ~$fps --"
  set_size "$idx" "$q" "$fps"
  local row rc vsync="n/a" rec verdict=PASS
  row=$(get_retime "$name" "$fps" "$ceil" "$regime" "$OUT/t0_${idx}_${fps}_retime.log"); rc=$?
  log "  retime: $row"
  IFS=, read -r _ _ _ sclk _ _ vts _ maxexp _ <<< "$row"
  python - "$sclk" "$xs" "$vts" "$xv" "$maxexp" "$xe" <<'EOF' || verdict=CHECK
import sys; s,xs,v,xv,e,xe = sys.argv[1:7]
ok = abs(float(s)-float(xs)) <= 0.06 and int(v) == int(xv) and abs(float(e)-float(xe)) <= 2
sys.exit(0 if ok else 1)
EOF
  if [ "$vs" = "yes" ]; then
    ctl xclkStat=1 > /dev/null; sleep $(( 60 / fps + 5 ))
    vsync=$(ramlog | grep -a 'VSYNC counted' | tail -1 | sed 's/.*VSYNC counted: //' | cut -c1-70)
    log "  VSYNC: $vsync"
  fi
  rec=$(record_clip "$idx" "$fps" "$q" "$dur" t0)
  local file frames act avg st sd bo bu m0 m1 still resc
  file=$(kv "$rec" file); frames=$(kv "$rec" frames); act=$(kv "$rec" actFps); avg=$(kv "$rec" avgBytes)
  st=$(kv "$rec" storageMs); sd=$(kv "$rec" sdKBs); bo=$(kv "$rec" boost); bu=$(kv "$rec" busy)
  m0=$(kv "$rec" motion0); m1=$(kv "$rec" motion1); still=$(kv "$rec" still); resc=$(kv "$rec" rescue)
  log "  clip $file: $frames frames, actual $act fps, avg ${avg}B, storage ${st}ms, SD ${sd}kB/s, boost $bo, busy $bu%, still $still, motion $m0 -> $m1"
  python - "$act" "$fps" <<'EOF' || verdict=FAIL
import sys; a,f = sys.argv[1:3]
sys.exit(0 if a and float(a) >= 0.98*float(f) else 1)
EOF
  [ "$resc" = "1" ] && { verdict=FAIL; ctl "quality=$q" > /dev/null; }
  echo "$name,$idx,$fps,$q,\"$row\",\"$vsync\",$file,$frames,$act,$avg,$st,$sd,$bo,$bu,$m0,$m1,$still,$resc,$verdict" >> "$CSV"
  log "  verdict: $verdict"
  [ "$verdict" = "FAIL" ] && anomaly "proof $name $fps failed" || pass
  assert_alive
}

# name idx fps q dur expSclk expVts expExpMs vsync regime ceil
proof HD    13 10 10 20 42.67 1968 95  yes vts    52
proof FHD   16 16 10 20 80.00 1128 62  yes vts    16
proof QSXGA 23 1  10 40 11.73 1968 952 no  vts    7
proof VGA   10 5  10 20 10.67 984  189 yes scaler 39   # pickPll fit at 5 fps (mul 80); 10.13 is the 1-4 fps floor

log "-- ceiling+1 probes (unclamped fps path): expect the 'beyond the ... ceiling/range' WRN --"
for entry in "HD:13:52" "1280X960:25:39" "FHD:16:16" "QSXGA:23:7" "VGA:10:39" "QVGA:6:39"; do
  IFS=: read -r name idx ceil <<< "$entry"
  set_size "$idx" 10 "$ceil"
  ctl "fps=$((ceil + 1))" > /dev/null; sleep 4
  w=$(ramlog | grep -a 'beyond the' | tail -1 | sed 's/^\[[^]]*\] //' | cut -c1-120)
  log "  $name $((ceil + 1)): ${w:-NO WARNING LOGGED}"
  echo "probe,$idx,$((ceil + 1)),10,\"${w:-NONE}\",,,,,,,,,,,,,," >> "$CSV"
  ctl "fps=$ceil" > /dev/null; sleep 2
done
assert_alive
log "T0 done -> $CSV. Eyeball the four stills in $OUT (still_t0_*.jpg) before starting T1."
