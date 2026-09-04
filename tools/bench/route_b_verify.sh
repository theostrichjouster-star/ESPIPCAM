#!/usr/bin/env bash
# Register verification for the route B ceiling at 1280X960 (plan quirky-wibbling-turtle,
# Phase B; re-run 4 Sep 2026 afternoon for the HTS 2156 / banding-off image). After the image
# is flashed, prove from the board's own registers and pins that
#   - 1280X960 at 41 runs 0x3108=0x11, mul 66, PIXCLK 88.00, HTS 2156, VTS 984, counts 41.48
#     three times with min = max (no 1280-wide stretch), still clean by both gates
#   - the banding filter is OFF (0x3A00[5] clear) with the persisted Exposure Level -2 and
#     banding config 0, at the ceiling and after every retime below it
#   - 1280X960 at 30 runs 0x3108=0x26, PIXCLK 80.00, HTS 2156, VTS 1200 (route A below the ceiling)
#   - 1280X960 at 10 still clean (the low-fps regime, clock walk on route A)
#   - HD after 1280X960 reads 0x3108=0x26, HTS 2060, PIXCLK 80, banding still off: the
#     stale-0x3108 hazard is gone and the filter state survives a set_framesize
#   - 1280X960 again reads 0x11 at 41
# Stills are kept for the user's eyeball. dumpCam is LOG_DIA (SD log only), so the clock
# tree is fetched from /web?log.txt after each dump.
#
#   BOARD=<addr> bash tools/bench/route_b_verify.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/route_b_verify}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
SETTLE=${SETTLE:-12}
CEIL=${CEIL:-41}; HTS=${HTS:-2156}; PRED=${PRED:-41.48}   # 88e6 / (2156 x 984)
VTS30=${VTS30:-1200}; PRED30=${PRED30:-30.92}             # 80e6 / (2156 x 30 x 1.03) -> 1200; 80e6 / (2156 x 1200)
FIELD_SIZE=13; FIELD_FPS=30
FAILS=0

restore() {
  log "== restore field config =="
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record) - $FAILS check(s) failed"
}
trap restore EXIT

regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.8; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }

# the clock tree from the SD log: the LAST dumpCam block
dump_tree() {
  ctl "dumpCam=1" > /dev/null; sleep 3
  curl -s -m 90 "$B/web?log.txt" | grep -a "dumpCamRegs\]" | grep -a "Frame size\|PCLK regs\|Clocks:\|Timing:\|Exposure:" | tail -5 | sed 's/^\[[^]]*\] //'
}

check() {  # check <label> <actual> <expected>
  if [ "$2" = "$3" ]; then log "   ok   $1 = $2"; else log "   FAIL $1 = $2 (expected $3)"; FAILS=$((FAILS + 1)); fi
}

band_check() {  # band_check <label>: 0x3A00[5] must be clear (filter off)
  local v; v=$(regrd 0x3A00)
  check "$1 0x3A00 bit 5 (banding)" "$(( 16#${v:-FF} & 0x20 ))" "0"
}

count3() {  # three VSYNC counts -> "max min"
  local rates="" i line r
  for i in 1 2 3; do
    ctl "xclkStat=1" > /dev/null; sleep 8
    line=$(ramlog | grep -a "VSYNC counted" | tail -1)
    r=$(printf '%s\n' "$line" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/'); rates="$rates ${r:-0}"
  done
  python -c "v=[float(x) for x in '$rates'.split()]; print('%.3f %.3f'%(max(v),min(v)))"
}

still() {  # still <tag> -> the still_color line
  curl -s -m 30 -o "$OUT/$1.jpg" "$B/control?still=1" || { log "ABORT: still fetch failed"; exit 7; }
  python "$HERE/still_color.py" "$OUT/$1.jpg"
}

log "== route B verification on $(status_field idfVer), lwipSndBuf $(status_field lwipSndBuf), ceiling $CEIL at HTS $HTS =="
wait_settled 240
assert_campaign_config 10
preflight
check "persisted ae_level" "$(status_field ae_level)" "-2"
check "persisted banding" "$(status_field banding)" "0"

# 1. the ceiling
set_size 25 10 "$CEIL"; sleep "$SETTLE"
log "1280X960 @$CEIL:"; dump_tree | tee -a "$LOGF"
r08=$(regrd 0x3108); mul=$(regrd 0x3036); h=$(regrd 0x380C)$(regrd 0x380D); v=$(regrd 0x380E)$(regrd 0x380F)
check "0x3108" "$r08" "11"; check "mul" "$((16#$mul))" "66"; check "HTS" "$((16#$h))" "$HTS"; check "VTS" "$((16#$v))" "984"
band_check "@$CEIL"
read -r mx mn <<< "$(count3)"
log "   VSYNC max $mx min $mn (predict $PRED, min = max)"
python -c "import sys; sys.exit(0 if abs($mx-$PRED)<0.2 and ($mx-$mn)<0.1 else 1)" && log "   ok   rate" || { log "   FAIL rate"; FAILS=$((FAILS+1)); }
s42=$(still 1280X960_$CEIL); log "   still @$CEIL: $s42"

# 2. below the ceiling, route A at the new HTS
ctl "fps=30" > /dev/null; sleep "$SETTLE"
log "1280X960 @30:"; dump_tree | tee -a "$LOGF"
r08=$(regrd 0x3108); mul=$(regrd 0x3036); h=$(regrd 0x380C)$(regrd 0x380D); v=$(regrd 0x380E)$(regrd 0x380F)
check "0x3108" "$r08" "26"; check "mul" "$((16#$mul))" "120"; check "HTS" "$((16#$h))" "$HTS"; check "VTS" "$((16#$v))" "$VTS30"
band_check "@30"
read -r mx mn <<< "$(count3)"; log "   VSYNC max $mx min $mn (predict $PRED30 sensor, min = max)"
s30=$(still 1280X960_30); log "   still @30: $s30"

# 3. the low-fps regime
ctl "fps=10" > /dev/null; sleep "$SETTLE"
log "1280X960 @10:"; dump_tree | tee -a "$LOGF"
check "0x3108" "$(regrd 0x3108)" "26"
band_check "@10"
s10=$(still 1280X960_10); log "   still @10: $s10"

# 4. the stale hazard: HD after 1280X960 must be route A at HTS 2060, filter still off
set_size 13 10 30; sleep "$SETTLE"
log "HD @30 after 1280X960:"; dump_tree | tee -a "$LOGF"
r08=$(regrd 0x3108); h=$(regrd 0x380C)$(regrd 0x380D)
check "0x3108" "$r08" "26"; check "HTS" "$((16#$h))" "2060"
band_check "HD"
read -r mx mn <<< "$(count3)"; log "   VSYNC max $mx min $mn (predict 30.92)"

# 5. and back
set_size 25 10 "$CEIL"; sleep "$SETTLE"
check "0x3108 back at $CEIL" "$(regrd 0x3108)" "11"
read -r mx mn <<< "$(count3)"; log "   VSYNC max $mx min $mn (predict $PRED)"

log "== gates: ratio 0.9-1.2, gsat/rsat < 3, hdiff within 1.25x of the @30 still =="
python - "$s42" "$s30" "$s10" "$CEIL" <<'PY'
import sys
rows = [r.split() for r in sys.argv[1:4]]
base = float(rows[1][9])
for tag, r in zip((sys.argv[4], "30", "10"), rows):
    w, h, by, mr, mg, mb, ratio, gs, rs, hd = r[:10]
    ok = 0.9 <= float(ratio) <= 1.2 and float(gs) < 3 and float(rs) < 3 and float(hd) <= max(3.0, 1.25 * base) and w == "1280"
    print("   %s still @%s: ratio %s gsat %s rsat %s hdiff %s vdiff %s bytes %s" % ("ok  " if ok else "FAIL", tag, ratio, gs, rs, hd, r[10], by))
PY
