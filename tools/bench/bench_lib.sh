#!/bin/bash
# Shared helpers for the bench sweeps (Git Bash + curl + python). Source this file.
#   BOARD=<address> bash tools/bench/fps_t1_register.sh
# The board address comes from the environment and is never written into the repo.
# Every loop here is bounded; every control call retries once then aborts the run.
set -u
: "${BOARD:?set BOARD=<board address> in the environment}"
B="http://$BOARD"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-FPS_RECAL_stills/retune_$(date +%Y%m%d)}"
mkdir -p "$OUT"
LOGF="$OUT/campaign.log"
ANOM=0   # consecutive anomalies; two in a row abort the tier (FPS_RECAL method)

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOGF"; }

# ctl key=value : one retry after 5 s, then abort. Prints the response body
ctl() {
  local r
  if ! r=$(curl -s -m 25 "$B/control?$1"); then
    sleep 5
    r=$(curl -s -m 25 "$B/control?$1") || { log "ABORT: control $1 failed twice"; exit 3; }
  fi
  printf '%s' "$r"
}

# the RTC ring: 7KB, wraps in ~95 lines, carries non-text bytes (hence grep -a)
ramlog() { curl -s -m 40 "$B/control?displayLog=1" | grep -a ''; }

# /status field, with the stray control characters the board sometimes emits stripped
status_field() { curl -s -m 15 "$B/status" | python "$HERE/jfield.py" "$1"; }

uptime_s() {
  local u d t h m s
  u=$(status_field up_time); [ -z "$u" ] && { echo 0; return; }
  d=${u%%-*}; t=${u#*-}; IFS=: read -r h m s <<< "$t"
  echo $(( 10#$d * 86400 + 10#$h * 3600 + 10#$m * 60 + 10#$s ))
}

# a reboot between points invalidates the tier: uptime must only ever grow
assert_alive() {
  local now prev
  now=$(uptime_s); prev=$(cat "$OUT/.uptime" 2>/dev/null || echo 0)
  if [ "$now" -lt "$prev" ]; then
    log "ABORT: board rebooted (uptime $prev -> $now). Crash snapshot, if any:"
    ramlog | grep -a 'crash_\|printResetReason\|Previous' | tail -4 | tee -a "$LOGF"
    exit 4
  fi
  echo "$now" > "$OUT/.uptime"
}

wait_settled() {  # discard the first minutes after any boot (bench rule); bounded 10 min
  local need=${1:-240} i u
  for i in $(seq 1 60); do u=$(uptime_s); [ "$u" -ge "$need" ] && { log "settled: uptime ${u}s"; return 0; }; sleep 10; done
  log "ABORT: uptime never reached ${need}s"; exit 5
}

# the campaign configuration, RAM only (never save=1 mid-campaign). record=0 only after the
# 60 s brownout holdoff window, whose expiry would restore doRecording from the boot value
assert_campaign_config() {
  local q=${1:-10}
  [ "$(uptime_s)" -gt 70 ] || wait_settled 75
  ctl enableMotion=0 > /dev/null
  ctl record=0 > /dev/null
  ctl idleFps=0 > /dev/null      # idleThrottle would retime the sensor to idleFps after 10 s idle
  ctl micGain=0 > /dev/null      # audio bytes would swamp frame length at 1-2 fps
  ctl "quality=$q" > /dev/null
  sleep 2
  local r f i
  r=$(status_field record); f=$(status_field idleFps); i=$(uptime_s)
  log "campaign config: record=$r idleFps=$f quality=$q uptime=${i}s tunedFps=$(status_field tunedFps) sdBusDiv=$(status_field sdBusDiv)"
  [ "$r" = "0" ] && [ "$f" = "0" ] || { log "ABORT: campaign config did not take"; exit 6; }
}

preflight() {  # refuse to sweep a board that is not in the campaign state
  local u; u=$(uptime_s)
  [ "$u" -ge 240 ] || { log "ABORT: uptime ${u}s < 240 (settle rule)"; exit 6; }
  [ "$(status_field record)" = "0" ] || { log "ABORT: record is on"; exit 6; }
  [ "$(status_field idleFps)" = "0" ] || { log "ABORT: idleFps is not 0"; exit 6; }
  [ "$(status_field tunedFps)" = "1" ] || { log "ABORT: tunedFps is off"; exit 6; }
  echo "$u" > "$OUT/.uptime"
}

anomaly() { ANOM=$((ANOM + 1)); log "ANOMALY ($ANOM): $*"; [ "$ANOM" -ge 2 ] && { log "ABORT: two consecutive anomalies"; exit 2; }; }
pass() { ANOM=0; }

# framesize resets captureFPS: always size first, then quality, then fps
set_size() { ctl "framesize=$1" > /dev/null; sleep 6; ctl "quality=$2" > /dev/null; ctl "fps=$3" > /dev/null; sleep 4; }

# the retime line for a request, retried while the capture task gets round to it (bounded)
get_retime() {  # get_retime <size name> <fps> <ceiling> <regime> <logfile> -> t1_point.py row on stdout
  local size=$1 fps=$2 ceil=$3 regime=$4 logf=$5 i L row rc
  for i in 1 2 3 4; do
    L=$(ramlog); printf '%s\n' "$L" > "$logf"
    row=$(printf '%s\n' "$L" | python "$HERE/t1_point.py" "$size" "$fps" "$ceil" "$regime" "FPS_RECAL_stills/sweep.csv"); rc=$?
    [ "$rc" -ne 2 ] && { printf '%s\n' "$row"; return $rc; }
    sleep 2
  done
  printf '%s\n' "$row"; return 2
}

motion_counts() { ctl motionStats=1 > /dev/null; sleep 1; ramlog | python "$HERE/parse_motion.py"; }

# record_clip <idx> <fps> <q> <dur s> <tag> : forced recording, stats from the LAST closeAvi
# block, staleness by filename, still saved. Prints key=value lines
record_clip() {  # optional 6th arg "probe": updateFPS frameKB/govBoost at 3.5 s and 10 s into the clip
  local idx=$1 fps=$2 q=$3 dur=$4 tag=$5 probe=${6:-} prev new L i m0 m1 p1="" p2=""
  m0=$(motion_counts)
  prev=$(ramlog | grep -a 'closeAvi\] Recorded' | tail -1)
  ctl forceRecord=1 > /dev/null
  if [ -n "$probe" ] && [ "$dur" -ge 15 ]; then
    sleep 3.5; p1=$(curl -s -m 10 "$B/control?updateFPS=1" | python "$HERE/jfield.py" frameKB)/$(curl -s -m 10 "$B/control?updateFPS=1" | python "$HERE/jfield.py" govBoost)
    sleep 6.5; p2=$(curl -s -m 10 "$B/control?updateFPS=1" | python "$HERE/jfield.py" frameKB)/$(curl -s -m 10 "$B/control?updateFPS=1" | python "$HERE/jfield.py" govBoost)
    sleep $((dur - 10))
  else sleep "$dur"; fi
  ctl forceRecord=0 > /dev/null
  echo "probe1=$p1"; echo "probe2=$p2"
  for i in $(seq 1 30); do
    sleep 1; L=$(ramlog); new=$(printf '%s\n' "$L" | grep -a 'closeAvi\] Recorded' | tail -1)
    [ -n "$new" ] && [ "$new" != "$prev" ] && break
  done
  printf '%s\n' "$L" > "$OUT/${tag}_${idx}_${fps}_q${q}.log"
  if [ "$new" = "$prev" ] || [ -z "$new" ]; then echo "file=NONE"; return 1; fi
  printf '%s\n' "$L" | python "$HERE/parse_avi.py"
  m1=$(motion_counts)
  echo "motion0=$m0"; echo "motion1=$m1"
  curl -s -m 25 -o "$OUT/still_${tag}_${idx}_${fps}_q${q}.jpg" "$B/control?still=1"
  echo "still=$(python "$HERE/jpeg_dims.py" "$OUT/still_${tag}_${idx}_${fps}_q${q}.jpg")"
  printf '%s\n' "$L" | grep -a -q -i 'rescue' && echo "rescue=1" || echo "rescue=0"
}

kv() { printf '%s\n' "$1" | grep -a "^$2=" | head -1 | cut -d= -f2-; }
