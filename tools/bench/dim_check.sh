#!/usr/bin/env bash
# The dim check: what the AEC does at a rate when the room is dark (4 Sep 2026, first run at
# 1280X960 41 and 20 on the HTS 2156 / banding-off image, lights dimmed by the user).
#
# Prediction, stated first: with banding off the AEC spends the whole frame before gain, so at
# 41 the exposure should read 980 lines = 24.01 ms (the frame) with gain well above the 1.19x
# floor, and at 20 up to 1814 lines = 48.9 ms at roughly half that gain. YAVG inside the
# Exposure Level -2 band (32..37) if the gain ceiling (31.9x) suffices, below it with gain
# pinned if not. VSYNC unchanged (41.48, 20.42): night mode is off, so no frame extension.
# Gain noise raises hdiff above the lit baseline - that is expected, not a corruption tell;
# vdiff is the flicker number (a dimmer is a flicker source with the banding filter off).
#
# Per point: fps=<f>, a long AEC settle, the retime line, 0x3108, exposure 0x3500-02, gain
# 0x350A/B, AEC extra lines 0x350C/D, avgZones (YAVG vs band), light level, three VSYNC
# counts, a 20 s forced recording, a still through still_color.py. Restores the field config.
#   BOARD=<addr> [RATES="41 20"] bash tools/bench/dim_check.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/dim_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
SIZE=${SIZE:-1280X960}; IDX=${IDX:-25}; Q=${Q:-10}
RATES=${RATES:-"41 20"}
SETTLE=${SETTLE:-20}
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/dim.csv"
[ -f "$CSV" ] || echo "fps,llevel,r3108,pixclk,hts,vts,maxExpMs,expLines,expMs,gain,extraLines,yavg,bandLo,bandHi,aec,vsyncMax,vsyncMin,file,frames,duration,actFps,avgBytes,storageMs,sdKBs,busy,rescues,w,h,ratio,gsat,rsat,hdiff,vdiff" > "$CSV"

restore() {
  log "== restore field config =="
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT

regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }

count3() {
  local rates="" i line r
  for i in 1 2 3; do
    ctl "xclkStat=1" > /dev/null; sleep 8
    line=$(ramlog | grep -a "VSYNC counted" | tail -1)
    r=$(printf '%s\n' "$line" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/'); rates="$rates ${r:-0}"
  done
  python -c "v=[float(x) for x in '$rates'.split()]; print('%.3f %.3f'%(max(v),min(v)))"
}

log "== dim check: $SIZE at $RATES =="
log "lighting: llevel=$(status_field llevel) night=$(status_field night) | ae_level=$(status_field ae_level) banding=$(status_field banding)"
assert_campaign_config "$Q"
preflight
first=1
for f in $RATES; do
  if [ -n "$first" ]; then set_size "$IDX" "$Q" "$f"; first=""; else ctl "fps=$f" > /dev/null; sleep 3; fi
  line=""
  for i in 1 2 3 4 5; do
    line=$(ramlog | grep -a "Tuned timing $SIZE:" | grep -a "for request $f," | tail -1)
    [ -n "$line" ] && break; sleep 2
  done
  [ -n "$line" ] || { log "ABORT: no retime line for $SIZE @$f"; exit 8; }
  read -r pix hts lf vts maxExpLog <<< "$(printf '%s\n' "$line" | sed 's/.*PIXCLK \([0-9.]*\)MHz, HTS \([0-9]*\) x\([0-9]\), VTS \([0-9]*\) -> sensor [0-9.]*fps for request [0-9]*, max exposure \([0-9]*\)ms.*/\1 \2 \3 \4 \5/')"
  sleep "$SETTLE"; assert_alive
  r08=$(regrd 0x3108)
  for r in 0x3500 0x3501 0x3502 0x350A 0x350B 0x350C 0x350D; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
  read -r expLines expMs maxExp gain extra <<< "$(python - "$pix" "$hts" "$lf" "$vts" "$(rd 0x3500)" "$(rd 0x3501)" "$(rd 0x3502)" "$(rd 0x350A)" "$(rd 0x350B)" "$(rd 0x350C)" "$(rd 0x350D)" <<'PY'
import sys
pix, hts, lf, vts = float(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
e0, e1, e2, g0, g1, x0, x1 = [int(v, 16) for v in sys.argv[5:12]]
tl = hts * lf / pix
lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
print("%d %.2f %.2f %.2f %d" % (lines, lines * tl / 1000.0, min(vts - 4, 1964) * tl / 1000.0, (((g0 & 3) << 8) | g1) / 16.0, (x0 << 8) | x1))
PY
)"
  ll=$(status_field llevel)
  read -r mx mn <<< "$(count3)"
  rec=$(record_clip "$IDX" "$f" "$Q" 20 dim)
  file=$(kv "$rec" file); du=$(kv "$rec" duration); frames=$(kv "$rec" frames); act=$(kv "$rec" actFps)
  avg=$(kv "$rec" avgBytes); stm=$(kv "$rec" storageMs); sd=$(kv "$rec" sdKBs); bu=$(kv "$rec" busy); rs=$(kv "$rec" rescues)
  stats=$(python "$HERE/still_color.py" "$OUT/still_dim_${IDX}_${f}_q${Q}.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  log "   @$f (llevel $ll): 0x3108=$r08 PIXCLK $pix HTS $hts VTS $vts | exposure $expLines lines = ${expMs}ms of max ${maxExp}ms, gain ${gain}x, AEC extra $extra lines, YAVG $yavg band $lo..$hi $st | VSYNC $mx / $mn | clip $frames fr in ${du}s = $act fps, ${avg}B, storage ${stm}ms, SD ${sd}kB/s, busy $bu%, rescues $rs | still ${w}x${h} ${bytes}B ratio $ratio gsat $gsat rsat $rsat hdiff $hdiff vdiff $vdiff"
  echo "$f,$ll,$r08,$pix,$hts,$vts,$maxExp,$expLines,$expMs,$gain,$extra,$yavg,$lo,$hi,$st,$mx,$mn,$file,$frames,$du,$act,$avg,$stm,$sd,$bu,$rs,$w,$h,$ratio,$gsat,$rsat,$hdiff,$vdiff" >> "$CSV"
  [ "$file" = "NONE" ] && anomaly "$SIZE @$f: no clip" || pass
done
log "== done: $CSV =="
