#!/usr/bin/env bash
# Web UI camera-control regression (4 Sep 2026): does any /control key the page can send
# disturb the tuner's registers, the frame rate, the sensor's JPEG quality, or fail to restore?
#
# Every picture control in data/MJPEG2SD.htm maps to one driver set_* call (esp32-camera
# 4335c93e, the pin in tools/core/README.md), and each of those writes a known register set -
# the prediction table in prediction.txt. The tuner (applySensorTuning) owns the clock, the
# window, HTS/VTS and the AEC limits, and runs only after set_framesize and on a retime, so a
# control that writes into that set from the web task is a regression the fps tiers never see.
#
# Method, one variable at a time, prediction first: per mainstay (HD 30 route A, 1280X960 41
# route B, FHDNARROW 1 at the clock floor) take a baseline snapshot of ~100 registers
# (regsnap.py), a VSYNC count off the pins (xclkStat) and a still; then per control send its
# min, its max and its persisted default, and after each: snapshot + diff against the baseline
# (expected registers are the control's own; anything else is a finding), /status readback,
# VSYNC, a still through still_color.py, the sensor's quality (0x4407), the rescue and WRN
# lines from the ring. After the default the diff must be empty. Then the scenarios: the UI's
# own sequences (manual exposure/gain, the AWB presets, colour bar, flip/mirror at the cropped
# sizes, the special-effect coupling, the mid-recording deferrals, the frame window).
#
# Hidden /control keys (tunedFps, banding, idleFps, xclkMhz, lencFhd, sdBusDiv) are AUDITED
# from source only - never sent here (user, 4 Sep). Nothing is saved (no save=1); the restore
# puts every camera key back to the /status snapshot taken at the start and proves it.
#
#   BOARD=<addr> bash tools/bench/ui_regress.sh            the full matrix + scenarios (~3 h)
#   DRY=1 BOARD=<addr> bash tools/bench/ui_regress.sh      HD only, three controls, one scenario
#   SIZES="13:30:HD" CONTROLS="aec agc" SCENARIOS="awb"    subsets; OUT= the run directory
# Bash traps from this bench: no variable named h or st (a read fills them), no 16#xx literal
# inside a python -c string, never edit this file while it runs.
set -u
OUT=${OUT:-FPS_RECAL_stills/ui_regress_$(date +%Y%m%d_%H%M)}
source "$(dirname "${BASH_SOURCE[0]}")/bench_lib.sh"
Q=${Q:-10}
FIELD_SIZE=13; FIELD_FPS=30
DRY=${DRY:-}
BOX=${BOX:-"700 650 1400 1300"}    # the star chart in the QSXGA frame (x0 y0 x1 y1), awb scenario
if [ -n "$DRY" ]; then
  SIZES=${SIZES:-"13:30:HD"}
  CONTROLS=${CONTROLS:-"brightness hmirror aec"}
  SCENARIOS=${SCENARIOS:-"manual_exposure"}
else
  SIZES=${SIZES:-"13:30:HD 25:41:1280X960 16:1:FHDNARROW"}
  CONTROLS=${CONTROLS:-all}
  SCENARIOS=${SCENARIOS:-"manual_exposure manual_gain awb colorbar flip effect deferral window"}
fi
CSV="$OUT/matrix.csv"
[ -f "$CSV" ] || echo "size,control,value,kind,exp,unexp,live,status_rb,vsync0,vsync,bytes,ratio,gsat,rsat,hdiff,vdiff,luma,yavg,aecst,expLines,gain16,r3503,r4407,rescue,wrn,gates" > "$CSV"
FINDINGS="$OUT/findings.txt"; NFIND=0
STATUS0="$OUT/status0.json"

# ---------------------------------------------------------------- the control table --------
# key -> "values|expected registers (regsnap.py diff syntax)|settle s|kind"
#   values: sent in order, then the persisted default from status0 (unless the last value is it)
#   kind: img (image changes by design, still logged not gated), none (nothing should change),
#         geom (flip/mirror), exp (exposure/gain: image changes), q (quality), pattern
#         (colour bar), hd (motion/recording panel keys, HD only, no sensor registers)
declare -A SPEC
SPEC[brightness]="-3 3|5587,5588|4|img"
SPEC[contrast]="-3 3|5586|4|img"
SPEC[saturation]="-4 4|5381-538B|4|img"
SPEC[sharpness]="-3 3|5308,5300-5303,5309-530C|4|img"
SPEC[denoise]="8 3|5308,5306|4|img"
SPEC[special_effect]="1 6|5580,5583,5584,5003|4|img"
SPEC[lenc]="0|5000|4|img"
SPEC[bpc]="0|5000|4|img"
SPEC[wpc]="0|5000|4|img"
SPEC[raw_gma]="0|5000|4|img"
SPEC[quality]="6 63|4407|4|q"
SPEC[micGain]="0 10||2|none"
SPEC[hmirror]="1|3814,3815,3820,3821,4514,4520|4|geom"
SPEC[vflip]="1|3814,3815,3820,3821,4514,4520|4|geom"
SPEC[ae_level]="-5 5|3A0F,3A10,3A11,3A1B,3A1E,3A1F|8|exp"
SPEC[gainceiling]="0 511|3A18,3A19|8|exp"
SPEC[aec2]="1|3A00|8|exp"
SPEC[aec_value]="0 MAX||4|none"
SPEC[agc_gain]="0 63||4|none"
SPEC[aec]="0|3503|8|exp"
SPEC[agc]="0|3503|8|exp"
SPEC[dcw]="0|5183|8|img"
SPEC[wb_mode]="1 4|3406|8|img"
SPEC[awb_gain]="0|3406|8|none"
SPEC[awb]="0||8|none"
SPEC[colorbar]="1|503D|8|pattern"
SPEC[enableMotion]="1||2|hd"
SPEC[motionVal]="1 10||2|hd"
SPEC[moveStopSecs]="2 60||2|hd"
SPEC[record]="1||2|hd"
SPEC[dbgMotion]="1||2|hd"
SPEC[lswitch]="0 100||2|hd"
SPEC[dashCamOn]="1||2|hd"
SPEC[zoneMask]="255||2|hd"
ORDER="brightness contrast saturation sharpness denoise special_effect lenc bpc wpc raw_gma quality micGain hmirror vflip ae_level gainceiling aec2 aec_value agc_gain aec agc dcw wb_mode awb_gain awb colorbar enableMotion motionVal moveStopSecs record dbgMotion lswitch dashCamOn zoneMask"
# keys whose in-run restore is the CAMPAIGN value, not the field one (the field value returns
# in the exit restore): record 1 would arm motion recording, enableMotion 1 the detector,
# micGain 5 the audio path
declare -A CAMPAIGN=([record]=0 [enableMotion]=0 [dbgMotion]=0 [dashCamOn]=0 [micGain]=0)
# the camera keys the exit restore replays from status0, in a safe order (size before rate
# and quality, the automatics on before their manual values, detection and recording last)
CAM_KEYS="framesize fps quality brightness contrast saturation sharpness denoise special_effect wb_mode aec_value aec2 agc_gain hmirror vflip awb awb_gain aec ae_level dcw agc gainceiling lenc colorbar bpc wpc raw_gma motionVal moveStopSecs lswitch zoneMask dashCamOn dbgMotion micGain idleFps enableMotion record"

# ---------------------------------------------------------------- helpers ------------------
sf0() { python "$HERE/jfield.py" "$1" < "$STATUS0"; }
finding() {  # finding <size> <control> <value> <code> <detail>
  NFIND=$((NFIND + 1))
  echo "$1 $2=$3 $4: $5" >> "$FINDINGS"
  log "   FINDING $1 $2=$3 $4: $5"
}
last_wrn() { ramlog | grep -a "not supported for camera type\|Unable to use\|Show Motion needs\|did not take\|did not read back" | tail -1; }
last_rescue() { ramlog | grep -a "No frames for" | tail -1; }
# regsnap <outfile> [extra regs "0x.... 0x...."] : the tuner set from dumpCam=1 (LOG_DIA, read
# back from the SD log tail - the server flushes the log before serving it, webServer.cpp), plus
# the core three and the control's own registers by camRegRd, one ring fetch. Under the 5 s gap
# a snapshot is ~35 s plus 5 s per extra register, so the extras are the control's list only
CORE_REGS="0x3503 0x3406 0x4407"
regsnap() {
  local out=$1 extra=${2:-} a
  ctl "dumpCam=1" > /dev/null; sleep 2; http_gap
  curl -s -m 90 "$B/web?log.txt" | grep -a '' | tail -80 > "$out.log"   # the raw dump text, kept
  python "$HERE/regsnap.py" dump < "$out.log" > "$out" 2>> "$OUT/regsnap_errors.txt" || log "   WARN: no complete dumpCam block in the SD log tail ($out)"
  for a in $CORE_REGS $extra; do ctl "camRegRd=$a" > /dev/null; done
  ramlog | python "$HERE/regsnap.py" parse $CORE_REGS $extra >> "$out" 2>> "$OUT/regsnap_errors.txt" || log "   WARN: register reads incomplete for $out (regsnap_errors.txt)"
}
regs_of() { python "$HERE/regsnap.py" expand "$1"; }   # "5587,5588,5381-538B" -> "0x5587 0x5588 ..."
vsync() {  # vsync <fps> : the counted rate off the pin; xclkStat waits up to 10 s for 60 frames
  local fps=$1 wait=4
  [ "$fps" -ge 10 ] || wait=13
  ctl "xclkStat=1" > /dev/null; sleep "$wait"
  ramlog | grep -a "VSYNC counted\|VSYNC count failed" | tail -1 | sed -n 's/.*VSYNC counted: \([0-9.]*\)fps.*/\1/p'
}
zones() {  # -> "yavg aecst"
  local zm zmin zmax yavg lo hi aecst
  ctl "avgZones=1" > /dev/null; sleep 1
  read -r zm zmin zmax yavg lo hi aecst <<< "$(ramlog | python "$HERE/parse_zones.py")"
  echo "$yavg $aecst"
}
# take_still <tag> <fps> : sets ST_* (w hh bytes ratio gsat rsat hdiff vdiff luma tries); the
# still handler waits 1.2 s for a frame, so below ~1 fps it is retried at 7/6 of the period
take_still() {
  local tag=$1 fps=$2 tries=0 retryGap stats w hh bytes mr mg mb ratio gsat rsat hdiff vdiff
  retryGap=$(python -c "print('%.2f' % max(0.2, 7.0 / (6 * $fps) - 1.25))")
  while [ "$tries" -lt 6 ]; do
    tries=$((tries + 1)); http_gap
    curl -s -m 90 -o "$OUT/$tag.jpg" "$B/control?still=1" && [ -s "$OUT/$tag.jpg" ] && break
    sleep "$retryGap"
  done
  stats=$(python "$HERE/still_color.py" "$OUT/$tag.jpg" 2>/dev/null) || stats="0 0 0 0 0 0 0 0 0 999 999"
  read -r w hh bytes mr mg mb ratio gsat rsat hdiff vdiff <<< "$stats"
  ST_W=$w; ST_H=$hh; ST_BYTES=$bytes; ST_RATIO=$ratio; ST_GSAT=$gsat; ST_RSAT=$rsat; ST_HDIFF=$hdiff; ST_VDIFF=$vdiff; ST_TRIES=$tries
  ST_LUMA=$(python -c "print('%.1f' % (0.299*$mr + 0.587*$mg + 0.114*$mb))")
}
box_stats() {  # box_stats <jpg> -> "frameRG frameBG boxR boxG boxB boxRG boxBG luma" (QSXGA chart box)
  python - "$1" $BOX <<'PY'
import sys
from PIL import Image, ImageStat
im = Image.open(sys.argv[1]).convert('RGB')
x0, y0, x1, y1 = [int(v) for v in sys.argv[2:6]]
f = ImageStat.Stat(im).mean
b = ImageStat.Stat(im.crop((x0, y0, x1, y1))).mean
luma = 0.299*f[0] + 0.587*f[1] + 0.114*f[2]
print("%.3f %.3f %.1f %.1f %.1f %.3f %.3f %.1f" % (f[0]/max(1,f[1]), f[2]/max(1,f[1]), b[0], b[1], b[2], b[0]/max(1,b[1]), b[2]/max(1,b[1]), luma))
PY
}
live_of() { python "$HERE/regsnap.py" live "$1"; }   # -> expLines gain16 awbR awbG awbB r3503 r4407
tuner_line() { ramlog | grep -a "Tuned timing" | grep -a "request $1," | tail -1 | sed 's/^\[[^]]*\] //'; }
pct_off() { python -c "a=float('${1:-0}' or 0); b=float('${2:-0}' or 0); print('%.2f' % (abs(a-b)*100/a if a else 999))"; }
settle_for() { python -c "print(max($1, int(4.0 / $2 + 2)))"; }   # 4 frames + 2 s, at least the spec

# ---------------------------------------------------------------- restore (trap) -----------
restore() {
  log "== restore: automatics on, every camera key from status0, size reload, AF continuous =="
  local k v bad=""
  # a board that has gone off the network (18:05 on 4 Sep: wedged after a size switch) would
  # cost 25 s per key here and then abort inside the trap - say so and leave instead. Replay
  # later with RESTORE_ONLY=1 OUT=<this run's directory>
  http_gap; curl -s -m 10 "$B/status" > /dev/null || { log "board unreachable - restore NOT done; replay with RESTORE_ONLY=1 OUT=$OUT"; return; }
  for k in aec=1 agc=1 awb=1 colorbar=0 special_effect=0 forceRecord=0; do http_gap; curl -s -m 25 "$B/control?$k" > /dev/null; done
  # the camera keys first, the three that change what the sensor does (idleFps, detection,
  # recording) last - AFTER the register check, or the idle throttle has retimed the sensor to
  # idleFps by the time the final snapshot is taken and it "differs" from the 30 fps baseline
  # (dry run 2: 0x3035/0x3036/VTS all moved, all the throttle's)
  for k in $CAM_KEYS; do
    case $k in idleFps|enableMotion|record) continue ;; esac
    v=$(sf0 "$k"); [ -n "$v" ] || continue
    http_gap; curl -s -m 25 "$B/control?$k=$v" > /dev/null
    [ "$k" = framesize ] && sleep 8
  done
  af_resume
  sleep 3
  if [ -f "$OUT/snap_HD_base.txt" ]; then
    regsnap "$OUT/snap_HD_final.txt"
    log "final HD registers vs the HD baseline (before idleFps returns): $(python "$HERE/regsnap.py" diff "$OUT/snap_HD_base.txt" "$OUT/snap_HD_final.txt")"
  fi
  for k in idleFps enableMotion record; do
    v=$(sf0 "$k"); [ -n "$v" ] || continue
    http_gap; curl -s -m 25 "$B/control?$k=$v" > /dev/null
  done
  for k in $CAM_KEYS; do
    v=$(sf0 "$k"); [ -n "$v" ] || continue
    [ "$(status_field "$k")" = "$v" ] || bad="$bad $k=$(status_field "$k")(want $v)"
  done
  [ -z "$bad" ] && log "field config: /status matches status0 for every camera key" || log "WARN: /status differs from status0:$bad"
  log "field config: framesize=$(status_field framesize) fps=$(status_field fps) quality=$(status_field quality) record=$(status_field record) enableMotion=$(status_field enableMotion) idleFps=$(status_field idleFps) micGain=$(status_field micGain) 0x3503=0x$(regrd 0x3503) 0x3406=0x$(regrd 0x3406) 0x503D=0x$(regrd 0x503D) 0x4407=0x$(regrd 0x4407)"
  log "== $NFIND finding(s) in $FINDINGS =="
}
trap restore EXIT

# ---------------------------------------------------------------- one point ----------------
# point <size> <fps> <key> <value> <expected regs> <settle> <kind> <baseline snap> <vsync0>
point() {
  local sz=$1 fps=$2 key=$3 val=$4 expReg=$5 settle=$6 kind=$7 S0=$8 v0=$9
  local wrn0 resc0 wrn1 resc1 tag snap d rc rb vs lv yavg aecst gates="" want_rb
  wrn0=$(last_wrn); resc0=$(last_rescue)
  ctl "$key=$val" > /dev/null
  sleep "$(settle_for "$settle" "$fps")"; assert_alive
  tag="${sz}_${key}_${val}"; snap="$OUT/snap_$tag.txt"
  # the control's own registers every point; at the restore point also the AEC limits and the
  # gain ceiling, the tuner-owned pair the dump does not print (a surprise there is the finding
  # class this campaign exists for)
  local extra; extra=$(regs_of "$expReg"); [ "$kind" = restore ] && extra="$extra $(regs_of "$EXPREG_PREV,3A02,3A03,3A14,3A15,3A18,3A19,3A00")"
  regsnap "$snap" "$extra"
  d=$(python "$HERE/regsnap.py" diff "$S0" "$snap" "$expReg"); rc=$?
  rb=$(status_field "$key")
  vs=$(vsync "$fps")
  take_still "$tag" "$fps"
  read -r yavg aecst <<< "$(zones)"
  lv=$(live_of "$snap")
  wrn1=$(last_wrn); resc1=$(last_rescue)
  local expLines gain16 awbR awbG awbB r3503 r4407
  read -r expLines gain16 awbR awbG awbB r3503 r4407 <<< "$lv"
  local exp unexp live missing
  exp=${d#exp=}; exp=${exp%%;*}; unexp=${d#*unexp=}; unexp=${unexp%%;*}; live=${d#*live=}; live=${live%%;*}; missing=${d##*missing=}
  # gates
  if [ "$kind" = restore ]; then
    [ "$exp" = "-" ] && [ "$unexp" = "-" ] && [ "$missing" = "-" ] || { gates="$gates RESTORE"; finding "$sz" "$key" "$val" RESTORE "registers differ from the baseline after the default: exp=$exp unexp=$unexp missing=$missing"; }
  else
    [ "$unexp" = "-" ] || { gates="$gates REG"; finding "$sz" "$key" "$val" REG "unexpected register change: $unexp"; }
    [ "$missing" = "-" ] || { gates="$gates SNAP"; finding "$sz" "$key" "$val" SNAP "snapshot incomplete: $missing"; }
  fi
  want_rb=$val; [ "$key" = dbgMotion ] && [ "$val" = 1 ] && want_rb=0   # the handler pushes back (motion off)
  [ "$rb" = "$want_rb" ] || { gates="$gates STATUS"; finding "$sz" "$key" "$val" STATUS "/status reads $rb"; }
  if [ -n "$vs" ] && [ -n "$v0" ]; then
    [ "$(python -c "print(1 if $(pct_off "$v0" "$vs") > 0.5 else 0)")" = 0 ] || { gates="$gates RATE"; finding "$sz" "$key" "$val" RATE "VSYNC $vs vs baseline $v0"; }
  else gates="$gates NOVSYNC"; fi
  local wantQ; wantQ=$(printf '%02X' "$Q"); [ "$key" = quality ] && [ "$kind" != restore ] && wantQ=$(printf '%02X' "$val")
  [ "$r4407" = "$wantQ" ] || { gates="$gates QS"; finding "$sz" "$key" "$val" QS "sensor quality 0x$r4407, config 0x$wantQ"; }
  [ "$resc1" = "$resc0" ] || { gates="$gates RESCUE"; finding "$sz" "$key" "$val" RESCUE "$resc1"; }
  [ "$wrn1" = "$wrn0" ] || { gates="$gates WRN"; finding "$sz" "$key" "$val" WRN "$wrn1"; }
  if [ "$kind" = none ] || [ "$kind" = geom ] || [ "$kind" = hd ] || [ "$kind" = restore ]; then
    local ok; ok=$(python -c "r=float('$ST_RATIO' or 0); r0=float('${BASE_RATIO:-0}' or 0); hd=float('$ST_HDIFF' or 999); hd0=float('${BASE_HDIFF:-1}' or 1); b=int('$ST_BYTES' or 0)
print(1 if b > 0 and r0 and abs(r-r0)/r0 <= 0.10 and hd <= 2*hd0 and float('$ST_GSAT') < 5 and float('$ST_RSAT') < 5 else 0)")
    [ "$ok" = 1 ] || { gates="$gates STILL"; finding "$sz" "$key" "$val" STILL "ratio $ST_RATIO (base $BASE_RATIO) hdiff $ST_HDIFF (base $BASE_HDIFF) gsat $ST_GSAT rsat $ST_RSAT bytes $ST_BYTES"; }
  fi
  local rescueFlag=0 wrnFlag=0; [ "$resc1" = "$resc0" ] || rescueFlag=1; [ "$wrn1" = "$wrn0" ] || wrnFlag=1
  log "   $sz $key=$val ($kind): exp=$exp unexp=$unexp live=$live | /status $rb | VSYNC $vs (base $v0) | still ${ST_W}x${ST_H} ${ST_BYTES}B luma $ST_LUMA ratio $ST_RATIO hdiff $ST_HDIFF (${ST_TRIES} req) | YAVG $yavg $aecst exp $expLines lines gain $gain16/16 0x3503 $r3503 0x4407 $r4407 |${gates:- clean}"
  echo "$sz,$key,$val,$kind,$exp,$unexp,$live,$rb,$v0,$vs,$ST_BYTES,$ST_RATIO,$ST_GSAT,$ST_RSAT,$ST_HDIFF,$ST_VDIFF,$ST_LUMA,$yavg,$aecst,$expLines,$gain16,$r3503,$r4407,$rescueFlag,$wrnFlag,${gates# }" >> "$CSV"
}

# ---------------------------------------------------------------- one mainstay -------------
run_size() {  # run_size <idx> <fps> <name>
  local idx=$1 fps=$2 name=$3 key spec vals expReg settle kind v last dflt aecMax S0 v0
  log "-- $name: idx $idx, request $fps, quality $Q --"
  set_size "$idx" "$Q" "$fps"; sleep 6
  [ "$fps" -ge 5 ] || sleep 8
  # the AEC settles from the AF hold's size changes slowly: dry run 2's HD baseline read 5x
  # gain and the first control found 2.4x, where it then stayed - give it a minute, and record
  # the AEC state the baseline was taken in
  sleep 45
  assert_alive
  log "tuner: $(tuner_line "$fps") | AEC before the baseline: $(zones)"
  http_gap; aecMax=$(curl -s -m 10 "$B/control?updateFPS=1" | python "$HERE/jfield.py" aecMax); [ -n "$aecMax" ] || aecMax=1200
  # the baseline carries everything a control could touch, once per size: the AEC limits and
  # ceiling, the ISP enables, the AF hold and VCM, the SDE / CIP / colour-matrix bytes
  S0="$OUT/snap_${name}_base.txt"
  regsnap "$S0" "$(regs_of "3A02,3A03,3A14,3A15,3A18,3A19,3A00,3A0F,3A10,3A11,3A1B,3A1E,3A1F,3C00,3C01,5000,503D,5183,4514,4520,3000,3602,3603,5580,5583,5584,5586,5587,5588,5003,5306,5308,5300,5381-538B")"
  v0=$(vsync "$fps")
  take_still "${name}_base" "$fps"; BASE_RATIO=$ST_RATIO; BASE_HDIFF=$ST_HDIFF
  log "baseline $name: VSYNC $v0, still ${ST_W}x${ST_H} ${ST_BYTES}B luma $ST_LUMA ratio $ST_RATIO hdiff $ST_HDIFF, live $(live_of "$S0"), aecMax $aecMax"
  for key in $ORDER; do
    [ "$CONTROLS" = all ] || case " $CONTROLS " in *" $key "*) ;; *) continue ;; esac
    spec=${SPEC[$key]}
    IFS='|' read -r vals expReg settle kind <<< "$spec"
    [ "$kind" = hd ] && [ "$name" != HD ] && continue
    dflt=${CAMPAIGN[$key]:-$(sf0 "$key")}
    [ -n "$dflt" ] || { log "   $key: no persisted value in status0 - skipped"; continue; }
    # a toggle is tested at the OPPOSITE of its live value, whatever that is (the bench board
    # sat at hmirror=1 on 4 Sep, so a fixed "1" would have measured nothing)
    case " hmirror vflip lenc bpc wpc raw_gma aec agc awb awb_gain dcw colorbar aec2 enableMotion record dbgMotion " in
      *" $key "*) vals=$(( 1 - dflt )) ;;
    esac
    last=""
    for v in $vals; do
      [ "$v" = MAX ] && v=$aecMax
      point "$name" "$fps" "$key" "$v" "$expReg" "$settle" "$kind" "$S0" "$v0"; last=$v
    done
    # the persisted default is the restore; its snapshot must match the baseline exactly (the
    # control's own registers are read again there: EXPREG_PREV hands them to point)
    EXPREG_PREV=$expReg
    point "$name" "$fps" "$key" "$dflt" "" "$settle" restore "$S0" "$v0"
    pass
  done
}

# ---------------------------------------------------------------- scenarios ----------------
SCSV="$OUT/scenarios.csv"
[ -f "$SCSV" ] || echo "scenario,size,step,r3503,r3406,r503D,r4407,expLines,gain16,awbR,awbG,awbB,yavg,aecst,vsync,bytes,luma,ratio,hdiff,boxRG,boxBG,note" > "$SCSV"
SC_VSYNC=0; SC_BOX=0; SC_EXTRA=""; EXPREG_PREV=""
sc_row() {  # sc_row <scenario> <size> <step> <fps> <note>  (snapshot + still + zones, one CSV row;
            #  SC_EXTRA = the scenario's own registers beyond the dump and the core three)
  local sc=$1 sz=$2 step=$3 fps=$4 note=$5 snap tag yavg aecst vs="" expLines gain16 awbR awbG awbB r3503 r4407 r3406 r503D bx="- -"
  tag="sc_${sc}_${sz}_${step}"; snap="$OUT/$tag.txt"
  regsnap "$snap" "$(regs_of "$SC_EXTRA")"; read -r expLines gain16 awbR awbG awbB r3503 r4407 <<< "$(live_of "$snap")"
  r3406=$(grep -a '^3406=' "$snap" | cut -d= -f2); r503D=$(grep -a '^503D=' "$snap" | cut -d= -f2)
  [ "$SC_VSYNC" = 1 ] && vs=$(vsync "$fps")
  take_still "$tag" "$fps"; read -r yavg aecst <<< "$(zones)"
  [ "$SC_BOX" = 1 ] && [ -s "$OUT/$tag.jpg" ] && bx=$(box_stats "$OUT/$tag.jpg" | awk '{print $6, $7}')
  log "   [$sc/$sz] $step: 0x3503 $r3503 0x3406 $r3406 0x503D $r503D 0x4407 $r4407 | exp $expLines lines gain $gain16/16 AWB $awbR/$awbG/$awbB | YAVG $yavg $aecst${vs:+ | VSYNC $vs} | still ${ST_BYTES}B luma $ST_LUMA ratio $ST_RATIO hdiff $ST_HDIFF | chart R/G B/G $bx | $note"
  echo "$sc,$sz,$step,$r3503,$r3406,$r503D,$r4407,$expLines,$gain16,$awbR,$awbG,$awbB,$yavg,$aecst,$vs,$ST_BYTES,$ST_LUMA,$ST_RATIO,$ST_HDIFF,${bx// /,},$note" >> "$SCSV"
  SC_LINES=$expLines; SC_GAIN=$gain16; SC_LUMA=$ST_LUMA
}
sc_manual_exposure() {  # the UI's own path: AEC off, the slider up, the slider down, AEC on
  local idx=$1 fps=$2 sz=$3 E up down aecMax lumaA settle
  log "-- scenario manual_exposure at $sz $fps fps --"
  SC_EXTRA="3500-3502,350A,350B"
  set_size "$idx" "$Q" "$fps"; sleep 6; [ "$fps" -ge 5 ] || sleep 8
  http_gap; aecMax=$(curl -s -m 10 "$B/control?updateFPS=1" | python "$HERE/jfield.py" aecMax); [ -n "$aecMax" ] || aecMax=1200
  settle=$(settle_for 4 "$fps")
  sc_row manual_exposure "$sz" A_auto "$fps" "AEC auto, settled"; E=$SC_LINES; lumaA=$SC_LUMA
  [ "${E:-0}" -gt 0 ] 2>/dev/null || { log "   ABORT scenario: no exposure reading ($E)"; return; }
  up=$(( E * 3 / 2 )); [ "$up" -le "$aecMax" ] || up=$aecMax; down=$(( E / 2 ))
  ctl aec=0 > /dev/null; sleep "$settle"
  sc_row manual_exposure "$sz" B_aec0 "$fps" "aec=0: predict 0x3503 01, exposure held at $E, luma ~$lumaA"
  ctl "aec_value=$up" > /dev/null; sleep "$settle"
  sc_row manual_exposure "$sz" "C_up_$up" "$fps" "aec_value $up (1.5x): predict exp $up lines, brighter than $lumaA"
  ctl "aec_value=$down" > /dev/null; sleep "$settle"
  sc_row manual_exposure "$sz" "D_down_$down" "$fps" "aec_value $down (0.5x): predict the black frame (luma < 8) - the 4 Sep decrease quirk"
  ctl aec=1 > /dev/null; sleep "$(settle_for 8 "$fps")"
  sc_row manual_exposure "$sz" E_aec1 "$fps" "aec=1: predict 0x3503 00 and luma back near $lumaA"
  ctl "aec_value=$down" > /dev/null; sleep "$settle"
  sc_row manual_exposure "$sz" F_value_under_auto "$fps" "aec_value $down with AEC auto: predict inert"
  ctl "aec_value=$(sf0 aec_value)" > /dev/null; sleep 1
}
sc_manual_gain() {  # AGC off, the gain slider at 0 / 1 / 30 / 63, AGC on
  local n want
  log "-- scenario manual_gain at HD 30 --"
  SC_EXTRA="350A,350B"
  set_size 13 "$Q" 30; sleep 6
  sc_row manual_gain HD A_auto 30 "AGC auto"
  ctl agc=0 > /dev/null; sleep 4
  sc_row manual_gain HD B_agc0 30 "agc=0: predict 0x3503 02 (or 03), gain held"
  for n in 0 1 30 63; do
    want=$(( n * 16 - 1 )); [ "$n" = 0 ] && want=0
    ctl "agc_gain=$n" > /dev/null; sleep 4
    sc_row manual_gain HD "C_gain_$n" 30 "agc_gain $n: predict 0x350A/B $want ($n=0 black)"
  done
  ctl agc=1 > /dev/null; sleep 6
  sc_row manual_gain HD D_agc1 30 "agc=1: predict recovery"
  ctl "agc_gain=$(sf0 agc_gain)" > /dev/null; sleep 1
}
sc_awb() {  # the presets, AWB off, Manual AWB off, simple vs advanced - at QSXGA where the chart box is known
  local m t
  log "-- scenario awb at QSXGA 5 fps, chart box $BOX --"
  SC_EXTRA="3400-3405,5183"
  set_size 23 "$Q" 5; sleep 10
  SC_BOX=1
  sc_row awb QSXGA A_auto 5 "AWB auto, advanced (dcw=1)"
  ctl awb=0 > /dev/null
  for t in 10 20; do sleep 10; sc_row awb QSXGA "B_awb0_${t}s" 5 "awb=0: predict the gains frozen at their last value"; done
  ctl awb=1 > /dev/null; sleep 10
  sc_row awb QSXGA C_awb1 5 "awb=1 again"
  for m in 1 2 3 4; do
    ctl "wb_mode=$m" > /dev/null; sleep 8
    sc_row awb QSXGA "D_preset_$m" 5 "wb_mode $m: predict 0x3406 01 and the driver's fixed gains (1: 5E0/410/540, 2: 650/410/4F0, 3: 520/410/660, 4: 420/3F0/710)"
  done
  ctl wb_mode=0 > /dev/null; sleep 8
  sc_row awb QSXGA E_preset_0 5 "wb_mode 0: predict 0x3406 00, gains moving again"
  ctl awb_gain=0 > /dev/null; sleep 4
  sc_row awb QSXGA F_awbgain0 5 "awb_gain=0: predict 0x3406 00"
  ctl wb_mode=2 > /dev/null; sleep 8
  sc_row awb QSXGA G_preset2_under_awbgain0 5 "wb_mode 2 with awb_gain 0: the UI hides the select; predict 0x3406 01 anyway (the handler calls set_wb_mode directly)"
  ctl wb_mode=0 > /dev/null; sleep 2; ctl awb_gain=1 > /dev/null; sleep 8
  ctl dcw=0 > /dev/null; sleep 30
  sc_row awb QSXGA H_simple_awb 5 "dcw=0 (0x5183[7] simple AWB) after 30 s: the chart R/G B/G against A - the green-bias question"
  ctl dcw=1 > /dev/null; sleep 30
  sc_row awb QSXGA I_advanced_awb 5 "dcw=1 (advanced) after 30 s: predict back to A"
  SC_BOX=0
}
sc_colorbar() {  # the test pattern: what the automatics do on it, and whether a size change clears it
  local t
  log "-- scenario colorbar at HD 30 --"
  SC_EXTRA="503D,3400-3405"
  set_size 13 "$Q" 30; sleep 6
  sc_row colorbar HD A_off 30 "baseline"
  ctl colorbar=1 > /dev/null
  for t in 2 12 22; do sleep 10; sc_row colorbar HD "B_on_${t}s" 30 "colorbar=1: 0x503D bits 7:6 set; AEC/AWB on the pattern"; done
  set_size 25 "$Q" 30; sleep 6
  sc_row colorbar 1280X960 C_after_size_change 30 "framesize 25 with the bar on: predict 0x503D still set (not in the reloaded block)"
  set_size 13 "$Q" 30; sleep 6
  ctl colorbar=0 > /dev/null; sleep 6
  sc_row colorbar HD D_off_again 30 "colorbar=0: predict 0x503D 00 and the scene back"
}
sc_flip() {  # mirror and flip on the cropped / binned custom sizes, singly and together
  local s idx fps sz S0 d
  log "-- scenario flip: 1280X960 (route B), VGANARROW (cropped binned window), FHDNARROW (full-res crop) --"
  SC_VSYNC=1; SC_EXTRA="4514,4520"
  for s in "25:41:1280X960" "28:30:VGANARROW" "16:1:FHDNARROW"; do
    IFS=: read -r idx fps sz <<< "$s"
    set_size "$idx" "$Q" "$fps"; sleep 6; [ "$fps" -ge 5 ] || sleep 8
    sc_row flip "$sz" A_base "$fps" "baseline"; S0="$OUT/sc_flip_${sz}_A_base.txt"
    ctl hmirror=1 > /dev/null; sleep "$(settle_for 4 "$fps")"
    sc_row flip "$sz" B_hmirror "$fps" "hmirror=1: predict only 0x3821 (bits 2:1), 0x4514 change; subsample bytes re-asserted unchanged"
    log "      diff: $(python "$HERE/regsnap.py" diff "$S0" "$OUT/sc_flip_${sz}_B_hmirror.txt" 3814,3815,3820,3821,4514,4520)"
    ctl vflip=1 > /dev/null; sleep "$(settle_for 4 "$fps")"
    sc_row flip "$sz" C_both "$fps" "hmirror=1 vflip=1"
    log "      diff: $(python "$HERE/regsnap.py" diff "$S0" "$OUT/sc_flip_${sz}_C_both.txt" 3814,3815,3820,3821,4514,4520)"
    ctl hmirror=0 > /dev/null; sleep 2; ctl vflip=0 > /dev/null; sleep "$(settle_for 4 "$fps")"
    sc_row flip "$sz" D_restored "$fps" "both off: predict the baseline registers exactly"
    d=$(python "$HERE/regsnap.py" diff "$S0" "$OUT/sc_flip_${sz}_D_restored.txt") || finding "$sz" flip restore RESTORE "$d"
    log "      diff: $d"
  done
  SC_VSYNC=0
}
sc_effect() {  # does special_effect 0 rewrite the SDE enables a brightness change relies on
  log "-- scenario effect coupling at HD 30 --"
  SC_EXTRA="5580,5583,5584,5586,5587,5588,5003"
  set_size 13 "$Q" 30; sleep 6
  sc_row effect HD A_base 30 "baseline"
  ctl brightness=3 > /dev/null; sleep 4
  sc_row effect HD B_bright3 30 "brightness 3: 0x5587/0x5588"
  ctl special_effect=1 > /dev/null; sleep 4
  sc_row effect HD C_effect1 30 "special_effect 1 (negative): 0x5580/83/84, 0x5003"
  ctl special_effect=0 > /dev/null; sleep 4
  sc_row effect HD D_effect0 30 "special_effect 0: predict 0x5580 back to the driver's default - is brightness 3 still in force (luma vs B, 0x5587/0x5588 vs B)?"
  log "      B vs D: $(python "$HERE/regsnap.py" diff "$OUT/sc_effect_HD_B_bright3.txt" "$OUT/sc_effect_HD_D_effect0.txt")"
  ctl brightness=0 > /dev/null; sleep 4
  sc_row effect HD E_restored 30 "brightness 0: predict the baseline"
  log "      A vs E: $(python "$HERE/regsnap.py" diff "$OUT/sc_effect_HD_A_base.txt" "$OUT/sc_effect_HD_E_restored.txt")"
}
wait_close() {  # wait for a new closeAvi block (bounded)
  local prev=$1 i new=""
  for i in $(seq 1 40); do sleep 1; new=$(ramlog | grep -a 'closeAvi\] Recorded' | tail -1); [ -n "$new" ] && [ "$new" != "$prev" ] && { echo "$new" | sed 's/^\[[^]]*\] //'; return 0; }; done
  echo "NO CLOSE"; return 1
}
sc_deferral() {  # fps and framesize sent mid-recording must wait for the clip to close
  local prev
  log "-- scenario deferral at HD 30 --"
  set_size 13 "$Q" 30; sleep 6
  log "   baseline VSYNC $(vsync 30)"
  prev=$(ramlog | grep -a 'closeAvi\] Recorded' | tail -1)
  ctl forceRecord=1 > /dev/null; sleep 5
  ctl fps=15 > /dev/null; sleep 2
  log "   fps=15 mid-clip: $(ramlog | grep -a 'takes effect when the recording stops' | tail -1 | sed 's/^\[[^]]*\] //') | /status fps $(status_field fps)"
  sleep 8; ctl forceRecord=0 > /dev/null
  log "   clip: $(wait_close "$prev")"; sleep 5
  log "   after the clip: VSYNC $(vsync 15) (predict ~15), /status fps $(status_field fps), tuner: $(tuner_line 15)"
  ctl fps=30 > /dev/null; sleep 6
  log "   fps=30 again: VSYNC $(vsync 30)"
  prev=$(ramlog | grep -a 'closeAvi\] Recorded' | tail -1)
  ctl forceRecord=1 > /dev/null; sleep 5
  ctl framesize=25 > /dev/null; sleep 2
  log "   framesize=25 mid-clip: $(ramlog | grep -a 'takes effect when the recording stops' | tail -1 | sed 's/^\[[^]]*\] //') | /status framesize $(status_field framesize) sensor $(status_field sensorState)"
  sleep 8; ctl forceRecord=0 > /dev/null
  log "   clip: $(wait_close "$prev")"; sleep 10
  log "   after the clip: /status framesize $(status_field framesize) fps $(status_field fps) sensor $(status_field sensorState), VSYNC $(vsync 30) (predict ~30 at 1280X960)"
  set_size 13 "$Q" 30; sleep 6
}
sc_window() {  # sharpness at the lowest quality against the JPEG frame window
  local s idx fps sz
  log "-- scenario window: q6 + sharpness 3 at QSXGA (983 KB buffer) and FHDNARROW (443 KB window) --"
  SC_EXTRA="5308,5300"
  for s in "23:5:QSXGA" "16:5:FHDNARROW"; do
    IFS=: read -r idx fps sz <<< "$s"
    set_size "$idx" 6 "$fps"; sleep 10
    sc_row window "$sz" A_q6 "$fps" "quality 6 baseline: bytes vs the window"
    ctl sharpness=3 > /dev/null; sleep 8
    sc_row window "$sz" B_q6_sharp3 "$fps" "sharpness 3: bytes up - a rescue would show in 0x4407"
    log "      rescue line: $(last_rescue)"
    ctl sharpness=0 > /dev/null; sleep 2; ctl "quality=$Q" > /dev/null; sleep 4
  done
}

# ---------------------------------------------------------------- run ----------------------
mkdir -p "$OUT"
cat > "$OUT/prediction.txt" <<'PRED'
UI control regression - predictions, stated before the first measurement (4 Sep 2026).
Driver esp32-camera 4335c93e; the register each control writes is in the SPEC table of
ui_regress.sh and the plan. The tuner set (clock, window, HTS/VTS, AEC limits, 0x3108) is not
written by any picture control, so:
 1. No control changes a strict-set register outside its own expected list, at any mainstay.
 2. VSYNC is unchanged (within 0.5%) at every point; the only rate changes are the deferred
    fps / framesize scenario, after the clip closes.
 3. 0x4407 tracks the config quality everywhere except the window scenario (QSXGA q6 +
    sharpness 3 may trip the rescue; FHDNARROW q6 lit should not).
 4. hmirror / vflip re-assert identical 0x3814/0x3815 and the binning bits; only 0x3820[2:1] /
    0x3821[2:1] and 0x4514 change; window, HTS, VTS, PLL untouched; colour ratio holds.
 5. aec=0 then aec_value UP integrates the new value; aec_value DOWN gives the black frame
    (luma < 8) at both 30 fps and 1 fps; aec=1 recovers within a few frames.
 6. agc=0 + agc_gain 0 -> 0x350A/B 0x0000 and a black frame; n -> n*16-1 otherwise.
 7. gainceiling 0 caps the AEC gain at 0 (dark frame); 511 halves the ceiling; 1023 restores.
 8. wb_mode 1-4 write 0x3406 = 1 and the fixed gains; wb_mode 0 clears it; awb=0 freezes the
    gains; a preset sent with awb_gain=0 still applies (the handler bypasses the UI logic).
 9. colorbar survives a framesize change; the AWB hunts on the pattern.
10. special_effect 0 rewrites 0x5580 to the driver's default; a brightness enable bit may
    be lost (coupling) - measured by luma B vs D.
11. Every restore point's snapshot equals the baseline, EXCEPT possibly saturation (the
    level-0 colour matrix may differ from the init table) and denoise/sharpness (the driver's
    level tables vs the init values) - those are findings if they differ.
12. dcw=0 (simple AWB) moves the chart R/G, direction unknown - the point of measuring.
PRED
if [ -n "${RESTORE_ONLY:-}" ]; then
  # replay a saved run's restore (after an abort with the board unreachable): the trap does it
  [ -s "$STATUS0" ] || { echo "RESTORE_ONLY: no $STATUS0"; trap - EXIT; exit 6; }
  log "== RESTORE_ONLY from $STATUS0 =="
  exit 0
fi
log "== UI control regression: sizes [$SIZES] controls [$CONTROLS] scenarios [$SCENARIOS] quality $Q dry=${DRY:-0} =="
log "board: uptime $(uptime_s)s, llevel=$(status_field llevel) night=$(status_field night) tunedFps=$(status_field tunedFps) gainceiling=$(status_field gainceiling) ae_level=$(status_field ae_level) banding=$(status_field banding)"
http_gap; curl -s -m 15 "$B/status" > "$STATUS0"
[ -s "$STATUS0" ] || { log "ABORT: no /status snapshot"; exit 6; }
log "status0: framesize=$(sf0 framesize) fps=$(sf0 fps) quality=$(sf0 quality) record=$(sf0 record) enableMotion=$(sf0 enableMotion) idleFps=$(sf0 idleFps) micGain=$(sf0 micGain) aec=$(sf0 aec) agc=$(sf0 agc) awb=$(sf0 awb) awb_gain=$(sf0 awb_gain) wb_mode=$(sf0 wb_mode) aec_value=$(sf0 aec_value) agc_gain=$(sf0 agc_gain) aec2=$(sf0 aec2) dcw=$(sf0 dcw) hmirror=$(sf0 hmirror) vflip=$(sf0 vflip) colorbar=$(sf0 colorbar) xclkMhz=$(sf0 xclkMhz)"
assert_campaign_config "$Q"
preflight
af_hold 13 "$Q" 30
log "AF held at VCM 0x$AF_VCM; 0x3000 0x$(regrd 0x3000)"
for s in $SIZES; do
  IFS=: read -r idx fps name <<< "$s"
  run_size "$idx" "$fps" "$name"
done
for sc in $SCENARIOS; do
  case $sc in
    manual_exposure) sc_manual_exposure 13 30 HD; [ -n "$DRY" ] || sc_manual_exposure 16 1 FHDNARROW ;;
    manual_gain) sc_manual_gain ;;
    awb) sc_awb ;;
    colorbar) sc_colorbar ;;
    flip) sc_flip ;;
    effect) sc_effect ;;
    deferral) sc_deferral ;;
    window) sc_window ;;
    *) log "unknown scenario $sc" ;;
  esac
  assert_alive
done
af_check
log "== matrix $CSV, scenarios $SCSV, $NFIND finding(s) =="
