#!/usr/bin/env bash
# PIXCLK below the 10 MHz floor at 1280X960, request 1 fps (4 Sep 2026): how low can SCLK go
# before the image breaks, and is the break the ROW TIME or the DIVIDER CHAIN?
#
# The 28 Aug walk (BOARD_TESTING, clock-tuned scaler sizes) set the floor on VGA through the
# system divider alone: 10.13 clean, 8.10 grainy with a 2.5% rate sag, 6.07 a solid
# false-colour frame. Two things changed together there. So two routes to the same clocks:
#   A. system divider 0x3035[7:4] at mul 76 with the /4 SCLK root (0x3108 = 0x26)
#   B. the /8 SCLK root: 0x3108 = 0x2B halves SCLK, SCLK2x and PCLK together (the mirror of
#      route B's 0x11, which doubled them), at a LOW system divider
# Equal image at equal SCLK -> the row time is the limit. B clean where A is grainy -> the
# divider chain is. Per rung: registers read back, VSYNC edge-aligned (the 10 s cap holds 5+
# edges down to ~0.5 fps), the AEC's exposure and gain, a still through still_color.py.
# Exposure ceiling at each rung = 1964 x HTS / SCLK (418 ms at 10.13, 1.0 s at 4.22).
#
# Prediction stated first: VSYNC = SCLK / (2156 x 1968) at every rung to 1%; the AEC holds
# the same milliseconds (fewer lines) at the 1.19x floor while the scene is lit; the image
# degrades somewhere below 8 MHz on route A as before, and route B decides why.
#
# Restore order on every exit: 0x3108 to 0x26 FIRST, then the PLL to the 10.13 floor combo,
# then a frame size reload and the field config.
#   BOARD=<addr> bash tools/bench/pixclk_floor_walk.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/pixclk_floor_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=25; Q=10; REQ=${REQ:-1}
SETTLE=${SETTLE:-18}
WALK_A=${WALK_A:-"6 7 8 9 10 12 15"}      # sys_div at mul 76, 0x3108 0x26: 8.44 7.24 6.33 5.63 5.07 4.22 3.38 MHz
WALK_B=${WALK_B:-"5 6 8 10"}              # sys_div at mul 76, 0x3108 0x2B: 5.07 4.22 3.17 2.53 MHz
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/walk.csv"
[ -f "$CSV" ] || echo "route,sysDiv,r3108,mul,sclkMHz,predFps,vsync,expLines,expMs,gain,maxExpMs,yavg,aec,w,h,bytes,ratio,gsat,rsat,hdiff,vdiff,rescue" > "$CSV"

restore() {
  log "== restore: 0x3108 0x26 FIRST, PLL 10.13 floor combo, frame size reload, field config =="
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 0.5
  curl -s -m 25 "$B/control?camPll=0,76,5,0,3,2,1,4" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT

regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
BASE_HDIFF=""

sample() {  # sample <route> <sysDiv> <sclk MHz>
  local route=$1 sd=$2 sclk=$3 pred r08 mul line vs tag stats rescue
  pred=$(python -c "print('%.3f'%($sclk*1e6/(2156*1968)))")
  sleep "$SETTLE"; assert_alive
  r08=$(regrd 0x3108); mul=$((16#$(regrd 0x3036)))
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
tl = 2156.0 / sclk  # us
lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
print("%d %.1f %.2f %.0f" % (lines, lines * tl / 1000.0, (((g0 & 3) << 8) | g1) / 16.0, 1964 * tl / 1000.0))
PY
)"
  tag="${route}_sd${sd}_${sclk}MHz"
  curl -s -m 60 -o "$OUT/$tag.jpg" "$B/control?still=1" || { log "   still fetch failed at $tag"; }
  stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  [ -n "$BASE_HDIFF" ] || BASE_HDIFF=$hdiff
  rescue=$(printf '%s\n' "$L" | grep -a -c "No frames for")
  log "   $tag: 0x3108=$r08 mul $mul | VSYNC $vs (pred $pred) | exposure $expLines lines = ${expMs}ms (ceiling ${maxExp}ms), gain ${gain}x, YAVG $yavg $st | still ${w}x${h} ${bytes}B ratio $ratio gsat $gsat rsat $rsat hdiff $hdiff vdiff $vdiff (base $BASE_HDIFF) | rescues $rescue"
  echo "$route,$sd,$r08,$mul,$sclk,$pred,$vs,$expLines,$expMs,$gain,$maxExp,$yavg,$st,$w,$h,$bytes,$ratio,$gsat,$rsat,$hdiff,$vdiff,$rescue" >> "$CSV"
}

log "== PIXCLK floor walk, 1280X960 request $REQ, lit: llevel=$(status_field llevel) =="
assert_campaign_config "$Q"
preflight
set_size "$IDX" "$Q" "$REQ"; sleep 4
log "tuner: $(ramlog | grep -a "Tuned timing 1280X960" | grep -a "request $REQ," | tail -1 | sed 's/^\[[^]]*\] //')"
log "-- baseline, the 10.13 MHz floor combo --"
sample A 5 10.13
log "-- route A: system divider at the /4 root --"
for sd in $WALK_A; do
  sclk=$(python -c "print('%.2f'%(76*2/3/$sd))")
  ctl "camPll=0,76,$sd,0,3,2,1,4" > /dev/null; sleep 2
  sample A "$sd" "$sclk"
done
log "-- back to the floor combo, then route B: the /8 SCLK root (0x3108 0x2B) --"
ctl "camPll=0,76,5,0,3,2,1,4" > /dev/null; sleep 2
ctl "camReg=0x3108,0x2B" > /dev/null; sleep 1
log "   0x3108 now $(regrd 0x3108) (want 2B)"
for sd in $WALK_B; do
  sclk=$(python -c "print('%.2f'%(76*2/3/$sd/2))")
  # set_pll rewrites 0x3108 to 0x26: the /4 root comes back with the PLL write (a SLOWER
  # transient than /8 at the new divider is impossible here, both are below 10 MHz), so the
  # PLL is written first and the /8 root re-applied after, read back
  ctl "camPll=0,76,$sd,0,3,2,1,4" > /dev/null; sleep 1
  ctl "camReg=0x3108,0x2B" > /dev/null; sleep 1
  sample B "$sd" "$sclk"
done
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
