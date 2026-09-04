#!/usr/bin/env bash
# The by-fps ladder for one size: what the sensor is programmed to at each requested rate and
# what a recording at that rate actually costs. First run 4 Sep 2026 for the HTS 2156 /
# banding-off image at 1280X960.
#
# Per point: fps=<f>, the retime line from the ring (PIXCLK, HTS, lf, VTS, sensor fps, max
# exposure), 0x3108 (the clock route), one VSYNC count off the pin at 5 fps and above (the
# retime line is arithmetic, the count is ground truth), the settled exposure and gain, then a
# forced recording (20 s, 40 s at 2 fps and below) parsed from its closeAvi block: delivered
# fps, frames, average frame bytes, storage ms per frame, SD write speed kB/s, governor boost,
# rescues. A still after the clip goes through still_color.py.
#
# Line time us = HTS x lf / PIXCLK; max exposure ms = min(VTS-4, 1964) x line time. Both are
# computed here from the registers the ring reports, at full precision (the retime line rounds
# the exposure to whole ms). The recordings need a lit room; check llevel in the log header.
#
# Restores the field config on exit.
#   BOARD=<addr> [SIZE=1280X960 IDX=25 RATES="41 40 ..."] bash tools/bench/fps_ladder.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/ladder_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
SIZE=${SIZE:-1280X960}; IDX=${IDX:-25}; Q=${Q:-10}
RATES=${RATES:-"41 40 39 38 37 36 35 30 25 20 15 10 5 2 1"}
SETTLE=${SETTLE:-10}
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/ladder.csv"
[ -f "$CSV" ] || echo "size,fps,route,pixclk,hts,lf,vts,lineUs,maxExpMs,sensorFps,counted,expMs,gain,file,duration,frames,actFps,avgBytes,storageMs,sdKBs,boost,rescues,busy,w,h,ratio,gsat,rsat,hdiff,vdiff" > "$CSV"

restore() {
  log "== restore field config =="
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record)"
}
trap restore EXIT

regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }

log "== fps ladder: $SIZE (idx $IDX) q$Q, rates $RATES =="
log "lighting: llevel=$(status_field llevel) night=$(status_field night) | ae_level=$(status_field ae_level) banding=$(status_field banding)"
assert_campaign_config "$Q"
preflight
first=1
for f in $RATES; do
  if [ -n "$first" ]; then set_size "$IDX" "$Q" "$f"; first=""; else ctl "fps=$f" > /dev/null; sleep 3; fi
  # the retime line for THIS request, retried while the capture task gets round to it
  line=""
  for i in 1 2 3 4 5; do
    line=$(ramlog | grep -a "Tuned timing $SIZE:" | grep -a "for request $f," | tail -1)
    [ -n "$line" ] && break; sleep 2
  done
  [ -n "$line" ] || { log "ABORT: no retime line for $SIZE @$f"; exit 8; }
  read -r pix hts lf vts sensor <<< "$(printf '%s\n' "$line" | sed 's/.*PIXCLK \([0-9.]*\)MHz, HTS \([0-9]*\) x\([0-9]\), VTS \([0-9]*\) -> sensor \([0-9.]*\)fps.*/\1 \2 \3 \4 \5/')"
  r08=$(regrd 0x3108)
  counted="-"
  if [ "$f" -ge 5 ]; then
    ctl "xclkStat=1" > /dev/null; sleep 8
    counted=$(ramlog | grep -a "VSYNC counted" | tail -1 | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
  fi
  sleep "$SETTLE"; assert_alive
  for r in 0x3500 0x3501 0x3502 0x350A 0x350B; do ctl "camRegRd=$r" > /dev/null; sleep 0.5; done
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r lineUs maxExp expMs gain <<< "$(python - "$pix" "$hts" "$lf" "$vts" "$(rd 0x3500)" "$(rd 0x3501)" "$(rd 0x3502)" "$(rd 0x350A)" "$(rd 0x350B)" <<'PY'
import sys
pix, hts, lf, vts = float(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
e0, e1, e2, g0, g1 = [int(x, 16) for x in sys.argv[5:10]]
tl = hts * lf / pix            # us
lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
gain = (((g0 & 3) << 8) | g1) / 16.0
print("%.2f %.2f %.2f %.2f" % (tl, min(vts - 4, 1964) * tl / 1000.0, lines * tl / 1000.0, gain))
PY
)"
  dur=20; [ "$f" -le 2 ] && dur=40
  rec=$(record_clip "$IDX" "$f" "$Q" "$dur" ladder)
  file=$(kv "$rec" file); du=$(kv "$rec" duration); frames=$(kv "$rec" frames); act=$(kv "$rec" actFps)
  avg=$(kv "$rec" avgBytes); st=$(kv "$rec" storageMs); sd=$(kv "$rec" sdKBs); bo=$(kv "$rec" boost)
  rs=$(kv "$rec" rescues); bu=$(kv "$rec" busy)
  stats=$(python "$HERE/still_color.py" "$OUT/still_ladder_${IDX}_${f}_q${Q}.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  log "   @$f: 0x3108=$r08 PIXCLK $pix HTS $hts x$lf VTS $vts | line ${lineUs}us maxExp ${maxExp}ms | sensor $sensor counted $counted | exp ${expMs}ms gain ${gain}x | clip $file: ${frames} fr in ${du}s = $act fps, ${avg}B/frame, storage ${st}ms, SD ${sd}kB/s, boost $bo, rescues $rs, busy $bu% | still ${w}x${h} ratio $ratio hdiff $hdiff vdiff $vdiff"
  echo "$SIZE,$f,$r08,$pix,$hts,$lf,$vts,$lineUs,$maxExp,$sensor,$counted,$expMs,$gain,$file,$du,$frames,$act,$avg,$st,$sd,$bo,$rs,$bu,$w,$h,$ratio,$gsat,$rsat,$hdiff,$vdiff" >> "$CSV"
  [ "$file" = "NONE" ] && anomaly "$SIZE @$f: no clip" || pass
  [ "$rs" != "0" ] && [ -n "$rs" ] && { ctl "quality=$Q" > /dev/null; log "quality re-asserted after a rescue"; }
done
log "== done: $CSV =="
python - "$CSV" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], newline="")))
print("| fps | route | PIXCLK MHz | HTS | VTS | line us | max exp ms | exposure ms / gain | delivered fps | avg KB | storage ms | SD kB/s | still ratio / hdiff / vdiff |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in rows:
    kb = "%.0f" % (int(r["avgBytes"]) / 1024) if r["avgBytes"] else "-"
    print("| %s | %s | %s | %s | %s | %s | %s | %s / %sx | %s | %s | %s | %s | %s / %s / %s |" % (
        r["fps"], "B" if r["route"] == "11" else "A", r["pixclk"], r["hts"], r["vts"], r["lineUs"], r["maxExpMs"],
        r["expMs"], r["gain"], r["actFps"], kb, r["storageMs"], r["sdKBs"], r["ratio"], r["hdiff"], r["vdiff"]))
PY
