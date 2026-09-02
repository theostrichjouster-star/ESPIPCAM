#!/bin/bash
# Phase 4 - playback verification: play each recorded boundary clip at its recorded fps and
# check the DEVICE side (per-cluster read 5.5-7.5 ms, SD wait <= 2 ms/frame). Delivered fps is
# modeled as min(recorded fps, transport / bytes per frame) and recorded, not judged.
# COM3 must be silenced first (esptool download mode) and record=0 / idleFps=0 must hold.
#   BOARD=<addr> bash tools/bench/fps_t4_playback.sh [--full]   (default: the core subset)
source "$(dirname "$0")/bench_lib.sh"
T3="$OUT/t3_record.csv"; CSV="$OUT/t4_playback.csv"
[ -f "$T3" ] || { log "ABORT: $T3 not found - run the recording tier first"; exit 6; }
FULL=""; [ "${1:-}" = "--full" ] && FULL=1
preflight
[ "$(status_field enableMotion)" = "0" ] || ctl enableMotion=0 > /dev/null
echo "idx,size,fps,file,recFps,avgBytes,rttAvg,modelFps,playFps,frames,readKBs,usPerCluster,readMs,waitMs,delayMs,httpMs,busy,intMin,verdict" > "$CSV"
T=""   # transport bytes/s, derived from the first clip that is transport-bound
prevMin=""; hot=0; total=0; good=0
core_ok() {  # the core subset: ceiling, cap-crossing, 5 and 1 fps per size
  case "$1:$2" in 6:39|6:20|6:5|6:1|10:39|10:20|10:5|10:1|13:52|13:19|13:5|13:1|25:39|25:19|25:5|25:1|16:16|16:9|16:5|16:1|23:7|23:6|23:5|23:1) return 0 ;; esac
  return 1
}
mapfile -t ROWS < <(tail -n +2 "$T3")   # not a pipe: abort must exit the script, not a subshell
for row in "${ROWS[@]}"; do
  IFS=, read -r idx name f q file du frames act avg rest <<< "$row"
  [ "$q" = "10" ] || continue; [ "$file" = "NONE" ] && continue
  [ -z "$FULL" ] && ! core_ok "$idx" "$f" && continue
  assert_alive
  im=$(status_field int_min)
  if [ -n "$prevMin" ] && [ $((prevMin - im)) -gt 20000 ]; then log "ABORT: int_min fell ${prevMin} -> ${im} between clips"; exit 7; fi
  prevMin=$im
  rtt=$(ping -n 5 -w 1000 "$BOARD" | grep -o 'Average = [0-9]*' | grep -o '[0-9]*'); rtt=${rtt:-?}
  model=$(python - "$f" "$avg" "$T" <<'EOF'
import sys; f, avg, T = sys.argv[1:4]
print(f"{min(float(f), float(T)/float(avg)):.1f}" if T and avg else f)
EOF
)
  dur=$(( frames / (f > 0 ? f : 1) + 5 ))
  log "== play $name $f fps $file: model $model fps (T=${T:-unknown}), RTT ${rtt}ms, int_min $im =="
  prev=$(ramlog | grep -a 'getNextFrame\] Playback /' | tail -1)
  ctl "sfile=$file" > /dev/null; sleep 1
  curl -s -m $((dur + 60)) -o /dev/null "$B/sustain?playback=1"
  for i in $(seq 1 20); do sleep 1; L=$(ramlog); new=$(printf '%s\n' "$L" | grep -a 'getNextFrame\] Playback /' | tail -1); [ -n "$new" ] && [ "$new" != "$prev" ] && break; done
  printf '%s\n' "$L" > "$OUT/t4_${idx}_${f}.log"
  P=$(printf '%s\n' "$L" | python "$HERE/parse_play.py")
  pf=$(kv "$P" playFps); fr=$(kv "$P" frames); rk=$(kv "$P" readKBs); uc=$(kv "$P" usPerCluster); rm=$(kv "$P" readMs)
  wm=$(kv "$P" waitMs); dm=$(kv "$P" delayMs); hm=$(kv "$P" httpMs); bu=$(kv "$P" busy); pfile=$(kv "$P" file)
  verdict=$(python - "$uc" "$wm" "$pfile" "$file" <<'EOF'
import sys; uc, wm, pfile, file = sys.argv[1:5]
why = []
if pfile != file: why.append("stale")
try:
    if not (5500 <= int(uc) <= 7500): why.append(f"cluster {uc}us")
    if int(wm) > 2: why.append(f"wait {wm}ms")
except ValueError: why.append("parse")
print("PASS" if not why else "FAIL(" + ";".join(why) + ")")
EOF
)
  # derive the transport figure from the first transport-bound clip (http send dominates)
  if [ -z "$T" ] && [ -n "$pf" ] && [ -n "$hm" ] && [ "${hm:-0}" -ge 20 ]; then T=$(python -c "print(int(float('$pf')*float('$avg')))"); log "  transport T derived: $T B/s"; fi
  echo "$idx,$name,$f,$file,$f,$avg,$rtt,$model,$pf,$fr,$rk,$uc,$rm,$wm,$dm,$hm,$bu,$im,$verdict" >> "$CSV"
  total=$((total + 1)); [ "$verdict" = "PASS" ] && good=$((good + 1))
  log "  played $pf fps ($fr fr): read ${rk}kB/s ${uc}us/cluster, read ${rm}ms wait ${wm}ms delay ${dm}ms http ${hm}ms busy $bu% -> $verdict"
  # spacing: never more than 2 consecutive high-byte-rate clips without a long rest (the 2 Sep reboots)
  rate=$(python -c "print(int(float('$f')*float('$avg')))"); if [ "$rate" -gt 1000000 ]; then hot=$((hot + 1)); else hot=0; fi
  if [ "$hot" -ge 2 ]; then log "  two hot clips - resting 120 s"; sleep 120; hot=0; else sleep 90; fi
done
log "T4 done -> $CSV"
