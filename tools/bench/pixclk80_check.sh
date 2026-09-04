#!/usr/bin/env bash
# The ceiling rungs on the 80 MHz tree: what 1280X960 requests 37-41 get when the PIXCLK
# ceiling is 80 MHz instead of route B's 88 (4 Sep 2026, user's question after the HTS 2156
# ladder). No reflash: each request is retimed by the tuner (route B), then the sensor is put
# where the tuner would put it with route B unavailable - 0x3108 back to 0x26 FIRST (SCLK
# drops to 44), the PLL to mul 120 (80 MHz on route A), then VTS to the 984 floor with the AEC
# ceiling 980 in one group write. That is the 80 MHz ceiling: 80e6 / (2156 x 984) = 37.71 fps
# for every request in the range, since 37 computes VTS 967 (below the floor) and 38-41 are
# above what 80 MHz delivers at the floor.
#
# Prediction, stated first: VSYNC 37.71 at every rung, min = max; delivered 37.0 at request 37
# and ~37.5-37.7 at 38-41 (the 26.5 ms period holds a 20 ms store at ~75% busy); max exposure
# 26.41 ms (980 x 26.95 us) against route B's 24.01; stills clean, route A colour (ratio ~1.0).
#
# Per rung: registers read back (0x3108, mul, HTS, VTS, AEC max), three VSYNC counts, settled
# exposure and gain, a 20 s forced recording, a still through still_color.py. A frame size
# change at the end reloads the tuned registers; then the field config, and a check that
# 1280X960 @41 comes back on route B.
#   BOARD=<addr> bash tools/bench/pixclk80_check.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/pixclk80_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
RATES=${RATES:-"37 38 39 40 41"}
SETTLE=${SETTLE:-10}
IDX=25; Q=10
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/pixclk80.csv"
[ -f "$CSV" ] || echo "fps,tunerRoute,tunerVts,r3108,mul,hts,vts,aecMax,vsyncMax,vsyncMin,expMs,gain,file,duration,frames,actFps,avgBytes,storageMs,sdKBs,boost,rescues,busy,w,h,ratio,gsat,rsat,hdiff,vdiff" > "$CSV"

restore() {
  log "== restore: frame size reload, field config =="
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

log "== PIXCLK 80 MHz ceiling check, 1280X960 requests $RATES (predict 37.71 sensor at every rung) =="
log "lighting: llevel=$(status_field llevel) night=$(status_field night) | ae_level=$(status_field ae_level) banding=$(status_field banding)"
assert_campaign_config "$Q"
preflight
first=1
for f in $RATES; do
  if [ -n "$first" ]; then set_size "$IDX" "$Q" "$f"; first=""; else ctl "fps=$f" > /dev/null; sleep 3; fi
  line=""
  for i in 1 2 3 4 5; do
    line=$(ramlog | grep -a "Tuned timing 1280X960:" | grep -a "for request $f," | tail -1)
    [ -n "$line" ] && break; sleep 2
  done
  [ -n "$line" ] || { log "ABORT: no retime line for request $f"; exit 8; }
  tunerVts=$(printf '%s\n' "$line" | sed 's/.*VTS \([0-9]*\) ->.*/\1/')
  tunerRoute=$(regrd 0x3108)
  # to the 80 MHz tree: larger root divider first, then the PLL, then the frame
  ctl "camReg=0x3108,0x26" > /dev/null; sleep 0.5
  ctl "camPll=0,120,1,0,3,2,1,4" > /dev/null; sleep 1
  # two group writes, not one: the control query string is capped (82 chars was refused as
  # "Query string too long"). The AEC ceiling shrinks first (980 fits inside every VTS in
  # play), then the frame - no interval in which the ceiling exceeds the frame
  ctl "camRegGrp=0x3A02,0x03,0x3A03,0xD4,0x3A14,0x03,0x3A15,0xD4" > /dev/null; sleep 1
  ctl "camRegGrp=0x380E,0x03,0x380F,0xD8" > /dev/null; sleep 2
  r08=$(regrd 0x3108); mul=$((16#$(regrd 0x3036))); hts=$((16#$(regrd 0x380C)$(regrd 0x380D)))
  vts=$((16#$(regrd 0x380E)$(regrd 0x380F))); aecMax=$((16#$(regrd 0x3A02)$(regrd 0x3A03)))
  if [ "$r08" != "26" ] || [ "$mul" != "120" ] || [ "$vts" != "984" ] || [ "$aecMax" != "980" ]; then
    log "ABORT: registers did not land at request $f: 0x3108=$r08 mul=$mul VTS=$vts AECmax=$aecMax"; exit 9
  fi
  read -r mx mn <<< "$(count3)"
  sleep "$SETTLE"; assert_alive
  for r in 0x3500 0x3501 0x3502 0x350A 0x350B; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r expMs gain <<< "$(python - "$hts" "$(rd 0x3500)" "$(rd 0x3501)" "$(rd 0x3502)" "$(rd 0x350A)" "$(rd 0x350B)" <<'PY'
import sys
hts = int(sys.argv[1]); e0, e1, e2, g0, g1 = [int(x, 16) for x in sys.argv[2:7]]
lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
print("%.2f %.2f" % (lines * hts / 80.0 / 1000.0, (((g0 & 3) << 8) | g1) / 16.0))
PY
)"
  rec=$(record_clip "$IDX" "$f" "$Q" 20 pclk80)
  file=$(kv "$rec" file); du=$(kv "$rec" duration); frames=$(kv "$rec" frames); act=$(kv "$rec" actFps)
  avg=$(kv "$rec" avgBytes); st=$(kv "$rec" storageMs); sd=$(kv "$rec" sdKBs); bo=$(kv "$rec" boost)
  rs=$(kv "$rec" rescues); bu=$(kv "$rec" busy)
  stats=$(python "$HERE/still_color.py" "$OUT/still_pclk80_${IDX}_${f}_q${Q}.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  log "   @$f: tuner had 0x3108=$tunerRoute VTS $tunerVts -> forced 0x26 mul $mul HTS $hts VTS $vts AECmax $aecMax | VSYNC $mx / $mn (pred 37.71) | exp ${expMs}ms gain ${gain}x | clip: $frames fr in ${du}s = $act fps, ${avg}B, storage ${st}ms, SD ${sd}kB/s, boost $bo, rescues $rs, busy $bu% | still ratio $ratio hdiff $hdiff vdiff $vdiff"
  echo "$f,$tunerRoute,$tunerVts,$r08,$mul,$hts,$vts,$aecMax,$mx,$mn,$expMs,$gain,$file,$du,$frames,$act,$avg,$st,$sd,$bo,$rs,$bu,$w,$h,$ratio,$gsat,$rsat,$hdiff,$vdiff" >> "$CSV"
  [ "$file" = "NONE" ] && anomaly "@$f: no clip" || pass
done
# the tuned registers come back with a size change: prove 1280X960 @41 is on route B again
set_size "$FIELD_SIZE" "$Q" 30; sleep 3
set_size "$IDX" "$Q" 41; sleep 3
log "after reload: 1280X960 @41 0x3108=$(regrd 0x3108) mul=$((16#$(regrd 0x3036))) VTS=$((16#$(regrd 0x380E)$(regrd 0x380F))) (expect 11 / 66 / 984)"
log "== done: $CSV =="
python - "$CSV" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], newline="")))
print("| request | tuner would run | forced | VSYNC max / min | exposure / gain | delivered fps | avg KB | storage ms | SD kB/s | busy | still ratio / hdiff / vdiff |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for r in rows:
    kb = "%.0f" % (int(r["avgBytes"]) / 1024) if r["avgBytes"] else "-"
    print("| %s | %s, VTS %s | 80 MHz, VTS %s | %s / %s | %s ms / %sx | %s | %s | %s | %s | %s%% | %s / %s / %s |" % (
        r["fps"], "route B" if r["tunerRoute"] == "11" else "route A", r["tunerVts"], r["vts"], r["vsyncMax"], r["vsyncMin"],
        r["expMs"], r["gain"], r["actFps"], kb, r["storageMs"], r["sdKBs"], r["busy"], r["ratio"], r["hdiff"], r["vdiff"]))
PY
