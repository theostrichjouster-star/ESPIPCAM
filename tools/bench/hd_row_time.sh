#!/usr/bin/env bash
# Is the cliff the CLOCK or the ROW TIME? Walk HTS at a fixed 96 MHz and find out.
#
# WHAT THE CLOCK WALK FOUND (hd_clock_96.sh, HD at 1440 rows, HTS 2112, VTS 738):
#
#   mul 66  88.00 MHz  24.00 us  counted 56.444 vs 56.46   clean
#   mul 69  92.00      22.96     counted 59.055 vs 59.03   clean
#   mul 70  93.33      22.63     counted 59.880 vs 59.88   clean
#   mul 71  94.67      22.31     counted 54.391 vs 60.74   MAGENTA, ratio 0.087 and 0.006
#   mul 72  96.00      22.00     counted 42.471 vs 61.59   striped, hdiff 31, rsat 22%
#
# Three things follow, and they pull in opposite directions:
#   - 92 MHz IS CLEAN HERE, twice, exact to 0.03 fps. This notebook records 92 as column-striped
#     on either route. That was measured at 1280X960 on a different geometry.
#   - The MAGENTA IS REAL and it is not subtle: ratio 0.087 and 0.006, where green is essentially
#     absent, against the 0.40-0.45 section 37 reported. So the failure mode exists. What today's
#     re-verification showed is only that it does not appear at a 23.41 us row on the 88 MHz route,
#     which remains true and is not contradicted by this.
#   - The clean/broken boundary sits between 22.63 us and 22.31 us, NOT near the 24 us this
#     project has treated as the binned row floor since section 37. Every rung from 24.50 down to
#     22.63 has now been measured clean.
#
# BUT CLOCK AND ROW TIME MOVED TOGETHER in that walk, so it cannot say which one bites. This
# separates them: the clock is PINNED at 96 MHz and only HTS moves, so any rung that comes back
# clean proves the clock alone is not the limit.
#
#   HTS 2244 -> 23.38 us -> 57.97 fps    should be clean if row time governs
#   HTS 2200 -> 22.92    -> 59.13        clean at 22.96 on the clock walk
#   HTS 2173 -> 22.64    -> 59.86        the last row time measured clean, reproduced at 96 MHz
#   HTS 2156 -> 22.46    -> 60.33        BETWEEN the clean 22.63 and the broken 22.31
#   HTS 2112 -> 22.00    -> 61.59        known broken at this clock, kept as the positive control
#
# 60 fps at 720p needs HTS <= 2168 at this clock and frame, which is a 22.58 us row - almost
# exactly on the boundary the clock walk found. That is why 2156 is the rung that matters: it is
# the user's own suggestion and it happens to be the discriminating point.
#
# All five are 0x08xx, so every HTS change is a single atomic byte to 0x380D and the mid-frame
# two-byte hazard never arises. The walk runs LONGEST LINE FIRST so it starts from the safest
# state, and it does not stop at a failure - the boundary is the object of the exercise.
#
#   BOARD=<addr> bash tools/bench/hd_row_time.sh   [MUL=72] [WALK="2244 2200 2173 2156 2112"]
set -u
OUT=${OUT:-FPS_RECAL_stills/hdrow_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; OTHER=${OTHER:-10}; FPS_REQ=${FPS_REQ:-52}; Q=${Q:-10}
MUL=${MUL:-72}
WALK=${WALK:-"2244 2200 2173 2156 2112"}
VTS=738
SHOTS=${SHOTS:-3}

CSV="$OUT/hdrow.csv"
echo "mul,pclkMHz,hts,rowUs,predFps,vsyncFps,impliedMHz,shot,bytes,ratio,gsat,rsat,hdiff,bandSpread,bandSatRatio,bandVerdict,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  curl -s -m 25 "$B/control?camReg=0x3036,0x42" > /dev/null; sleep 1.5   # multiplier DOWN first
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 1.5   # then leave route B
  curl -s -m 25 "$B/control?camReg=0x3036,0x78" > /dev/null; sleep 1.5
  for kv in "camReg=0x380F,0xE8" "camReg=0x380D,0x0C" "camReg=0x3807,0xAF" "camReg=0x3813,0x08" "camReg=0x5001,0x83"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  curl -s -m 25 "$B/control?framesize=$OTHER" > /dev/null; sleep 7
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) 0x3108=$(regrd 0x3108) 0x3036=$(regrd 0x3036)"
  log "          HTS $(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) )) window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )) yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) ))"
}
trap restore EXIT

vsync_or_fail() {
  ctl "xclkStat=1" > /dev/null; sleep 11
  ctl "xclkStat=1" > /dev/null; sleep 11
  ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1
}

rung() {
  local hts=$1 pclk rowUs pred V f imp i fjpg bytes st verdict got
  ctl "camReg=0x380D,$(printf '0x%02X' $((hts & 0xFF)))" > /dev/null; sleep 4
  got=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))
  [ "$got" = "$hts" ] || { anomaly "HTS $hts reads $got"; return; }
  pclk=$(python -c "print('%.2f' % ($MUL * 2.0 / 3 * 2))")
  rowUs=$(python -c "print('%.2f' % ($hts / ($MUL * 2.0 / 3 * 2)))")
  pred=$(python -c "print('%.2f' % ($MUL * 2.0 / 3 * 2 * 1e6 / ($hts * $VTS)))")
  V=$(vsync_or_fail)
  if printf '%s' "$V" | grep -aq 'count failed'; then f=NOFRAMES; imp=-; else
    f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
    imp=$(printf '%s' "$V" | sed 's/.*implied PIXCLK \([0-9.]*\)MHz.*/\1/'); fi
  log "   HTS $hts at ${pclk}MHz = ${rowUs}us row, predicted $pred fps: VSYNC $f (implied $imp)"
  for i in $(seq 1 "$SHOTS"); do
    fjpg="$OUT/hts${hts}_s${i}.jpg"
    http_gap; curl -s -m 25 -o "$fjpg" "$B/control?still=1"
    bytes=$(stat -c %s "$fjpg" 2>/dev/null || echo 0)
    st=$(python "$HERE/still_color.py" "$fjpg" 2>/dev/null) || st="0 0 0 0 0 0 0 0 0 999 999"
    # the WHOLE-FRAME gate is not sufficient on this axis. At 93.33 MHz it scored 0.850 and 1.010
    # with hdiff 3.1 on a frame whose bottom half was blue and yellow garbage, because the good
    # half averaged the bad half back into range. still_bands.py compares quarters against each
    # other and is what catches regional damage
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
elif abs(float(f) / pred - 1) * 100 > 2: print("RATE%+.0f%%" % (-abs(float(f) / pred - 1) * 100))
else: print("clean")
EOF
)
    # a whole-frame pass with a BANDED reading is still a failure, and that combination is
    # exactly the 93.33 MHz case that the whole-frame gate alone let through
    bandV=$(printf '%s' "$bands" | awk '{print $3}')
    [ "$verdict" = clean ] && [ "$bandV" != clean ] && verdict="BANDED($bandV)"
    log "      still $i: $bytes B ratio $(echo "$st" | awk '{print $7}') rsat $(echo "$st" | awk '{print $9}') hdiff $(echo "$st" | awk '{print $10}') | bands $bands -> $verdict"
    echo "$MUL,$pclk,$hts,$rowUs,$pred,$f,$imp,$i,$bytes,$(echo "$st" | awk '{print $7","$8","$9","$10}'),$(printf '%s' "$bands" | tr ' ' ','),$verdict" >> "$CSV"
  done
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"

ctl "framesize=$OTHER" > /dev/null; sleep 7
ctl "framesize=$IDX" > /dev/null; sleep 7
ctl "quality=$Q" > /dev/null
ctl "fps=$FPS_REQ" > /dev/null; sleep 4
log "== HD, 1440-row readout, VTS $VTS, clock PINNED at mul $MUL. Only the row time moves =="
ctl "camReg=0x5001,0xA3" > /dev/null; sleep 2
ctl "camReg=0x3813,0x00" > /dev/null; sleep 2
ctl "camReg=0x3807,0x8F" > /dev/null; sleep 3
ctl "camReg=0x5001,0x83" > /dev/null; sleep 2
ctl "camReg=0x380D,0xC4" > /dev/null; sleep 2      # HTS 2244 first: the LONGEST line, so the
                                                    # clock rise below lands on the safest row
ctl "camReg=0x380F,0xE2" > /dev/null; sleep 3      # VTS 738
ctl "camReg=0x3036,0x42" > /dev/null; sleep 2      # mul 66 on route A = 44 MHz, a slow transient
ctl "camReg=0x3108,0x11" > /dev/null; sleep 3      # route B -> 88 MHz
ctl "camReg=0x3036,$(printf '0x%02X' "$MUL")" > /dev/null; sleep 4
[ "$((0x$(regrd 0x3036)))" = "$MUL" ] && [ "$(regrd 0x3108)" = "11" ] || { log "ABORT: clock did not take"; exit 6; }
log "   pinned: 0x3036=$(regrd 0x3036) 0x3108=$(regrd 0x3108), VTS $(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))"

for h in $WALK; do rung "$h"; done

log "== result =="
column -s, -t < "$CSV"
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
by = {}
for r in rows:
    by.setdefault(r["hts"], []).append(r)
print("   HTS   row us  predicted  counted   verdicts")
cleanRows = []
for hts, rs in sorted(by.items(), key=lambda kv: -int(kv[0])):
    r = rs[0]
    vs = " ".join(x["verdict"] for x in rs)
    print("   %-5s %-7s %-10s %-9s %s" % (hts, r["rowUs"], r["predFps"], r["vsyncFps"], vs))
    if all(x["verdict"] == "clean" for x in rs):
        cleanRows.append((float(r["rowUs"]), int(hts), r["vsyncFps"]))
if cleanRows:
    us, hts, f = min(cleanRows)
    print("\n   96 MHz IS NOT ITSELF THE LIMIT: HTS %d ran clean at %s fps on a %.2f us row." % (hts, f, us))
    if float(f) >= 60:
        print("   And it cleared 60 fps at 720p. One run is not a soak - look at the frames.")
    else:
        print("   It did not reach 60 fps: the row time that stays clean is longer than 60 fps allows.")
else:
    print("\n   Every rung failed at 96 MHz, including rows measured clean at lower clocks.")
    print("   So the CLOCK is the limit here, not the row time.")
EOF
log "== done. Stills in $OUT =="
