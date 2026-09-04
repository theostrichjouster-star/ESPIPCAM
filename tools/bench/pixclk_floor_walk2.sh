#!/usr/bin/env bash
# PIXCLK floor walk, part 2 (4 Sep 2026): the gap between the 10.13 MHz floor combo (mul 76,
# sys_div 5) and the first dead rung of part 1 (8.44 MHz, mul 76 / sys_div 6 - noise, hdiff
# 22.7). Same divider, lower multiplier: mul 72 .. 60 at sys_div 5 gives 9.60 .. 8.00 MHz at
# VCO 480 .. 400 (route B ran VCO 440 clean at the top end). Two purposes:
#   - the real floor: the lowest SCLK whose still passes the gates and the eye
#   - the mechanism: 8.40 MHz via mul 63 / sys_div 5 against part 1's 8.44 via mul 76 /
#     sys_div 6. Clean here and noise there -> the divider chain; noise here too -> row time.
# The user's two conditions for low-clock work: motion detection off (campaign config,
# enableMotion 0; the detector's check counter is read at the start and the end and must
# not move) and autofocus off (the sensor's MCU runs the AF program in continuous mode from
# boot - 0x3022 0x08 releases it, 0x3029 must read 0x70 idle). Per rung as part 1.
#   BOARD=<addr> bash tools/bench/pixclk_floor_walk2.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/pixclk_floor2_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=25; Q=10; REQ=${REQ:-1}
SETTLE=${SETTLE:-18}
WALK_MUL=${WALK_MUL:-"72 68 66 64 63 60"}   # at sys_div 5, 0x3108 0x26: 9.60 9.07 8.80 8.53 8.40 8.00 MHz
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/walk.csv"
[ -f "$CSV" ] || echo "route,mul,sysDiv,r3108,sclkMHz,predFps,vsync,expLines,expMs,gain,maxExpMs,yavg,aec,af,w,h,bytes,ratio,gsat,rsat,hdiff,vdiff,rescue" > "$CSV"

restore() {
  log "== restore: 0x3108 0x26 FIRST, PLL 10.13 floor combo, AF continuous again, frame size reload, field config =="
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 0.5
  curl -s -m 25 "$B/control?camPll=0,76,5,0,3,2,1,4" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  af_resume   # continuous AF again, as the boot leaves it
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record) AF 0x3029=$(regrd 0x3029)"
}
trap restore EXIT

regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
checks() { ctl motionStats=1 > /dev/null; sleep 1; ramlog | grep -a "dumpMotionStats" | grep -a -o "[0-9]* checks\|No checks recorded" | tail -1; }
BASE_HDIFF=""

sample() {  # sample <route> <mul> <sysDiv> <sclk MHz>
  local route=$1 mul=$2 sd=$3 sclk=$4 pred r08 gotMul line vs af tag stats rescue
  pred=$(python -c "print('%.3f'%($sclk*1e6/(2156*1968)))")
  sleep "$SETTLE"; assert_alive
  r08=$(regrd 0x3108); gotMul=$((16#$(regrd 0x3036))); af=$(regrd 0x3029)
  ctl "xclkStat=1" > /dev/null; sleep 12
  line=$(ramlog | grep -a "VSYNC counted\|VSYNC count failed" | tail -1)
  vs=$(printf '%s\n' "$line" | sed -n 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/p'); [ -n "$vs" ] || vs=0
  for r in 0x3500 0x3501 0x3502 0x350A 0x350B; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
  read -r expLines expMs gain maxExp <<< "$(python - "$sclk" "$(rd 0x3500)" "$(rd 0x3501)" "$(rd 0x3502)" "$(rd 0x350A)" "$(rd 0x350B)" <<'PY'
import sys
sclk = float(sys.argv[1]); e0, e1, e2, g0, g1 = [int(x, 16) for x in sys.argv[2:7]]
tl = 2156.0 / sclk
lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
print("%d %.1f %.2f %.0f" % (lines, lines * tl / 1000.0, (((g0 & 3) << 8) | g1) / 16.0, 1964 * tl / 1000.0))
PY
)"
  tag="${route}_mul${mul}_sd${sd}_${sclk}MHz"
  curl -s -m 60 -o "$OUT/$tag.jpg" "$B/control?still=1" || log "   still fetch failed at $tag"
  stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  [ -n "$BASE_HDIFF" ] || BASE_HDIFF=$hdiff
  rescue=$(printf '%s\n' "$L" | grep -a -c "No frames for")
  log "   $tag: 0x3108=$r08 mul $gotMul AF 0x$af | VSYNC $vs (pred $pred) | exposure $expLines lines = ${expMs}ms (ceiling ${maxExp}ms), gain ${gain}x, YAVG $yavg $st | still ${w}x${h} ${bytes}B ratio $ratio gsat $gsat rsat $rsat hdiff $hdiff vdiff $vdiff (base $BASE_HDIFF) | rescues $rescue"
  echo "$route,$mul,$sd,$r08,$sclk,$pred,$vs,$expLines,$expMs,$gain,$maxExp,$yavg,$st,$af,$w,$h,$bytes,$ratio,$gsat,$rsat,$hdiff,$vdiff,$rescue" >> "$CSV"
}

log "== PIXCLK floor walk 2, 1280X960 request $REQ, lit: llevel=$(status_field llevel) =="
assert_campaign_config "$Q"
preflight
# The user's condition: the lens must HOLD its position through the low-clock rungs, not go
# to rest. af_hold (bench_lib.sh): continuous AF at FHD 10 fps until the VCM settles, pause,
# then back to 1280X960 with the VCM DAC read before and after. Earlier attempts today: a
# release sent the lens to rest (baseline hdiff 2.8 -> 1.9); single focus at 2.39 fps never
# converged and the pause was ignored; even at 30 fps the program stayed "focusing" until an
# MCU restart brought it back
af_hold "$IDX" "$Q" 30
log "motion: $(checks) (counter reset)"
ctl "fps=$REQ" > /dev/null; sleep 4
log "tuner: $(ramlog | grep -a "Tuned timing 1280X960" | grep -a "request $REQ," | tail -1 | sed 's/^\[[^]]*\] //')"
log "AF at $REQ fps: status 0x$(regrd 0x3029), VCM 0x$(af_vcm) (held at 0x$AF_VCM)"
log "-- baseline, the 10.13 MHz floor combo (mul 76 / sys_div 5) --"
sample M 76 5 10.13
log "-- route M: multiplier at sys_div 5 --"
for m in $WALK_MUL; do
  sclk=$(python -c "print('%.2f'%($m*2/3/5))")
  ctl "camPll=0,$m,5,0,3,2,1,4" > /dev/null; sleep 2
  sample M "$m" 5 "$sclk"
done
log "motion checks during the walk: $(checks) (must be none) | AF at the end: status 0x$(regrd 0x3029)"; af_check
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
