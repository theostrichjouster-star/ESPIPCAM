#!/usr/bin/env bash
# fpsPriority A/B: does trading quality actually buy the requested rate?
#
# One size, one rate, one scene, the toggle the only variable. ON should climb the governor boost
# past the old cap of 4 until frames fit the period and delivery reaches the request; OFF should
# pin at 4 and fall short, which is the behaviour before 7 Sep 2026.
#
# Each arm runs TWICE, alternating ON/OFF/ON/OFF rather than in blocks, because the scene drifts:
# an hour of failing light moved 1280X960's delivered rate by 3fps on its own, so two runs of the
# same setting back to back would confound the toggle with the room.
#
#   BOARD=<addr> POINTS="25:1280X960:41 13:HD:52" bash tools/bench/fps_priority_ab.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/fpspri_ab_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

DUR=${DUR:-20}
REPS=${REPS:-2}
Q=${Q:-10}
POINTS=${POINTS:-"25:1280X960:41 13:HD:52"}

CSV="$OUT/fpspri_ab.csv"
echo "size,reqFps,fpsPriority,rep,actFps,ratio,frameKB,storageMs,busy,boost,govWrites,sdKBs,rescue" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  for kv in "fpsPriority=$(s0 fpsPriority)" "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.2
  done
  log "restored: fpsPriority=$(status_field fpsPriority) framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
}
trap restore EXIT

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"
[ "$(status_field tunedFps)" = "1" ] || { log "ABORT: tunedFps is off"; exit 6; }
log "== fpsPriority A/B, ${DUR}s clips, $REPS reps, alternating so the scene cannot pick a side =="

for r in $(seq 1 "$REPS"); do
  for pri in 1 0; do
    ctl "fpsPriority=$pri" > /dev/null; sleep 1
    got=$(status_field fpsPriority)
    [ "$got" = "$pri" ] || { anomaly "fpsPriority did not take: asked $pri, read $got"; continue; }
    for p in $POINTS; do
      IFS=: read -r idx name fps <<< "$p"
      set_size "$idx" "$Q" "$fps"
      R=$(record_clip "$idx" "$fps" "$Q" "$DUR" "pri${pri}r${r}")
      act=$(kv "$R" actFps)
      [ -z "$act" ] && { anomaly "$name pri=$pri rep$r: no stats"; continue; }
      pass
      fkb=$(python -c "print('%.0f' % (float('$(kv "$R" avgBytes)')/1024))" 2>/dev/null)
      ratio=$(python -c "print('%.4f' % (float('$act')/$fps))" 2>/dev/null)
      log "   $name @$fps  fpsPriority=$pri rep$r -> delivered $act (${ratio}) | frame ${fkb}KB | storage $(kv "$R" storageMs)ms | busy $(kv "$R" busy)% | BOOST $(kv "$R" boost) | writes $(kv "$R" govWrites)"
      echo "$name,$fps,$pri,$r,$act,$ratio,$fkb,$(kv "$R" storageMs),$(kv "$R" busy),$(kv "$R" boost),$(kv "$R" govWrites),$(kv "$R" sdKBs),$(kv "$R" rescue)" >> "$CSV"
    done
  done
done

log "== done =="
column -s, -t < "$CSV"
