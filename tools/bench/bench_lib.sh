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
# The register-tier reference to compare each point against. REF=- means no comparison,
# ie this run is GENERATING a reference. Needed because a stale reference DIFFs at every
# point, and two consecutive anomalies abort the tier - so a regeneration run would die
# after two points if the comparison were unconditional
REF="${REF:-FPS_RECAL_stills/sweep.csv}"
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
    row=$(printf '%s\n' "$L" | python "$HERE/t1_point.py" "$size" "$fps" "$ceil" "$regime" "$REF"); rc=$?
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

# one register, hex string without 0x, from the ring (camRegRd is LOG_INF so it lands there)
regrd() { ctl "camRegRd=$1" > /dev/null; sleep 0.6; ramlog | grep -a "camRegRd: $1" | tail -1 | sed 's/.*= 0x\([0-9A-Fa-f]*\).*/\1/'; }

# Autofocus hold for low-rate work (4 Sep 2026). The OV5640 AF program (downloaded at boot
# with INCLUDE_AF) runs continuous AF; at 1-2 fps it never converges and ignores commands, and
# a release (0x08) sends the lens to rest and changes the focus. So: focus where the program
# has detail and frames (FHD at 10 fps), then stop the sensor's MCU (0x3000[5]), which freezes
# the VCM, and prove it with the VCM DAC (0x3602/0x3603) before and after the size change.
# Command handshake, the library's own: ack 0x3023 = 1, the command in 0x3022, the MCU clears
# the ack when taken. A wedged program (ack never clears) is restarted from its loaded image
# by the same MCU reset bit; measured 12:06: status back to 0x70 idle, continuous re-accepted,
# VCM settled.
# The DAC code (datasheet table 3-2): code = 0x3603[5:0] << 4 | 0x3602[7:4], 0x3602[3:0] the
# slew mode. af_vcm prints 0x3602 then 0x3603, so 0xB20A is code 171.
af_cmd() { ctl "camReg=0x3023,0x01" > /dev/null; sleep 0.3; ctl "camReg=0x3022,$1" > /dev/null; local i a; for i in $(seq 1 10); do sleep 1; a=$(regrd 0x3023); [ "$a" = "00" ] && return 0; done; return 1; }
af_vcm() { echo "$(regrd 0x3602)$(regrd 0x3603)"; }
af_code() { printf '%d' $(( ((16#${1:2:2} & 0x3F) << 4) | (16#${1:0:2} >> 4) )); }
AF_VCM=""
af_hold() {  # af_hold <size idx to return to> <quality> <fps to return to>
  set_size 16 10 10; sleep 3
  if ! af_cmd 0x04; then
    log "AF program not answering - MCU restart (0x3000 bit 5)"
    ctl "camReg=0x3000,0x20" > /dev/null; sleep 0.5; ctl "camReg=0x3000,0x00" > /dev/null; sleep 3
    af_cmd 0x04 || { log "ABORT: AF program dead after the MCU restart"; exit 9; }
  fi
  local v1 v2 i; v1=$(af_vcm)
  for i in $(seq 1 15); do sleep 2; v2=$(af_vcm); [ "$v2" = "$v1" ] && [ "$i" -ge 2 ] && break; v1=$v2; done
  log "AF at FHD 10 fps: status 0x$(regrd 0x3029), VCM 0x$v2 (code $(af_code "$v2")) stable after $((i * 2))s"
  # The hold: the sensor's MCU into reset (0x3000[5]). This AF blob knows only 0x03 (single)
  # and 0x04 (continuous) - a pause (0x06) is never acknowledged, healthy or not - and with
  # the MCU stopped nothing rewrites the VCM DAC. Measured 12:11: VCM 0x0210 identical
  # through the reset, a size change, the drop to 1 fps and 8 s; the 1 fps still sharper
  # (hdiff 3.5) than any walk-1 frame. Releasing the reset restarts the program to idle
  ctl "camReg=0x3000,0x20" > /dev/null; sleep 1
  AF_VCM=$(af_vcm)
  [ "$(regrd 0x3000)" = "20" ] || { log "ABORT: MCU reset bit did not take - lens not held"; exit 9; }
  # In the dark the program parks the lens near rest instead of focusing (13:08, llevel 17:
  # code 42 against 171 / 242 / 256 from three lit holds). With the MCU stopped the DAC is
  # ours to write: AF_VCM_SET=<3602><3603> (hex, e.g. B20A = code 171, the lit FHD stretch)
  # puts the lens where a lit hold left it, and the readback is the proof
  if [ -n "${AF_VCM_SET:-}" ] && [ "$AF_VCM" != "$AF_VCM_SET" ]; then
    local was=$AF_VCM
    ctl "camReg=0x3603,0x${AF_VCM_SET:2:2}" > /dev/null; sleep 0.3
    ctl "camReg=0x3602,0x${AF_VCM_SET:0:2}" > /dev/null; sleep 1
    AF_VCM=$(af_vcm)
    [ "$AF_VCM" = "$AF_VCM_SET" ] || { log "ABORT: VCM set to 0x$AF_VCM_SET reads back 0x$AF_VCM"; exit 9; }
    log "AF program had left the lens at 0x$was (code $(af_code "$was")); VCM set to 0x$AF_VCM (code $(af_code "$AF_VCM")) with the MCU held"
  fi
  set_size "$1" "$2" "$3"; sleep 3
  log "AF MCU held (0x3000 0x20) at VCM 0x$AF_VCM; after the size change VCM 0x$(af_vcm)"
}
af_check() { local v; v=$(af_vcm); [ "$v" = "$AF_VCM" ] && log "AF held: VCM 0x$v unchanged" || log "WARN: VCM moved 0x$AF_VCM -> 0x$v"; }
af_resume() {  # MCU out of reset (the program restarts to idle), then continuous AF as the boot leaves it
  ctl "camReg=0x3000,0x00" > /dev/null; sleep 3
  af_cmd 0x04 && log "AF MCU released, continuous again, status 0x$(regrd 0x3029)" || log "WARN: AF continuous command not acknowledged after the MCU release (status 0x$(regrd 0x3029))"
}
