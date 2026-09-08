#!/usr/bin/env bash
# HDV2 soak. NOT an image soak - a RATE soak, and the distinction is the whole point.
#
# WHY HDV2 NEEDS ONE, AND WHAT IT DOES NOT NEED. Almost everything HDV2 is made of is already
# soaked: HTS 2112 at 88 MHz has 9/9 clean stills at 1280X960, 9/9 at HD and 18/18 at a shorter
# line (BOARD_TESTING 38.41), and route B has been counted repeatedly. Its stills gate clean and
# have been eyeballed. What is NOT covered is the one number that is genuinely tight:
#
#   VTS 738 sits TWO LINES above a measured MARGINAL point.
#
#   VTS 740, 738 -> exact, every reading (738 has 6 clean readings across two runs and four arms)
#   VTS 736      -> MARGINAL: 55.453 / 55.469 / 55.453 clean, and 33.613 / 35.403 / 36.566 collapsed
#   VTS 734, 732 -> exactly HALF rate
#   VTS 728      -> no frames
#
# A 0.27% margin over a bistable point is thinner than anything else in this tuner, and if it ever
# drifts - with temperature, with a scene, with a board - THE SYMPTOM IS THE WORST KIND THIS
# PROJECT HAS: the frames stay complete, correct and perfectly legible, and the rate silently
# halves. Every image gate passes it. Only a VSYNC count or a delivered-rate check sees it, which
# is exactly why the still that looks good is not evidence here.
#
# SO THE SOAK COUNTS, and it varies the two things that could move the margin:
#   - STATE ENTRIES. applyTightRows runs on every size selection, so each cycle leaves HDV2 and
#     comes back, re-running the scaler/offset/window sequence from scratch.
#   - TIME AND TEMPERATURE. atemp is logged every cycle; this project has a thermal notebook for
#     a reason, and a cold board is not evidence about a warm one.
# Recordings are interleaved so the DELIVERED rate is checked too, not just the sensor's.
#
# PASS is every count within 2% of 56.46 and no recording below 90% of request. A single count
# near 28 fails the run outright and is reported loudly - that is the half-rate mode, not noise.
#
#   BOARD=<addr> CYCLES=25 bash tools/bench/hdv2_soak.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/hdv2soak_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=30; OTHER=${OTHER:-13}; FPS=${FPS:-56}; Q=${Q:-10}
CYCLES=${CYCLES:-25}
EXPECT=56.46
STILL_EVERY=${STILL_EVERY:-5}
CLIP_EVERY=${CLIP_EVERY:-8}
DUR=${DUR:-20}

CSV="$OUT/hdv2_soak.csv"
echo "cycle,atempC,upTimeS,vts,vsyncFps,pctOfExpect,stillRatio,stillHdiff,bandVerdict,clipFps,clipPct,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" "micGain=$(s0 micGain)" \
            "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps)"
}
trap restore EXIT

vsync2() {  # two counts, the second wins; a dry window logs a DIFFERENT line and must be seen
  ctl "xclkStat=1" > /dev/null; sleep 11
  ctl "xclkStat=1" > /dev/null; sleep 11
  ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"
log "== HDV2 soak: $CYCLES entries at request $FPS, expecting $EXPECT fps =="
log "   the failure being hunted is a SILENT HALVING to ~28 with perfectly clean frames"

fails=0
for c in $(seq 1 "$CYCLES"); do
  # leave and re-enter, so applyTightRows runs its whole sequence again
  ctl "framesize=$OTHER" > /dev/null; sleep 6
  ctl "framesize=$IDX" > /dev/null; sleep 6
  ctl "quality=$Q" > /dev/null
  ctl "fps=$FPS" > /dev/null; sleep 4
  vts=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))
  temp=$(status_field atemp); up=$(uptime_s)
  V=$(vsync2)
  if printf '%s' "$V" | grep -aq 'count failed'; then f=NOFRAMES; pct=0
  else
    f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
    pct=$(python -c "print('%.1f' % (float('$f') / $EXPECT * 100))" 2>/dev/null || echo 0)
  fi
  sr=-; sh=-; bv=-
  if [ $((c % STILL_EVERY)) -eq 1 ] || [ "$STILL_EVERY" -eq 1 ]; then
    fj="$OUT/soak_c${c}.jpg"; http_gap; curl -s -m 25 -o "$fj" "$B/control?still=1"
    st=$(python "$HERE/still_color.py" "$fj" 2>/dev/null) || st=""
    [ -n "$st" ] && { sr=$(echo "$st" | awk '{print $7}'); sh=$(echo "$st" | awk '{print $10}'); }
    bv=$(python "$HERE/still_bands.py" "$fj" --terse 2>/dev/null | awk '{print $3}')
  fi
  cf=-; cp=-
  if [ $((c % CLIP_EVERY)) -eq 0 ]; then
    R=$(record_clip "$IDX" "$FPS" "$Q" "$DUR" "soak${c}")
    cf=$(kv "$R" actFps)
    [ -n "$cf" ] && cp=$(python -c "print('%.1f' % (float('$cf')/$FPS*100))" 2>/dev/null)
  fi
  verdict=$(python - "$f" "$pct" "$cp" "${bv:--}" <<'EOF'
import sys
f, pct, cp, bv = sys.argv[1], float(sys.argv[2] or 0), sys.argv[3], sys.argv[4]
if f == "NOFRAMES": print("NOFRAMES")
elif 45 <= pct <= 55: print("HALFRATE")
elif pct < 98 or pct > 102: print("RATE%.0f%%" % pct)
elif bv not in ("-", "clean", ""): print("BANDED")
elif cp not in ("-", "") and float(cp) < 90: print("DELIVERY%.0f%%" % float(cp))
else: print("ok")
EOF
)
  [ "$verdict" = ok ] || fails=$((fails + 1))
  log "   cycle $c/$CYCLES: ${temp}C up${up}s VTS $vts | VSYNC $f (${pct}% of $EXPECT) | still $sr/$sh $bv | clip $cf ($cp%) -> $verdict"
  echo "$c,$temp,$up,$vts,$f,$pct,$sr,$sh,$bv,$cf,$cp,$verdict" >> "$CSV"
done

log "== result: $fails of $CYCLES cycles not ok =="
column -s, -t < "$CSV"
python - "$CSV" "$EXPECT" <<'EOF' | tee -a "$LOGF"
import csv, sys, statistics as st
rows = list(csv.DictReader(open(sys.argv[1]))); exp = float(sys.argv[2])
good = [float(r["vsyncFps"]) for r in rows if r["verdict"] == "ok"]
bad = [r for r in rows if r["verdict"] != "ok"]
temps = [float(r["atempC"]) for r in rows if r["atempC"] not in ("", "-")]
if good:
    print("   counted %.3f-%.3f (mean %.3f) against %.2f expected, %d clean entries"
          % (min(good), max(good), st.mean(good), exp, len(good)))
if temps: print("   board temperature spanned %.1f to %.1f C" % (min(temps), max(temps)))
if not bad:
    print("\n   SOAK PASSES. VTS 738 held across every state entry and the whole temperature range")
    print("   walked. Note what this does NOT cover: one board, one room, one session.")
else:
    print("\n   %d FAILURES:" % len(bad))
    for r in bad: print("     cycle %s at %sC: %s (counted %s)" % (r["cycle"], r["atempC"], r["verdict"], r["vsyncFps"]))
    if any(r["verdict"] == "HALFRATE" for r in bad):
        print("   HALF RATE SEEN. VTS 738 is not safe - the margin over the marginal 736 is gone.")
        print("   Raise HDV2_VTS_FLOOR, or drop HDV2: at 744 it is identical to HD and pointless.")
EOF
