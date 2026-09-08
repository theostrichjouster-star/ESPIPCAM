#!/usr/bin/env bash
# Is the magenta cast caused by HTS below the ~2060 line floor, or by a short ROW TIME?
#
# THE TWO MODELS. Section 37 saw a magenta cast (ratio 0.40-0.45) walking HTS down at 80 MHz and
# concluded a bistable readout state whose probability rises as the ROW gets shorter, with a floor
# near 24 us. Section 38.41 then measured 54 samples clean at 88 MHz across rows of 24.50, 24.00
# and 23.41 us, and 38.42 found the cast appearing reliably above 90 MHz at rows as long as
# 23.38 us. A 23.41 us row clean and a 23.38 us row corrupt cannot both be explained by row time.
#
# The alternative: EVERY section 37 magenta sample had HTS below 2060 - 1960, 1900, 1850, 1800,
# 1750, 1650 - and 2060 is the measured register floor where "the sensor stops following" the
# programmed line. Behaviour below it is undefined, which would also explain the non-monotonicity
# (SVGA magenta at 1960 but clean at 1900 and 1850) that made the effect look bistable.
#
# WHY THE OBVIOUS EXPERIMENT DOES NOT WORK. Walking HTS below 2060 at 88 MHz is what was asked
# for, and by itself it discriminates nothing: at 88 MHz a line of 1960 is a 22.27 us row, so the
# floor model and the row-time model BOTH predict corruption. Every rung would confirm both.
#
# SO THERE ARE TWO ARMS, and the second is the one that decides:
#
#   ARM A, 88 MHz  - the requested walk. HTS 2060 is a known-clean control (9/9 in 38.41).
#                    Below it, rows are 22.73 / 22.27 / 21.59 / 21.02 us. Both models predict
#                    failure, so this arm can only FALSIFY - if these come back clean, both models
#                    are wrong. It also reproduces section 37 on a clock it never used.
#
#   ARM B, 60 MHz  - THE DISCRIMINATOR. Same lines, but every row is 30.8 to 34.3 us, far longer
#                    than any row this project has ever seen fail.
#                       floor model  -> corrupt below HTS 2060, despite the long row
#                       row model    -> clean at every rung, because the rows are enormous
#                    These predictions are opposite, so this arm settles it.
#
# A THIRD READING COMES FREE. The VSYNC count gives the line the sensor ACTUALLY ran:
# effective HTS = PIXCLK / (counted fps x VTS). Section 37 noted that asking for 1900 "came back
# as an effective 2069" but never tabulated it. If the sensor clamps the line below 2060, that is
# visible here directly, and it is the mechanism the floor model claims.
#
# EVERY HTS CHANGE GOES THROUGH THE 0x3212 GROUP WRITE, without exception. 2060 to 2000 crosses a
# high-byte boundary (0x080C to 0x07D0), and CLAUDE.md records that a mid-frame two-byte HTS write
# once latched a magenta cast that an entire campaign then blamed on the line length. Using two
# plain writes here would reintroduce exactly the confound this run exists to remove.
#
# Stock HD geometry throughout - 1472-row window, yOff 8, VTS 744 - so the readout work of 38.41
# is not a variable and the conditions sit as close to section 37 as the sizes allow.
#
#   BOARD=<addr> bash tools/bench/hts_floor_hypothesis.sh   [WALK="2060 2000 1960 1900 1850"]
set -u
OUT=${OUT:-FPS_RECAL_stills/htsfloor_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; OTHER=${OTHER:-10}; FPS_REQ=${FPS_REQ:-52}; Q=${Q:-10}
WALK=${WALK:-"2060 2000 1960 1900 1850"}
VTS=744
SHOTS=${SHOTS:-3}

CSV="$OUT/htsfloor.csv"
echo "arm,pclkMHz,htsAsked,htsRead,rowUs,predFps,vsyncFps,effHts,shot,bytes,ratio,gsat,rsat,hdiff,bandSpread,bandSat,bandVerdict,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  # group-write the line back before anything else, then leave route B the safe way round
  curl -s -m 25 "$B/control?camRegGrp=0x380C,0x08,0x380D,0x0C" > /dev/null; sleep 2
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 1.5
  curl -s -m 25 "$B/control?camReg=0x3036,0x78" > /dev/null; sleep 1.5
  curl -s -m 25 "$B/control?framesize=$OTHER" > /dev/null; sleep 7
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) 0x3108=$(regrd 0x3108) 0x3036=$(regrd 0x3036) HTS $(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))"
}
trap restore EXIT

vsync_or_fail() {
  ctl "xclkStat=1" > /dev/null; sleep 11
  ctl "xclkStat=1" > /dev/null; sleep 11
  ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1
}

setHts() {  # atomic, always: both bytes land at one frame boundary via 0x3212
  local hts=$1
  ctl "camRegGrp=0x380C,$(printf '0x%02X' $(((hts >> 8) & 0x1F))),0x380D,$(printf '0x%02X' $((hts & 0xFF)))" > /dev/null
  sleep 4
  echo "$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))"
}

rung() {  # rung <armName> <pclkMHz> <hts>
  local arm=$1 pclk=$2 hts=$3 got rowUs pred V f i fjpg bytes st bands bandV verdict eff
  got=$(setHts "$hts")
  [ "$got" = "$hts" ] || { anomaly "$arm HTS $hts read back $got"; return; }
  rowUs=$(python -c "print('%.2f' % ($hts / $pclk))")
  pred=$(python -c "print('%.2f' % ($pclk * 1e6 / ($hts * $VTS)))")
  V=$(vsync_or_fail)
  if printf '%s' "$V" | grep -aq 'count failed'; then f=NOFRAMES; eff=-; else
    f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
    eff=$(python -c "print('%.0f' % ($pclk * 1e6 / (float('$f') * $VTS)))" 2>/dev/null || echo -); fi
  log "   $arm ${pclk}MHz HTS $hts (${rowUs}us row), predicted $pred: VSYNC $f -> EFFECTIVE LINE $eff clocks"
  for i in $(seq 1 "$SHOTS"); do
    fjpg="$OUT/${arm}_hts${hts}_s${i}.jpg"
    http_gap; curl -s -m 25 -o "$fjpg" "$B/control?still=1"
    bytes=$(stat -c %s "$fjpg" 2>/dev/null || echo 0)
    st=$(python "$HERE/still_color.py" "$fjpg" 2>/dev/null) || st="0 0 0 0 0 0 0 0 0 999 999"
    bands=$(python "$HERE/still_bands.py" "$fjpg" --terse 2>/dev/null) || bands="0 0 NOSTILL"
    verdict=$(python - "$st" "$f" "$pred" <<'EOF'
import sys
p = sys.argv[1].split(); f, pred = sys.argv[2], float(sys.argv[3])
ratio, gsat, rsat, hdiff = float(p[6]), float(p[7]), float(p[8]), float(p[9])
if f == "NOFRAMES": print("NOFRAMES")
elif ratio == 0: print("NOSTILL")
elif ratio < 0.60: print("MAGENTA")
elif hdiff > 25: print("STRIPED/NOISE")
elif ratio > 1.25: print("GREENBLOW")
else: print("clean")
EOF
)
    bandV=$(printf '%s' "$bands" | awk '{print $3}')
    [ "$verdict" = clean ] && [ "$bandV" != clean ] && verdict="BANDED($bandV)"
    log "      still $i: $bytes B ratio $(echo "$st" | awk '{print $7}') hdiff $(echo "$st" | awk '{print $10}') | bands $bands -> $verdict"
    echo "$arm,$pclk,$hts,$got,$rowUs,$pred,$f,$eff,$i,$bytes,$(echo "$st" | awk '{print $7","$8","$9","$10}'),$(printf '%s' "$bands" | tr ' ' ','),$verdict" >> "$CSV"
  done
  if [ "$f" = NOFRAMES ]; then
    log "      recovering: line back to 2060"
    setHts 2060 > /dev/null
  fi
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"
ctl "framesize=$OTHER" > /dev/null; sleep 7
ctl "framesize=$IDX" > /dev/null; sleep 7
ctl "quality=$Q" > /dev/null
ctl "fps=$FPS_REQ" > /dev/null; sleep 4
log "== stock HD geometry: window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )), yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) )), VTS $(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) )) =="

log "-- ARM A: 88 MHz, the requested walk. Rows 23.41 down to 21.02 us. Both models predict failure --"
ctl "camReg=0x3036,0x42" > /dev/null; sleep 2      # mul 66 on route A = 44 MHz, a slow transient
ctl "camReg=0x3108,0x11" > /dev/null; sleep 3      # -> 88 MHz
for h in $WALK; do rung A88 88 "$h"; done

log "-- ARM B: 60 MHz, THE DISCRIMINATOR. Same lines, rows 34.3 down to 30.8 us --"
setHts 2060 > /dev/null
ctl "camReg=0x3108,0x26" > /dev/null; sleep 3      # leave route B FIRST: 88 -> 44 MHz
ctl "camReg=0x3036,0x5A" > /dev/null; sleep 3      # mul 90 on route A = 60 MHz
[ "$((0x$(regrd 0x3036)))" = "90" ] && [ "$(regrd 0x3108)" = "26" ] || { log "ABORT: 60 MHz did not take"; exit 6; }
for h in $WALK; do rung B60 60 "$h"; done

log "== result =="
column -s, -t < "$CSV"
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import collections, csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
agg = collections.OrderedDict()
for r in rows:
    k = (r["arm"], int(r["htsAsked"]))
    a = agg.setdefault(k, {"row": r["rowUs"], "pred": r["predFps"], "fps": r["vsyncFps"],
                           "eff": r["effHts"], "v": collections.Counter()})
    a["v"][r["verdict"]] += 1
print("   arm  HTS   row us  predicted  counted   effective line   frames")
for (arm, hts), a in agg.items():
    print("   %-4s %-5d %-7s %-10s %-9s %-16s %s"
          % (arm, hts, a["row"], a["pred"], a["fps"], a["eff"],
             " ".join("%s x%d" % (k, v) for k, v in a["v"].most_common())))
def allClean(arm, hts):
    a = agg.get((arm, hts))
    return a is not None and list(a["v"]) == ["clean"]
below = sorted({int(r["htsAsked"]) for r in rows if int(r["htsAsked"]) < 2060})
bClean = [h for h in below if allClean("B60", h)]
bBad = [h for h in below if not allClean("B60", h)]
print()
if bBad and not bClean:
    print("   FLOOR MODEL CONFIRMED. At 60 MHz every line below 2060 failed despite rows of 30-34 us,")
    print("   which is far longer than any row this project has seen fail. Row time is not the cause;")
    print("   programming HTS below the register floor is. The ~24 us row floor should go.")
elif bClean and not bBad:
    print("   ROW-TIME MODEL FAVOURED. At 60 MHz every line below 2060 was CLEAN, so a short line is")
    print("   only harmful when it also makes a short row. The floor hypothesis is dead as stated.")
else:
    print("   MIXED at 60 MHz: clean at %s, failed at %s." % (bClean, bBad))
    print("   Neither model as stated. Look at the effective-line column - a line the sensor clamps")
    print("   is a different regime from one it honours, and the boundary may not be exactly 2060.")
EOF
log "== done. Stills in $OUT =="
