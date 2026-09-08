#!/usr/bin/env bash
# Do the new ceilings actually DELIVER? The register table says what the sensor was told; this
# says what reaches the card. One clip per point, fpsPriority left at its configured value so the
# answer describes the shipped behaviour rather than a bench-only one.
#   BOARD=<addr> POINTS="13:HD:56 25:1280X960:42" bash tools/bench/ceiling_delivery.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/ceildel_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
POINTS=${POINTS:-"13:HD:56 25:1280X960:42"}
DUR=${DUR:-25}; Q=${Q:-10}
CSV="$OUT/ceiling_delivery.csv"
echo "size,reqFps,actFps,ratio,frameKB,storageMs,busy,boost,govWrites,sdKBs,rescue" > "$CSV"
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
wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"
log "== delivered rate at the new ceilings, fpsPriority=$(status_field fpsPriority) =="
for p in $POINTS; do
  IFS=: read -r idx name fps <<< "$p"
  set_size "$idx" "$Q" "$fps"
  R=$(record_clip "$idx" "$fps" "$Q" "$DUR" "ceil")
  act=$(kv "$R" actFps)
  [ -z "$act" ] && { anomaly "$name: no stats"; continue; }
  pass
  fkb=$(python -c "print('%.1f' % (float('$(kv "$R" avgBytes)')/1024))" 2>/dev/null)
  ratio=$(python -c "print('%.3f' % (float('$act')/$fps))" 2>/dev/null)
  log "   $name @$fps -> delivered $act ($ratio of request) | frame ${fkb}KB | storage $(kv "$R" storageMs)ms | busy $(kv "$R" busy)% | boost $(kv "$R" boost) | writes $(kv "$R" govWrites) | rescue $(kv "$R" rescue)"
  echo "$name,$fps,$act,$ratio,$fkb,$(kv "$R" storageMs),$(kv "$R" busy),$(kv "$R" boost),$(kv "$R" govWrites),$(kv "$R" sdKBs),$(kv "$R" rescue)" >> "$CSV"
done
log "== done =="
column -s, -t < "$CSV"
