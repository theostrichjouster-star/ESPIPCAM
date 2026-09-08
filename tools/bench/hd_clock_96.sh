#!/usr/bin/env bash
# HD at 1440 rows, HTS 2112, VTS 738: walk the clock to 96 MHz and see where 60 fps arrives.
#
# THE POINT. With the readout cut to 720 binned rows and its measured VTS floor of 738, HD's rate
# is 96e6 / (2112 x 738) = 61.58 fps at the datasheet's own maximum pixel clock - so 60 fps at
# 720p, which is unreachable at 88 MHz by arithmetic, becomes reachable at 96 with room to spare.
# Table 8-5 puts fPCLK typical at 48 MHz and MAXIMUM at 96, so this is the edge of the part's
# specification rather than past it, and table 2-1's own 720p 60 fps mode calls for 96/192 MHz.
#
# WHAT SAYS IT WILL FAIL. This notebook records 92 MHz as column-striped and 96 as corrupt "by
# every route", and CLAUDE.md carries ~90 MHz as the sensor's digital-path cliff. Those were
# measured at 1280X960 on a different geometry, and today the neighbouring claim from the same
# campaign - the bistable magenta latch below a ~24 us row - failed to reproduce in 54 samples
# including 18 below its stated threshold. So the cliff is worth re-walking rather than assuming,
# but it is a real prior and 22.00 us at mul 72 is a shorter row than anything yet proven here.
#
# ALSO IN DOUBT IS THE RECEIVING END, and it is a different mechanism from anything the sensor
# does. Whatever breaks first, the instrument below cannot tell the sensor's digital path from the
# ESP32's DVP sampling - both show up as a corrupt or missing frame. The DVP registers are read
# out per rung so at least the sensor's own PCLK divider is on the record.
#
# THE WALK is the PLL multiplier alone on route B (0x3108 = 0x11 throughout, so no root-divider
# transient anywhere). VCO = mul x REFIN and REFIN is 6.67 MHz, so mul 72 puts the VCO at 480 MHz
# against section 2.5's 800 ceiling - in spec at every rung.
#
#   mul 66 -> 88.00 MHz -> 56.43 fps, 24.00 us row
#   mul 69 -> 92.00      -> 59.00,     22.96
#   mul 70 -> 93.33      -> 59.85,     22.63
#   mul 71 -> 94.67      -> 60.71,     22.30   <- 60 fps first cleared here
#   mul 72 -> 96.00      -> 61.58,     22.00
#
# The walk does NOT stop at the first failure: the prior is a cliff, and a cliff should be shown
# to be one rather than assumed after a single bad rung. Column striping is what hdiff catches -
# section 37 measured clean 1280x960 stills at 2.0-2.6, first green corruption 3.3, random noise
# 31, alternating-column stripes 121 - and a rate gate catches the half-rate mode that leaves the
# picture perfectly clean. Both are screens; the contact sheet is the gate.
#
#   BOARD=<addr> bash tools/bench/hd_clock_96.sh   [MULS="66 69 70 71 72"]
set -u
OUT=${OUT:-FPS_RECAL_stills/hd96_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; OTHER=${OTHER:-10}; FPS_REQ=${FPS_REQ:-52}; Q=${Q:-10}
MULS=${MULS:-"66 69 70 71 72"}
HTS=2112; VTS=738
SHOTS=${SHOTS:-2}

CSV="$OUT/hd96.csv"
echo "mul,pclkMHz,rowUs,predFps,vsyncFps,impliedMHz,pclkDiv,pclkMan,shot,bytes,W,H,ratio,gsat,rsat,hdiff,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  # multiplier back to something safe BEFORE leaving route B: mul 72 on route A dividers would be
  # 48 MHz which is harmless, but mul 120 under 0x3108 = 0x11 would be 160 - so PLL first here,
  # then the root dividers, which is the opposite order to a route-B ENTRY and deliberately so
  curl -s -m 25 "$B/control?camReg=0x3036,0x42" > /dev/null; sleep 1.5
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 1.5
  curl -s -m 25 "$B/control?camReg=0x3036,0x78" > /dev/null; sleep 1.5
  for kv in "camReg=0x380F,0xE8" "camReg=0x380D,0x0C" "camReg=0x3807,0xAF" "camReg=0x3813,0x08" "camReg=0x5001,0x83"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  curl -s -m 25 "$B/control?framesize=$OTHER" > /dev/null; sleep 7   # a REAL size change rewrites the window
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
  log "          0x3108=$(regrd 0x3108) 0x3036=$(regrd 0x3036) HTS=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) )) VTS=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))"
  log "          window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )) yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) ))"
}
trap restore EXIT

vsync_or_fail() {  # two counts, second wins, and a dry window logs a DIFFERENT line
  ctl "xclkStat=1" > /dev/null; sleep 11
  ctl "xclkStat=1" > /dev/null; sleep 11
  ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1
}

rung() {
  local mul=$1 pclk rowUs pred V f imp pdiv pman i fjpg bytes st verdict dims
  ctl "camReg=0x3036,$(printf '0x%02X' "$mul")" > /dev/null; sleep 4
  local got; got=$((0x$(regrd 0x3036)))
  [ "$got" = "$mul" ] || { anomaly "mul $mul reads $got"; return; }
  [ "$(regrd 0x3108)" = "11" ] || { log "ABORT: left route B"; exit 7; }
  pclk=$(python -c "print('%.2f' % ($mul * 2.0 / 3 * 2))")
  rowUs=$(python -c "print('%.2f' % ($HTS / ($mul * 2.0 / 3 * 2)))")
  pred=$(python -c "print('%.2f' % ($mul * 2.0 / 3 * 2 * 1e6 / ($HTS * $VTS)))")
  pdiv=$(regrd 0x3824); pman=$(regrd 0x4837)
  V=$(vsync_or_fail)
  if printf '%s' "$V" | grep -aq 'count failed'; then f=NOFRAMES; imp=-; else
    f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
    imp=$(printf '%s' "$V" | sed 's/.*implied PIXCLK \([0-9.]*\)MHz.*/\1/'); fi
  log "   mul $mul = ${pclk}MHz, row ${rowUs}us, predicted $pred fps: VSYNC $f (implied $imp), 0x3824=$pdiv 0x4837=$pman"
  for i in $(seq 1 "$SHOTS"); do
    fjpg="$OUT/mul${mul}_s${i}.jpg"
    http_gap; curl -s -m 25 -o "$fjpg" "$B/control?still=1"
    bytes=$(stat -c %s "$fjpg" 2>/dev/null || echo 0)
    st=$(python "$HERE/still_color.py" "$fjpg" 2>/dev/null) || st="0 0 0 0 0 0 0 0 0 999 999"
    dims="$(echo "$st" | awk '{print $1, $2}')"
    verdict=$(python - "$st" "$f" "$pred" <<'EOF'
import sys
p = sys.argv[1].split(); f, pred = sys.argv[2], float(sys.argv[3])
ratio, gsat, rsat, hdiff = float(p[6]), float(p[7]), float(p[8]), float(p[9])
if f == "NOFRAMES": print("NOFRAMES")
elif ratio == 0: print("NOSTILL")
elif hdiff > 25: print("STRIPED/NOISE")
elif ratio < 0.60: print("MAGENTA")
elif ratio > 1.25: print("GREENBLOW")
elif abs(float(f) / pred - 1) * 100 > 2:
    print("HALFRATE" if abs(float(f) / (pred / 2) - 1) * 100 <= 5 else "RATE%+.0f%%" % (-abs(float(f)/pred-1)*100))
else: print("clean")
EOF
)
    log "      still $i: $dims $bytes B ratio $(echo "$st" | awk '{print $7}') gsat $(echo "$st" | awk '{print $8}') hdiff $(echo "$st" | awk '{print $10}') -> $verdict"
    echo "$mul,$pclk,$rowUs,$pred,$f,$imp,$pdiv,$pman,$i,$bytes,$(echo "$st" | awk '{print $1","$2","$7","$8","$9","$10}'),$verdict" >> "$CSV"
  done
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"

ctl "framesize=$OTHER" > /dev/null; sleep 7
ctl "framesize=$IDX" > /dev/null; sleep 7
ctl "quality=$Q" > /dev/null
ctl "fps=$FPS_REQ" > /dev/null; sleep 4
log "== HD -> 1440-row readout, HTS $HTS, VTS $VTS, then the clock walked to 96 MHz =="
# geometry first, all at 80 MHz where every candidate line is long: scaler on so the offset change
# has a legal intermediate, offset, window, then the line and the frame
ctl "camReg=0x5001,0xA3" > /dev/null; sleep 2
ctl "camReg=0x3813,0x00" > /dev/null; sleep 2
ctl "camReg=0x3807,0x8F" > /dev/null; sleep 3
ctl "camReg=0x5001,0x83" > /dev/null; sleep 2
ctl "camReg=0x380D,0x40" > /dev/null; sleep 2      # HTS 2112, 26.4 us at 80 MHz
ctl "camReg=0x380F,0xE2" > /dev/null; sleep 3      # VTS 738, above the 720 rows read
# THE MULTIPLIER COMES DOWN BEFORE THE ROOT DIVIDERS HALVE. Writing 0x3108 = 0x11 with mul still
# at 120 would run the clock at 120 x 2/3 x 2 = 160 MHz until the next write caught up - far past
# the cliff this run exists to probe, and exactly the transient applyTunedTiming orders against.
# mul 66 on route A dividers is 44 MHz, so the intermediate is SLOW in both steps
ctl "camReg=0x3036,0x42" > /dev/null; sleep 2      # mul 66 -> 44 MHz while still on route A
ctl "camReg=0x3108,0x11" > /dev/null; sleep 3      # halved dividers double it to 88
log "   pre-walk check: HTS $(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) )) VTS $(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) )) window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )) yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) ))"

for m in $MULS; do rung "$m"; done

log "== result =="
column -s, -t < "$CSV"
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
seen = {}
for r in rows:
    seen.setdefault(r["mul"], []).append(r)
print("   mul   MHz    row us  predicted  counted    verdicts")
ok60 = []
for mul, rs in seen.items():
    r = rs[0]
    vs = " ".join(x["verdict"] for x in rs)
    print("   %-5s %-6s %-7s %-10s %-10s %s" % (mul, r["pclkMHz"], r["rowUs"], r["predFps"], r["vsyncFps"], vs))
    if all(x["verdict"] == "clean" for x in rs):
        try:
            if float(r["vsyncFps"]) >= 60: ok60.append((mul, r["pclkMHz"], r["vsyncFps"]))
        except ValueError:
            pass
if ok60:
    m, mhz, f = ok60[0]
    print("\n   60 fps REACHED and clean: mul %s = %s MHz counted %s fps at 720p." % (m, mhz, f))
    print("   That contradicts the ~90 MHz cliff. Soak it before believing it - the frames must be")
    print("   looked at, and a rate this close to the part's fPCLK maximum needs more than one run.")
else:
    print("\n   No clean rung reached 60 fps. The clock cliff stands on this geometry.")
EOF
log "== done. Stills in $OUT =="
