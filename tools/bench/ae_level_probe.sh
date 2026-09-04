#!/usr/bin/env bash
# Exposure level and banding probe, 1280X960 at 42 / 30 / 10 (4 Sep 2026).
#
# The route B verification stills looked dark. The dump showed why: the AEC chose ~20 ms at
# all three rates (two 50 Hz bands) with gain 1.44-1.50x, and the luminance target band was
# 32..37 because the persisted Exposure Level is -2. Banding was manual 50 Hz from the
# driver's init table (0x3C01=0xA4, 0x3C00=0x04). This walks Exposure Level through the
# requested values on the 60 Hz grid and records, per rate, what the AEC settles on.
#
# Per point, all from the RTC ring (no SD log fetch): exposure lines 0x3500-0x3502, gain
# 0x350A/B, HTS and VTS, the retime line's PIXCLK, avgZones for YAVG against the band, and a
# still through still_color.py. Exposure ms = lines x HTS / PIXCLK (binned, lf 1).
#
# Sequence per level: band select FIRST, then the level. A level change moves the target
# band, which forces the AEC to re-seek - it never re-seeks from a stable point on its own
# (applyAecLimits), so the band change alone could leave the old 50 Hz multiple in force.
# Each fps change re-runs applyAecLimits, which snaps the held exposure onto the live grid.
#
# RAM only. Restores Exposure Level -2 and 50 Hz manual banding, then the field config.
#   BOARD=<addr> bash tools/bench/ae_level_probe.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/ae_level}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
SETTLE=${SETTLE:-15}
LEVELS=${LEVELS:-"-1 0"}
RATES=${RATES:-"42 30 10"}
# 60 -> 0x3C00 bit 2 clear; 50 -> set (0x3C01 stays manual, 0xA4); off -> 0x3A00 bit 5 (band
# function enable) cleared, exposure freeform up to the frame. The datasheet's own note: with
# banding off the minimum integration time is no longer quantised, and under mains light the
# picture can carry rolling flicker bands - vdiff in the still stats is the number for that,
# the eyeball is the judge
BAND=${BAND:-60}
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/points.csv"
[ -f "$CSV" ] || echo "band,level,fps,pixclk,hts,vts,expLines,expMs,frameMs,maxExpMs,gain,yavg,bandLo,bandHi,aec,bytes,ratio,gsat,rsat,hdiff,vdiff" > "$CSV"
AEC00_START=""

restore() {
  log "== restore: level -2, 50 Hz manual banding (0x3A00 back to ${AEC00_START:-0x78}), field config =="
  curl -s -m 25 "$B/control?camReg=0x3A00,${AEC00_START:-0x78}" > /dev/null; sleep 0.5
  curl -s -m 25 "$B/control?camReg=0x3C00,0x04" > /dev/null; sleep 0.5
  curl -s -m 25 "$B/control?ae_level=-2" > /dev/null; sleep 0.5
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) ae_level=$(status_field ae_level) record=$(status_field record)"
}
trap restore EXIT

regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }

sample() {  # sample <band> <level> <fps>
  local band=$1 level=$2 fps=$3 L e0 e1 e2 g0 g1 h v pix expLines expMs frameMs maxExp gain zones tag stats
  sleep "$SETTLE"
  assert_alive
  for r in 0x3500 0x3501 0x3502 0x350A 0x350B 0x380C 0x380D 0x380E 0x380F; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  e0=$(rd 0x3500); e1=$(rd 0x3501); e2=$(rd 0x3502); g0=$(rd 0x350A); g1=$(rd 0x350B)
  h=$(rd 0x380C)$(rd 0x380D); v=$(rd 0x380E)$(rd 0x380F)
  pix=$(printf '%s\n' "$L" | grep -a "Tuned timing 1280X960" | tail -1 | sed 's/.*PIXCLK \([0-9.]*\)MHz.*/\1/')
  zones=$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")   # mean min max yavg lo hi state
  read -r zm zmin zmax yavg lo hi st <<< "$zones"
  read -r expLines expMs frameMs maxExp gain <<< "$(python - "$e0" "$e1" "$e2" "$g0" "$g1" "$h" "$v" "${pix:-0}" <<'PY'
import sys
e0,e1,e2,g0,g1,h,v,pix = sys.argv[1:9]
lines = (((int(e0,16)&0x0F)<<12) | (int(e1,16)<<4) | (int(e2,16)>>4))
gain = (((int(g0,16)&3)<<8) | int(g1,16)) / 16.0
hts, vts, pix = int(h,16), int(v,16), float(pix)
tl = hts / (pix*1e6) if pix else 0
print("%d %.2f %.2f %.2f %.2f" % (lines, lines*tl*1e3, vts*tl*1e3, min(vts-4,1964)*tl*1e3, gain))
PY
)"
  tag="1280X960_${band}Hz_L${level}_${fps}"
  curl -s -m 30 -o "$OUT/$tag.jpg" "$B/control?still=1" || { log "ABORT: still fetch failed"; exit 7; }
  stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg") || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w hh bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  log "   band $band level $level @$fps: exposure $expLines lines = ${expMs}ms of ${frameMs}ms frame (max ${maxExp}), gain ${gain}x, YAVG $yavg band $lo..$hi $st | still ${bytes}B ratio $ratio hdiff $hdiff vdiff $vdiff"
  echo "$band,$level,$fps,$pix,$((16#$h)),$((16#$v)),$expLines,$expMs,$frameMs,$maxExp,$gain,$yavg,$lo,$hi,$st,$bytes,$ratio,$gsat,$rsat,$hdiff,$vdiff" >> "$CSV"
}

log "== exposure level probe: band ${BAND}Hz, levels $LEVELS, rates $RATES =="
assert_campaign_config 10
preflight
log "start: ae_level=$(status_field ae_level) 0x3C00=$(regrd 0x3C00) 0x3C01=$(regrd 0x3C01) 0x3C0C=$(regrd 0x3C0C)"
set_size 25 10 42; sleep 4

# the band first: bit 2 of 0x3C00 selects 50 Hz when set, with 0x3C01[7] manual; "off"
# clears the band function enable, 0x3A00[5], and remembers the byte for the restore
AEC00_START=0x$(regrd 0x3A00)
if [ "$BAND" = "off" ]; then
  ctl "camReg=0x3A00,$(printf '0x%02X' $(( AEC00_START & ~0x20 )))" > /dev/null
elif [ "$BAND" = "60" ]; then ctl "camReg=0x3C00,0x00" > /dev/null
else ctl "camReg=0x3C00,0x04" > /dev/null; fi
sleep 1
log "band select: 0x3A00=$(regrd 0x3A00) (was $AEC00_START) 0x3C00=$(regrd 0x3C00) 0x3C0C=$(regrd 0x3C0C)"

for level in $LEVELS; do
  ctl "ae_level=$level" > /dev/null; sleep 2
  log "-- level $level: 0x3A0F=$(regrd 0x3A0F) 0x3A10=$(regrd 0x3A10) (target band) --"
  for fps in $RATES; do
    ctl "fps=$fps" > /dev/null; sleep 4
    sample "$BAND" "$level" "$fps"
  done
done
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
