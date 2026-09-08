#!/usr/bin/env bash
# HD on the 88 MHz route, and the quality the storage path needs to carry the result.
#
# THE ARITHMETIC FIRST, because it decides what is worth measuring. HD is binned, so
# fps = PIXCLK / (HTS x VTS) with a line costing 1 x HTS. Rearranged around the ROW TIME:
#
#     fps = 1 / (tROW x VTS)      and      tROW = HTS / PIXCLK
#
# so the row time a rate demands depends on VTS ALONE - the clock and HTS cannot change it,
# they only trade against each other to reach it. HD reads a 1472-row window binned 2x2, which
# is 736 rows out, so VTS cannot go below ~744 (rows + 8; below the rows read is the measured
# split-frame trap - half-width frames duplicated with a seam, and they pass every automated
# check). 60 fps at VTS 744 therefore needs tROW = 22.40 us, against a measured binned floor of
# ~24 us (24.5 has never latched the bistable magenta readout, 23.75 sometimes does). 60 fps is
# below the floor by construction, at ANY clock:
#
#     at 88 MHz it needs HTS 1971, below the 2060 line floor where the sensor stops following
#     at 92 MHz it needs HTS 2061 - a legal line, but 92 MHz is column-striped (measured)
#     holding HTS legal instead needs VTS 680-712, below the 736 rows being read
#
# Put the other way: 720 rows at the 24.5 us floor is 17.64 ms of readout, so 56.7 fps is the
# hard sensor ceiling for 720p on this part however fast the clock runs.
#
# WHAT IS REACHABLE is the same in-spec route B 1280X960 already uses (0x3108 = 0x11, VCO 440,
# mul 66 -> 88 MHz), at the same 2156 line that holds 24.5 us there:
#
#     88e6 / (2156 x 744) = 54.86 fps,   against 80e6 / (2060 x 744) = 52.20 today
#
# PART A measures that by register - no firmware - and gates the frame, because the whole risk
# of a shorter line is a corruption that byte counts and dimensions do not see.
# PREDICTION: 54.86 fps VSYNC-counted, still ratio/hdiff indistinguishable from the baseline.
#
# PART B answers the quality question the rate raises. The 1-bit SD bus sustains ~3600 KB/s
# measured, so a rate needs frames under 3600/fps KB: 69 at 52, 65 at 55, 60 at 60. This walks
# base quality at HD's ceiling with fpsPriority OFF, so the quality under test is the quality
# in force rather than one the governor has already moved, and reports the boost per rung so a
# boosted reading is never read as an honest one.
#
#   BOARD=<addr> bash tools/bench/hd88_probe.sh
#   PART=a|b|ab   QLIST="10 13 16 19 22 25 28"   DUR=20
set -u
OUT=${OUT:-FPS_RECAL_stills/hd88_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13              # HD
NAME=HD
CEIL=52             # HD's current maxTunedFPS
HTS_A=2060          # route A line, 25.75 us at 80 MHz
HTS_B=2156          # route B line, 24.50 us at 88 MHz - the length 1280X960 settled on
PART=${PART:-ab}
DUR=${DUR:-20}
QLIST=${QLIST:-"10 13 16 19 22 25 28"}
Q=${Q:-10}

CSV="$OUT/hd88.csv"
echo "part,stage,pclkMHz,hts,vts,vsyncFps,impliedMHz,q,actFps,frameKB,storageMs,busy,boost,sdKBs,rescue,ratio,gsat,rsat,hdiff" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  # 0x3108 FIRST, always: leaving route B with the multiplier written first would run SCLK at
  # 160 MHz on the halved dividers until 0x3108 caught up, well past the ~90 MHz cliff
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 1.2
  curl -s -m 25 "$B/control?camReg=0x3036,0x78" > /dev/null; sleep 1.2   # mul 120, PIXCLK 80
  for kv in "fpsPriority=$(s0 fpsPriority)" "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.2
  done
  # the framesize replay re-runs applySensorTuning, which puts HTS and the PLL back by itself;
  # this only proves it did
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality) fpsPriority=$(status_field fpsPriority)"
  log "          0x3108=$(regrd 0x3108) 0x3036=$(regrd 0x3036) HTS=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))"
}
trap restore EXIT

# VSYNC count + implied clock off the ring. xclkStat is refused while capturing, so nothing
# may be recording or streaming when this is called.
#
# TWO counts, and the SECOND is the answer. Measured 7 Sep 2026: the first count after a
# framesize or timing change reads LOW and by a wide margin - 47.455 then 52.204 then 52.138 at
# an unchanged HTS 2060, and 49.715 in the first run of this probe. xclkStat averages 60 VSYNC
# edges inside a 10 s cap, so one settling hiccup anywhere in ~1.2 s of frames drags the whole
# window down. A single count nearly cost this campaign a false finding: 49.7 against 52.2
# predicted read exactly like a line the sensor was not honouring, and the HTS walk that chased
# it (hd_hts_count.sh) found 2060/2112/2156/2200 all counting within 0.1% of arithmetic
vsync_count() {
  ctl "xclkStat=1" > /dev/null
  sleep 8
  local first
  first=$(ramlog | grep -a 'VSYNC counted' | tail -1)
  log "      (first count, discarded: $(printf '%s' "$first" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/') fps)"
  ctl "xclkStat=1" > /dev/null
  sleep 8
  ramlog | grep -a 'VSYNC counted' | tail -1
}

still_gate() {  # still_gate <tag> -> "ratio gsat rsat hdiff" on stdout, file kept
  # one declaration per line: bash expands every word of a `local` BEFORE it assigns any of
  # them, so `local tag=$1 f="...${tag}..."` reads tag while it is still unset - fatal under set -u
  local tag=$1
  local f="$OUT/still_${tag}.jpg"
  local st
  http_gap; curl -s -m 25 -o "$f" "$B/control?still=1"
  st=$(python "$HERE/still_color.py" "$f" 2>/dev/null) || { echo "0 0 0 999"; return 1; }
  echo "$st" | awk '{print $7, $8, $9, $10}'
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"
[ "$(status_field tunedFps)" = "1" ] || { log "ABORT: tunedFps is off"; exit 6; }

# ---------------------------------------------------------------- PART A: the sensor
if [ "$PART" = a ] || [ "$PART" = ab ]; then
  log "== PART A: does HD reach 54.86 fps on route B? =="
  set_size "$IDX" "$Q" "$CEIL"
  sleep 4
  hts=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))
  vts=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))
  r3108=$(regrd 0x3108); mul=$(regrd 0x3036); r3035=$(regrd 0x3035)
  log "   baseline: 0x3108=$r3108 0x3035=$r3035 0x3036=$mul (mul $((0x$mul))) HTS $hts VTS $vts"
  [ "$hts" = "$HTS_A" ] || anomaly "baseline HTS is $hts, expected $HTS_A"
  [ "$vts" = "744" ] || anomaly "baseline VTS is $vts, expected 744"
  V=$(vsync_count); log "   baseline $V"
  fpsA=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
  impA=$(printf '%s' "$V" | sed 's/.*implied PIXCLK \([0-9.]*\)MHz.*/\1/')
  GA=$(still_gate "routeA_${CEIL}")
  log "   baseline still: ratio/gsat/rsat/hdiff = $GA"
  echo "A,routeA,80.00,$hts,$vts,$fpsA,$impA,$Q,,,,,,,,$(echo $GA | tr ' ' ',')" >> "$CSV"
  pass

  # Route B, in the order that keeps every transient SLOW and every line LONG:
  #   1. HTS up first  - 2156 at 80 MHz is a 26.95 us line, longer than the 25.75 it replaces
  #   2. multiplier    - mul 66 on route A dividers is 44 MHz, half speed, never fast
  #   3. 0x3108 = 0x11 - the halved dividers double it to 88, landing on the 24.50 us line
  # Only 0x380D changes: 2060 = 0x080C, 2156 = 0x086C, so there is no two-byte transient at all
  log "   -> route B: HTS $HTS_B, mul 66, 0x3108 = 0x11"
  ctl "camReg=0x380D,0x6C" > /dev/null; sleep 2
  ctl "camReg=0x3036,0x42" > /dev/null; sleep 2
  ctl "camReg=0x3108,0x11" > /dev/null; sleep 3
  hts=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))
  vts=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))
  r3108=$(regrd 0x3108); mul=$(regrd 0x3036)
  log "   route B registers: 0x3108=$r3108 0x3036=$mul (mul $((0x$mul))) HTS $hts VTS $vts"
  [ "$hts" = "$HTS_B" ] && [ "$r3108" = "11" ] && [ "$((0x$mul))" = "66" ] \
    || { anomaly "route B registers did not take - HTS $hts 0x3108 $r3108 mul $((0x$mul))"; }
  V=$(vsync_count); log "   route B $V"
  fpsB=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
  impB=$(printf '%s' "$V" | sed 's/.*implied PIXCLK \([0-9.]*\)MHz.*/\1/')
  GB=$(still_gate "routeB_${CEIL}")
  log "   route B still: ratio/gsat/rsat/hdiff = $GB"
  echo "B,routeB,88.00,$hts,$vts,$fpsB,$impB,$Q,,,,,,,,$(echo $GB | tr ' ' ',')" >> "$CSV"
  python - "$fpsA" "$fpsB" "$GA" "$GB" <<'EOF' | tee -a "$LOGF"
import sys
a, b = float(sys.argv[1] or 0), float(sys.argv[2] or 0)
ga, gb = sys.argv[3].split(), sys.argv[4].split()
print("   PREDICTED 52.20 -> 54.86 (+5.1%%); COUNTED %.3f -> %.3f (%+.1f%%)"
      % (a, b, (b / a - 1) * 100 if a else 0))
print("   still gates  ratio %s -> %s   hdiff %s -> %s   (clean ratio 1.02-1.14, magenta 0.40-0.45)"
      % (ga[0], gb[0], ga[3], gb[3]))
bad = abs(b - 54.86) > 0.6 or not (1.00 <= float(gb[0]) <= 1.20) or float(gb[3]) > float(ga[3]) * 1.6
print("   VERDICT: " + ("CHECK THIS - rate or frame gate out of range" if bad else "route B is clean at HD"))
EOF
  pass
  # hand the sensor back before part B: a quality ladder must run on the shipped timing
  ctl "camReg=0x3108,0x26" > /dev/null; sleep 2
  ctl "camReg=0x3036,0x78" > /dev/null; sleep 2
  ctl "framesize=$IDX" > /dev/null; sleep 6
fi

# ---------------------------------------------------------------- PART B: the storage path
if [ "$PART" = b ] || [ "$PART" = ab ]; then
  log "== PART B: what quality does HD's ceiling need? fpsPriority OFF so the base quality stands =="
  ctl "fpsPriority=0" > /dev/null; sleep 1
  [ "$(status_field fpsPriority)" = "0" ] || { log "ABORT: fpsPriority did not turn off"; exit 6; }
  for q in $QLIST; do
    set_size "$IDX" "$q" "$CEIL"
    R=$(record_clip "$IDX" "$CEIL" "$q" "$DUR" "qlad")
    act=$(kv "$R" actFps)
    [ -z "$act" ] && { anomaly "q$q: no stats"; continue; }
    pass
    fkb=$(python -c "print('%.1f' % (float('$(kv "$R" avgBytes)')/1024))" 2>/dev/null)
    G=$(python "$HERE/still_color.py" "$OUT/still_qlad_${IDX}_${CEIL}_q${q}.jpg" 2>/dev/null | awk '{print $7,$8,$9,$10}')
    [ -z "$G" ] && G="0 0 0 999"
    log "   q$q -> delivered $act of $CEIL | frame ${fkb}KB | storage $(kv "$R" storageMs)ms | busy $(kv "$R" busy)% | boost $(kv "$R" boost) | $(kv "$R" sdKBs)kB/s"
    echo "B,qladder,80.00,$HTS_A,744,,,$q,$act,$fkb,$(kv "$R" storageMs),$(kv "$R" busy),$(kv "$R" boost),$(kv "$R" sdKBs),$(kv "$R" rescue),$(echo $G | tr ' ' ',')" >> "$CSV"
  done
fi

log "== done =="
column -s, -t < "$CSV"
