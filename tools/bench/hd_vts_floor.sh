#!/usr/bin/env bash
# How low can HD's VTS go once the readout is only 720 binned rows?
#
# THIS IS THE WHOLE QUESTION, and the bisect (hd_window_bisect.sh) is what reduced it to this.
# Frame rate is PIXCLK / (HTS x VTS). Rows read do not appear in it. So shrinking HD's window
# from 1472 array rows to 1440 buys NOTHING on its own - measured, 54.845 against a 54.825
# baseline, unchanged to within the counter's noise - and its entire value is that it PERMITS a
# lower VTS, because VTS may never sit below the rows being read.
#
# The bisect also settled two things that were confounded before:
#   - A ZERO ISP offset is fine. It looked fatal because the step that introduced it left the
#     pre-scale at 736 against a 720 output for one stage; once the window shrank to match, the
#     sensor delivered a clean 69 KB still at ratio 1.002.
#   - VTS 728 IS FATAL. With 720 rows read it should be legal by the driver's own binned rule
#     (rows/2 + 8, which reproduces 744 at HD, 984 at VGA and XGA, 1968 at QSXGA) and by the
#     datasheet's "1296x728 with dummy" for 720p. It is not: no frames at all, the same total
#     failure applyHtsFloor records for dropping FHD's VTS from 1488 to 1340.
#
# So the eight-line blanking rule does not hold for a window the driver never programs, and the
# real floor is somewhere in 728 < VTS <= 744. Every step of the walk is worth 0.14% of frame
# rate, so where it lands decides whether this is worth 2.2%, 1.1% or nothing:
#
#   VTS 744 -> 54.86 fps    VTS 736 -> 55.45    VTS 732 -> 55.76    VTS 728 -> 56.06
#
# All at 88 MHz and HTS 2156. Each candidate is a single byte (0x02E8 / 0x02E4 / 0x02E0 / 0x02DC
# / 0x02DA / 0x02D8 all share the high byte), so there is no two-byte transient anywhere.
#
# A rung that delivers nothing is RECOVERED by writing VTS back up before the next one, rather
# than walking on through a dead sensor - the FHD note says restoring recovers immediately, and
# this checks that claim at every failure instead of assuming it.
#
#   BOARD=<addr> bash tools/bench/hd_vts_floor.sh   [WALK="744 740 736 732 730 728"]
set -u
OUT=${OUT:-FPS_RECAL_stills/hdvts_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; OTHER=${OTHER:-10}; FPS_REQ=${FPS_REQ:-52}; Q=${Q:-10}
WALK=${WALK:-"744 740 736 732 730 728"}
HTS=2156; PCLK=88

CSV="$OUT/vtsfloor.csv"
echo "scaler,vts,predFps,vsyncFps,recovered,stillBytes,ratio,gsat,rsat,hdiff,verdict" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  for kv in "camReg=0x380F,0xE8" "camReg=0x3807,0xAF" "camReg=0x3813,0x08" "camReg=0x5001,0x83" \
            "camReg=0x3108,0x26" "camReg=0x3036,0x78"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  # a REAL size change, because set_framesize only rewrites the window when the size changes
  curl -s -m 25 "$B/control?framesize=$OTHER" > /dev/null; sleep 7
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.5
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
  log "          window y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )) yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) )) VTS $(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) )) 0x5001=$(regrd 0x5001)"
}
trap restore EXIT

# TWO counts, second wins, and a failure must be readable as a failure. Both halves were learned
# the hard way today: grepping only for "VSYNC counted" returns the PREVIOUS rung's number when
# the board logs "VSYNC count failed" instead, which reported a dead sensor as a clean 54.836;
# and a single count after a timing change can read low for settling reasons
vsync_or_fail() {
  ctl "xclkStat=1" > /dev/null; sleep 11
  ctl "xclkStat=1" > /dev/null; sleep 11
  ramlog | grep -aE 'VSYNC counted|VSYNC count failed' | tail -1
}

rung() {  # rung <scalerLabel> <vts>
  local tag=$1 vts=$2 V f fjpg bytes st ratio gsat rsat hdiff verdict rec=-
  ctl "camReg=0x380F,$(printf '0x%02X' $((vts & 0xFF)))" > /dev/null; sleep 3
  local got=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))
  [ "$got" = "$vts" ] || { anomaly "VTS $vts did not read back (got $got)"; return; }
  V=$(vsync_or_fail)
  if printf '%s' "$V" | grep -aq 'count failed'; then f=NOFRAMES; else
    f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/'); fi
  fjpg="$OUT/${tag}_vts${vts}.jpg"
  http_gap; curl -s -m 25 -o "$fjpg" "$B/control?still=1"
  bytes=$(stat -c %s "$fjpg" 2>/dev/null || echo 0)
  st=$(python "$HERE/still_color.py" "$fjpg" 2>/dev/null) || st=""
  if [ -n "$st" ]; then
    ratio=$(echo "$st" | awk '{print $7}'); gsat=$(echo "$st" | awk '{print $8}')
    rsat=$(echo "$st" | awk '{print $9}'); hdiff=$(echo "$st" | awk '{print $10}'); verdict=ok
  else ratio=-; gsat=-; rsat=-; hdiff=-; verdict=NOSTILL; fi
  [ "$f" = "NOFRAMES" ] && verdict=NOFRAMES
  local pred; pred=$(python -c "print('%.2f' % (${PCLK}e6 / ($HTS * $vts)))")
  # THE RATE IS A GATE, not a reading. Below the blanking floor this sensor keeps producing
  # perfectly clean, complete frames and simply delivers HALF of them - counted 27.88 against
  # 55.76 predicted at VTS 732, with a still that decodes at 1280x720 and gates at ratio 1.012.
  # Nothing in the frame says anything is wrong, so only the count catches it. Note this is NOT
  # the split-frame trap of applyHtsFloor's warning, where the picture itself is seamed
  if [ "$verdict" = ok ]; then
    verdict=$(python -c "
f, p = float('$f'), float('$pred')
d = abs(f / p - 1) * 100
print('ok' if d <= 2 else ('HALFRATE' if abs(f / (p / 2) - 1) * 100 <= 5 else 'RATE%+.0f%%' % (-d)))")
  fi
  if [ "$verdict" != "ok" ]; then
    # put VTS back at once and prove the claim that restoring recovers immediately
    ctl "camReg=0x380F,0xE8" > /dev/null; sleep 4
    local V2; V2=$(vsync_or_fail)
    if printf '%s' "$V2" | grep -aq 'count failed'; then rec=NO; else
      rec=$(printf '%s' "$V2" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/'); fi
  fi
  log "   $tag VTS $vts (predicted $pred): VSYNC $f | still $bytes bytes ratio $ratio hdiff $hdiff | $verdict$([ "$rec" != - ] && echo "  [recovery at 744: $rec]")"
  echo "$tag,$vts,$pred,$f,$rec,$bytes,$ratio,$gsat,$rsat,$hdiff,$verdict" >> "$CSV"
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"

log "== HD at 88 MHz, HTS 2156, window cut to 1440 array rows = 720 binned. Where is the VTS floor? =="
ctl "framesize=$OTHER" > /dev/null; sleep 7
ctl "framesize=$IDX" > /dev/null; sleep 7
ctl "quality=$Q" > /dev/null
ctl "fps=$FPS_REQ" > /dev/null; sleep 4
ctl "camReg=0x380D,0x6C" > /dev/null; sleep 1
ctl "camReg=0x3036,0x42" > /dev/null; sleep 2
ctl "camReg=0x3108,0x11" > /dev/null; sleep 3
# the known-good route into the short window, from the bisect: scaler on first so the
# intermediate pre-scale mismatch is legal, then offset, then the window
ctl "camReg=0x5001,0xA3" > /dev/null; sleep 2
ctl "camReg=0x3813,0x00" > /dev/null; sleep 2
ctl "camReg=0x3807,0x8F" > /dev/null; sleep 3
log "   geometry: y $(( 0x$(regrd 0x3802) * 256 + 0x$(regrd 0x3803) ))..$(( 0x$(regrd 0x3806) * 256 + 0x$(regrd 0x3807) )), yOff $(( 0x$(regrd 0x3812) * 256 + 0x$(regrd 0x3813) )), HTS $(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))"

log "-- scaler ON (pre-scale 720 = output 720, a 1:1 pass) --"
for v in $WALK; do rung scalerON "$v"; done

log "-- scaler OFF (the shipped arrangement: a true 1:1 pass) --"
ctl "camReg=0x380F,0xE8" > /dev/null; sleep 2
ctl "camReg=0x5001,0x83" > /dev/null; sleep 3
for v in $WALK; do rung scalerOFF "$v"; done

log "== result =="
column -s, -t < "$CSV"
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
for tag in ("scalerON", "scalerOFF"):
    rs = [r for r in rows if r["scaler"] == tag]
    if not rs: continue
    ok = [r for r in rs if r["verdict"] == "ok"]
    bad = [r for r in rs if r["verdict"] != "ok"]
    if ok:
        lowest = min(ok, key=lambda r: int(r["vts"]))
        print("   %-9s lowest VTS that delivers: %s at %s fps (predicted %s)"
              % (tag, lowest["vts"], lowest["vsyncFps"], lowest["predFps"]))
    if bad:
        print("   %-9s failed at VTS %s" % (tag, ", ".join(r["vts"] for r in bad)))
    if ok:
        gain = 744.0 / int(min(ok, key=lambda r: int(r["vts"]))["vts"])
        print("   %-9s so the readout change is worth %+.1f%% against VTS 744" % (tag, (gain - 1) * 100))
EOF
