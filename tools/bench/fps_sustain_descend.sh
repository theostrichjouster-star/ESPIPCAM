#!/usr/bin/env bash
# The rate each frame size can actually SUSTAIN, and therefore what its ceiling should be.
#
# Goal 2 of the retune brief (CLAUDE.md open item 1a): eliminate dropped frames by lowering each
# ceiling to what the pipeline carries rather than what the sensor can emit. A ceiling is a promise;
# 1280X960 promised 41 and delivered 38.1 (§38.32), which is the fault this closes.
#
# METHOD, and why it is a descend and not a sweep. Delivered fps at a size's ceiling IS an estimate
# of the sustainable rate - if the pipeline carried 38.1 of a requested 41, it can carry about 38 -
# so the walk jumps straight there rather than stepping down one rung at a time. That turns a
# 147-rung walk at QVGANARROW into a handful. Each candidate must then pass TWICE before it is
# accepted, because one clip is a sample and the thing being measured is scene- and card-dependent.
#
# WHAT THIS MEASURES IS THE ROOM AS MUCH AS THE BOARD. Frame size follows the scene, storage time
# follows frame size, and delivered fps follows storage time - the same reason gov_size_regress.sh
# reports delivered fps and refuses to gate on it. A ceiling set in a lit room is not proven for a
# dark one, where frames are larger. The run records avgKB per rung so the scene is on the record.
#
#   BOARD=<addr> bash tools/bench/fps_sustain_descend.sh
# SIZES_LIST overrides the set as name:idx:ceiling. DUR the clip length, TOL the fraction of the
# request that counts as sustained, CONFIRM how many passing clips accept a rate, MAXRUNGS the
# per-size bound, MINFREEGB the free-space floor that aborts the run.
set -u
OUT=${OUT:-FPS_RECAL_stills/fps_sustain_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

DUR=${DUR:-25}
TOL=${TOL:-0.99}
CONFIRM=${CONFIRM:-2}
MAXRUNGS=${MAXRUNGS:-8}
MINFREEGB=${MINFREEGB:-2.0}
Q=${Q:-10}

# the eleven the camera panel offers, ceilings from frameData maxTunedFPS
if [ -n "${SIZES_LIST:-}" ]; then read -r -a SIZES <<< "$SIZES_LIST"
else SIZES=("QVGANARROW:29:147" "VGANARROW:28:77" "HD:13:52" "1280X960:25:41" "QVGA:6:39" "VGA:10:39" \
            "FHDNARROW:16:16" "FHDMID:26:12" "FHDFULL:27:9" "QHD:20:9" "QSXGA:23:7"); fi

CSV="$OUT/rungs.csv"
SUM="$OUT/sustained.csv"
echo "size,idx,ceiling,reqFps,actFps,ratio,verdict,busy,boost,govWrites,storageMs,sdKBs,avgKB,rescue" > "$CSV"
echo "size,idx,oldCeiling,sustained,change" > "$SUM"

# The exit restore is a TRAP because bench_lib's ctl() and preflight() call exit directly - without
# it an abort leaves micGain and stillSave at 0, and that becomes the next run's captured start
# state (the §38.32 lesson, learned the expensive way).
S0=$(curl -s -m 25 "$B/status")
s0() { printf '%s' "$S0" | python "$HERE/jfield.py" "$1"; }
restore() {
  log "== restoring the start state =="
  for kv in "framesize=$(s0 framesize)" "fps=$(s0 fps)" "quality=$(s0 quality)" \
            "micGain=$(s0 micGain)" "stillSave=$(s0 stillSave)" "idleFps=$(s0 idleFps)" \
            "enableMotion=$(s0 enableMotion)" "record=0"; do
    curl -s -m 25 "$B/control?$kv" > /dev/null; sleep 1.2
  done
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) q=$(status_field quality) idleFps=$(status_field idleFps) micGain=$(status_field micGain) stillSave=$(status_field stillSave)"
}
trap restore EXIT

freegb() { curl -s -m 25 "$B/status" | python "$HERE/jfield.py" free_bytes | sed 's/GB//'; }

preflight_soft() {   # the campaign state, in the order this script needs it applied
  # The settle rule is NOT optional here. Run at 207s uptime this measured QVGA 39 at busy 99% with
  # 22 ms of MONITORING time - waiting inside esp_camera_fb_get - where the settled board reads 16%
  # and 2 ms. Every delivered-fps number this script produces would be depressed by it, and a
  # depressed number is exactly what it would then write into a ceiling.
  wait_settled "${SETTLE:-240}"
  assert_campaign_config "$Q"
  [ "$(status_field tunedFps)" = "1" ] || { log "ABORT: tunedFps is off"; exit 6; }
}

log "== sustainable rate per size: ${#SIZES[@]} sizes, ${DUR}s clips, tol $TOL, confirm $CONFIRM =="
log "start state: framesize=$(s0 framesize) fps=$(s0 fps) q=$(s0 quality) idleFps=$(s0 idleFps) micGain=$(s0 micGain)"
preflight_soft

rung() {  # rung <name> <idx> <ceiling> <fps> -> echoes "ratio act verdict"
  local name=$1 idx=$2 ceil=$3 fps=$4 R act ratio verdict busy boost gw sms sdkb avgb resc
  # Never hand the board a malformed control. The first version of this script walked with an empty
  # candidate and sent "/control?fps=" eight times inside five minutes, each with a framesize change
  # and a forced recording behind it - and COM4 went off the network and needed a power cycle plus a
  # peer reset to come back. A bad rate is a bug in this script, not something to send and find out.
  case "$fps" in ''|*[!0-9]*) log "   $name: REFUSING to record at rate '$fps'"; echo "0 0 badrate"; return;; esac
  if [ "$fps" -lt 1 ] || [ "$fps" -gt 200 ]; then
    log "   $name: REFUSING rate $fps (outside 1..200)"; echo "0 0 badrate"; return
  fi
  set_size "$idx" "$Q" "$fps"
  R=$(record_clip "$idx" "$fps" "$Q" "$DUR" "sus")
  act=$(kv "$R" actFps); busy=$(kv "$R" busy); boost=$(kv "$R" boost)
  gw=$(kv "$R" govWrites); sms=$(kv "$R" storageMs); sdkb=$(kv "$R" sdKBs)
  avgb=$(kv "$R" avgBytes); resc=$(kv "$R" rescue)
  if [ -z "$act" ]; then
    anomaly "$name @$fps: no closeAvi stats"
    echo "$name,$idx,$ceil,$fps,,,NOSTATS,,,,,,," >> "$CSV"
    echo "0 0 nostats"; return
  fi
  pass
  read -r ratio verdict <<< "$(python -c "
a=float('$act'); r=$fps
p=a/r if r else 0
print('%.4f %s' % (p, 'SUSTAINED' if p >= $TOL else 'SHORT'))")"
  local avgkb=$(python -c "print('%.0f' % (float('${avgb:-0}')/1024))" 2>/dev/null)
  log "   $name req $fps -> delivered $act (${ratio}) $verdict | busy ${busy}% boost $boost govWrites $gw | ${sms}ms/frame ${sdkb}kB/s ${avgkb}KB rescue $resc"
  echo "$name,$idx,$ceil,$fps,$act,$ratio,$verdict,$busy,$boost,$gw,$sms,$sdkb,$avgkb,$resc" >> "$CSV"
  echo "$ratio $act $verdict"
}

for s in "${SIZES[@]}"; do
  IFS=: read -r name idx ceil <<< "$s"
  fg=$(freegb)
  if python -c "import sys; sys.exit(0 if float('$fg') < $MINFREEGB else 1)"; then
    log "ABORT: free space ${fg}GB below the ${MINFREEGB}GB floor - clips are filling the card"; exit 7
  fi
  log "-- $name (idx $idx), ceiling $ceil, ${fg}GB free --"
  try=$ceil; passes=0; accepted=""
  for n in $(seq 1 "$MAXRUNGS"); do
    case "$try" in ''|*[!0-9]*) log "   $name: candidate '$try' is not a rate - stopping this size"; break;; esac
    [ "$try" -lt 1 ] && break
    # tail -1 because rung() also LOGS, and log() tees to stdout - without it the read takes the
    # log line as the result, every rung reads SHORT, and the walk records clips at an empty rate.
    # Measured: the first smoke run did exactly that seven times over
    read -r ratio act verdict <<< "$(rung "$name" "$idx" "$ceil" "$try" | tail -1)"
    if [ "$verdict" = "SUSTAINED" ]; then
      passes=$((passes + 1))
      [ "$passes" -ge "$CONFIRM" ] && { accepted=$try; break; }
      log "   $name: $try passed ($passes/$CONFIRM) - repeating to confirm"
      continue
    fi
    passes=0
    [ "$verdict" = "badrate" ] && { log "   $name: abandoning this size, the walk produced an invalid rate"; break; }
    [ "$verdict" = "nostats" ] && { try=$((try - 1)); [ "$try" -lt 1 ] && break; continue; }
    # the delivered rate IS the estimate of what the pipeline carries; never step up, always down
    next=$(python -c "
import math
a=float('$act'); t=$try
n=min(int(math.floor(a)), t-1)
print(max(n,1))")
    [ "$next" -ge "$try" ] && next=$((try - 1))
    [ "$next" -lt 1 ] && { log "   $name: walked to 1 without sustaining"; break; }
    log "   $name: $try short, next candidate $next"
    try=$next
  done
  if [ -n "$accepted" ]; then
    chg=$([ "$accepted" -eq "$ceil" ] && echo "keep" || echo "$ceil -> $accepted")
    log "== $name SUSTAINS $accepted (ceiling $ceil, $chg) =="
    echo "$name,$idx,$ceil,$accepted,$chg" >> "$SUM"
  else
    log "== $name: no rate confirmed within $MAXRUNGS rungs =="
    echo "$name,$idx,$ceil,,unresolved" >> "$SUM"
  fi
done

log "== done =="
column -s, -t < "$SUM"
log "rungs: $CSV"
