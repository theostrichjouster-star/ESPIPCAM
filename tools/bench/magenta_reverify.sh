#!/usr/bin/env bash
# Is the "bistable magenta readout latch" real, and is a 24.0 us row safe?
#
# WHY THIS IS IN DOUBT. The claim (BOARD_TESTING 37, CLAUDE.md) is that a binned row shorter
# than ~24 us can drop the sensor into a magenta readout state, bistably: ratio G/avg(R,B)
# around 0.44 with 11% of R saturated, against 1.02-1.15 clean. It carries real cost - it is the
# whole reason 1280X960 runs HTS 2156 (24.50 us at 88 MHz, ceiling 41.48) instead of HTS 2112
# (24.00 us, ceiling 42.34), and it is what stops HD's route B line going shorter than 2156.
#
# The evidence for it is thinner than its influence:
#   - It was FIRST attributed to a two-byte HTS write landing mid-frame, and the non-monotonic
#     data that suggested a latch (SVGA magenta at 1960 but clean at 1900 and 1850; XGA magenta
#     1850-1650, clean at 1600) came entirely from plain two-byte writes.
#   - The re-attribution to a readout state rests on ONE line length: HTS 1900 at 80 MHz,
#     23.75 us, written atomically through the 0x3212 group write, magenta on both arms of run 3
#     and clean on run 1.
#   - Those same runs were writing ANALOG registers, and 0x370C = 0x03 is independently known to
#     remap colour "yellow to magenta, blue to green". Its signature differs (ratio 0.85, not
#     0.44), but the campaign also logged a real instrument fault in which the analog registers
#     were found left behind at 0x52/0x03 at the start of a following run.
#   - "Never seen at 24.5 us" rests on eight samples; 24.0 us has "a dozen or more" and was
#     explicitly recorded as NOT a soak.
#
# THE DESIGN. Row time is walked at a FIXED clock by HTS alone, on route B at 88 MHz:
#
#     HTS 2156 -> 24.50 us    the length the claim calls safe
#     HTS 2112 -> 24.00 us    the disputed one, worth +2.1% of frame rate
#     HTS 2060 -> 23.41 us    the POSITIVE CONTROL
#
# 2060 is the point that makes this a test rather than a demonstration. It is SHORTER than the
# 23.75 us that reportedly latches, so if the effect is real it should latch here more readily
# than anywhere it has been seen. If many samples across several state re-entries stay clean at
# 23.41 us, the bistable-latch claim cannot stand as written and the 2156 line is costing frame
# rate for nothing. HTS never goes below 2060, which is the measured register floor where the
# sensor stops following the line at all - below that the confound is the floor, not the row.
#
# Two confounds are removed by construction rather than argued away:
#   - NO mid-frame two-byte write. 2156/2112/2060 are 0x086C/0x0840/0x080C, so the high byte
#     never changes and every write here is a single atomic byte to 0x380D.
#   - ANALOG registers are read back at the start and at the end, because the campaign that
#     produced the original claim leaked 0x3709/0x370C into a following run.
#
# Each rung takes CYCLES state re-entries, and STILLS stills within each, because a bistable
# state is a per-entry coin flip and repeated stills inside one entry are one trial. A re-entry
# is a full framesize reload plus re-application of the line and the clock, which restarts the
# readout the way a user changing size would.
#
# The lens is deliberately left on continuous AF. The magenta signature is a factor of 2.3 in
# the channel ratio, which focus cannot fake; hdiff is reported for stripe and confetti
# corruption and read against each size's own clean baseline, not an absolute.
#
#   BOARD=<addr> bash tools/bench/magenta_reverify.sh
#   SIZES="25:1280X960:41 13:HD:52"   WALK="2156 2112 2060"   CYCLES=3 STILLS=3
set -u
OUT=${OUT:-FPS_RECAL_stills/magenta_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

SIZES=${SIZES:-"25:1280X960:41 13:HD:52"}
WALK=${WALK:-"2156 2112 2060"}
CYCLES=${CYCLES:-3}
STILLS=${STILLS:-3}
Q=${Q:-10}
PCLK_MHZ=88

CSV="$OUT/magenta.csv"
# still_color.py prints ELEVEN fields and vdiff is the last of them. Leaving it out of this header
# cost a wrong verdict on the first run: csv.DictReader then mapped "verdict" onto vdiff, so every
# row read as a number instead of "clean" and the summary announced 18 of 18 samples bad while
# every per-still log line said clean. Count the fields against still_color.py's docstring
echo "size,hts,rowUs,cycle,shot,W,H,bytes,meanR,meanG,meanB,ratio,gsat,rsat,hdiff,vdiff,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  # 0x3108 back to route A FIRST: with the multiplier written first, mul 120 on route B's halved
  # dividers would run SCLK at 160 MHz, far past the ~90 MHz cliff
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 1.2
  curl -s -m 25 "$B/control?camReg=0x3036,0x78" > /dev/null; sleep 1.2
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.2
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
  log "          0x3108=$(regrd 0x3108) 0x3036=$(regrd 0x3036) analog 0x3709=$(regrd 0x3709) 0x370C=$(regrd 0x370C)"
}
trap restore EXIT

# put the sensor on route B at the wanted line: HTS FIRST (at 80 MHz every candidate line is
# LONGER than 24.5 us, so the intermediate state is never short), then the multiplier at half
# speed, then the halved dividers which double it to 88
enter_state() {
  local idx=$1 fps=$2 hts=$3
  ctl "framesize=$idx" > /dev/null; sleep 6
  ctl "quality=$Q" > /dev/null
  ctl "fps=$fps" > /dev/null; sleep 4
  ctl "camReg=0x380D,$(printf '0x%02X' $((hts & 0xFF)))" > /dev/null; sleep 1
  ctl "camReg=0x380C,$(printf '0x%02X' $(((hts >> 8) & 0x1F)))" > /dev/null; sleep 1
  ctl "camReg=0x3036,0x42" > /dev/null; sleep 2
  ctl "camReg=0x3108,0x11" > /dev/null; sleep 3
  local got r3108 mul
  got=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))
  r3108=$(regrd 0x3108); mul=$(regrd 0x3036)
  [ "$got" = "$hts" ] && [ "$r3108" = "11" ] && [ "$((0x$mul))" = "66" ] && return 0
  log "   state entry FAILED: HTS $got (wanted $hts), 0x3108 $r3108, mul $((0x$mul))"
  return 1
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"
log "== analog registers at start: 0x3709=$(regrd 0x3709) 0x370C=$(regrd 0x370C) (the leak this campaign once had) =="
log "== row time walked by HTS alone at ${PCLK_MHZ} MHz; 2060 = 23.41us is the POSITIVE CONTROL =="

for sz in $SIZES; do
  IFS=: read -r idx name fps <<< "$sz"
  for hts in $WALK; do
    rowUs=$(python -c "print('%.2f' % ($hts / $PCLK_MHZ.0))")
    for c in $(seq 1 "$CYCLES"); do
      if ! enter_state "$idx" "$fps" "$hts"; then anomaly "$name HTS $hts cycle $c: state entry failed"; continue; fi
      pass
      for sh in $(seq 1 "$STILLS"); do
        f="$OUT/${name}_hts${hts}_c${c}_s${sh}.jpg"
        http_gap; curl -s -m 25 -o "$f" "$B/control?still=1"
        st=$(python "$HERE/still_color.py" "$f" 2>/dev/null) || st="0 0 0 0 0 0 0 0 0 999 999"
        # the magenta signature is unmistakable and needs no tuning: G collapses to under half
        # the red/blue mean while red saturates. Everything else is reported, not judged
        v=$(python - "$st" <<'EOF'
import sys
p = sys.argv[1].split()
ratio, gsat, rsat, hdiff = float(p[6]), float(p[7]), float(p[8]), float(p[9])
if ratio == 0: print("NOFRAME")
elif ratio < 0.60 and rsat > 5: print("MAGENTA")
elif ratio < 0.60: print("CASTLOW")
elif ratio > 1.25: print("GREENBLOW")
elif hdiff > 25: print("NOISE")
else: print("clean")
EOF
)
        log "   $name HTS $hts (${rowUs}us) c$c s$sh: ratio $(echo $st | awk '{print $7}') gsat $(echo $st | awk '{print $8}') rsat $(echo $st | awk '{print $9}') hdiff $(echo $st | awk '{print $10}') -> $v"
        echo "$name,$hts,$rowUs,$c,$sh,$(echo $st | tr ' ' ','),$v" >> "$CSV"
      done
    done
  done
done

log "== analog registers at end: 0x3709=$(regrd 0x3709) 0x370C=$(regrd 0x370C) =="
log "== verdict per rung =="
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import collections, csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
agg = collections.OrderedDict()
for r in rows:
    k = (r["size"], int(r["hts"]), r["rowUs"])
    a = agg.setdefault(k, {"n": 0, "bad": 0, "ratios": [], "hdiff": [], "kinds": collections.Counter()})
    a["n"] += 1
    a["kinds"][r["verdict"]] += 1
    if r["verdict"] != "clean": a["bad"] += 1
    try:
        a["ratios"].append(float(r["ratio"])); a["hdiff"].append(float(r["hdiff"]))
    except ValueError:
        pass
print("%-10s %5s %7s %6s %14s %14s  %s" % ("size", "HTS", "row us", "n", "ratio min-max", "hdiff min-max", "outcome"))
for (size, hts, us), a in agg.items():
    rr = "%.3f-%.3f" % (min(a["ratios"]), max(a["ratios"])) if a["ratios"] else "-"
    hh = "%.1f-%.1f" % (min(a["hdiff"]), max(a["hdiff"])) if a["hdiff"] else "-"
    kinds = " ".join("%s x%d" % (k, v) for k, v in a["kinds"].most_common())
    print("%-10s %5d %7s %6d %14s %14s  %s" % (size, hts, us, a["n"], rr, hh, kinds))
ctrl = [a for (s, h, u), a in agg.items() if h == 2060]
if ctrl:
    n = sum(a["n"] for a in ctrl); bad = sum(a["bad"] for a in ctrl)
    print("\nPOSITIVE CONTROL, 23.41 us (shorter than the 23.75 that reportedly latches):")
    print("  %d samples, %d not clean." % (n, bad))
    print("  " + ("The latch reproduces - the claim stands and 24.0 us is the marginal call."
                  if bad else
                  "NOTHING latched below the reported threshold. On this evidence the bistable "
                  "magenta latch does not reproduce, and HTS 2156 is buying margin against an "
                  "effect this run could not provoke. The eyeball gate decides, not this table."))
EOF
log "== done. Every still is in $OUT - the channel gates are a screen, the eyeball is the gate =="
