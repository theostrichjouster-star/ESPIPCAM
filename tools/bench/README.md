# Bench sweep scripts

Host-side automation for the frame-rate calibration campaigns (Git Bash + curl + python on
Windows). The method is the one recorded in BOARD_TESTING.md §19-20 and FPS_RECAL.md.

```
export BOARD=<board address>        # never written into the repo
export OUT=FPS_RECAL_stills/retune_$(date +%Y%m%d)   # optional; git-ignored
bash tools/bench/fps_t0_proofs.sh   # campaign config, four one-variable proofs, ceiling+1 probes
bash tools/bench/fps_t1_register.sh # every integer fps per mainstay, regression vs sweep.csv
bash tools/bench/fps_t3_record.sh   # boundary recordings, LIT room, stills for the eyeball gate
bash tools/bench/fps_t4_playback.sh # play each boundary clip back (silence the other board first)
```

Rules the scripts enforce: the board must have been up 240 s; `record=0`, `idleFps=0`,
`tunedFps=1`; the RTC ring is read per point with `grep -a`; a closeAvi block is identified by
its filename, never its frame count; motion counters are read before and after a clip because
reading resets them; a reboot (uptime decreasing) aborts the tier; two consecutive anomalies
abort the tier; every poll is bounded; no background jobs are left behind.

Runtime `/control` changes do not persist - re-assert the standing config and `save=1`
deliberately when the campaign ends. `bench_lib.sh` holds the helpers; the `*.py` files are the
parsers for the retime line, the closeAvi and playback stats blocks, and JPEG dimensions.

## Inventory

Every script takes `BOARD=` from the environment, writes to `OUT=` (default under
`FPS_RECAL_stills/`, git-ignored) with a `campaign.log`, a CSV and the stills, and restores the
field configuration on exit (`trap restore EXIT`). Launch long runs detached (`nohup ... &`
from Git Bash) and tail the log; never edit a script while it runs. Measured results for each
are in BOARD_TESTING.md under the section named.

Library and parsers:
- `bench_lib.sh` - `ctl`, `ramlog`, `status_field`, `regrd` (one register from the ring),
  `preflight`, `assert_campaign_config`, `assert_alive` (reboot detection), `set_size`,
  `record_clip`, and the lens hold: `af_hold <idx> <q> <fps>` (continuous AF at FHD until
  the VCM DAC settles, then the sensor MCU into reset; `AF_VCM_SET=<3602><3603>` places the
  lens at a lit code when the room is dark), `af_check`, `af_resume`, `af_code` (DAC decode)
- `t1_point.py` (retime line + gates), `parse_avi.py` (closeAvi block), `parse_play.py`
  (playback block), `parse_motion.py` (detector counters), `parse_zones.py` (the avgZones
  grid: mean / min / max / YAVG / band / AEC state), `jfield.py` (one `/status` field),
  `jpeg_dims.py`, `still_color.py` (w h bytes, channel means, ratio, saturation, adjacent
  pixel noise hdiff / vdiff - the two gates that catch a corrupt still; the eyeball is the third)

The frame-rate tiers (§19-20, §26, §30a):
- `fps_t0_proofs.sh` - one-variable proofs and ceiling+1 probes
- `fps_t1_register.sh` - the regression tier against `FPS_RECAL_stills/sweep.csv`
  (`SIZES_LIST=`, `REF=-` to generate, `--from <idx>:<fps>` to resume)
- `fps_t3_record.sh` - boundary recordings in a lit room, stills for the eyeball
- `fps_t4_playback.sh` - each boundary clip played back (silence the other board first)
- `fps_ladder.sh` - one row per requested fps for a size: route, clock, HTS, VTS, line time,
  exposure ceiling, settled exposure and gain, VSYNC, a clip, a still
- `frame_window_descend.sh` - the JPEG frame-window cliff by random-pattern quality descend

Clock and route checks (§37):
- `route_b_verify.sh` - proves a flashed image runs 1280X960 on route B at the ceiling
- `pixclk80_check.sh` - the ceiling rungs with the sensor forced onto the 80 MHz tree
- `sclk96_probe.sh` - the route B ladder and HTS walk at 1280X960 (restores 0x3108 first on
  every exit path - copy that order anywhere 0x3108 is written)
- `overdrive_ab.sh`, `subsample_ab.sh` - A/B rigs for the overdrive and the subsample probe

Exposure (§37):
- `dim_check.sh` - per rate in a dim room: exposure line, gain, zones, VSYNC, a clip, a still
- `ae_level_probe.sh` - Exposure Level x banding x fps grid from the ring
- `hts_stretch.sh` - exposure by lengthening the line at the clock floor, HTS walked by
  register at VTS 1968 (`IDX=`, `HTS0=`, `WALK=`, `SCLK=`, `Q=`, `PLL_MUL=` to put the sensor
  on the floor after the tuner's retime, `AF_VCM_SET=`); stills retried at 7/6 of the frame
  period, 0x4407 read per rung. Lit and dark at FHDNARROW and QSXGA to HTS 8191
- `manual_exposure.sh` - AEC and AGC off, exposure and VTS by register past 1964 lines at
  QSXGA on the floor (`POINTS="vts:lines ..."`, `GAIN=`); the control point (same exposure,
  doubled frame) validates the rig before the 2x step. Gain goes through the driver's
  `agc_gain` and the exposure is only ever raised: a manual-mode exposure DEcrease gives a
  persistent flat black frame (found 4 Sep 2026, mechanism unknown)
- `manual_probe.sh` - which registers take effect in manual mode: auto settled, the same
  values by hand, gain 0x3FF, exposure halved, AEC-only manual, then the driver's `agc_gain`
  and `aec` / `aec_value` with their registers read back; one still per step at 1 fps
- `awb_eval.sh` - the AWB during long exposures: QSXGA at fps 5, fps 1, HTS 5600, HTS 8191
  and a manual 3932-line stage; the 12-bit AWB gains (0x3400-0x3405) and the manual bit
  (0x3406) sampled every 10 s, a still per stage with the star-chart box's and the frame's
  R/G and B/G (`BOX=` the chart in QSXGA pixels, `EGAIN=` the manual stage's agc_gain)

Dead ends kept as records (§31, §37) - do not re-walk without a new mechanism:
- `hts_floor.sh` - the HTS floor walk whose gates passed corrupt frames (the reason for
  `still_color.py`)
- `pixclk_floor_walk.sh`, `pixclk_floor_walk2.sh`, `pclk_port_probe.sh` - SCLK below 10 MHz
  by the system divider, the multiplier and the /8 root; the DVP port clock exonerated
- `analog_probe.sh`, `analog_stages.sh` - the binned analog registers (0x3709, 0x370C)
