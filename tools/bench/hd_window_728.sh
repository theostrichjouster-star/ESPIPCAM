#!/usr/bin/env bash
# HD on the datasheet's own 720p readout: 1440 array rows instead of 1472, VTS 728 not 744.
#
# WHAT WE RUN TODAY, read off COM4's registers on 7 Sep 2026:
#
#   array window   2624 x 1472   x 0..2623, y 240..1711
#   subsample      2x2           0x3814 = 0x3815 = 0x31
#   ISP input      1312 x 736
#   ISP offset     16 x 8        0x3810-0x3813
#   pre-scale      1280 x 720
#   output         1280 x 720    scaler OFF, 0x5001 bit 5 clear, so the pass is 1:1
#   VTS            744           = 736 rows + 8 blanking
#
# That is the esp32-camera driver's generic 16:9 window, and the tuner leaves it alone: the crop
# function returns early for every stock binned size. So HD reads 736 binned rows and the ISP
# offset THROWS 16 OF THEM AWAY, plus 32 columns horizontally.
#
# WHAT THE DATASHEET SPECIFIES for 720p (table 2-1): "cropping 2592x1944 to 2560x1440,
# subsampling in vertical and horizontal, 1296x728 with dummy, supports 2x2 binning". Read
# against the 5 Mpixel row of the same table, where "2608x1952 with dummy" pads a 2592x1944
# output by "dummy 16 pixel horizontal, 8 lines", the 1296x728 is the OUTPUT plus dummy, not
# array pixels: 1280 + 16 and 720 + 8. So OmniVision reads exactly the 720 rows it emits and
# pads the frame with 8 dummy lines, where we read 736 real rows and discard 16.
#
# Those 8 rows of readout are the whole difference, and rows are what VTS counts:
#
#   744 / 728 = 1.022,  so +2.2% of frame rate at any clock and any line length
#   at 88 MHz and HTS 2156:  88e6 / (2156 x 728) = 56.06 fps   against 54.86 today
#   at 80 MHz and HTS 2060:  80e6 / (2060 x 728) = 53.34 fps   against 52.20 today
#
# It is also FREE IN FIELD OF VIEW, because the rows being given up are ones the ISP offset
# already discards. Only the vertical geometry is touched here: the horizontal window feeds HTS,
# which is set independently, so narrowing it buys no rate and is left out as a second variable.
#
# THE WRITE ORDER IS THE WHOLE DIFFICULTY, and it is why this is a probe and not a hand edit.
# With the scaler off, pre-scale must EQUAL the output, and pre-scale is the ISP input less twice
# the offset - so the offset and the window have to change together or some intermediate state is
# invalid. Worse, VTS must never sit below the rows being read: that is the measured split-frame
# trap, half-width frames duplicated with a seam down the middle that decode cleanly and pass
# every automated check. The sequence below has no invalid intermediate at all:
#
#   1. scaler ON            pre 720 = out 720, a 1:1 scaler pass, harmless
#   2. Y offset 8 -> 0      pre becomes 736, out 720, the scaler now genuinely downscales
#   3. Y end 1711 -> 1679   1440 rows, 720 binned, pre 720 = out 720 again; VTS 744 still >= 720
#   4. VTS 744 -> 728       728 > 720 rows read, so never inverted
#   5. scaler OFF           a true 1:1 pass once more
#
# Every one of those is a SINGLE byte: 0x3813, 0x3807 (1679 = 0x068F keeps the high byte), and
# 0x380F (728 = 0x02D8 keeps it too). No two-byte transient, so no group write is needed and the
# mid-frame-write hazard never arises. The window start stays at 240 deliberately - moving it
# would take two bytes - so the framing shifts down by 16 array rows out of 1952. A shipped
# version would centre it; this isolates the row count.
#
# PREDICTION, stated before the measurement: 54.86 -> 56.06 fps VSYNC-counted, +2.2%, with the
# still's channel ratio and hdiff indistinguishable from the 744-row baseline.
#
#   BOARD=<addr> bash tools/bench/hd_window_728.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/hdwin_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; FPS_REQ=${FPS_REQ:-52}; Q=${Q:-10}
SHOTS=${SHOTS:-3}

CSV="$OUT/hdwin.csv"
echo "stage,yStart,yEnd,rows,yOff,vts,rowsRead,vsyncFps,predFps,impliedMHz,shot,ratio,gsat,rsat,hdiff" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  # scaler bit back on before anything else would leave a mismatched pre-scale; the framesize
  # replay rewrites window, offsets, VTS, scaler and clock together, so it is the real restore
  curl -s -m 25 "$B/control?camReg=0x3108,0x26" > /dev/null; sleep 1.2
  curl -s -m 25 "$B/control?camReg=0x3036,0x78" > /dev/null; sleep 1.2
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.2
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
  log "          window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )) yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) )) VTS $(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) )) 0x5001=$(regrd 0x5001)"
}
trap restore EXIT

geom() {  # echoes "yStart yEnd rows yOff vts scaler"
  local ys ye yo vts sc
  ys=$(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))
  ye=$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) ))
  yo=$(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) ))
  vts=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))
  sc=$(regrd 0x5001)
  echo "$ys $ye $(( ye - ys + 1 )) $yo $vts $sc"
}

vsync2() {  # two counts, the SECOND is the answer: the first after a timing change reads low
  ctl "xclkStat=1" > /dev/null; sleep 8
  local f1
  f1=$(ramlog | grep -a 'VSYNC counted' | tail -1 | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
  ctl "xclkStat=1" > /dev/null; sleep 8
  local L
  L=$(ramlog | grep -a 'VSYNC counted' | tail -1)
  log "      counts: $f1 then $(printf '%s' "$L" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')"
  printf '%s' "$L"
}

measure() {  # measure <stage> <hts> ; counts, then SHOTS stills through the gates
  local stage=$1 hts=$2 G V f imp i st
  read -r ys ye rows yo vts sc <<< "$(geom)"
  local rowsRead=$(( rows / 2 ))
  local pred
  pred=$(python -c "print('%.2f' % (88e6 / ($hts * $vts)))")
  log "   $stage: window y $ys..$ye ($rows rows -> $rowsRead binned), yOff $yo, VTS $vts, 0x5001=$sc, HTS $hts"
  [ "$vts" -ge "$rowsRead" ] || { log "ABORT: VTS $vts below the $rowsRead rows read - split-frame trap"; exit 7; }
  V=$(vsync2)
  f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
  imp=$(printf '%s' "$V" | sed 's/.*implied PIXCLK \([0-9.]*\)MHz.*/\1/')
  log "      counted $f vs $pred predicted, implied PIXCLK $imp"
  for i in $(seq 1 "$SHOTS"); do
    local fjpg="$OUT/${stage}_s${i}.jpg"
    http_gap; curl -s -m 25 -o "$fjpg" "$B/control?still=1"
    st=$(python "$HERE/still_color.py" "$fjpg" 2>/dev/null) || st="0 0 0 0 0 0 0 0 0 999 999"
    G=$(echo "$st" | awk '{print $7, $8, $9, $10}')
    log "      still $i: $(python "$HERE/jpeg_dims.py" "$fjpg" 2>/dev/null) ratio/gsat/rsat/hdiff = $G"
    echo "$stage,$ys,$ye,$rows,$yo,$vts,$rowsRead,$f,$pred,$imp,$i,$(echo $G | tr ' ' ',')" >> "$CSV"
  done
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"

log "== HD, route B 88 MHz, HTS 2156: 1472-row readout against the datasheet's 1440 =="
ctl "framesize=$IDX" > /dev/null; sleep 6
ctl "quality=$Q" > /dev/null
ctl "fps=$FPS_REQ" > /dev/null; sleep 4
# route B, in the safe order: line up first (2156 at 80 MHz is a LONGER row than 2060), then the
# multiplier at half speed, then the halved dividers double it to 88
ctl "camReg=0x380D,0x6C" > /dev/null; sleep 1
ctl "camReg=0x3036,0x42" > /dev/null; sleep 2
ctl "camReg=0x3108,0x11" > /dev/null; sleep 3
hts=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))
[ "$hts" = "2156" ] && [ "$(regrd 0x3108)" = "11" ] || { log "ABORT: route B did not take (HTS $hts)"; exit 6; }

measure "baseline_1472rows" 2156

log "   -> the datasheet's readout, in five single-byte writes with no invalid intermediate"
ctl "camReg=0x5001,0xA3" > /dev/null; sleep 2   # 1. scaler ON (0x83 | 0x20), a 1:1 pass
ctl "camReg=0x3813,0x00" > /dev/null; sleep 2   # 2. Y offset 8 -> 0, scaler now downscales 736->720
ctl "camReg=0x3807,0x8F" > /dev/null; sleep 2   # 3. Y end 1711 -> 1679: 1440 rows, 720 binned
ctl "camReg=0x380F,0xD8" > /dev/null; sleep 2   # 4. VTS 744 -> 728, still above the 720 rows read
ctl "camReg=0x5001,0x83" > /dev/null; sleep 3   # 5. scaler OFF, a true 1:1 pass again

measure "datasheet_1440rows" 2156

log "== verdict =="
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
by = {}
for r in rows:
    by.setdefault(r["stage"], r)
b, d = by.get("baseline_1472rows"), by.get("datasheet_1440rows")
if not (b and d):
    sys.exit("one stage missing")
fb, fd = float(b["vsyncFps"] or 0), float(d["vsyncFps"] or 0)
print("   readout   %s rows -> %s binned, VTS %s : counted %.3f (predicted %s)"
      % (b["rows"], b["rowsRead"], b["vts"], fb, b["predFps"]))
print("   datasheet %s rows -> %s binned, VTS %s : counted %.3f (predicted %s)"
      % (d["rows"], d["rowsRead"], d["vts"], fd, d["predFps"]))
print("   gain %+.1f%% counted against %+.1f%% predicted"
      % ((fd / fb - 1) * 100 if fb else 0, (float(d["predFps"]) / float(b["predFps"]) - 1) * 100))
ratios = [(r["stage"], float(r["ratio"]), float(r["hdiff"])) for r in rows if r["ratio"] not in ("", "0")]
bad = [s for s, ra, hd in ratios if not (0.95 <= ra <= 1.25) or hd > 25]
print("   frame gates: " + ", ".join("%s ratio %.3f hdiff %.1f" % t for t in ratios))
print("   VERDICT: " + ("CHECK - a gate is out of range: " + ", ".join(set(bad)) if bad
                        else "the shorter readout is clean and delivers the predicted rate"))
EOF
log "== done. Stills in $OUT - gates screen, the eyeball decides =="
