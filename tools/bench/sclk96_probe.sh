#!/usr/bin/env bash
# The SCLK ceiling in spec, 1280X960 (plan quirky-wibbling-turtle, 4 Sep 2026).
#
# The datasheet's 45 fps for 1280x960 is a 22.58us line (HTS 2168 at 96 MHz) times VTS 984.
# The tuner reaches every clock by raising the PLL multiplier with 0x3108 fixed at 0x26
# (SCLK = pll_clki/4), so 80 MHz already sits at the VCO's 800 MHz limit and the 88-92 MHz
# image cliff was measured with the VCO past it. 0x3108 = 0x11 halves every root divider,
# so the same SCLK comes from half the VCO: mul 72 gives VCO 480, pll_clki 192, SCLK 96,
# SCLK2x 192, DVP port 24 MHz - the datasheet's own clock set, entirely in spec.
#
# Runs 1-6 (05:40-06:06) established, one variable at a time:
#   - the PLL decode is exact (mul 72 -> 48.00 MHz implied, mul 120 -> 80.00)
#   - set_pll leaves 0x3108 at 0x26, so the root divider is ours to write
#   - HTS 2472 at SCLK 80 (30.9us) is clean and stretch-free; HTS 2060 is intermittently
#     ~3% slow on a 1280-wide output with the AEC extension registers at zero
#   - SCLK 96 via 0x3108 turns the frame into random noise (hdiff 31 against 2.4) even on
#     the 25.75us line that is clean at 80 MHz, with the VSYNC rate still exact. So the
#     cliff is NOT the VCO: something in the sensor's clocked path fails between 80 and 96
# This run finds WHERE between 80 and 96 it fails, on the in-spec route, then walks HTS
# down at the highest clean clock.
#
# Three gates per point; the image gates decide, the rate gate is recorded:
#   rate    VSYNC counted off the pin, max of 3, within 1.5% of the prediction (3.5% at the
#           HTS 2060 baseline because of the stretch above)
#   colour  still_color.py ratio 0.9-1.2, G and R saturation under 3%
#   noise   horizontally adjacent luma difference within 1.25x of this run's baseline - the
#           channel means alone called a frame of pure confetti "borderline"
#
# RESTORE ORDER MATTERS. The tuner never touches 0x3108, so a stale 0x11 would double the
# next tuned clock. 0x3108 goes back to 0x26 FIRST, then the PLL, then HTS, then the field
# config. The EXIT trap does this on every exit path, including bench_lib aborts.
#
#   BOARD=<addr> bash tools/bench/sclk96_probe.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/sclk96}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
SETTLE=${SETTLE:-12}
IDX=25; VTS=984
FIELD_SIZE=13; FIELD_FPS=30
MULS=${MULS:-"63 66 69"}            # SCLK 84, 88, 92 on the 0x3108=0x11 route (96 is known bad)
WALK=${WALK:-"2300 2200 2112 2060"} # HTS at the best clock: 2112 is the 24us line at SCLK 88

RESTORED=0
restore() {
  [ "$RESTORED" = "1" ] && return
  RESTORED=1
  log "== restore: 0x3108 -> 0x26 first, then PLL mul 120, HTS 2060, field config =="
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?camPll=0,120,1,0,3,2,1,4" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?camReg=0x380D,0x0C" > /dev/null; sleep 0.4
  curl -s -m 25 "$B/control?camReg=0x380C,0x08" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config restored: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT

regrd() {  # regrd 0xADDR -> hex value string from the ring
  ctl "camRegRd=$1" > /dev/null; sleep 0.8
  ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'
}

# HTS bytes: low byte first when decreasing, high byte first when increasing, so no transient
# is ever shorter than both ends (hts_floor.sh rule); same high byte means one atomic write
set_hts() {  # set_hts <want> <current>
  local want=$1 cur=$2 lo hi
  lo=$(printf '0x%02X' $(( want & 0xFF ))); hi=$(printf '0x%02X' $(( (want >> 8) & 0xFF )))
  if [ $(( (want >> 8) & 0xFF )) -eq $(( (cur >> 8) & 0xFF )) ]; then ctl "camReg=0x380D,$lo" > /dev/null
  elif [ "$want" -lt "$cur" ]; then ctl "camReg=0x380D,$lo" > /dev/null; sleep 0.4; ctl "camReg=0x380C,$hi" > /dev/null
  else ctl "camReg=0x380C,$hi" > /dev/null; sleep 0.4; ctl "camReg=0x380D,$lo" > /dev/null; fi
  sleep 1
  log "   HTS reads 0x$(regrd 0x380C)$(regrd 0x380D) (wanted $want)"
}

# set_sclk <mul> : the in-spec route. set_pll rewrites 0x3108 to 0x26 (verified), so the
# transient is the SLOW config (SCLK = pll_clki/4) and 0x11 is written after
set_sclk() {
  local mul=$1 r08
  ctl "camPll=0,$mul,1,0,3,2,1,4" > /dev/null; sleep 2
  r08=$(regrd 0x3108)
  [ "$r08" = "26" ] || { log "ABORT: set_pll left 0x3108 at $r08, not 0x26 - the root divider decode is wrong"; exit 8; }
  ctl "camReg=0x3108,0x11" > /dev/null; sleep 2
  log "   mul $mul, 0x3108=$(regrd 0x3108): SCLK $(python -c "print('%.1f'%($mul*4/3))") MHz expected"
}
# the 0x11 route: SCLK = pll_clki/2 = (mul x 20/3 / 2.5) / 2 = mul x 4/3 MHz. Run 7 (06:11)
# had this halved instead of doubled and failed a point that measured 84.00 MHz exactly
sclk_of_mul() { python -c "print($1*4/3*1e6)"; }

# sample <tag> <predicted fps> [rate tolerance] : settle, count VSYNC x3, still, three gates.
# Sets G_RATE G_COLOUR G_NOISE; returns 0 only when the IMAGE is clean (colour and noise)
sample() {
  local tag=$1 pred=$2 tol=${3:-0.015} rate implied line stats verdict rates="" i r1 rmin
  sleep "$SETTLE"
  assert_alive
  for i in 1 2 3; do
    ctl "xclkStat=1" > /dev/null; sleep 8
    line=$(ramlog | grep -a "VSYNC counted" | tail -1)
    r1=$(printf '%s\n' "$line" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
    rates="$rates ${r1:-0}"
  done
  rate=$(python -c "print(max(float(x) for x in '$rates'.split()))")
  rmin=$(python -c "print(min(float(x) for x in '$rates'.split()))")
  implied=$(printf '%s\n' "$line" | sed 's/.*implied PIXCLK \([0-9.]*\)MHz.*/\1/')
  implied=$(python -c "print('%.2f'%($implied * $rate / ${r1:-1}))" 2>/dev/null || echo "$implied")
  curl -s -m 30 -o "$OUT/${tag}.jpg" "$B/control?still=1" || { log "ABORT: still fetch failed"; exit 7; }
  stats=$(python "$HERE/still_color.py" "$OUT/${tag}.jpg") || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  [ -n "${BASE_HDIFF:-}" ] || BASE_HDIFF=$hdiff
  verdict=$(python - "$rate" "$pred" "$w" "$h" "$ratio" "$gsat" "$rsat" "$hdiff" "$BASE_HDIFF" "$tol" <<'PY'
import sys
rate, pred, w, h, ratio, gsat, rsat, hdiff, base, tol = [float(x) for x in sys.argv[1:11]]
gr = "OK" if pred and abs(rate - pred) / pred <= tol else "FAIL"
gc = "OK" if (0.9 <= ratio <= 1.2 and gsat < 3.0 and rsat < 3.0 and w == 1280 and h == 960) else "FAIL"
gn = "OK" if hdiff <= max(3.0, 1.25 * base) else "FAIL"
print(gr, gc, gn)
PY
)
  read -r G_RATE G_COLOUR G_NOISE <<< "$verdict"
  log "   $tag: ${rate}fps max of 3 (min $rmin; pred $pred, implied SCLK ${implied}MHz) still ${bytes}B ratio $ratio gsat $gsat% rsat $rsat% hdiff $hdiff -> rate:$G_RATE colour:$G_COLOUR noise:$G_NOISE"
  echo "$tag,$pred,$rate,$rmin,$implied,$w,$h,$bytes,$mr,$mg,$mb,$ratio,$gsat,$rsat,$hdiff,$G_RATE,$G_COLOUR,$G_NOISE" >> "$OUT/points.csv"
  [ "$G_COLOUR" = "OK" ] && [ "$G_NOISE" = "OK" ]
}

[ -f "$OUT/points.csv" ] || echo "tag,predFps,vsyncMax,vsyncMin,impliedMHz,w,h,bytes,meanR,meanG,meanB,ratio,gsatPct,rsatPct,hdiff,gateRate,gateColour,gateNoise" > "$OUT/points.csv"
log "== SCLK ceiling probe, 1280X960, lights on: muls $MULS, walk $WALK =="
assert_campaign_config 10
preflight
set_size $IDX 10 39
sleep 4
log "registers at start: 0x3108=$(regrd 0x3108) 0x3036=$(regrd 0x3036) 0x380C=$(regrd 0x380C) 0x380D=$(regrd 0x380D)"

log "A1 baseline SCLK 80 HTS 2060, predict 39.47"
sample A1_sclk80_hts2060 39.47 0.035 || { log "ABORT: baseline image failed its own gates"; exit 8; }
log "A2 HTS 2472 at SCLK 80 (30.9us, stretch-free), predict 32.89"
set_hts 2472 2060
sample A2_sclk80_hts2472 32.89 && [ "$G_RATE" = "OK" ] || { log "ABORT: HTS 2472 at SCLK 80 failed before any clock change"; exit 8; }

# B: raise the clock on the 30.9us-equivalent line, one multiplier at a time, stop at the
# first corrupt frame. Every point is the same HTS 2472 / VTS 984, only the clock moves
BEST_MUL=""
for mul in $MULS; do
  sclk=$(sclk_of_mul "$mul")
  pred=$(python -c "print('%.2f'%($sclk/(2472*$VTS)))")
  log "B mul $mul via 0x3108=0x11 (SCLK $(python -c "print('%.0f'%($sclk/1e6))"), VCO $(python -c "print('%.0f'%($mul*20/3))")), HTS 2472, predict $pred"
  set_sclk "$mul"
  if sample "B_mul${mul}_hts2472" "$pred" && [ "$G_RATE" = "OK" ]; then
    BEST_MUL=$mul
  else
    log "   SCLK $(python -c "print('%.0f'%($sclk/1e6))") is the first bad clock on the in-spec route"
    break
  fi
done
if [ -z "$BEST_MUL" ]; then
  log "RESULT: no clock above 80 MHz is clean on the 0x3108 route either. The ceiling is SCLK 80."
  exit 0
fi

# C: at the best clean clock, walk the line down. Image gates stop the walk; a rate FAIL with
# a clean image is the 1280-wide stretch and is recorded, not acted on
sclk=$(sclk_of_mul "$BEST_MUL")
log "RESULT: highest clean clock on the in-spec route is mul $BEST_MUL = SCLK $(python -c "print('%.0f'%($sclk/1e6))") MHz"
if [ "$BEST_MUL" != "${MULS##* }" ]; then
  log "C re-arming mul $BEST_MUL after the failed point above"
  ctl "camReg=0x3108,0x26" > /dev/null; sleep 1
  set_sclk "$BEST_MUL"
fi
prev=2472
for h in $WALK; do
  pred=$(python -c "print('%.2f'%($sclk/($h*$VTS)))")
  log "C HTS $h at SCLK $(python -c "print('%.0f'%($sclk/1e6))") ($(python -c "print('%.2f'%($h/($sclk/1e6)))")us), predict $pred"
  set_hts "$h" "$prev"; prev=$h
  if ! sample "C_mul${BEST_MUL}_hts$h" "$pred"; then
    log "   edge: HTS $h is the first corrupt line at this clock; the clean ceiling is the point above"
    break
  fi
  [ "$G_RATE" = "OK" ] || log "   (image clean, rate short: the 1280-wide output stretch at HTS $h)"
done
log "== done: $OUT/points.csv =="
column -s, -t < "$OUT/points.csv" 2>/dev/null || cat "$OUT/points.csv"
