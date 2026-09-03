#!/usr/bin/env bash
# Readout-row A/B for the scaler sizes. QVGA, VGA and 1280X960 share one 2624x1952 readout
# subsampled 2x2 to 1312x976 at VTS 984, which is why all three cap at 39.47fps. QVGA and VGA
# do not need those rows - the ISP throws most of them away - so reading fewer should raise
# their ceiling. 1280X960 is excluded on purpose: it outputs 960 rows and cannot lose any.
#
# VSYNC off the pin is the ground truth, not the computed figure: the whole question is
# whether the sensor honours a 4x vertical increment at all (datasheet section 3.2 claims
# only 2x binning, and beyond that pixels are skipped rather than averaged).
#   BOARD=<addr> bash tools/bench/subsample_ab.sh
set -u
B=${BOARD:?set BOARD}
OUT=FPS_RECAL_stills/subsample_ab
mkdir -p "$OUT"
CSV="$OUT/points.csv"
echo "size,idx,label,yFactor,vts,probeLine,vsyncFps,stillBytes,stillDims" > "$CSV"

ctl() { curl -s -m 25 "http://$B/control?$1" >/dev/null; }
ring() { curl -s -m 30 "http://$B/control?displayLog=1"; }

# xclkStat measures XCLK and VSYNC off the pins and is refused while capturing
vsync() { ctl "xclkStat=1"; sleep 4; ring | grep -a -i "xclkStat\|VSYNC" | tail -1; }

echo "-- campaign config (RAM only) --"
ctl "record=0"; ctl "enableMotion=0"; ctl "idleFps=0"; ctl "quality=10"
sleep 5

point() {  # point <name> <idx> <label> <yFactor> <vts>
  local name=$1 idx=$2 label=$3 yF=$4 vts=$5 probe="" vs="" sz dims
  ctl "framesize=$idx"; sleep 7
  ctl "fps=39"; sleep 6
  if [ "$label" != "baseline" ]; then
    # after framesize/fps, because set_framesize reloads the register block and the fps
    # tuning rewrites VTS - the probe has to be the last write
    ctl "subSample=$yF,$vts"; sleep 4
    probe=$(ring | grep -a "subSample:" | tail -1 | sed 's/^\[[^]]*\] //')
  else
    probe="(2x, driver VTS 984)"
  fi
  vs=$(vsync | sed 's/^\[[^]]*\] //')
  curl -s -m 30 "http://$B/control?still=1" > "$OUT/still_${name}_${label}.jpg"
  sz=$(wc -c < "$OUT/still_${name}_${label}.jpg")
  dims=$(python tools/bench/jpeg_dims.py "$OUT/still_${name}_${label}.jpg" 2>/dev/null || echo "?")
  echo "  $name/$label: $probe"
  echo "            vsync: $vs"
  echo "            still: $sz bytes, $dims"
  echo "$name,$idx,$label,$yF,$vts,\"$probe\",\"$vs\",$sz,$dims" >> "$CSV"
}

for spec in "QVGA:6" "VGA:10"; do
  IFS=: read -r name idx <<< "$spec"
  echo "== $name =="
  point "$name" "$idx" baseline 2 984
  point "$name" "$idx" sub4 4 496
done

echo "-- restoring: 2x subsample and the field config --"
ctl "framesize=6"; sleep 5; ctl "subSample=2,984"; sleep 3
ctl "framesize=13"; sleep 6; ctl "quality=10"; ctl "fps=30"; ctl "idleFps=5"; ctl "enableMotion=1"; ctl "record=1"
echo "== $CSV =="
cat "$CSV"
