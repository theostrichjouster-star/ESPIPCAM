# CLAUDE.md

Fork of s60sc/ESP32-CAM_MJPEG2SD for the XIAO ESP32S3 Sense + OV5640. Two bench
boards, heavily instrumented, iterated against real hardware.

**Read BOARD_TESTING.md before any hardware work.** It is the local bench notebook -
deliberately gitignored (it holds board LAN addresses, efuse MACs, and measured
session history) - and it is the source of truth for board identities, calibration
values, and every measured law. If it is missing, ask the user for it; do not guess
board addresses. Keep this file free of IPs and MACs too: the repo is public.

## Build and flash

- Compile: `arduino-cli compile -e --fqbn "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" .`
- OTA (preferred): arm with `/control?startOTA=<name.bin>`, then POST the RAW body:
  `curl --data-binary "@build/esp32.esp32.XIAO_ESP32S3/ESP32-CAM_MJPEG2SD.ino.bin" http://<board>/upload`
  Never `-F`/multipart. UI/data files (.htm) go through the same startOTA gate and
  land in /data.
- Rollback ladder: a fresh image boots PENDING_VERIFY and is confirmed only after
  camera + storage + wifi validate (otaConfirm); unconfirmed image + any reset =
  automatic revert. After a failed-looking deployment, check WHICH image is actually
  running before blaming the transfer.
- Serial fallback: manual boot mode (hold BOOT, tap RESET, release BOOT), then
  `arduino-cli upload -p <port> --fqbn "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" .`
  This is NOT app-only (an earlier version of this file said so and was wrong):
  platform.txt writes bootloader@0x0, partitions@0x8000, boot_app0@0xe000 and
  app@0x10000 together. It DOES preserve NVS at 0x9000 (wifi creds, battScale
  calibration, the COM3 DVDD mod) and the SD card, which is the part that matters.
  Because it rewrites the bootloader it also resets otadata - so it destroys the
  known-good image in the other OTA slot. Prefer OTA when that revert path matters.
- Custom core (raised lwip TCP send buffer - see tools/core/README.md): select the
  tree per-invocation, never install it over the stock one:
  `arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" --build-property "tools.esp32-arduino-libs.path=C:\esp32libs\lwip65535" --build-property "runtime.tools.esp32-arduino-libs.path=C:\esp32libs\lwip65535" --build-path "$(pwd)/build-65535" .`
  A plain compile silently produces a STOCK-core image; `lwipSndBuf` in /status is
  the only thing that reveals which core a running board carries.
- NEVER: `erase_flash`, formatSD, or anything that clears NVS. A corrupted SD FAT is
  repaired with `chkdsk /f` on a PC, never by formatting.

## Board HTTP interface

- `/status` - JSON of all state
- `/control?<key>=<val>` - set config (RAM only); `/control?save=1` persists
- `/control?displayLog=1` - RAM log; lives in RTC memory and SURVIVES resets, so it
  holds the pre-crash tail the SD log may be missing
- `/web?log.txt` - SD log (large; fetch with `-m 90`)
- `/sustain?stream=0` - live MJPEG stream; `/control?sfile=/` - file listings
- `/control?reset=1` - soft restart

## Bench discipline

- One variable at a time, with the prediction stated BEFORE the measurement.
- **Silence the other board before any RF/throughput measurement.** The two boards
  sit inches apart and desensitize each other - measured ~2x on throughput. Every
  measurement taken before this was understood is depressed by an unknown amount.
  `esptool --chip esp32s3 --port COMx --before default_reset --after no_reset
  read_mac` parks a board in download mode with the radio fully off;
  `--before no_reset --after hard_reset chip_id` brings it back.
- Record link RTT alongside every throughput number. Throughput here is
  window/RTT, so a radio drift masquerades perfectly as a config effect. RF also
  varies hugely by time of day: 3-5ms RTT at 2am vs 150-250ms midday.
- Discard the first ~2-4 minutes after any boot. Early runs are garbage (seen: 5,
  10 and 123 frames where the settled figure was ~500) and one was nearly misread
  as a catastrophic regression.
- Windows `ping` counts "Destination host unreachable" replies FROM THE ROUTER as
  received, so 0% loss can mean the host is entirely gone. Check `arp -a`.
- Verification is measured, not structural: the user eyeballs stills/clips; power via
  inline USB meter (5V side, includes charge offset - deltas are the signal);
  voltages via multimeter. Structural checks alone pass on corrupt frames.
- Concurrency fixes get soak tests (races need repetition, not one green run).
- Board acts possessed? Check SD free space first (a full card wedges as a fake wifi
  failure), then audit the host PC for orphaned automation from earlier sessions.
  All host-side polling loops must be bounded.
- Serial diagnostics: 115200 baud. **Opening the port SOMETIMES reboots the board** -
  measured 3 Sep 2026 both ways within 20 minutes, same script, DtrEnable/RtsEnable
  set false before Open(): once `rst:0x15 (USB_UART_CHIP_RESET)` and a full reboot,
  once no reset at all. An earlier version of this file called the combination proven
  non-resetting, which is wrong; calling it always-resetting is equally wrong. Treat
  it as unpredictable. Consequence: any "board is dead, no serial output" call made by
  attaching after the fact may be reading a board the attach just reset. To watch an
  event, open the port FIRST, let the board settle, and keep it open - never attach
  mid-incident and never attach to preserve a state you care about. When that reset
  does happen it preserves RTC memory (reason 11, crash snapshot fires), unlike the
  RESET button which reports POWERON and wipes it, so it is the gentler recovery when
  it works at all. On a genuinely wedged board it does not work: the USB peripheral is
  gone with the CPU, which is what the pySerial write timeout means.
- Panic output goes to the IDF console, which is UART0 (`CONFIG_ESP_CONSOLE_UART_DEFAULT`)
  with USB Serial/JTAG only secondary. The ROM banner does reach USB, so USB is usable,
  but do not assume a silent USB port means a silent chip.
- Windows traps: use the Edit tool, never sed (CRLF corruption); no double quotes
  inside PowerShell here-string commit messages; `taskkill /F /PID <pid>`.

## Hard-won code rules

- Sensor rate authority is desiredFPS() -> applySensorTuning(). setFPS() alone only
  changes the frame timer - the sensor keeps streaming at its programmed rate.
- Task teardown must quiesce, never kill: wait for capturePassBusy to clear (task
  parked at its notify wait) before vTaskDelete, or the orphaned SCCB/FATFS lock
  deadlocks whatever takes it next (see OTAprereq).
- Weak C symbols from the core (e.g. verifyRollbackLater) need `extern "C"` - a C++
  definition mangles to a different name and the core default silently wins.
- SD log durability: fflush BEFORE fsync; WRN/ERR lines force a sync, INF lines may
  sit in the stdio buffer - after a crash, trust the RTC RAM log for the tail.
- Logging must stay non-blocking (LOG_SEND is bounded at 50ms with a drop counter).
- jsonBuff is shared by design but only ever touched from the single httpd worker -
  keep it that way.
- Never program HTS above 2277 on a binned frame size (line-cost flip).

## Git

- Remote is the ESPIPCAM fork; push with `git push origin HEAD:main`.
- Small, independently verified commits - the rollback ladder plus git history is
  the bisection safety net.
- BOARD_TESTING.md and the other local bench notes stay untracked; never commit
  them and never copy board addresses into tracked files.

## Diagnosing the stream

`/status` and the end-of-stream log line carry the instruments; use them before
theorising. `streamSkipped` (logged as "N skipped (sender busy)") counts frames
the capture task had ready but could not hand over because the previous one was
still being sent. sent + skipped == frames the sensor offered, which equals the
capture rate - that is the built-in calibration.

- **skipped high** -> transport-bound (the sender is blocked in httpd send).
- **skipped ~0** -> camera-bound; the transport is delivering everything offered.

`/status` also carries `lwipSndBuf`/`lwipWnd`/`lwipMss`/`idfVer` (which core the
image was built against) and `int_free`/`int_block`/`int_min`/`psram_min`. The
MINIMUM-ever memory figures are the ones that matter: a burst that briefly
squeezes memory is invisible to instantaneous polling, and the failure it causes
shows up only on the NEXT boot as a refused camera frame buffer.

## Docs map

- `BATTERY.md` - battery deployment guide (committed)
- `tools/core/README.md` - custom arduino-esp32 core: why, how to build, the four
  version pins, the sdkconfig gate, and candidate future config changes (committed)
- `tools/bench/README.md` - how to run the sweep campaigns and the rules they enforce
- `BOARD_TESTING.md` - local bench notebook: boards, calibrations, measured laws.
  Numbered sections; §12 is the register cheat sheet, §19-20 the fps/frame-window
  method, §31-34 the recent dead ends and campaigns
- `THERMAL_SOAK.md`, `FPS_RECAL.md` - local bench campaigns (untracked)
- `OV5640_*_post.md` - writeups of the DVDD and overheating work (untracked)

## Datasheets - READ THEM, they are in the repo

**Both are here as searchable markdown, gitignored** (third-party copyright, never
redistribute through this public fork):

- `OV5640_datasheet.md` - 251KB, full conversion. Pipe tables survived the conversion, so
  register tables and mode tables are greppable
- `esp32_s3_datasheet_en-3367378.md` - 112KB, Espressif doc 3367378

**Search them before answering any register, mode or timing question.** They are cheap to
grep and answer things this project previously guessed at:

```
grep -n -i "table 2-1" OV5640_datasheet.md     # find a table, then sed -n around the hit
grep -n "0x3814" OV5640_datasheet.md            # a register's row in the big table
grep -n -i "binning\|subsampl" OV5640_datasheet.md
```

Register rows look like
`|0x3814|TIMING X INC|0x11|RW|Bit[7:4]:...|Horizontal odd subsample increment...|`, so
grepping the bare address lands on both the definition and every mention.

**Never infer datasheet content.** Two costly precedents: the SCLK/PIXCLK misnaming sent a
whole investigation after an ISP clock ceiling that does not exist, and I twice told the
user a capability was missing that was already in the firmware. If a document is not to
hand, ask - do not reconstruct it from memory.

Second-hand readings are scattered through BOARD_TESTING.md and the code comments, always
cited by section: table 2-1 scaling methods and per-mode pixel clocks (§10), table 4-2
subsample increments (§31), §3.2 auto vertical binning (§31), figure 4-2/4-3 windowing and
pre-scale arithmetic (`applyCropWindow`), table 8-5 pixel clock limits (`camClocks`).
Those are our reading; the files above are the source. Prefer the source.

## Where log output actually goes

Getting this wrong wastes a whole diagnostic cycle - `dumpCam` looks silent if you only
read the RTC ring.

- `LOG_DIA` -> **SD log only**, deliberately, to keep boot dumps out of the 7KB RTC ring.
  `dumpCamRegs()` is entirely LOG_DIA. Read it with `/web?log.txt` (large, use `-m 90`).
- `LOG_INF` and above -> RTC ring **and** SD. `/control?displayLog=1` reads the ring,
  which survives resets. `camRegRd` is LOG_INF, so it lands in the ring.
- `LOG_WRN`/`LOG_ERR`/`LOG_ALT` force an SD sync; INF may sit in the stdio buffer.

## On-board diagnostics (`/control?<key>`)

Registers and timing:
- `dumpCam=1` - clock tree, PLL decode, **geometry** (window / subsample / ISP input /
  pre-scale / output / offsets / binning bits / scaler enable), timing, exposure, JPEG state
- `camRegRd=0x3800` - read ONE register (LOG_INF, reaches the ring)
- `camReg=0x380C,0x0A` - write one register. Lost on the next `set_framesize`. Writing
  timing registers mid-stream has hung the board once (BOARD_TESTING E3) - order writes so
  no transient is invalid, and never write a VTS below the rows being read
- `xclkStat=1` - **ground truth**: counts XCLK and VSYNC off the pins and back-solves the
  implied pixel clock. Refused while capturing. Use this, never the computed figure
- `camPll=<csv>` - set the PLL directly; `subSample=<y>,<vts>[,<x>]` - subsample probe,
  kept as the record of a dead end (§31)

State and budgets:
- `updateFPS=1` - fpsCeil, aecMax, budgetKBs, live frameKB, govBoost, frameCapKB
- `motionStats=1`, `zoneStats`, `avgZones` - detector counters and the AEC 4x4 zone grid
- `sdBusClk` / `sdBusDiv`, `battScale` / `sagTest`, `extDVDD`, `lencFhd` (LENC A/B)

Destructive or dangerous:
- `wdtTest` - **DO NOT RUN.** Wedged the board 3 of 3 times and never fired a watchdog
  reboot (§29). Needs the LOG_WRN heartbeat first
- `crashTest`, `bodLevel`/`bodDump`, `formatSD` (never)

## Bench scripts (`tools/bench/`, `BOARD=<addr>` from the environment)

- `bench_lib.sh` - shared helpers: preflight (240s uptime, record=0, idleFps=0,
  tunedFps=1), campaign config, per-point ring reads, reboot detection, two-anomaly abort
- `fps_t0_proofs.sh` - one-variable proofs and ceiling+1 probes
- `fps_t1_register.sh` - **the regression tier**: every integer fps per mainstay against
  `FPS_RECAL_stills/sweep.csv`. `SIZES_LIST=` overrides the size set for one run,
  `REF=-` generates a reference instead of checking one, `--from <idx>:<fps>` resumes
- `fps_t3_record.sh` - boundary recordings, lit room, stills for the eyeball gate
- `fps_t4_playback.sh` - plays each boundary clip back (silence the other board first)
- `frame_window_descend.sh` - frame-window cliff by random-pattern quality descend
  (§20 method, §34 run). Recordings not stills, low fps, abort on any HTTP failure
- `overdrive_ab.sh`, `subsample_ab.sh` - the A/B rigs for those two changes
- Parsers: `t1_point.py` (retime line + gates), `parse_avi.py`, `parse_play.py`,
  `parse_motion.py`, `jfield.py` (/status field), `jpeg_dims.py`

Reference data: `FPS_RECAL_stills/sweep.csv` is the current register reference (222 points
across 9 sizes as of 3 Sep 2026); `sweep_20260828_flat_overdrive.csv` is the superseded
pre-overdrive one, kept deliberately as a measurement record.

## Tuner entry points (mjpeg2sd.cpp)

`applySensorTuning()` is the one place that runs after every `set_framesize`, in order:
`setOutputSize` (custom output) -> `applyCropWindow` (readout geometry, all sizes now,
binned included) -> `applyHtsFloor` (binned line length) -> `applyScalerClock` **or**
`applyTunedTiming` (rate) -> `applyAecLimits` (banding and exposure ceiling, last because
it reads back whatever landed). `cropPreScaleW()` picks each size's target pre-scale;
`camClocks()` is the single clock decode; `senLineFactor()` returns 1 binned, 2 full-res.
