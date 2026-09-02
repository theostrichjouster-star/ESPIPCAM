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
