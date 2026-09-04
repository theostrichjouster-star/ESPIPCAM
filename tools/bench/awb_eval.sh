#!/usr/bin/env bash
# Does the AWB work during long exposures? (4 Sep 2026, after the manual stills came out magenta)
#
# The OV5640's white balance is the ISP's AWB block writing 12-bit R / G / B gains into
# 0x3400-0x3405 (0x400 = 1x) once per frame; 0x3406[0] makes them manual. The question is
# whether those gains converge and hold at 1 fps and below the way they do at a normal rate,
# and whether the picture's neutral target comes out neutral. Everything at QSXGA so the framing
# is the same at every stage; the lens held at a lit DAC code; mic and motion off; one lamp,
# unchanged (the user holds it).
#
# Stages, the AWB gains sampled every 10 s through each, a still at the end:
#   A  fps 5, AEC auto      the "normal rate" in this room (dark: the frame may be black - then
#                           the AWB has nothing to work on and this stage shows how it idles)
#   B  fps 1, HTS 2844      the AEC's 986 ms at the new 63.9x ceiling: a lit frame
#   C  HTS 5600 (0.51 fps)  the stretch, ~1.9 s
#   D  HTS 8191 (0.35 fps)  the register's end, 2.8 s
#   E  manual exposure 3932 lines in a 3936 frame at HTS 2844, gain 8x through the driver
#                           (unsaturated) - the AWB with the AEC off
# Metrics: the gains (converged = the same value at the last three samples), the still's
# channel means over the star-chart box and over the whole frame as R/G and B/G (1.00 = neutral;
# a working AWB puts the chart near 1.00 at every stage; a stalled one leaves the lamp's cast).
# Prediction stated first in the run's prediction.txt.
#   BOARD=<addr> bash tools/bench/awb_eval.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/awb_eval_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=${IDX:-23}; Q=${Q:-28}
BOX=${BOX:-"700 650 1400 1300"}    # the star chart in the QSXGA frame (x0 y0 x1 y1)
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/awb.csv"
[ -f "$CSV" ] || echo "stage,t,awbR,awbG,awbB,r3406,expLines,gain16,yavg,aec" > "$CSV"
SCSV="$OUT/stills.csv"
[ -f "$SCSV" ] || echo "stage,bytes,frameRG,frameBG,boxR,boxG,boxB,boxRG,boxBG,luma,hdiff,tries" > "$SCSV"
hexb() { printf '0x%02X' "$1"; }
wr() { ctl "camReg=$1,$(hexb "$2")" > /dev/null; sleep 0.4; }
set_hts() {  # plain pair, high byte first when raising, low first when lowering
  local want=$1 hi lo cur; hi=$(( (want >> 8) & 0xFF )); lo=$(( want & 0xFF ))
  cur=$(( 16#$(regrd 0x380C)$(regrd 0x380D) ))
  if [ "$want" -gt "$cur" ]; then wr 0x380C $hi; wr 0x380D $lo; else wr 0x380D $lo; wr 0x380C $hi; fi
  sleep 2; local got=$(( 16#$(regrd 0x380C)$(regrd 0x380D) ))
  [ "$got" = "$want" ] || { log "ABORT: HTS $want reads back $got"; exit 9; }
}
restore() {
  log "== restore: AEC auto, frame size reload, field config, AF continuous =="
  wr 0x3503 0x00
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  af_resume
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record) 0x3503=0x$(regrd 0x3503) 0x3406=0x$(regrd 0x3406)"
}
trap restore EXIT
checks() { ctl motionStats=1 > /dev/null; sleep 1; ramlog | grep -a "dumpMotionStats" | grep -a -o "[0-9]* checks\|No checks recorded" | tail -1; }

awb_sample() {  # awb_sample <stage> <t>
  local L
  for r in 0x3400 0x3401 0x3402 0x3403 0x3404 0x3405 0x3406 0x3500 0x3501 0x3502 0x350A 0x350B; do ctl "camRegRd=$r" > /dev/null; sleep 0.4; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
  local R=$(( (16#$(rd 0x3400) & 0x0F) << 8 | 16#$(rd 0x3401) )) G=$(( (16#$(rd 0x3402) & 0x0F) << 8 | 16#$(rd 0x3403) )) Bv=$(( (16#$(rd 0x3404) & 0x0F) << 8 | 16#$(rd 0x3405) ))
  local lines=$(( ((16#$(rd 0x3500) & 0x0F) << 16 | 16#$(rd 0x3501) << 8 | 16#$(rd 0x3502)) / 16 )) g16=$(( (16#$(rd 0x350A) & 3) << 8 | 16#$(rd 0x350B) ))
  log "   $1 t=${2}s: AWB R $R G $G B $Bv (x/1024: $(python -c "print('%.3f %.3f %.3f' % ($R/1024.0, $G/1024.0, $Bv/1024.0))")) 0x3406 0x$(rd 0x3406) | exposure $lines lines, gain $(python -c "print('%.2f' % ($g16/16.0))")x, YAVG $yavg $st"
  echo "$1,$2,$R,$G,$Bv,$(rd 0x3406),$lines,$g16,$yavg,$st" >> "$CSV"
}
stage_still() {  # stage_still <stage> <pred fps>
  local tag=$1 pred=$2 tries=0 retryGap
  retryGap=$(python -c "print('%.2f' % max(0.2, 7.0 / (6 * $pred) - 1.25))")
  while [ "$tries" -lt 6 ]; do
    tries=$((tries + 1))
    curl -s -m 90 -o "$OUT/$tag.jpg" "$B/control?still=1" && [ -s "$OUT/$tag.jpg" ] && break
    sleep "$retryGap"
  done
  [ -s "$OUT/$tag.jpg" ] || { log "   no still at $tag after $tries requests"; echo "$tag,0,-,-,-,-,-,-,-,-,-,$tries" >> "$SCSV"; return; }
  local m; m=$(python - "$OUT/$tag.jpg" $BOX <<'PY'
import sys
from PIL import Image, ImageStat
im = Image.open(sys.argv[1]).convert('RGB')
x0, y0, x1, y1 = [int(v) for v in sys.argv[2:6]]
f = ImageStat.Stat(im).mean
b = ImageStat.Stat(im.crop((x0, y0, x1, y1))).mean
luma = 0.299*f[0] + 0.587*f[1] + 0.114*f[2]
print("%.3f %.3f %.1f %.1f %.1f %.3f %.3f %.1f" % (f[0]/max(1,f[1]), f[2]/max(1,f[1]), b[0], b[1], b[2], b[0]/max(1,b[1]), b[2]/max(1,b[1]), luma))
PY
)
  read -r frg fbg br bg bb brg bbg luma <<< "$m"
  local stats; stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  log "   $tag still: ${bytes}B luma $luma | frame R/G $frg B/G $fbg | chart box RGB $br $bg $bb -> R/G $brg B/G $bbg | hdiff $hdiff, $tries request(s)"
  echo "$tag,$bytes,$frg,$fbg,$br,$bg,$bb,$brg,$bbg,$luma,$hdiff,$tries" >> "$SCSV"
}
run_stage() {  # run_stage <stage> <seconds> <pred fps>
  # stg, not st: awb_sample's read fills st with the AEC state, and bash's dynamic scope
  # would hand that back here (16:13: every still named "stable.jpg")
  local stg=$1 secs=$2 pred=$3 t=0
  while [ "$t" -lt "$secs" ]; do sleep 10; t=$((t + 10)); awb_sample "$stg" "$t"; done
  stage_still "$stg" "$pred"
}

log "== AWB evaluation, QSXGA, quality $Q, llevel=$(status_field llevel) night=$(status_field night) awb=$(status_field awb) awb_gain=$(status_field awb_gain) wb_mode=$(status_field wb_mode) =="
assert_campaign_config "$Q"
ctl micGain=0 > /dev/null
preflight
af_hold "$IDX" "$Q" 5
log "motion: $(checks) (counter reset)"; sleep 3
log "AWB config: 0x3406 0x$(regrd 0x3406) (0 = auto) | tuner: $(ramlog | grep -a "Tuned timing" | grep -a "request 5," | tail -1 | sed 's/^\[[^]]*\] //')"
if [ -z "${ONLY_E:-}" ]; then   # ONLY_E=1: straight to the manual stage (its gain by EGAIN)
log "-- A: fps 5, AEC auto --";            run_stage A 40 5.0
ctl fps=1 > /dev/null; sleep 4
log "tuner: $(ramlog | grep -a "Tuned timing" | grep -a "request 1," | tail -1 | sed 's/^\[[^]]*\] //')"
log "-- B: fps 1, HTS 2844 --";            run_stage B 60 1.01
set_hts 5600; log "-- C: HTS 5600 --";     run_stage C 80 0.514
set_hts 8191; log "-- D: HTS 8191 --";     run_stage D 100 0.351
set_hts 2844
else
ctl fps=1 > /dev/null; sleep 4
log "tuner: $(ramlog | grep -a "Tuned timing" | grep -a "request 1," | tail -1 | sed 's/^\[[^]]*\] //')"
sleep 20; awb_sample B0 20   # the AEC's own balance at fps 1 before the manual switch
fi
# E: manual, unsaturated. Enter, gain by the driver, VTS up then the exposure up (an increase
# from the AEC's value - the entry quirk needs one; a decrease blacks the frame: BOARD_TESTING 37)
wr 0x3503 0x03; ctl "agc_gain=${EGAIN:-1}" > /dev/null; sleep 0.6
wr 0x380E 0x0F; wr 0x380F 0x60
wr 0x3502 0xC0; wr 0x3501 0xF5; wr 0x3500 0x00
log "-- E: manual 3932 lines in VTS 3936 at HTS 2844, agc_gain=${EGAIN:-1} (0x3503 0x$(regrd 0x3503), 0x3406 0x$(regrd 0x3406)) --"
run_stage E 80 0.506
log "motion checks during the run: $(checks) (must be none)"; af_check
log "== done: $CSV, $SCSV =="
column -s, -t < "$CSV" 2>/dev/null; column -s, -t < "$SCSV" 2>/dev/null
