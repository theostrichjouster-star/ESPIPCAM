#!/usr/bin/env bash
# Is the sub-10 MHz speckle the sensor's row time or the DVP port path? (4 Sep 2026, after
# pixclk_floor_walk.sh: at SCLK 8.44 MHz the scene is sharp and correctly coloured under a dense
# speckle that triples the JPEG size; at 6.33 the frame is pink columns with no scene.)
#
# The DVP port clock is pll_clki / 4 (0x3108[5:4]) / pclk_div (0x3824), set with the PLL by
# camPll's last argument; SCLK is pll_clki / 4 (0x3108[1:0]) / sys_div. So the two move
# independently. 0x4417[0] is the JPEG FIFO overflow flag (the dump reads it for exactly this
# question: clock too slow for the encoder, or the encoder's FIFO overrun).
#
# Points, each with 0x4417, VSYNC, exposure/gain, a still through the gates:
#   1. SCLK 10.13 / PCLK 2.53  (mul 76, sys_div 5, pclk_div 4) - the clean floor combo
#   2. SCLK 10.13 / PCLK 1.27  (pclk_div 8)   - port clock halved at a clean SCLK
#   3. SCLK  8.44 / PCLK 2.11  (sys_div 6, pclk_div 4) - the speckle rung as walked
#   4. SCLK  8.44 / PCLK 4.22  (pclk_div 2)   - same SCLK, port clock doubled
#   5. SCLK  8.44 / PCLK 8.44  (pclk_div 1)
#   6. SCLK  6.33 / PCLK 6.33  (sys_div 8, pclk_div 1) - the dead rung with a fast port
# Prediction stated first: if the speckle is the port path, 2 speckles and 4/5 are clean;
# if it is the row time, 2 is clean and 3/4/5 speckle alike. AF released, motion off.
#   BOARD=<addr> bash tools/bench/pclk_port_probe.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/pclk_port_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=25; Q=10; REQ=1
SETTLE=${SETTLE:-15}
POINTS=${POINTS:-"5:4 5:8 6:4 6:2 6:1 8:1"}   # sys_div:pclk_div at mul 76
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/probe.csv"
[ -f "$CSV" ] || echo "sysDiv,pclkDiv,sclkMHz,pclkMHz,r4417,predFps,vsync,expLines,expMs,gain,yavg,w,h,bytes,ratio,gsat,rsat,hdiff,vdiff" > "$CSV"

restore() {
  log "== restore: PLL floor combo (pclk_div 4), AF continuous, frame size reload, field config =="
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 0.5
  curl -s -m 25 "$B/control?camPll=0,76,5,0,3,2,1,4" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  af_resume
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record) AF 0x3029=$(regrd 0x3029)"
}
trap restore EXIT
regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
checks() { ctl motionStats=1 > /dev/null; sleep 1; ramlog | grep -a "dumpMotionStats" | grep -a -o "[0-9]* checks\|No checks recorded" | tail -1; }

log "== DVP port clock probe, 1280X960 request $REQ, lit: llevel=$(status_field llevel) =="
assert_campaign_config "$Q"
preflight
# hold the lens: af_hold (bench_lib.sh) focuses at FHD, pauses, returns here with the VCM read
af_hold "$IDX" "$Q" 30
log "motion: $(checks) (reset)"
ctl "fps=$REQ" > /dev/null; sleep 4
for pt in $POINTS; do
  sd=${pt%%:*}; pd=${pt##*:}
  # pll_clki already carries sys_div; SCLK = pll_clki / 4 and PCLK = pll_clki / 4 / pclk_div,
  # so PCLK = SCLK / pclk_div (the first run's label dropped sys_div, 5x too high; the
  # register writes were right)
  sclk=$(python -c "print('%.2f'%(76*2/3/$sd))"); pclk=$(python -c "print('%.2f'%(76*2/3/$sd/$pd))")
  ctl "camPll=0,76,$sd,0,3,2,1,$pd" > /dev/null; sleep 2
  pred=$(python -c "print('%.3f'%($sclk*1e6/(2156*1968)))")
  sleep "$SETTLE"; assert_alive
  ctl "xclkStat=1" > /dev/null; sleep 12
  vs=$(ramlog | grep -a "VSYNC counted\|VSYNC count failed" | tail -1 | sed -n 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/p'); [ -n "$vs" ] || vs=0
  r4417=$(regrd 0x4417)
  for r in 0x3500 0x3501 0x3502 0x350A 0x350B; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
  read -r expLines expMs gain <<< "$(python - "$sclk" "$(rd 0x3500)" "$(rd 0x3501)" "$(rd 0x3502)" "$(rd 0x350A)" "$(rd 0x350B)" <<'PY'
import sys
sclk = float(sys.argv[1]); e0, e1, e2, g0, g1 = [int(x, 16) for x in sys.argv[2:7]]
lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
print("%d %.1f %.2f" % (lines, lines * 2156.0 / sclk / 1000.0, (((g0 & 3) << 8) | g1) / 16.0))
PY
)"
  tag="sclk${sclk}_pclk${pclk}"
  curl -s -m 60 -o "$OUT/$tag.jpg" "$B/control?still=1" || log "   still fetch failed at $tag"
  stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  log "   SCLK $sclk / PCLK $pclk (sd $sd, pclk_div $pd): 0x4417=0x$r4417 | VSYNC $vs (pred $pred) | exposure $expLines lines = ${expMs}ms, gain ${gain}x, YAVG $yavg $st | still ${w}x${h} ${bytes}B ratio $ratio gsat $gsat rsat $rsat hdiff $hdiff vdiff $vdiff"
  echo "$sd,$pd,$sclk,$pclk,$r4417,$pred,$vs,$expLines,$expMs,$gain,$yavg,$w,$h,$bytes,$ratio,$gsat,$rsat,$hdiff,$vdiff" >> "$CSV"
done
log "motion checks during the probe: $(checks) (must be none) | AF: status 0x$(regrd 0x3029)"; af_check
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
