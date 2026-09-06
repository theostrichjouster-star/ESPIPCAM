#!/bin/bash
# SD governor regression across the mainstays, in a LIT room.
#   BOARD=<address> bash tools/bench/gov_size_regress.sh
#
# Why this exists: the governor's measurement became a rolling trailing window and its decision
# became per-frame (BOARD_TESTING 38.31), and every threshold in it turned from a tick count into a
# time. That is a change to code which runs on EVERY saved frame at EVERY size, and it had only been
# measured at two of them - HD 30 and QSXGA 5. This sweeps the rest of the register tier's set.
#
# The reference is the frameData comment for each size: those figures are the 2-3 Sep 2026 lit q10
# recordings, taken BEFORE any of this, so they are a real before-and-after and not a self-check.
#
# WHAT IS GATED - the governor, and only the governor, because only that is scene-independent:
#   - no no-frame rescues
#   - govWrites consistent with the boost. A boost of N costs N writes up and N back down, so
#     2N + a couple is healthy and anything far above it is churn, which is the specific fault a
#     per-frame MEASUREMENT caused (38.30: 96 writes in an 87 s steady HD clip)
#   - a boost above the recorded figure ONLY when the demand did not justify it. The push rule is a
#     fixed percentage of the SD budget, so a scene with bigger frames boosts correctly - measured
#     6 Sep 2026 at 1280X960, boost 4 on 113 KB frames where the baseline had 98 KB and boost 0
#
# WHAT IS REPORTED BUT NOT GATED: delivered fps. At these ceilings it is dominated by storage time,
# storage time follows frame size, and frame size follows the scene - so gating it reports the ROOM
# as a code regression. Measured 6 Sep 2026: FHDNARROW delivered 13.1 fps at 230 KB frames and 16.0
# at 210 KB in the same lit room ten minutes apart, with the governor doing nothing in either. The
# KB/s push line cannot see storage time at all; that is the open storage-time follow-up.
#
# SIZES_LIST overrides the set, space separated as name:idx:ceiling:expFps:expKB:expBoost.
# DUR sets the clip length (default 30 s: the ease interval is 10 s and relax 2 s, so a shorter
# clip cannot show the governor settling).

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-FPS_RECAL_stills/govregress_$(date +%Y%m%d_%H%M%S)}"
source "$HERE/bench_lib.sh"

DUR=${DUR:-30}
Q=${Q:-10}
# name:idx:ceiling:expectedFps:expectedFrameKB:expectedBoost - the recorded lit q10 figures from
# appGlobals.h frameData. HD (52) and QSXGA (7) are deliberately absent: both were measured against
# this build on 6 Sep 2026 (38.29-31) and re-running them adds nothing.
# 1280X960 is the tight one by design - 98 KB frames at ~39 fps sit at 86-88% of the SD budget,
# just under the governor's 90% push line, so it is the size most likely to reveal a measurement
# that reads high. QHD is the other, at 93% busy and 75% of budget.
if [ -n "${SIZES_LIST:-}" ]; then read -r -a SIZES <<< "$SIZES_LIST"
else SIZES=("QVGA:6:39:39.0:11:0" "VGA:10:39:39.0:31:0" "1280X960:25:41:39.0:98:0" \
            "FHDNARROW:16:16:16.0:232:1" "FHDMID:26:12:12.0:152:0" "FHDFULL:27:9:9.0:188:0" \
            "QHD:20:9:9.0:372:0"); fi

CSV="$OUT/gov_regress.csv"
echo "size,idx,reqFps,expFps,actFps,fpsPct,expKB,avgKB,expBoost,boost,govWrites,govWin,rescues,busy,sdKBs,storageMs,easeLeft,govVerdict,rateNote" > "$CSV"

log "SD governor size regression: ${#SIZES[@]} sizes, ${DUR}s clips at q$Q -> $CSV"
# assert_campaign_config zeroes micGain and stillSave and does not put them back, so capture them
# first. Without this a run silently leaves the board mute with Get Still no longer filing
MIC0=$(status_field micGain); SAVE0=$(status_field stillSave); FS0=$(status_field framesize)
FPS0=$(status_field fps); IDLE0=$(status_field idleFps)
log "restore point: framesize=$FS0 fps=$FPS0 idleFps=$IDLE0 micGain=$MIC0 stillSave=$SAVE0"

# On an EXIT trap, not inline at the end. bench_lib's preflight and ctl call exit DIRECTLY, so an
# abort skipped the restore entirely: on 6 Sep 2026 a run aborted on the 240 s settle rule AFTER
# assert_campaign_config had zeroed micGain and stillSave, left them zero, and the NEXT run then
# captured those zeros as its own restore point and faithfully put them back. Capturing state is
# worthless if the abort paths do not go through the restore
GOV_RESTORED=0
gov_restore() {
  [ "$GOV_RESTORED" = "1" ] && return 0
  GOV_RESTORED=1
  log "restoring the start-of-run state: framesize=$FS0 fps=$FPS0 idleFps=$IDLE0 micGain=$MIC0 stillSave=$SAVE0"
  # plain curl, not ctl: ctl aborts the run on a failure and this may already BE the abort path,
  # which would recurse. A board that cannot answer gets logged, not retried into a loop
  local kv
  for kv in "framesize=$FS0" "fps=$FPS0" "quality=$Q" "micGain=$MIC0" "stillSave=$SAVE0" "idleFps=$IDLE0"; do
    http_gap
    curl -s -m 20 "$B/control?$kv" > /dev/null || log "restore: $kv did not answer"
    case "$kv" in framesize=*) sleep 6 ;; esac
  done
  sleep 2
  log "restored: framesize=$(status_field framesize) fps=$(status_field fps) idleFps=$(status_field idleFps) micGain=$(status_field micGain) stillSave=$(status_field stillSave)"
}
trap gov_restore EXIT

assert_campaign_config "$Q"
preflight
# The governor's own settings must be the shipped defaults or the run measures something else.
# GOVWIN overrides the window for a deliberate A/B - a smaller one is a NOISIER measurement, which
# is the question when a marginal size sits just under the push line
log "governor: $(ctl "govWinMs=${GOVWIN:-1000}") $(ctl govEaseSecs=10)"

FAIL=0
for entry in "${SIZES[@]}"; do
  IFS=: read -r name idx ceil expFps expKB expBoost <<< "$entry"
  log "== $name (idx $idx) at its ceiling $ceil =="
  set_size "$idx" "$Q" "$ceil"
  # A size change retimes the sensor from the capture task; let the AEC settle before recording, and
  # confirm the board took the size rather than assuming it. 15 s, not 8: the first point of a run
  # follows assert_campaign_config clearing idleFps, and the sensor can still be on the idle
  # throttle's rate. On 6 Sep 2026 the first sweep's OPENING point read 34.3 fps of 39 with 26 ms of
  # monitoring wait and only 2 ms of storage - nothing to do with the card or the governor - and the
  # same point re-run mid-sweep read 39.0 twice. Do not shorten this
  sleep 15
  got=$(status_field framesize)
  [ "$got" = "$idx" ] || { anomaly "$name: framesize reads $got, asked $idx"; continue; }
  cap=$(ctl updateFPS=1 | python "$HERE/jfield.py" frameCapKB)

  R=$(record_clip "$idx" "$ceil" "$Q" "$DUR" "gov" probe) || { anomaly "$name: no clip recorded"; continue; }
  act=$(kv "$R" actFps); avgB=$(kv "$R" avgBytes); boost=$(kv "$R" boost)
  gw=$(kv "$R" govWrites); gwin=$(kv "$R" govWin); resc=$(kv "$R" rescues)
  busy=$(kv "$R" busy); sdkb=$(kv "$R" sdKBs); stms=$(kv "$R" storageMs); eleft=$(kv "$R" easeLeft)
  avgKB=$(( ${avgB:-0} / 1024 ))
  pct=$(python -c "print(f'{100*${act:-0}/${expFps}:.1f}')" 2>/dev/null || echo 0)

  # the rate gate is rate-dependent, as everywhere in this bench: the VSYNC/frame counter's window
  # is only a dozen edges at a low rate, so 6% below 10 fps and 3% above (fps_t1_register)
  tol=$(python -c "print(6 if ${expFps} < 10 else 3)")
  # THE GOVERNOR GATES - these are the regression, and they are scene-independent. Measured
  # 6 Sep 2026 across 10 clips at 7 sizes: churn 0 and rescues 0 everywhere, one correct boost.
  v=""
  [ "${resc:-0}" -eq 0 ] || v="${v}RESCUE "
  # 2 writes per boost step plus 2 slack; a clip that never boosted should never write at all
  maxw=$(( 2 * ${boost:-0} + 2 ))
  [ "${gw:-0}" -le "$maxw" ] || v="${v}CHURN "
  # A boost above the recorded figure is only a finding if the DEMAND did not justify it: the rule
  # is a fixed percentage of the budget, so a scene with bigger frames boosts correctly. Compare
  # demand against the push line rather than against a boost taken in a different room
  push=$(python -c "print(int(4458 * 0.90))")
  dem=$(python -c "print(int(${act:-0} * ${avgKB:-0}))")
  if [ "${boost:-0}" -gt "${expBoost}" ] && [ "$dem" -lt "$push" ]; then v="${v}BOOST "; fi
  [ -z "$v" ] && v="pass" || FAIL=$((FAIL + 1))
  # THE RATE is reported, NOT gated. It is dominated by storage time at these ceilings and storage
  # time follows frame size, which follows the scene: measured 6 Sep 2026, FHDNARROW delivered
  # 13.1 fps at 230 KB frames and 16.0 at 210 KB in the same room 10 minutes apart, with the
  # governor doing nothing in either. Gating it here would report the room as a code regression.
  # The KB/s push line cannot see this at all - that is the storage-time follow-up, still open
  rn="rate-ok"
  python -c "import sys; sys.exit(0 if abs(100-${pct:-0}) <= ${tol} else 1)" || rn="rate-low"

  echo "$name,$idx,$ceil,$expFps,$act,$pct,$expKB,$avgKB,$expBoost,$boost,$gw,$gwin,$resc,$busy,$sdkb,$stms,$eleft,$v,$rn" >> "$CSV"
  log "$name: ${act}fps of $ceil (${pct}% of the recorded ${expFps}), ${avgKB}KB vs ${expKB}, boost $boost vs $expBoost, writes $gw, rescues $resc, busy ${busy}%, window ${cap}KB -> governor $v, $rn"
  [ "$v" = "pass" ] && pass
done

log "done: $FAIL of ${#SIZES[@]} sizes flagged on the GOVERNOR gates (rate is reported, not gated)"
column -s, -t "$CSV" 2>/dev/null || cat "$CSV"
gov_restore   # the EXIT trap would do this anyway; explicit here so it lands before the exit status
exit $(( FAIL > 0 ? 1 : 0 ))
