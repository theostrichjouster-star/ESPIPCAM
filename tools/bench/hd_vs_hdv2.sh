#!/usr/bin/env bash
# HD against HDV2 at the only request where they differ. Their registers are identical at 53 of 56
# requests, so the whole question is whether HDV2's 0.46 fps of sensor surplus at 54-56 turns into
# delivered frames - HD runs the sensor at exactly 56.00 for a request of 56, which applyTunedTiming
# documents as the "ceiling requests deliver ~1% low" case, and HDV2 restores 0.8% of overdrive.
#
# ALTERNATED, never in blocks: an hour of failing light moved 1280X960's delivered rate by 3 fps on
# its own (BOARD_TESTING 38.40), so two runs of the same size back to back would confound the size
# with the room.
#   BOARD=<addr> REPS=2 FPS=56 bash tools/bench/hd_vs_hdv2.sh
set -u
OUT=${OUT:-FPS_RECAL_stills/hdv2ab_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
FPS=${FPS:-56}; DUR=${DUR:-25}; REPS=${REPS:-2}; Q=${Q:-10}
CSV="$OUT/hd_vs_hdv2.csv"
echo "size,idx,rep,reqFps,actFps,ratio,frameKB,storageMs,busy,boost,govWrites,rescue" > "$CSV"
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
log "== HD vs HDV2 at request $FPS, alternating so the room cannot pick a side =="
for r in $(seq 1 "$REPS"); do
  for p in "HD:13" "HDV2:30"; do
    IFS=: read -r name idx <<< "$p"
    set_size "$idx" "$Q" "$FPS"
    R=$(record_clip "$idx" "$FPS" "$Q" "$DUR" "ab")
    act=$(kv "$R" actFps)
    [ -z "$act" ] && { anomaly "$name rep$r: no stats"; continue; }
    pass
    fkb=$(python -c "print('%.1f' % (float('$(kv "$R" avgBytes)')/1024))" 2>/dev/null)
    ratio=$(python -c "print('%.4f' % (float('$act')/$FPS))" 2>/dev/null)
    log "   $name rep$r -> delivered $act ($ratio) | frame ${fkb}KB | storage $(kv "$R" storageMs)ms | busy $(kv "$R" busy)% | boost $(kv "$R" boost)"
    echo "$name,$idx,$r,$FPS,$act,$ratio,$fkb,$(kv "$R" storageMs),$(kv "$R" busy),$(kv "$R" boost),$(kv "$R" govWrites),$(kv "$R" rescue)" >> "$CSV"
  done
done
log "== result =="
column -s, -t < "$CSV"
python - "$CSV" <<'EOF' | tee -a "$LOGF"
import csv, sys, statistics as st
rows = list(csv.DictReader(open(sys.argv[1])))
for name in ("HD", "HDV2"):
    v = [float(r["actFps"]) for r in rows if r["size"] == name]
    if v: print("   %-5s delivered %s  mean %.2f" % (name, "/".join("%.1f" % x for x in v), st.mean(v)))
a = [float(r["actFps"]) for r in rows if r["size"] == "HD"]
b = [float(r["actFps"]) for r in rows if r["size"] == "HDV2"]
if a and b:
    d = st.mean(b) - st.mean(a)
    print("   HDV2 - HD = %+.2f fps, against +0.46 of sensor surplus" % d)
    # A DIFFERENCE SMALLER THAN THE WITHIN-SIZE SPREAD IS NOT A DIFFERENCE. Measured 7 Sep 2026:
    # HD gave 48.5 then 53.2 in one alternating run while HDV2 gave 51.3 then 53.2, so the spread
    # inside HD alone was 4.7 fps against a 1.4 fps mean gap - and the second rep of each was
    # IDENTICAL. Comparing the means alone called that a win, which it is not
    spread = max(max(a) - min(a), max(b) - min(b)) if len(a) > 1 and len(b) > 1 else 0.0
    print("   widest within-size spread %.2f fps" % spread)
    if abs(d) <= spread:
        print("   NOT A RESULT: the gap is inside the spread of a single size, so this cannot")
        print("   separate the size from the room. More reps, or a scene that holds still.")
    elif d > 0:
        print("   HDV2's margin reaches the card, and by more than the run's own noise.")
    else:
        print("   HDV2 delivered LESS, by more than the run's own noise - investigate before shipping it.")
EOF
