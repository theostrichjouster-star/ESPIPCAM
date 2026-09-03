#!/usr/bin/env bash
# Frame-window cliff for one size: the largest frame PROVEN to be delivered, and the first
# that is not. Method is BOARD_TESTING section 20, followed exactly because its two accidents
# both came from departing from it:
#   - descend the quality NUMBER from 63 (small frames) toward 6 (large), probing each step.
#     "Kill to q6 and let the watchdog converge" is INVALID - post-kill states show hysteresis.
#   - probe at ~5fps, never at a high rate: at high fps the SD saturates, the webserver starves
#     and the readings come back garbage. The cliff is CONSTANT per size across fps and clock,
#     which is what makes low-rate probing valid.
#   - NEVER take a still with the pattern on above VGA. The first HD q6 random still hard-hung
#     COM4 and needed a physical power cycle.
#   - an HTTP failure ABORTS. Treating an error as "alive" once walked FHD blind to q8 and
#     wedged the board. The watchdog protects against crossing the cliff, not against being
#     parked far beyond it.
#   BOARD=<addr> SIZE=<idx> NAME=<label> [FPS=5] bash tools/bench/frame_window_descend.sh
set -u
B=${BOARD:?set BOARD}
SIZE=${SIZE:?set SIZE=<framesize index>}
NAME=${NAME:?set NAME=<label>}
FPS=${FPS:-5}
OUT=${OUT:-FPS_RECAL_stills/window_$NAME}
mkdir -p "$OUT"
CSV="$OUT/descend.csv"
echo "quality,frameKB,govBoost,avgFrameBytes,frames,rescue,verdict" > "$CSV"

say() { echo "[$(date +%H:%M:%S)] $*"; }
# every fetch goes through here: an empty body or a curl failure aborts the run outright
get() {
  local r
  r=$(curl -s -m 20 "http://$B$1") || { say "ABORT: HTTP failed on $1"; bail; }
  [ -z "$r" ] && { say "ABORT: empty reply from $1"; bail; }
  printf '%s' "$r"
}
ctl() { curl -s -m 20 "http://$B/control?$1" >/dev/null || { say "ABORT: control $1 failed"; bail; }; }

bail() {
  say "restoring: pattern off, quality 10, field config"
  curl -s -m 20 "http://$B/control?camReg=0x503D,0x00" >/dev/null 2>&1
  curl -s -m 20 "http://$B/control?forceRecord=0" >/dev/null 2>&1
  curl -s -m 20 "http://$B/control?quality=10" >/dev/null 2>&1
  exit 9
}

ring() { curl -s -m 30 "http://$B/control?displayLog=1" 2>/dev/null | grep -a '' ; }

say "== $NAME (idx $SIZE) at ${FPS}fps, random pattern, descending quality =="
ctl "framesize=$SIZE"; sleep 8
ctl "record=0"; ctl "enableMotion=0"; ctl "idleFps=0"; ctl "fps=$FPS"; sleep 5
# pattern LAST and never change size after it: set_framesize reloads the whole register block
ctl "camReg=0x503D,0x81"; sleep 3
say "pattern on (0x503D=0x81)"

dead=0; lastAlive=""; lastAliveKB=0; q=${Q0:-63}
while [ "$q" -ge 6 ]; do
  get "/status" > /dev/null              # liveness gate before every step
  ctl "quality=$q"                       # re-assert: a rescue is sticky and rebases the governor
  sleep 2
  before=$(ring | grep -a -c "quality .* rescue" || true)
  ctl "forceRecord=1"
  sleep 3.5
  p=$(get "/control?updateFPS=1")        # aborts on failure rather than guessing alive
  kb=$(printf '%s' "$p" | python tools/bench/jfield.py frameKB)
  bo=$(printf '%s' "$p" | python tools/bench/jfield.py govBoost)
  sleep 1
  ctl "forceRecord=0"
  sleep 4
  L=$(ring); printf '%s\n' "$L" > "$OUT/q${q}.log"
  after=$(printf '%s\n' "$L" | grep -a -c "quality .* rescue" || true)
  resc=$(( after - before )); [ "$resc" -lt 0 ] && resc=0
  avg=$(printf '%s\n' "$L" | grep -a "Average frame length" | tail -1 | grep -o '[0-9]\+' | tail -1)
  frames=$(printf '%s\n' "$L" | grep -a "closeAvi\] Recorded" | tail -1 | sed 's/.*\///')
  [ -z "${avg:-}" ] && avg=0
  if [ "${kb:-0}" -gt 0 ] && [ "$resc" -eq 0 ]; then
    verdict=alive; dead=0; lastAlive=$q; lastAliveKB=$kb
  else
    verdict=dead; dead=$(( dead + 1 ))
  fi
  printf '%s,%s,%s,%s,%s,%s,%s\n' "$q" "${kb:-}" "${bo:-}" "$avg" "${frames:-}" "$resc" "$verdict" >> "$CSV"
  say "q$q: frameKB=${kb:-?} boost=${bo:-?} avgBytes=$avg rescues=$resc -> $verdict"
  [ "$dead" -ge 2 ] && { say "two consecutive dead steps - cliff bracketed, stopping"; break; }
  # Adaptive step: coarse while frames are small, single steps near the edge. The bands are
  # deliberately tight at the top because one quality step is worth a LOT of bytes at the big
  # sizes - QHD under the pattern went 755KB at q40, so a 6-step jump there would fly straight
  # past the maxFrameBuffSize gate and park the board deep in overload, which is the wedge
  # A DEAD step reports frameKB 0, which would send the size bands below straight back to the
  # coarsest jump and walk the board deeper into overload - the exact wedge vector section 20
  # warns about. Observed live on the QHD run: q37 died and the next step taken was q31.
  # Once anything has died, only ever move one step
  if [ "$verdict" = dead ]; then q=$(( q - 1 ))
  elif [ "${kb:-0}" -lt 200 ]; then q=$(( q - 6 ))
  elif [ "${kb:-0}" -lt 400 ]; then q=$(( q - 3 ))
  elif [ "${kb:-0}" -lt 600 ]; then q=$(( q - 2 ))
  else q=$(( q - 1 )); fi
done

say "largest PROVEN-delivered: q$lastAlive at ${lastAliveKB}KB"
bail
