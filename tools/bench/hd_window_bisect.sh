#!/usr/bin/env bash
# WHICH of the five writes to HD's readout stops the sensor delivering frames?
#
# hd_window_728.sh applied the datasheet's 720p geometry in five single-byte writes and then
# measured. The result was not the predicted +2.2%: the board produced NO FRAMES AT ALL - two
# "VSYNC count failed - no frames inside 10s" lines and the no-frame rescue walking quality 12
# through 28 - while the stills came back zero bytes. That script also misreported it as a clean
# 54.836, because it read the last "VSYNC counted" line in the ring and the failures log a
# DIFFERENT line, so the number it found was the previous stage's. That is fixed here: a stage
# with no fresh count is reported as NOFRAMES, never as the stale one.
#
# So this measures after EVERY write instead of after all five, which is the only way to say
# which one did it. The five, in the order they were applied:
#
#   1. scaler ON        0x5001 0x83 -> 0xA3   a 1:1 pass, should change nothing
#   2. Y offset 8 -> 0  0x3813               pre-scale becomes 736, the scaler now downscales
#   3. Y end 1711->1679 0x3807               1440 rows, 720 binned, pre-scale 720 again
#   4. VTS 744 -> 728   0x380F               8 blanking lines, which is what the driver's own
#                                            binned rule (rows/2 + 8) and the datasheet's
#                                            "1296x728 with dummy" both say is correct
#   5. scaler OFF       0x5001 0xA3 -> 0x83  a true 1:1 pass once more
#
# Prior art says step 4 is the suspect: applyHtsFloor's comment records that dropping FHD's VTS
# from 1488 to 1340 "stopped frame output entirely, and restoring it recovered immediately", and
# that a VTS below the rows read gives seamed half-width frames rather than none. But 728 is not
# below 720, so if step 4 is the one that bites then the eight-line blanking rule is wrong for a
# window the driver never programs, which is worth knowing on its own.
#
# EVERY STAGE IS UNDONE IN REVERSE at the end, and then the framesize is cycled through another
# size and back. That cycle is not optional: set_framesize only rewrites the window when the size
# actually CHANGES, so replaying framesize=13 over itself leaves a hand-modified window in place -
# which is how the previous run left the board on a 1440-row readout.
#
#   BOARD=<addr> bash tools/bench/hd_window_bisect.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/hdbisect_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; OTHER=${OTHER:-10}; FPS_REQ=${FPS_REQ:-52}; Q=${Q:-10}

CSV="$OUT/bisect.csv"
echo "step,what,yStart,yEnd,rows,rowsRead,yOff,vts,scaler,vsyncFps,stillBytes,ratio,hdiff,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring: registers back in reverse, then a REAL framesize change =="
  for kv in "camReg=0x5001,0xA3" "camReg=0x380F,0xE8" "camReg=0x3807,0xAF" "camReg=0x3813,0x08" "camReg=0x5001,0x83" \
            "camReg=0x3108,0x26" "camReg=0x3036,0x78"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  curl -s -m 25 "$B/control?framesize=$OTHER" > /dev/null; sleep 7
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
  log "          window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )) yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) )) 0x5001=$(regrd 0x5001)"
}
trap restore EXIT

# a count that can say NOFRAMES. The board logs "VSYNC counted:" on success and "VSYNC count
# failed" on a dry 10 s window, so the ring must be checked for BOTH and the newer one wins -
# grepping only for success silently returns the previous stage's number
vsync_or_fail() {
  ctl "xclkStat=1" > /dev/null; sleep 11
  ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1
}

stage() {  # stage <n> <what>
  local n=$1 what=$2 ys ye yo vts sc V f bytes st ratio hdiff verdict fjpg
  ys=$(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))
  ye=$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) ))
  yo=$(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) ))
  vts=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))
  sc=$(regrd 0x5001)
  V=$(vsync_or_fail)
  if printf '%s' "$V" | grep -aq 'count failed'; then f="NOFRAMES"; else
    f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/'); fi
  fjpg="$OUT/step${n}.jpg"
  http_gap; curl -s -m 25 -o "$fjpg" "$B/control?still=1"
  bytes=$(stat -c %s "$fjpg" 2>/dev/null || echo 0)
  st=$(python "$HERE/still_color.py" "$fjpg" 2>/dev/null) || st=""
  if [ -n "$st" ]; then
    ratio=$(echo "$st" | awk '{print $7}'); hdiff=$(echo "$st" | awk '{print $10}'); verdict=ok
  else ratio=-; hdiff=-; verdict=NOSTILL; fi
  [ "$f" = "NOFRAMES" ] && verdict=NOFRAMES
  log "   step $n $what: y $ys..$ye ($(( ye - ys + 1 )) rows -> $(( (ye - ys + 1) / 2 )) binned) yOff $yo VTS $vts 0x5001=$sc"
  log "          -> VSYNC $f | still $bytes bytes ratio $ratio hdiff $hdiff | $verdict"
  echo "$n,$what,$ys,$ye,$(( ye - ys + 1 )),$(( (ye - ys + 1) / 2 )),$yo,$vts,$sc,$f,$bytes,$ratio,$hdiff,$verdict" >> "$CSV"
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"

log "== HD, route B 88 MHz HTS 2156: which write stops the frames? =="
ctl "framesize=$OTHER" > /dev/null; sleep 7          # force a real reload so the window is stock
ctl "framesize=$IDX" > /dev/null; sleep 7
ctl "quality=$Q" > /dev/null
ctl "fps=$FPS_REQ" > /dev/null; sleep 4
ctl "camReg=0x380D,0x6C" > /dev/null; sleep 1        # HTS 2156, a LONGER line at 80 MHz
ctl "camReg=0x3036,0x42" > /dev/null; sleep 2        # mul 66, half speed
ctl "camReg=0x3108,0x11" > /dev/null; sleep 3        # halved dividers double it to 88
[ "$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))" = "2156" ] || { log "ABORT: HTS did not take"; exit 6; }

stage 0 "baseline_1472rows_VTS744"
ctl "camReg=0x5001,0xA3" > /dev/null; sleep 3; stage 1 "scalerON"
ctl "camReg=0x3813,0x00" > /dev/null; sleep 3; stage 2 "yOffset0"
ctl "camReg=0x3807,0x8F" > /dev/null; sleep 3; stage 3 "window1440rows"
ctl "camReg=0x380F,0xD8" > /dev/null; sleep 3; stage 4 "VTS728"
ctl "camReg=0x5001,0x83" > /dev/null; sleep 3; stage 5 "scalerOFF"

log "== result =="
column -s, -t < "$CSV"
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
first = next((r for r in rows if r["verdict"] != "ok"), None)
if first is None:
    good = [r for r in rows if r["vsyncFps"] not in ("NOFRAMES", "")]
    print("   every stage delivered. Rates: " + ", ".join("%s %s" % (r["what"], r["vsyncFps"]) for r in good))
else:
    print("   frames stop at step %s, %s - every earlier stage delivered" % (first["step"], first["what"]))
    print("   that stage: %s rows read, VTS %s, yOffset %s, 0x5001 %s"
          % (first["rowsRead"], first["vts"], first["yOff"], first["scaler"]))
EOF
