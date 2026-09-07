#!/usr/bin/env bash
# Where does a long exposure stop DELIVERING frames? Walk the night slider and count.
#
# Why the stream is held open for the whole walk, not opened per point: settleSensor() runs at the
# END of processFrame(), and processFrame() returns early when esp_camera_fb_get() hands back NULL.
# So a session that has stopped delivering frames cannot be retimed OR left - the request is
# accepted, retimePending is raised, and nothing ever services it. Measured 7 Sep 2026: a 5 s
# session with nothing consuming logged "No frames" every 10 s for two and a half minutes, walked
# the rescue from quality 34 to 56, and ignored a 2500 ms request entirely. A viewer is also the
# realistic case - anyone using this panel has the page's viewfinder open.
#
#   BOARD=<addr> bash tools/bench/night_ceiling_probe.sh
# MSLIST overrides the rungs. Each rung: request it, hold a stream across the change, then count
# complete frames in a fixed window and read the registers back.
set -u
OUT=${OUT:-FPS_RECAL_stills/night_ceiling_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
IDX=${IDX:-23}
MSLIST=${MSLIST:-"3180 4000 4500 5000"}
WIN=${WIN:-40}     # stream window per rung, seconds
CSV="$OUT/ceiling.csv"
echo "reqMs,gotMs,hts,vts,expLines,r3503,winS,frames,partials,framesPerS,periodS,ratio,bytes,luma,hdiff" > "$CSV"

S0_SIZE=$(status_field framesize); S0_FPS=$(status_field fps)
S0_IDLE=$(status_field idleFps); S0_MIC=$(status_field micGain); S0_Q=$(status_field quality)
log "restore point: framesize=$S0_SIZE fps=$S0_FPS idleFps=$S0_IDLE micGain=$S0_MIC quality=$S0_Q"
restore() {
  log "== restore =="
  curl -s -m 25 "$B/control?nightExp=$IDX,0" > /dev/null; sleep 4
  # a stream gives the capture task the frames it needs to actually service the exit
  curl -s -m 12 "$B/sustain?stream=0" -o /dev/null 2>/dev/null || true
  sleep 4
  for k in framesize=$S0_SIZE fps=$S0_FPS idleFps=$S0_IDLE micGain=$S0_MIC quality=$S0_Q; do
    curl -s -m 25 "$B/control?$k" > /dev/null; sleep 0.4
  done
  sleep 3
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) idleFps=$(status_field idleFps) 0x3503=0x$(regrd 0x3503)"
}
trap restore EXIT

log "== night delivery ceiling: rungs $MSLIST, ${WIN}s stream window each, size idx $IDX =="

for ms in $MSLIST; do
  log "-- ${ms}ms --"
  # the request goes in FIRST, then the stream carries the frames the retime needs. The reply is
  # read but not trusted for the timing: it can return before the capture task has applied it
  R=$(ctl "nightExp=$IDX,$ms")
  # hold the stream across the change and for the counting window in one capture
  curl -s -m "$((WIN + 12))" "$B/sustain?stream=0" -o "$OUT/$ms.raw" 2>/dev/null || true
  got=$(python "$HERE/stream_grab.py" "$OUT/$ms.raw" "$OUT/$ms.jpg" 2>/dev/null) || got="0 0 0"
  read -r fbytes frames partials <<< "$got"
  # registers AFTER the window, so they describe the state the frames were taken in
  hts=$(( 16#$(regrd 0x380C)$(regrd 0x380D) ))
  vts=$(( 16#$(regrd 0x380E)$(regrd 0x380F) ))
  el=$(( ((16#$(regrd 0x3500) & 0x0F) << 16 | 16#$(regrd 0x3501) << 8 | 16#$(regrd 0x3502)) / 16 ))
  mode=$(regrd 0x3503)
  gotMs=$(printf '%s' "$R" | python -c "import json,sys; print(json.load(sys.stdin).get('gotMs',''))" 2>/dev/null)
  # frames per second over the window, and the ratio to the frame period the registers imply.
  # 1.0 means every frame the sensor produced was delivered; 0.5 means every other one
  perS=$(python -c "print('%.3f' % ($frames / float($WIN + 12)))")
  perd=$(python -c "print('%.2f' % ($vts * $hts * 2 / 10.1333e6))")
  ratio=$(python -c "p=$perd; print('%.2f' % ($perS * p)) if p > 0 else print('0')")
  luma=0; hdiff=999
  if [ -s "$OUT/$ms.jpg" ]; then
    read -r w h b mr mg mb r gs rs hd vd <<< "$(python "$HERE/still_color.py" "$OUT/$ms.jpg")"
    luma=$(python -c "print('%.1f' % (0.299*$mr + 0.587*$mg + 0.114*$mb))"); hdiff=$hd
  fi
  rm -f "$OUT/$ms.raw"
  log "   req ${ms}ms got ${gotMs}ms | HTS $hts VTS $vts exp $el lines 0x3503 0x$mode | $frames complete + $partials truncated in $((WIN + 12))s = $perS/s against a ${perd}s period (delivered/produced $ratio) | frame ${fbytes}B luma $luma hdiff $hdiff"
  echo "$ms,$gotMs,$hts,$vts,$el,$mode,$((WIN + 12)),$frames,$partials,$perS,$perd,$ratio,$fbytes,$luma,$hdiff" >> "$CSV"
done

log "== done: $CSV =="
column -s, -t < "$CSV"
