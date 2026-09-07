#!/usr/bin/env bash
# Verify the Long Exposure panel's 5 s ceiling end to end, through the SAME control the page sends.
#
# What it proves, in the order the page would exercise it:
#   1 the range the panel is handed reaches 5000 ms
#   2 a 5 s request lands: HTS at the register's end, the rest of the frame in DUMMY LINES, the
#     exposure written manually (0x3503 = 0x01, AGC left auto) and every register read back
#   3 a still ARRIVES - stillWaitMs scales to the frame, so the path a user actually taps works
#   4 a request BELOW the 3.18 s handover goes back to the AEC owning the exposure (0x3503 0x00)
#   5 a request ABOVE 5 s is clamped, not refused
#   6 turning the session off hands the exposure, the size and the rate all back
#
#   BOARD=<addr> bash tools/bench/night5s_verify.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/night5s_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=${IDX:-23}   # QSXGA - the size the panel offers, and the one the 5 s measurement was made at
FAIL=0
ok()   { log "  PASS  $1"; }
bad()  { log "  FAIL  $1"; FAIL=$((FAIL + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1: $2"; else bad "$1: got $2, wanted $3"; fi; }
near() { # near <what> <got> <want> <tolerance>
  if python -c "import sys; sys.exit(0 if abs($2 - $3) <= $4 else 1)"; then ok "$1: $2 (wanted ~$3)"
  else bad "$1: got $2, wanted $3 +/- $4"; fi
}
jf() { printf '%s' "$1" | python -c "import json,sys; print(json.load(sys.stdin).get('$2',''))" 2>/dev/null; }
expLines() { echo $(( ((16#$(regrd 0x3500) & 0x0F) << 16 | 16#$(regrd 0x3501) << 8 | 16#$(regrd 0x3502)) / 16 )); }
vtsNow() { echo $(( 16#$(regrd 0x380E)$(regrd 0x380F) )); }
htsNow() { echo $(( 16#$(regrd 0x380C)$(regrd 0x380D) )); }

S0_SIZE=$(status_field framesize); S0_FPS=$(status_field fps); S0_IDLE=$(status_field idleFps)
S0_MIC=$(status_field micGain); S0_Q=$(status_field quality)
log "restore point: framesize=$S0_SIZE fps=$S0_FPS idleFps=$S0_IDLE micGain=$S0_MIC quality=$S0_Q"
restore() {
  log "== restore =="
  curl -s -m 25 "$B/control?nightExp=$IDX,0" > /dev/null; sleep 6
  for k in framesize=$S0_SIZE fps=$S0_FPS idleFps=$S0_IDLE micGain=$S0_MIC quality=$S0_Q; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.4
  done
  sleep 3
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) idleFps=$(status_field idleFps) micGain=$(status_field micGain) 0x3503=0x$(regrd 0x3503)"
}
trap restore EXIT

log "== Long Exposure 5 s verification, size idx $IDX =="

# 1 + 2: the 5 s request, and the range that comes back with it
R=$(ctl "nightExp=$IDX,5000"); sleep 14   # two retimes plus the settle
log "nightExp reply: $R"
near "range max reported to the panel" "$(jf "$R" maxMs)" 5000 1
near "frame achieved (gotMs)" "$(jf "$R" gotMs)" 5000 60
check "requested ms echoed" "$(jf "$R" reqMs)" 5000
check "session on" "$(jf "$R" night)" 1
H=$(htsNow); V=$(vtsNow); E=$(expLines); M=$(regrd 0x3503)
check "HTS at the 13 bit register end" "$H" 8191
near "VTS grown by dummy lines" "$V" 3093 6
near "exposure lines" "$E" 3089 6
check "0x3503 - AEC manual, AGC auto" "$M" 01
# the arithmetic the whole feature rests on, checked against the registers rather than the reply
LINE_US=$(python -c "print('%.2f' % (8191 * 2 / 10.1333))")
near "exposure from the registers (ms)" "$(python -c "print('%.0f' % ($E * $LINE_US / 1000))")" 5000 60
near "dummy lines (VTS minus the 1952 rows read)" "$(( V - 1952 ))" 1141 6

# 3: a still, through the path a user taps. stillWaitMs is 2 x the frame + 500, so ~10.5 s here
log "-- still through /control?still=1 (the page's own path) --"
GOT=0
for i in 1 2 3; do
  if curl -s -m 60 -o "$OUT/night5s.jpg" "$B/control?still=1" && [ -s "$OUT/night5s.jpg" ]; then GOT=$i; break; fi
  sleep 6
done
if [ "$GOT" -gt 0 ]; then
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$(python "$HERE/still_color.py" "$OUT/night5s.jpg")"
  L=$(python -c "print('%.1f' % (0.299*$mr + 0.587*$mg + 0.114*$mb))")
  ok "still arrived on request $GOT: ${w}x${h} ${bytes}B luma $L ratio $ratio hdiff $hdiff vdiff $vdiff"
  check "still is full 5MP" "${w}x${h}" "2560x1920"
  if python -c "import sys; sys.exit(0 if 3 < $L < 250 else 1)"; then ok "luma $L is inside the usable range"
  else bad "luma $L is black or blown"; fi
  if python -c "import sys; sys.exit(0 if $hdiff > 0.2 else 1)"; then ok "hdiff $hdiff - the frame has real detail"
  else bad "hdiff $hdiff - flat frame, no scene content"; fi
else bad "no still in 3 requests at a 5 s frame - stillWaitMs is meant to be ~10.5 s here"; fi

# 4: below the handover, the AEC takes the exposure back
log "-- 2500 ms: below the 3.18 s handover --"
R=$(ctl "nightExp=$IDX,2500"); sleep 14
near "frame achieved" "$(jf "$R" gotMs)" 2500 60
check "0x3503 back to auto" "$(regrd 0x3503)" 00
near "VTS back at the engine cap" "$(vtsNow)" 1968 2
if [ "$(htsNow)" -lt 8191 ]; then ok "HTS $(htsNow) - the line carries it again"; else bad "HTS still 8191"; fi

# 5: past the ceiling, clamped rather than refused
log "-- 8000 ms: past the ceiling --"
R=$(ctl "nightExp=$IDX,8000"); sleep 14
check "clamped request" "$(jf "$R" reqMs)" 5000
near "frame achieved" "$(jf "$R" gotMs)" 5000 60
check "still manual" "$(regrd 0x3503)" 01

# 6: off
log "-- session off --"
R=$(ctl "nightExp=$IDX,0"); sleep 12
check "session off" "$(jf "$R" night)" 0
check "0x3503 handed back" "$(regrd 0x3503)" 00
check "size restored" "$(status_field framesize)" "$S0_SIZE"
check "rate restored" "$(status_field fps)" "$S0_FPS"

log "== $( [ "$FAIL" -eq 0 ] && echo "ALL CHECKS PASSED" || echo "$FAIL CHECK(S) FAILED" ) =="
exit $(( FAIL > 0 ))
