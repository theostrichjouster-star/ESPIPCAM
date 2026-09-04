#!/usr/bin/env bash
# Which registers actually set exposure and gain in manual mode? (4 Sep 2026, after
# manual_exposure.sh produced flat black frames at 0x3503 = 0x03 with 1964 lines and 0x1FF in
# 0x3500-02 / 0x350A-B, where the AEC had just made a band frame from the same exposure.)
#
# FHDNARROW request 1 (1.17 fps, 853 ms ceiling) so stills come every second and the room gives
# a measurable signal. Steps, each with the registers read back, YAVG and a still's luma:
#   auto        the AEC settled: E lines, G gain, luma_auto
#   sameByReg   0x3503 = 0x03, the same E and G written to 0x3500-02 / 0x350A-B (must equal auto)
#   gainMax     gain 0x3FF (63.9x), E unchanged
#   expHalf     gain back to G, E/2
#   aecOnly     0x3503 = 0x01 (AEC manual, AGC auto) at E - the AGC picks its own gain
#   agcCtl      0x3503 = 0x03, then the firmware's agc_gain=30 - which register does the driver write?
#   aecCtl      aec=0 then aec_value=E/16 by the firmware's controls - what does the driver write?
# Restore: aec=1, agc=1, 0x3503 = 0x00, frame size reload, field config.
#   BOARD=<addr> bash tools/bench/manual_probe.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/manual_probe_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=${IDX:-16}; Q=${Q:-24}; REQ=1
FIELD_SIZE=13; FIELD_FPS=30
CSV="$OUT/probe.csv"
[ -f "$CSV" ] || echo "step,r3503,expLines,gain16,extra,vts,yavg,bytes,luma,hdiff" > "$CSV"
hexb() { printf '0x%02X' "$1"; }
wr() { ctl "camReg=$1,$(hexb "$2")" > /dev/null; sleep 0.4; }
exp_write() { local v=$(( $1 * 16 )); wr 0x3500 $(( (v >> 16) & 0x0F )); wr 0x3501 $(( (v >> 8) & 0xFF )); wr 0x3502 $(( v & 0xF0 )); }
gain_write() { wr 0x350A $(( ($1 >> 8) & 3 )); wr 0x350B $(( $1 & 0xFF )); }
restore() {
  log "== restore: aec=1 agc=1, 0x3503 0x00, frame size reload, field config =="
  ctl aec=1 > /dev/null; ctl agc=1 > /dev/null; wr 0x3503 0x00
  curl -s -m 25 "$B/control?framesize=$FIELD_SIZE" > /dev/null; sleep 8
  for k in quality=10 fps=$FIELD_FPS idleFps=5 enableMotion=1 record=1 micGain=5; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.3
  done
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) record=$(status_field record) 0x3503=0x$(regrd 0x3503)"
}
trap restore EXIT
E=0; G16=0
shot() {  # shot <tag> [settle s]
  local tag=$1 s=${2:-8} L
  sleep "$s"
  for r in 0x3503 0x3500 0x3501 0x3502 0x350A 0x350B 0x350C 0x350D 0x380E 0x380F; do ctl "camRegRd=$r" > /dev/null; sleep 0.4; done
  ctl "avgZones=1" > /dev/null; sleep 1
  L=$(ramlog)
  rd() { printf '%s\n' "$L" | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }
  read -r zm zmin zmax yavg lo hi st <<< "$(printf '%s\n' "$L" | python "$HERE/parse_zones.py")"
  local lines=$(( ((16#$(rd 0x3500) & 0x0F) << 16 | 16#$(rd 0x3501) << 8 | 16#$(rd 0x3502)) / 16 ))
  local g16=$(( (16#$(rd 0x350A) & 3) << 8 | 16#$(rd 0x350B) )) extra=$(( 16#$(rd 0x350C)$(rd 0x350D) )) vts=$(( 16#$(rd 0x380E)$(rd 0x380F) ))
  local tries=0
  while [ "$tries" -lt 4 ]; do tries=$((tries + 1)); curl -s -m 60 -o "$OUT/$tag.jpg" "$B/control?still=1" && [ -s "$OUT/$tag.jpg" ] && break; sleep 0.7; done
  local stats; stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w h bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  local luma; luma=$(python -c "print('%.1f' % (0.299*$mr + 0.587*$mg + 0.114*$mb))")
  log "   $tag: 0x3503 0x$(rd 0x3503) | exposure $lines lines, gain $g16/16 = $(python -c "print('%.2f' % ($g16/16.0))")x, extra $extra, VTS $vts | YAVG $yavg $st | still ${bytes}B luma $luma hdiff $hdiff"
  echo "$tag,$(rd 0x3503),$lines,$g16,$extra,$vts,$yavg,$bytes,$luma,$hdiff" >> "$CSV"
  SHOT_LINES=$lines; SHOT_G16=$g16
}

log "== manual-mode register probe, size idx $IDX request $REQ, quality $Q, llevel=$(status_field llevel) =="
assert_campaign_config "$Q"
ctl micGain=0 > /dev/null
preflight
set_size "$IDX" "$Q" "$REQ"
log "tuner: $(ramlog | grep -a "Tuned timing" | grep -a "request $REQ," | tail -1 | sed 's/^\[[^]]*\] //')"
shot auto 30; E=$SHOT_LINES; G16=$SHOT_G16
[ "$G16" -gt 0 ] && [ "$E" -gt 0 ] || { log "ABORT: auto read exposure $E gain $G16"; exit 9; }

wr 0x3503 0x03; exp_write "$E"; gain_write "$G16"
shot sameByReg
gain_write 1023
shot gainMax
gain_write "$G16"; exp_write $(( E / 2 ))
shot expHalf
exp_write "$E"; wr 0x3503 0x01
shot aecOnly
wr 0x3503 0x03; ctl agc_gain=30 > /dev/null; sleep 1
shot agcCtl
wr 0x3503 0x00; ctl aec=0 > /dev/null; sleep 1
shot aecCtl0
ctl "aec_value=$(( E / 16 ))" > /dev/null; sleep 1
shot aecCtlValue
log "== done: $CSV =="
column -s, -t < "$CSV" 2>/dev/null || cat "$CSV"
