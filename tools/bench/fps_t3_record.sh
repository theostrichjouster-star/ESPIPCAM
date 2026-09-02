#!/bin/bash
# Phase 3 - recording tier at the boundary rates, LIT room, q10 plus a like-for-like q6 block.
# Per point: fps, 4 s, motion counters (reading resets them: before and after), forced
# recording (20 s, 40 s at fps <= 2), the LAST closeAvi block by filename, a still.
# Run only after the user has confirmed the scene is lit.
#   BOARD=<addr> bash tools/bench/fps_t3_record.sh [--from <idx>:<fps>:<q>]
source "$(dirname "$0")/bench_lib.sh"
CSV="$OUT/t3_record.csv"
FROM=""; [ "${1:-}" = "--from" ] && FROM=$2
MATRIX=("QVGA:6:1,2,5,20,38,39:10:320x240" "VGA:10:1,2,5,20,38,39:10:640x480"
        "HD:13:1,2,5,18,19,51,52:10:1280x720" "1280X960:25:1,2,5,18,19,38,39:10:1280x960"
        "FHD:16:1,2,5,9,10,15,16:10:1920x1080" "QSXGA:23:1,2,5,6,7:10:2560x1920"
        "HD:13:51,52:6:1280x720" "1280X960:25:19,38,39:6:1280x960")

preflight
log "lighting check: llevel=$(status_field llevel) night=$(status_field night) atemp=$(status_field atemp)"
[ -f "$CSV" ] || echo "idx,size,fps,q,file,duration,frames,reqFps,actFps,avgBytes,storageMs,sdKBs,boost,rescues,busy,motion0,motion1,probe1,probe2,still,verdict" > "$CSV"
skipping=${FROM:+1}; total=0; good=0
for entry in "${MATRIX[@]}"; do
  IFS=: read -r name idx list q dims <<< "$entry"
  IFS=, read -r -a fpsList <<< "$list"
  first=1
  for f in "${fpsList[@]}"; do
    if [ -n "$skipping" ]; then [ "$idx:$f:$q" = "$FROM" ] && skipping="" || continue; fi
    if [ -n "$first" ]; then set_size "$idx" "$q" "$f"; first=""; else ctl "fps=$f" > /dev/null; sleep 4; fi
    dur=20; [ "$f" -le 2 ] && dur=40
    probe=""; case ",$list," in *",$f,"*) [ "$f" -ge 15 ] && probe=probe ;; esac
    log "== $name $f fps q$q, ${dur}s =="
    rec=$(record_clip "$idx" "$f" "$q" "$dur" t3 $probe)
    file=$(kv "$rec" file); frames=$(kv "$rec" frames); act=$(kv "$rec" actFps); avg=$(kv "$rec" avgBytes)
    st=$(kv "$rec" storageMs); sd=$(kv "$rec" sdKBs); bo=$(kv "$rec" boost); rs=$(kv "$rec" rescues); bu=$(kv "$rec" busy)
    du=$(kv "$rec" duration); m0=$(kv "$rec" motion0); m1=$(kv "$rec" motion1); p1=$(kv "$rec" probe1); p2=$(kv "$rec" probe2)
    still=$(kv "$rec" still); resc=$(kv "$rec" rescue)
    verdict=$(python - "$act" "$f" "$frames" "${du:-$dur}" "$m0" "$m1" "$rs" "$still" "$dims" "$file" <<'EOF'
import sys, re
act, f, frames, dur, m0, m1, rs, still, dims, fn = sys.argv[1:11]
why = []
if fn == "NONE": why.append("noclip")
try:
    if float(act) < 0.98 * float(f): why.append(f"rate {act}")
    # against the clip's OWN duration (closeAvi "AVI duration"), not the nominal button
    # time: a 20 s press closes at ~21 s, and at 20 fps that is 20 extra frames
    if abs(int(frames) - int(dur) * int(f)) > max(2, int(f)): why.append(f"frames {frames}")
except ValueError: why.append("parse")
b0 = re.search(r"bad=(\d+)", m0); b1 = re.search(r"bad=(\d+)", m1)
if b1 and int(b1.group(1)) > 0: why.append(f"bad {b1.group(1)}")
if rs not in ("", "0"): why.append(f"rescues {rs}")
if not still.startswith(dims + " "): why.append(f"still {still}")
print("PASS" if not why else "FAIL(" + ";".join(why) + ")")
EOF
)
    echo "$idx,$name,$f,$q,$file,$du,$frames,$act,$avg,$st,$sd,$bo,$rs,$bu,\"$m0\",\"$m1\",$p1,$p2,$still,$verdict" >> "$CSV"
    total=$((total + 1))
    log "  $file: $frames fr, actual $act, avg ${avg}B, storage ${st}ms, SD ${sd}kB/s, boost $bo, busy $bu%, probes $p1 $p2, still $still -> $verdict"
    # a rate or frame-count miss at a ceiling point is the MEASUREMENT (the SD budget or the
    # frame window binding, which is what the tier exists to find) - only structural
    # failures count toward the two-anomaly abort
    case "$verdict" in
      PASS) good=$((good + 1)); pass ;;
      *noclip*|*bad*|*rescues*|*parse*|*still*) anomaly "$name $f q$q: $verdict" ;;
      *) log "  MISS (measured, not an anomaly): $name $f q$q $verdict"; pass ;;
    esac
    [ "$resc" = "1" ] && { ctl "quality=$q" > /dev/null; log "quality re-asserted after a rescue"; }
    assert_alive
  done
done
log "T3 done: $good / $total clips pass -> $CSV. Eyeball every ceiling still (still_t3_*.jpg)."
