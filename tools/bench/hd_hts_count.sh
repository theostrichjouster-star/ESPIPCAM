#!/usr/bin/env bash
# Is HD's 2060 line actually honoured? Counted, on route A, one register at a time.
#
# WHY THIS EXISTS. The 88 MHz probe (hd88_probe.sh) counted 54.844 fps on route B against 54.86
# predicted - exact - and 49.715 on route A against 52.20 predicted, a 4.8% shortfall. Route B
# agreeing while route A misses says the discrepancy is not the clock: back-solved, route A
# behaved as though its line were 2163 clocks while the register read 2060. Route B's 2156 line
# back-solves to 2157. So the suspicion is that a 2060 line is NOT honoured at VTS 744 and the
# sensor silently runs a longer one - which would mean HD's ceiling of 52 has never been
# physically reachable, and every "52 delivered 49.4" reading blamed on storage time was the
# SENSOR's own ceiling all along.
#
# HD's row count is fixed here (VTS 744) and only 0x380D moves - 2060/2112/2156/2200 are
# 0x080C/0x0840/0x086C/0x0898, so the high byte never changes and there is no two-byte
# transient to order. All on route A at 80 MHz, so the clock is not a variable.
#
# PREDICTIONS at 80 MHz, VTS 744, if the line is honoured:
#     HTS 2060 -> 52.20    HTS 2112 -> 50.91    HTS 2156 -> 49.87    HTS 2200 -> 48.87
# If instead the counts sit flat near 49.8 until 2156, the effective line is pinned ~2160.
# The baseline is counted THREE times first, because a single VSYNC count is one sample.
set -u
OUT=${OUT:-FPS_RECAL_stills/hd_hts_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"

IDX=13; CEIL=52; Q=${Q:-10}
WALK=${WALK:-"2060 2112 2156 2200"}
REPS=${REPS:-3}

CSV="$OUT/hd_hts.csv"
echo "hts,rep,vts,vsyncFps,predFps,impliedMHz,effHts" > "$CSV"

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

count_one() {  # count_one <hts> <rep>; appends a CSV row, logs the line
  local hts=$1 rep=$2 V f imp vts
  ctl "xclkStat=1" > /dev/null
  sleep 8
  V=$(ramlog | grep -a 'VSYNC counted' | tail -1)
  f=$(printf '%s' "$V" | sed 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/')
  imp=$(printf '%s' "$V" | sed 's/.*implied PIXCLK \([0-9.]*\)MHz.*/\1/')
  vts=$(( 0x$(regrd 0x380E) * 256 + 0x$(regrd 0x380F) ))
  python - "$hts" "$rep" "$vts" "$f" "$imp" "$CSV" <<'EOF' | tee -a "$LOGF"
import sys
hts, rep, vts, f, imp, csv = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5], sys.argv[6]
f = float(f) if f else 0.0
pred = 80e6 / (hts * vts)
eff = 80e6 / (f * vts) if f else 0
print("   HTS %d rep%s: counted %.3f fps vs %.2f predicted (%+.1f%%), implied PIXCLK %s, effective line %.0f clocks"
      % (hts, rep, f, pred, (f / pred - 1) * 100 if pred else 0, imp, eff))
open(csv, "a").write("%d,%s,%d,%.3f,%.2f,%s,%.0f\n" % (hts, rep, vts, f, pred, imp, eff))
EOF
}

wait_settled "${SETTLE:-240}"
assert_campaign_config "$Q"
[ "$(status_field tunedFps)" = "1" ] || { log "ABORT: tunedFps is off"; exit 6; }

log "== HD at $CEIL, route A 80 MHz: is the line honoured? =="
set_size "$IDX" "$Q" "$CEIL"
sleep 4
r3108=$(regrd 0x3108); mul=$(regrd 0x3036); r3035=$(regrd 0x3035)
hts=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))
log "   registers as tuned: 0x3108=$r3108 0x3035=$r3035 0x3036=$mul (mul $((0x$mul))) HTS $hts"
[ "$r3108" = "26" ] && [ "$((0x$mul))" = "120" ] || { log "ABORT: not on route A at 80 MHz"; exit 6; }

for rep in $(seq 1 "$REPS"); do count_one 2060 "$rep"; done
for hts in $WALK; do
  [ "$hts" = 2060 ] && continue
  ctl "camReg=0x380D,$(printf '0x%02X' $((hts & 0xFF)))" > /dev/null; sleep 2
  got=$(( 0x$(regrd 0x380C) * 256 + 0x$(regrd 0x380D) ))
  [ "$got" = "$hts" ] || { anomaly "HTS $hts did not read back (got $got)"; continue; }
  pass
  count_one "$hts" 1
done

log "== done =="
column -s, -t < "$CSV"
