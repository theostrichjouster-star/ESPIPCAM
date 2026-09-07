#!/usr/bin/env bash
# What does one JPEG quality step actually BUY in frame size, storage time and delivered rate?
#
# The governor trades quality for delivery by raising the quality INDEX (higher index = smaller
# frame). GOV_STEP_GROWTH says a step is worth ~1.5x in frame size, but that constant was derived
# for the ease-down's safety gate, not measured as a rate lever. This measures the lever directly:
# one size, one rate, a ladder of base qualities, everything else held.
#
# It is the evidence for "let the user ask for a higher fps and let quality meet it" - if a step
# does not buy meaningful storage time, no amount of governor headroom will buy the rate either.
#
#   BOARD=<addr> SIZE=25 NAME=1280X960 FPS=34 QLIST="10 12 14 16 18" bash tools/bench/quality_buys_rate.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/quality_lever_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

SIZE=${SIZE:-25}
NAME=${NAME:-1280X960}
FPS=${FPS:-34}
DUR=${DUR:-15}
QLIST=${QLIST:-"10 12 14 16 18"}

CSV="$OUT/quality_lever.csv"
echo "size,fps,q,actFps,ratio,frameKB,storageMs,busy,boost,sdKBs,rescue" > "$CSV"

S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring =="
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.2
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality)"
}
trap restore EXIT

wait_settled "${SETTLE:-240}"
assert_campaign_config 10
[ "$(status_field tunedFps)" = "1" ] || { log "ABORT: tunedFps is off"; exit 6; }

log "== $NAME at $FPS fps: what a quality step buys, q in [$QLIST] =="
for q in $QLIST; do
  set_size "$SIZE" "$q" "$FPS"
  R=$(record_clip "$SIZE" "$FPS" "$q" "$DUR" "qlev")
  act=$(kv "$R" actFps); fkb=$(python -c "print('%.0f' % (float('$(kv "$R" avgBytes)')/1024))" 2>/dev/null)
  sms=$(kv "$R" storageMs); busy=$(kv "$R" busy); boost=$(kv "$R" boost)
  sdkb=$(kv "$R" sdKBs); resc=$(kv "$R" rescue)
  ratio=$(python -c "print('%.4f' % (float('${act:-0}')/$FPS))" 2>/dev/null)
  log "   q$q -> delivered $act (${ratio}) | frame ${fkb}KB | storage ${sms}ms | busy ${busy}% | boost $boost | ${sdkb}kB/s"
  echo "$NAME,$FPS,$q,$act,$ratio,$fkb,$sms,$busy,$boost,$sdkb,$resc" >> "$CSV"
done

log "== done =="
column -s, -t < "$CSV"
