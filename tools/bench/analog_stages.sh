#!/usr/bin/env bash
# Stills only: which of the reference binned-mode analog values scrambles the colour, and does
# a one-column ISP offset put it back? (4 Sep 2026, follow-up to analog_probe.sh run 1, where
# 0x3709=0x52 + 0x370C=0x03 at VGA / HTS 2060 turned yellow to magenta and blue to green with
# the geometry unchanged - a colour-phase change, not a tint.)
#
# Each stage: group-write the registers, settle, one still, channel stats, then put every
# register back to the value read at the start. VGA at 39 (HTS 2060, SCLK 80).
#   BOARD=<addr> bash tools/bench/analog_stages.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/analog}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
SETTLE=${SETTLE:-12}
FIELD_SIZE=13; FIELD_FPS=30
STAGES=${STAGES:-"r09:0x3709,0x52 r0C:0x370C,0x03 both:0x3709,0x52,0x370C,0x03 both_off17:0x3709,0x52,0x370C,0x03,0x3811,0x11 both_off15:0x3709,0x52,0x370C,0x03,0x3811,0x0F both_off18:0x3709,0x52,0x370C,0x03,0x3811,0x12"}
BACK=""

restore() {
  log "== restore: $BACK, field config =="
  [ -n "$BACK" ] && curl -s -m 25 "$B/control?camRegGrp=$BACK" > /dev/null; sleep 1
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT
regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }

still_stats() {  # still_stats <tag>
  curl -s -m 30 -o "$OUT/$1.jpg" "$B/control?still=1" || { log "ABORT: still fetch failed"; exit 7; }
  python "$HERE/still_color.py" "$OUT/$1.jpg"
}

log "== analog stages, VGA =="
assert_campaign_config 10
preflight
set_size 10 10 39; sleep 4
BACK="0x3708,0x$(regrd 0x3708),0x3709,0x$(regrd 0x3709),0x370C,0x$(regrd 0x370C),0x3810,0x$(regrd 0x3810),0x3811,0x$(regrd 0x3811)"
log "start values: $BACK"
sleep "$SETTLE"
log "   baseline: $(still_stats VGA_stage_baseline)"
for st in $STAGES; do
  name=${st%%:*}; regs=${st#*:}
  ctl "camRegGrp=$regs" > /dev/null; sleep "$SETTLE"
  log "   $name ($regs): $(still_stats VGA_stage_$name)"
  ctl "camRegGrp=$BACK" > /dev/null; sleep 3
done
log "== done =="
