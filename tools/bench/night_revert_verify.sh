#!/usr/bin/env bash
# Verify the Long Exposure panel back at its 3.18 s ceiling, and separately that a board which has
# stopped delivering frames can now be recovered WITHOUT a reboot.
#
# The second half is the one worth having. settleSensor() runs at the end of processFrame(), and
# processFrame() returned early whenever esp_camera_fb_get() handed back NULL - so a state that
# stopped frame delivery could not be retimed OR left. Measured 7 Sep 2026: a 4.5 s session ignored
# a shorter exposure, a frame size change and its own exit, and sat on a 4.49 s frame while /status
# reported HD 30 until a reboot. Both no-frame paths now service the retime.
#
#   BOARD=<addr> bash tools/bench/night_revert_verify.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/night_revert_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=${IDX:-23}
FAIL=0
ok()   { log "  PASS  $1"; }
bad()  { log "  FAIL  $1"; FAIL=$((FAIL + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1: $2"; else bad "$1: got $2, wanted $3"; fi; }
near() { if python -c "import sys; sys.exit(0 if abs($2 - $3) <= $4 else 1)"; then ok "$1: $2 (wanted ~$3)"
         else bad "$1: got $2, wanted $3 +/- $4"; fi; }
jf() { printf '%s' "$1" | python -c "import json,sys; print(json.load(sys.stdin).get('$2',''))" 2>/dev/null; }
vtsNow() { echo $(( 16#$(regrd 0x380E)$(regrd 0x380F) )); }
htsNow() { echo $(( 16#$(regrd 0x380C)$(regrd 0x380D) )); }

S0_SIZE=$(status_field framesize); S0_FPS=$(status_field fps)
S0_IDLE=$(status_field idleFps); S0_MIC=$(status_field micGain); S0_Q=$(status_field quality)
log "restore point: framesize=$S0_SIZE fps=$S0_FPS idleFps=$S0_IDLE micGain=$S0_MIC quality=$S0_Q"
restore() {
  log "== restore =="
  curl -s -m 25 "$B/control?nightExp=$IDX,0" > /dev/null; sleep 5
  curl -s -m 12 "$B/sustain?stream=0" -o /dev/null 2>/dev/null || true
  for k in framesize=$S0_SIZE fps=$S0_FPS idleFps=$S0_IDLE micGain=$S0_MIC quality=$S0_Q; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.4
  done
  sleep 4; curl -s -m 12 "$B/sustain?stream=0" -o /dev/null 2>/dev/null || true; sleep 2
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) idleFps=$(status_field idleFps) 0x3503=0x$(regrd 0x3503) VTS=$(vtsNow)"
}
trap restore EXIT

log "== PART 1: the panel back at 3.18 s =="
R=$(ctl "nightExp=$IDX,3180"); sleep 12
log "nightExp reply: $R"
near "range max handed to the panel" "$(jf "$R" maxMs)" 3180 1
near "frame achieved" "$(jf "$R" gotMs)" 3180 40
check "session on" "$(jf "$R" night)" 1
check "the AEC still owns the exposure" "$(regrd 0x3503)" 00
near "VTS at the engine cap - no dummy lines" "$(vtsNow)" 1968 2
near "line at the 13 bit register end" "$(htsNow)" 8187 6
# the frame a user would actually get, through the page's own path
GOT=0
for i in 1 2 3 4; do
  if curl -s -m 60 -o "$OUT/night318.jpg" "$B/control?still=1" && [ -s "$OUT/night318.jpg" ]; then GOT=$i; break; fi
  sleep 4
done
if [ "$GOT" -gt 0 ]; then
  read -r w h bytes mr mg mb ratio gs rs hd vd <<< "$(python "$HERE/still_color.py" "$OUT/night318.jpg")"
  L=$(python -c "print('%.1f' % (0.299*$mr + 0.587*$mg + 0.114*$mb))")
  ok "still arrived on request $GOT: ${w}x${h} ${bytes}B luma $L hdiff $hd"
  check "full 5MP" "${w}x${h}" "2560x1920"
else bad "no still in 4 requests at 3.18 s - this is the shipped path"; fi

log "-- a request past the ceiling is clamped, not refused --"
R=$(ctl "nightExp=$IDX,5000"); sleep 12
check "clamped to the ceiling" "$(jf "$R" reqMs)" 3180
near "frame still 3.18 s" "$(jf "$R" gotMs)" 3180 40
check "no manual exposure engaged" "$(regrd 0x3503)" 00

log "-- session off --"
R=$(ctl "nightExp=$IDX,0"); sleep 10
check "session off" "$(jf "$R" night)" 0
check "size restored" "$(status_field framesize)" "$S0_SIZE"
check "rate restored" "$(status_field fps)" "$S0_FPS"

log "== PART 2: recovery from a no-frame state, with no reboot =="
# Force one by hand: a frame far longer than anything that delivers. At HD idle the line is ~100us,
# so VTS 65535 is a ~6.6 s frame - well past the 4.0-4.5 s delivery cliff
V0=$(vtsNow); log "VTS before: $V0, framesize $(status_field framesize)"
ctl "camRegGrp=0x380E,0xFF,0x380F,0xFF" > /dev/null; sleep 3
VSTUCK=$(vtsNow)
if [ "$VSTUCK" = "65535" ]; then ok "forced VTS 65535 - a ~6.6 s frame, past the delivery cliff"
else bad "could not force the stuck state: VTS reads $VSTUCK"; fi
sleep 20
# no frames should be arriving now. The rescue's WARN is the witness
NOFRAME=$(ramlog | grep -a -c "No frames for")
if [ "$NOFRAME" -gt 0 ]; then ok "board is in a no-frame state ($NOFRAME rescue lines)"
else log "  NOTE  no rescue lines yet - the state may not have taken hold"; fi
# THE TEST: ask for a different frame size and see whether it lands. Before the fix this was
# ignored, because the only code that services it sat below processFrame's early return
log "asking for VGA (10) while no frames are arriving"
ctl "framesize=10" > /dev/null
RECOVERED=0
for i in $(seq 1 12); do
  sleep 5
  if [ "$(status_field framesize)" = "10" ] && [ "$(vtsNow)" != "65535" ]; then RECOVERED=$((i * 5)); break; fi
done
if [ "$RECOVERED" -gt 0 ]; then ok "recovered in ~${RECOVERED}s WITHOUT a reboot: framesize 10, VTS $(vtsNow)"
else bad "still stuck after 60 s - VTS $(vtsNow), framesize $(status_field framesize)"; fi

log "== $( [ "$FAIL" -eq 0 ] && echo "ALL CHECKS PASSED" || echo "$FAIL CHECK(S) FAILED" ) =="
exit $(( FAIL > 0 ))
