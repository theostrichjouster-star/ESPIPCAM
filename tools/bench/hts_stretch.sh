#!/usr/bin/env bash
# Exposure beyond the clock floor by stretching the LINE, not the frame (4 Sep 2026).
#
# The two other levers are closed: VTS past 1968 parks the AEC (dim check 2), and SCLK below
# 10 MHz speckles the frame in the sensor's own clock domain (pixclk_floor_walk*, pclk_port_probe).
# What is left is tROW = HTS x lf / SCLK. At full resolution the line already costs 2 x HTS and
# the tuner leaves HTS alone, so HTS is walked here by group write at FHDNARROW, request 1
# (SCLK 10.13, HTS 2200, VTS 1968, sensor 1.17 fps): every rung keeps VTS at 1968, inside the
# AEC's range, and the exposure ceiling is 1964 x 2 x HTS / SCLK.
#
# Prediction stated first: VSYNC = 10.13e6 / (2 x HTS x 1968) at every rung (1.17 at 2200,
# 0.32 at 8000); ceiling 0.85 s at 2200 up to 3.1 s at 8000; the AEC holds the same
# milliseconds in fewer lines while the room is lit; the image stays clean if the sensor
# treats HTS above the readout as blanking at full resolution (binned sizes flip their line
# cost above 2277 - that is why this is full resolution). Frames above ~4 s would trip the
# no-frame rescue at FPS 1, so 8000 is the top rung.
#
# Lens held (af_hold), motion off (campaign config, counter checked), HTS through the group
# write (both bytes at one frame boundary), restore order: HTS 2200 back by group write, then
# the frame size reload, field config, AF continuous.
#   BOARD=<addr> bash tools/bench/hts_stretch.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/hts_stretch_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=${IDX:-16}; Q=10; REQ=1; HTS0=${HTS0:-2200}; LF=${LF:-2}
SETTLE=${SETTLE:-20}
WALK=${WALK:-"2400 2844 3500 4200 5600 7000 8000"}
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/stretch.csv"
[ -f "$CSV" ] || echo "hts,lineUs,predFps,vsync,ceilMs,expLines,expMs,gain,yavg,aec,w,h,bytes,ratio,gsat,rsat,hdiff,vdiff,rescue" > "$CSV"

set_hts_grp() {  # group write first; at ~1 fps the firmware's 1.5 s launch poll misses the
                 # frame boundary and the group never lands (12:32: HTS read back 2200 after
                 # a 2400 write), so a plain pair follows, high byte first when raising and
                 # low byte first when lowering - no transient shorter than either value
  local want=$1 hi lo got
  hi=$(printf '0x%02X' $(( (want >> 8) & 0xFF ))); lo=$(printf '0x%02X' $(( want & 0xFF )))
  ctl "camRegGrp=0x380C,$hi,0x380D,$lo" > /dev/null; sleep 2
  got=$(( 16#$(regrd 0x380C)$(regrd 0x380D) ))
  if [ "$got" != "$want" ]; then
    if [ "$want" -gt "$got" ]; then ctl "camReg=0x380C,$hi" > /dev/null; sleep 0.3; ctl "camReg=0x380D,$lo" > /dev/null
    else ctl "camReg=0x380D,$lo" > /dev/null; sleep 0.3; ctl "camReg=0x380C,$hi" > /dev/null; fi
    sleep 2; got=$(( 16#$(regrd 0x380C)$(regrd 0x380D) ))
    log "   HTS $want by plain pair (group write did not land): reads $got"
  fi
}
restore() {
  log "== restore: HTS $HTS0 (low byte first, lowering), frame size reload, field config, AF continuous =="
  curl -s -m 25 "$B/control?camReg=0x380D,$(printf '0x%02X' $(( HTS0 & 0xFF )))" > /dev/null; sleep 0.3
  curl -s -m 25 "$B/control?camReg=0x380C,$(printf '0x%02X' $(( (HTS0 >> 8) & 0xFF )))" > /dev/null; sleep 2
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  af_resume
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT
checks() { ctl motionStats=1 > /dev/null; sleep 1; ramlog | grep -a "dumpMotionStats" | grep -a -o "[0-9]* checks\|No checks recorded" | tail -1; }
BASE_HDIFF=""

sample() {  # sample <hts>
  local hts=$1 pred lineUs ceil line vs tag stats rescue
  read -r lineUs pred ceil <<< "$(python -c "tl=$hts*$LF/10.13; print('%.1f %.3f %.0f'%(tl, 1e6/(tl*1968), 1964*tl/1000))")"
  sleep "$SETTLE"; assert_alive
  ctl "xclkStat=1" > /dev/null; sleep 14
  line=$(ramlog | grep -a "VSYNC counted\|VSYNC count failed" | tail -1)
  vs=$(printf '%s\n' "$line" | sed -n 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/p'); [ -n "$vs" ] || vs=0
  for r in 0x3500 0x3501 0x3502 0x350A 0x350B 0x380C 0x380D; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
  read -r gotHts expLines expMs gain <<< "$(python - "$LF" "$(rd 0x380C)" "$(rd 0x380D)" "$(rd 0x3500)" "$(rd 0x3501)" "$(rd 0x3502)" "$(rd 0x350A)" "$(rd 0x350B)" <<'PY'
import sys
lf = int(sys.argv[1]); h0, h1, e0, e1, e2, g0, g1 = [int(x, 16) for x in sys.argv[2:9]]
hts = (h0 << 8) | h1; tl = hts * lf / 10.13
lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
print("%d %d %.1f %.2f" % (hts, lines, lines * tl / 1000.0, (((g0 & 3) << 8) | g1) / 16.0))
PY
)"
  tag="hts${hts}"
  curl -s -m 90 -o "$OUT/$tag.jpg" "$B/control?still=1" || log "   still fetch failed at $tag"
  stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  [ -n "$BASE_HDIFF" ] || BASE_HDIFF=$hdiff
  rescue=$(printf '%s\n' "$L" | grep -a -c "No frames for")
  log "   HTS $gotHts (line ${lineUs}us): VSYNC $vs (pred $pred) | ceiling ${ceil}ms, exposure $expLines lines = ${expMs}ms, gain ${gain}x, YAVG $yavg $st | still ${w}x${h} ${bytes}B ratio $ratio gsat $gsat rsat $rsat hdiff $hdiff vdiff $vdiff (base $BASE_HDIFF) | rescues $rescue"
  echo "$gotHts,$lineUs,$pred,$vs,$ceil,$expLines,$expMs,$gain,$yavg,$st,$w,$h,$bytes,$ratio,$gsat,$rsat,$hdiff,$vdiff,$rescue" >> "$CSV"
}

log "== HTS stretch, size idx $IDX request $REQ, lit: llevel=$(status_field llevel) =="
assert_campaign_config "$Q"
preflight
af_hold "$IDX" "$Q" "$REQ"
log "motion: $(checks) (counter reset)"
sleep 3
log "tuner: $(ramlog | grep -a "Tuned timing" | grep -a "request $REQ," | tail -1 | sed 's/^\[[^]]*\] //')"
log "-- baseline HTS $HTS0 --"
sample "$HTS0"
for hw in $WALK; do   # hw, not h: sample() reads the still's height into h (bash dynamic scope)
  set_hts_grp "$hw"
  sample "$hw"
  [ "$w" = "0" ] && { log "   no still at HTS $hw - stopping the walk here"; break; }
done
log "motion checks during the walk: $(checks) (must be none)"; af_check
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
