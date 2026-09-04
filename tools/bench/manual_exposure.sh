#!/usr/bin/env bash
# Manual exposure past 1964 lines, AEC off, QSXGA at the clock floor (4 Sep 2026).
#
# The datasheet lists 1964 x tROW as the maximum exposure interval, and the AEC engine parks
# once VTS passes ~1984 (BOARD_TESTING 37, dim check 2). Whether the INTEGRATION is capped at
# 1964 lines, or only the engine, is the question. Datasheet 4.6.2: manual exposure needs
# 0x3503[0]; the value in 0x3500-0x3502 is lines x 16 (low nibble 0); it must be less than
# {0x380E,0x380F} + {0x350C,0x350D}; to go past the frame, raise the frame first.
#
# One variable per step, everything else held: HTS the tuner's 2844 at the 10.13 floor
# (PLL_MUL 76, line 561.5 us), gain manual and fixed, lens held at a lit DAC code, mic and
# motion off, sensor quality by config. Points (VTS / exposure lines):
#   P1 1968 / 1964   the reference, the AEC's own ceiling
#   P2 3936 / 1964   control: same exposure in a doubled frame (brightness must not move)
#   P3 3936 / 3932   2.00x the lines
#   P4 5904 / 5900   3.00x
#   P5 6600 / 6596   3.36x (frame 3.71 s, under the 4 s no-frame rescue at FPS 1)
#   P6 1968 / 1964   the reference again (lamp drift check)
# Prediction stated first: P2 = P1 within 10%; if the integration follows the register
# (branch A) P3 = 2 x P1, P4 = 3x, P5 = 3.4x in still luma (the black clip makes the ratios
# run high, not low); if the sensor caps at 1964 lines (branch B) P3-P5 = P1. VSYNC follows
# VTS: 10.13e6 / (2 x 2844 x VTS) = 0.905, 0.452, 0.452, 0.302, 0.270, 0.905 fps.
#
# Register order (each intermediate value valid): exposure UP = 0x3502, 0x3501, 0x3500 (the
# intermediate is never above the target); exposure DOWN = 0x3500, 0x3501, 0x3502, and always
# before the frame comes down; VTS UP = 0x380E then 0x380F; VTS DOWN = 0x380F then 0x380E.
#
# The quirk found on the way (manual_probe.sh, 15:24-15:28, FHDNARROW 1.17 fps): in manual
# mode an exposure register DEcrease gives a persistent flat black frame (1964 -> 982 lines by
# register, 1964 -> 122 by the driver's aec_value: luma 4, hdiff 0, YAVG 0-2, the AGC parked
# at 2x), while the same-value rewrite, an increase (982 -> 1964) and gain changes in both
# directions through the driver (0x1FF -> 0x3FF doubled YAVG, agc_gain=30 -> 479/16) give
# proper frames. The 15:19 QSXGA run went black at P1 after a direct 0x350A/0x350B decrease
# (0x2FF -> 0x1FF). Mechanism unknown - not in the datasheet's 4.6.2. So this rig sets the
# gain through the driver, only ever raises the exposure, and keeps P6 (back to 1964) as a
# deliberate third instance of the decrease: black expected there, bright would refute it.
#   BOARD=<addr> bash tools/bench/manual_exposure.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/manual_exposure_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=${IDX:-23}; Q=${Q:-28}; REQ=1; HTS=${HTS:-2844}; LF=2; SCLK=${SCLK:-10.13}; PLL_MUL=${PLL_MUL:-76}
POINTS=${POINTS:-"1968:1964 3936:1964 3936:3932 5904:5900 6600:6596 1968:1964"}
GAIN=${GAIN:-}            # manual gain (x), else picked from the AEC's settled state
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/manual.csv"
[ -f "$CSV" ] || echo "point,vts,expLines,expMs,predFps,vsync,r3503,gain,extra350C,yavg,w,h,bytes,luma,ratio,hdiff,vdiff,qs,stillTries,rescue" > "$CSV"

hexb() { printf '0x%02X' "$1"; }
wr() { ctl "camReg=$1,$(hexb "$2")" > /dev/null; sleep 0.4; }
rd16() { echo $(( 16#$(regrd "$1")$(regrd "$2") )); }
set_exposure() {  # lines; the write order keeps every intermediate value valid
  local v=$(( $1 * 16 )) cur b0 b1 b2
  cur=$(( (16#$(regrd 0x3500) & 0x0F) << 16 | 16#$(regrd 0x3501) << 8 | 16#$(regrd 0x3502) ))
  b0=$(( (v >> 16) & 0x0F )); b1=$(( (v >> 8) & 0xFF )); b2=$(( v & 0xF0 ))
  if [ "$v" -ge "$cur" ]; then wr 0x3502 $b2; wr 0x3501 $b1; wr 0x3500 $b0
  else wr 0x3500 $b0; wr 0x3501 $b1; wr 0x3502 $b2; fi
  local got=$(( ((16#$(regrd 0x3500) & 0x0F) << 16 | 16#$(regrd 0x3501) << 8 | 16#$(regrd 0x3502)) / 16 ))
  [ "$got" = "$1" ] || { log "ABORT: exposure $1 lines reads back $got"; exit 9; }
}
set_vts() {
  local cur; cur=$(rd16 0x380E 0x380F)
  if [ "$1" -ge "$cur" ]; then wr 0x380E $(( $1 >> 8 )); wr 0x380F $(( $1 & 0xFF ))
  else wr 0x380F $(( $1 & 0xFF )); wr 0x380E $(( $1 >> 8 )); fi
  local got; got=$(rd16 0x380E 0x380F)
  [ "$got" = "$1" ] || { log "ABORT: VTS $1 reads back $got"; exit 9; }
}
set_gain() {  # x, through the driver's agc_gain (manual_probe.sh 15:27: agc_gain=30 landed
              # 479/16 = 29.94x and the frame stayed bright, where a direct 0x350A/0x350B
              # DEcrease in the 15:19 run was followed by a flat black frame - see the header)
  local n; n=$(python -c "print(min(63, max(1, int(round($1)))))")
  ctl "agc_gain=$n" > /dev/null; sleep 0.6
  local got=$(( (16#$(regrd 0x350A) & 3) << 8 | 16#$(regrd 0x350B) ))
  [ "$got" -ge $(( n * 16 - 16 )) ] && [ "$got" -le $(( n * 16 )) ] || { log "ABORT: agc_gain=$n reads back $got/16"; exit 9; }
  log "gain: agc_gain=$n -> $got/16 = $(python -c "print('%.2f' % ($got / 16.0))")x"
}
restore() {
  log "== restore: exposure 1964, VTS 1968, AEC/AGC auto, frame size reload, field config, AF continuous =="
  set_exposure 1964; set_vts 1968
  wr 0x3503 0x00; log "0x3503 back to 0x$(regrd 0x3503)"
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  af_resume
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT
checks() { ctl motionStats=1 > /dev/null; sleep 1; ramlog | grep -a "dumpMotionStats" | grep -a -o "[0-9]* checks\|No checks recorded" | tail -1; }
BASE_LUMA=""

sample() {  # sample <point> <vts> <lines>
  local pt=$1 vts=$2 lines=$3 tl expMs pred period settle line vs L qs tries retryGap rescue
  read -r tl expMs pred period <<< "$(python -c "tl=$HTS*$LF/$SCLK; f=1e6/(tl*$vts); print('%.1f %.1f %.3f %.2f'%(tl, $lines*tl/1000, f, 1/f))")"
  settle=$(python -c "print(int(4 * $period + 4))")   # four frames plus the pipeline
  sleep "$settle"; assert_alive
  ctl "xclkStat=1" > /dev/null; sleep 14
  line=$(ramlog | grep -a "VSYNC counted\|VSYNC count failed" | tail -1)
  vs=$(printf '%s\n' "$line" | sed -n 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/p'); [ -n "$vs" ] || vs=0
  for r in 0x3503 0x3500 0x3501 0x3502 0x350A 0x350B 0x350C 0x350D 0x380E 0x380F 0x4407; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
  local gotLines=$(( ((16#$(rd 0x3500) & 0x0F) << 16 | 16#$(rd 0x3501) << 8 | 16#$(rd 0x3502)) / 16 ))
  local gotVts=$(( 16#$(rd 0x380E)$(rd 0x380F) )) extra=$(( 16#$(rd 0x350C)$(rd 0x350D) ))
  local g16=$(( (16#$(rd 0x350A) & 3) << 8 | 16#$(rd 0x350B) )) gain
  gain=$(python -c "print('%.2f' % ($g16 / 16.0))")
  qs=$(rd 0x4407); tries=0
  retryGap=$(python -c "print('%.2f' % max(0.2, 7.0 / (6 * $pred) - 1.25))")
  while [ "$tries" -lt 6 ]; do
    tries=$((tries + 1))
    curl -s -m 90 -o "$OUT/$pt.jpg" "$B/control?still=1" && [ -s "$OUT/$pt.jpg" ] && break
    sleep "$retryGap"
  done
  [ -s "$OUT/$pt.jpg" ] || log "   no still at $pt after $tries requests"
  local stats; stats=$(python "$HERE/still_color.py" "$OUT/$pt.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  local luma; luma=$(python -c "print('%.1f' % (0.299*$mr + 0.587*$mg + 0.114*$mb))")
  [ -n "$BASE_LUMA" ] || BASE_LUMA=$luma
  local rel; rel=$(python -c "print('%.2f' % ($luma / $BASE_LUMA)) if $BASE_LUMA > 0 else print('-')")
  rescue=$(printf '%s\n' "$L" | grep -a -c "No frames for")
  log "   $pt: VTS $gotVts, exposure $gotLines lines = ${expMs}ms, extra $extra, 0x3503 0x$(rd 0x3503), gain ${gain}x | VSYNC $vs (pred $pred) | YAVG $yavg | still ${w}x${h} ${bytes}B luma $luma (x$rel of P1) ratio $ratio hdiff $hdiff vdiff $vdiff qs 0x$qs, $tries request(s) | rescues $rescue"
  echo "$pt,$gotVts,$gotLines,$expMs,$pred,$vs,$(rd 0x3503),$gain,$extra,$yavg,$w,$h,$bytes,$luma,$ratio,$hdiff,$vdiff,$qs,$tries,$rescue" >> "$CSV"
}

log "== manual exposure past 1964 lines, size idx $IDX, quality $Q, SCLK $SCLK (PLL_MUL $PLL_MUL), llevel=$(status_field llevel) night=$(status_field night) =="
assert_campaign_config "$Q"
ctl micGain=0 > /dev/null; log "audio: micGain=$(status_field micGain)"
preflight
af_hold "$IDX" "$Q" "$REQ"
log "motion: $(checks) (counter reset)"
sleep 3
log "tuner: $(ramlog | grep -a "Tuned timing" | grep -a "request $REQ," | tail -1 | sed 's/^\[[^]]*\] //')"
r3108=$(regrd 0x3108); r3035=$(regrd 0x3035); was=$(regrd 0x3036)
[ "$r3108" = "26" ] || { log "ABORT: 0x3108 is 0x$r3108, not route A - no PLL write"; exit 9; }
[ "${r3035:0:1}" = "5" ] || { log "ABORT: 0x3035 is 0x$r3035, system divider not 5"; exit 9; }
ctl "camReg=0x3036,$(hexb "$PLL_MUL")" > /dev/null; sleep 3
got=$(regrd 0x3036); [ "$((16#$got))" = "$PLL_MUL" ] || { log "ABORT: 0x3036 reads 0x$got"; exit 9; }
log "PLL multiplier $((16#$was)) -> $PLL_MUL by register (0x3035 0x$r3035, 0x3108 0x$r3108): SCLK $SCLK MHz"
[ "$(rd16 0x380C 0x380D)" = "$HTS" ] || { log "ABORT: HTS is not $HTS"; exit 9; }

# calibration: the AEC's own settled state at the reference line, then the manual gain that
# puts P1 near luma 25 (the clip zone below ~15 bends ratios, saturation above ~200 flattens
# them) so that 3.4x lands near 85
sleep 40
for r in 0x3500 0x3501 0x3502 0x350A 0x350B; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
ctl "avgZones=1" > /dev/null; sleep 1; L=$(ramlog)
rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
aecLines=$(( ((16#$(rd 0x3500) & 0x0F) << 16 | 16#$(rd 0x3501) << 8 | 16#$(rd 0x3502)) / 16 ))
aecG16=$(( (16#$(rd 0x350A) & 3) << 8 | 16#$(rd 0x350B) ))   # bash does the hex; python only the float
aecGain=$(python -c "print('%.2f' % ($aecG16 / 16.0))")
[ "$aecG16" -gt 0 ] || { log "ABORT: AEC gain read back 0 - calibration invalid"; exit 9; }
if [ -z "$GAIN" ]; then
  # the sensor's manual gain goes to 63.9x (datasheet 4.6.3 "maximum of 64x"; 0x3FF measured
  # 2x the YAVG of 0x1FF at 15:26) - the 31.94x is only the AEC's configured ceiling
  GAIN=$(python -c "y=max(1,$yavg); g=$aecLines*$aecGain*(25.0/y)/1964; print('%.2f' % min(63.0, max(1.19, g)))")
fi
log "AEC settled: $aecLines lines, ${aecGain}x, YAVG $yavg $st -> manual gain ${GAIN}x (expected P1 luma ~$(python -c "print('%.0f' % (25.0 * $GAIN / max(0.01, $aecLines*$aecGain*(25.0/max(1,$yavg))/1964)))"))"
wr 0x3503 0x03; [ "$(regrd 0x3503)" = "03" ] || { log "ABORT: 0x3503 did not take"; exit 9; }
set_gain "$GAIN"
log "manual: 0x3503 0x03, gain ${GAIN}x, exposure and VTS by register from here"
n=0
for pt in $POINTS; do
  n=$((n + 1)); vts=${pt%%:*}; lines=${pt##*:}
  cur=$(rd16 0x380E 0x380F)
  if [ "$vts" -ge "$cur" ]; then set_vts "$vts"; set_exposure "$lines"; else set_exposure "$lines"; set_vts "$vts"; fi
  sample "P$n" "$vts" "$lines"
done
log "motion checks during the run: $(checks) (must be none)"; af_check
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
