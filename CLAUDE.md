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
- **Never fire HTTP requests at a board back to back** (`bench_lib.sh` `http_gap`, run-wide
  through a timestamp file; every helper and every direct curl goes through it). COM4 went off
  the network on 4 Sep 2026 at 18:04:35, seconds after a burst of back-to-back `/status` and
  `/control` requests followed by a size switch, and needed a finger on the button. The gap that
  bought was 5 s; **the measured floor is 1 s** (`MIN_HTTP_GAP`, the default since 5 Sep 2026):
  the UI regression's full run put ~3500 requests through it at 1 s over 3.5 h with no failure,
  no reboot and no peer reset. A register snapshot is still `dumpCam=1` plus a handful of
  `camRegRd`, never a hundred.
- **Peer reset**: COM3's D1 (GPIO 2) is wired to COM4's RESET pad; `/control?peerReset=1` on
  COM3 pulls it open-drain low for 5 s (`peerReset()`, mjpeg2sd.cpp), and `bench_lib.sh`
  `peer_reset` does that with `PEER=<COM3 address>` after a control call to COM4 fails twice
  and 30 s of silence. GPIO 9 is NOT free - it is the SD card command line on both boards
  (`camera_pins.h` `SD_MMC_CMD 9`), and wiring it took COM3 off the network. The far board
  comes back POWERON: RTC ring and crash snapshot gone, SD log tail and restart breadcrumb kept.
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
- **Every file in /data suddenly unopenable? It is the mount's FIVE open-file slots, not the card.**
  `utilsFS.cpp` calls `SD_MMC.begin(...)` with four arguments and the core's fifth is
  `maxOpenFiles = 5`. The SD log holds one, a recording holds its AVI plus the CSV and SRT, and an
  aborted browser transfer leaves another (`WARN sendChunks Failed to send to browser ...
  ESP_ERR_HTTPD_RESP_SEND` opens each window). Past that, EVERY open fails - reads and writes, any
  file - until a remount, while the boot listing still shows the files present with their real
  timestamps. Measured on COM4 twice on 5 Sep 2026 (§38.8); COM3 took the identical uploads at the
  identical bus clock with recording OFF and never failed. **A reset cures it; do not blame the
  card and do not reach for `chkdsk` - I did, and it was wrong.** `/control?peerReset=1` on COM3 is
  the fastest recovery.
- Board acts possessed? Check SD free space first (a full card wedges as a fake wifi
  failure), then audit the host PC for orphaned automation from earlier sessions.
  All host-side polling loops must be bounded.
- **Close every orphaned task before any pause** (user's rule, 5 Sep 2026, after two crashes in
  one campaign). Before a compaction, a handoff, a context pause or the end of a session, stop
  what this session started and SAY so in the pause message: background tasks and monitors
  (`TaskStop`), any `nohup`/background bench run, helper servers and stub processes (check the
  ports, `taskkill /F /PID`), browser tabs opened for testing, and anything still polling a
  board. A crash does not clean up after itself, and an orphaned loop firing at a board is the
  failure mode this file already warns about above - the audit is cheaper than the diagnosis.
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
- 0x3108 (root dividers) survives every framesize change and the driver never touches it; the
  tuner writes it on every retime since Phase B (0x26 BEFORE the PLL, 0x11 after), but a bench
  probe that leaves 0x11 behind doubles the next clock written under it. Restore it to 0x26
  BEFORE any PLL write, and read it back.
- Never trust a still on byte count, dimensions or the AEC's health: the HTS floor campaign
  passed magenta, green-blown and confetti frames on all three. Channel ratio plus adjacent
  pixel noise (`still_color.py`) plus the user's eyeball.
- The binned row-time floor is ~24us and the magenta readout latch is bistable near it: never
  seen at 24.5us, sometimes at 23.75. 1280X960 runs HTS 2156 (24.5us at 88 MHz) for that
  margin; a shorter line needs a soak, not a dozen clean stills.
- The banding filter is off by config (`banding=0`) so the AEC spends the whole frame on
  exposure before gain; `applyAecLimits` re-asserts it after every retime.
- Never program VTS past ~1984 and expect the AEC to use the frame: its range is 1964 x tROW
  (datasheet), and above that VTS it parks at 2-28% of the frame with the gain doing the work
  (measured 4 Sep 2026). The low-fps branch holds VTS at 1968 and lets the frame timer
  decimate; more exposure at 1-2 fps needs a longer tROW, not a longer frame.
- SCLK below 10 MHz is a dead end in the sensor's own clock domain (speckle from 9.6 MHz,
  flat frames by 6.3, by every route, port clock and FIFO exonerated). Lengthen tROW with HTS
  instead: at full resolution HTS is free blanking to 8000 (3.1 s ceiling); binned sizes flip
  their line cost above 2277.
- Continuous autofocus runs from boot. Any measurement that compares stills must hold the
  lens (`af_hold`, MCU reset) and say so; a moving lens changes hdiff and the eyeball alike.
  The AF program cannot focus in low light: it parks the lens wherever it gives up (DAC codes
  42 / 114 / 612 in one dark room against 171-256 lit), so a dark hold must place the lens at
  a lit code by register (`AF_VCM_SET`, datasheet table 3-2: code = 0x3603[5:0] << 4 |
  0x3602[7:4]) with the MCU stopped, and prove it by readback.
- HTS is 13 bits (0x380C[4:0], 0x380D): 8191 is the register's end, 3.18 s of ceiling at the
  10.13 MHz floor. Above the AEC band the sensor trades gain for line length 1:1 (57
  gain-seconds in the dark bench room, measured 4 Sep 2026: 26x at 2.2 s, 20x at 2.7 s, 19x
  at 3.1 s), so the 1.19x floor there would need HTS 120,000 - 15x past the register.
- Stills below ~0.8 fps are a coin flip: the still handler waits `MAX_FRAME_WAIT` (1.2 s) for
  the capture task to keep a frame, so one request lands with probability 1.2 x fps. Retry,
  and read "no still" at a slow rate as that before blaming the frame. Retries at a fixed
  cadence near the frame period are ONE trial, not several (six misses in a row at QSXGA
  7000, requests 2.45 s apart against a 2.56 s frame): walk the phase, 7/6 of the period.
- In the dark the q10 frame does not exist: at 31.9x the noise pushes FHD past its 443 KB
  frame window and QSXGA past the 983 KB buffer, the driver delivers nothing, and the no-frame
  rescue steps the sensor's quality (sticky) until it fits - FHD q20-24, QSXGA q24 in the
  dark and q28 in band (860-955 KB). `/status` still says the config's quality; 0x4407 (JPEG
  CTRL07) and the file's quantizer table say the sensor's.
- The 1964-line exposure limit is the AEC engine's, not the pixel's: with 0x3503 = 0x03 and
  the exposure written by register, 3932 lines gave 5x the AEC's best frame at the same gain
  and 5900+ saturated (4 Sep 2026, BOARD_TESTING §37). But manual mode has two unexplained
  quirks: after entering it the frame does not integrate its register value until a LARGE
  exposure increase is written, and any exposure DEcrease gives a persistent flat black frame.
  **Both belong to FULL manual mode - 0x3503 = 0x03, AEC and AGC both off** (BOARD_TESTING §38.5,
  5 Sep 2026): with AEC manual and AGC left auto (0x3503 = 0x01) the driver's `aec_value` halves
  the exposure cleanly at 30 fps and at 1 fps alike, luma tracking the exposure, and `aec=1`
  recovers in one settle. So the web UI's Manual Exposure slider is not the hazard; the register
  route with both loops off is. Gain through the driver's `agc_gain` is safe both ways; the gain
  scale above 0x1FF is unproven except 0x3FF = 2 x 0x1FF. Leave manual mode by 0x3503 = 0x00
  (AEC auto), never by stepping down. `agc_gain` 0 and 1 are the SAME 1x gain, not a black frame.
- The AWB works at every rate down to 0.35 fps (HTS 8191 at QSXGA) and under manual
  exposure: it is the ISP's frame-counted state machine, so it converges in a handful of
  frames whatever the rate, then holds within 0.5% (4 Sep 2026, `awb_eval.sh`). Its gains
  track the AEC's gain (the high-gain pedestal), not the rate. It hunts on a frame without
  signal (YAVG ~13): give it a lit frame before judging it. The chart box's R/G and B/G is the
  witness; the whole-frame ratio is confounded by the pedestal at high gain. **`awb=0` is raw, not
  frozen**: it clears 0x5001 bit 0 (a read-modify-write, the tuner's scaler bit 5 survives) so the
  ISP stops APPLYING the gains, which keep their converged values in 0x3400-0x3405 - the chart
  falls to 0.46 R/G and stays there (5 Sep 2026, §38.5). A still needs a converged AWB: the
  FHDNARROW baseline of that run was taken too early and read 1.746 where every later point read
  1.10-1.19.
- **`dcw` is not downsizing and its sense is inverted.** The driver's flag and the web page's
  "DCW (Downsize)" label both mislead: it writes 0x5183[7], which the datasheet defines as AWB
  simple enable (0 = advance, 1 = simple, sensor reset default 0x90 = simple). So `dcw=0` gives
  **simple** AWB and bit 7 SET. **Simple is the default since 5 Sep 2026** on the user's decision -
  it measured the more neutral of the two on the star chart, R/G 0.965 and B/G 0.918 against
  advanced's 0.882 / 0.882 - changed in `appConfig` and persisted on both boards (`/status` dcw 0,
  0x5183 = 0x94 on each). **Still owed: the same comparison in a dark room.**
- The colour bar is not in the block `set_framesize` reloads, so **0x503D survives a size change**
  and, being persistable, can boot a board into test bars (5 Sep 2026, §38.5).
- The web page's state tests must go through `isOn()`: `/status` sends every value as a STRING and
  `"0"` is truthy in JavaScript, so `value ? a : b` takes the wrong branch on every load (it hid the
  Manual Exposure slider whenever the AEC was manual). A control the page hides or disables must not
  be sent either (`isInert()`) - a hidden select still reached the sensor. The repeating section
  markers are CLASSES, not repeated ids: `hideBuiltOut` matches class or id, and breaking that makes
  compiled-out sections silently reappear on the builds that need them hidden (5 Sep 2026, §38.11).
- **In `data/MJPEG2SD.htm` the static markup is OV2640's - read the sensor branch, never the HTML.**
  On load the page re-ranges every slider for the detected model (`changeRange` in the
  `OV3660 || OV5640` branch) and relabels `aec2` to "Night Mode", `awb_gain` to "Manual AWB" and
  `dcw` to "Advanced AWB". Reading the markup alone produced two wrong findings and a wrong
  mislabel claim (5 Sep 2026, §38.7). Live for the OV5640: `ae_level` -5..5, `agc_gain` 0..63 with
  1x/64x ends, `aec_value` 0..the live `aecMax`, `gainceiling` 0..1023 since that day (it was 511,
  half the range, so a board at 1023 displayed as 511). A page claim is only true if read off a
  board.
- The gain ceiling is `gainceiling~1023` (63.9x, the datasheet's 64x) since 4 Sep 2026,
  persisted on both boards by `save=1`; 511 (31.94x) was a repair value from eb61cf1, not a
  limit. Gain-seconds figures measured before 15:52 that day were under the 511 ceiling.
- At request 1 the tuner will not go below 1 fps: QSXGA runs 11.33 MHz at HTS 2844 (the 10.13
  floor gives 0.905), so its 8191 ceiling is 2.84 s where FHDNARROW's is 3.18 s. Measured to
  the register's end at QSXGA in the dark: 2840 ms at 23x, 0.27 fps, every rung at 1964 lines;
  and on the floor by a multiplier write after the retime (`PLL_MUL=76`, 0x3108 0x26 and
  0x3035 0x51 checked first): 3176 ms at 19x, 0.308 fps counted. The exposure is the line
  times 1964 and nothing else; the clock and the size only set the line.

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
- `UI_REVIEW.md` - the web UI's camera controls: what the 5 Sep regression found, and every
  proposal with its measurement (committed). **All of A1-A4, B1-B3, C1-C5 and D1-D3 are done and
  deployed**; B1/B2 and part of C3 are retracted in place (I had read the static markup - see the
  sensor-branch rule above). After any UI change, `DRY=1` on `ui_regress.sh` is the smoke test and
  the full run is the gate. Still owed: the dark-room AWB comparison, the `colorbar` firmware half
  (never persist, clear at boot), the `wb_mode` firmware gate, and the open-file leak audit
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
- **Log timestamps are UTC** (the boards' `timezone` is GMT0), seven hours ahead of this
  bench's local clock in September. A grep for a local hour in the SD log lands on another
  day's traffic: on 4 Sep 2026 that turned the morning's probes (11:04 local = 18:04 UTC)
  into a phantom "orphaned script" running during an 18:04 local incident. Convert first.

## On-board diagnostics (`/control?<key>`)

Registers and timing:
- `dumpCam=1` - clock tree, PLL decode, **geometry** (window / subsample / ISP input /
  pre-scale / output / offsets / binning bits / scaler enable), timing, exposure, JPEG state
- `camRegRd=0x3800` - read ONE register (LOG_INF, reaches the ring)
- `camReg=0x380C,0x0A` - write one register. Lost on the next `set_framesize`. Writing
  timing registers mid-stream has hung the board once (BOARD_TESTING E3) - order writes so
  no transient is invalid, and never write a VTS below the rows being read
- `camRegGrp=0x380C,0x08,0x380D,0x40` - up to 8 registers landed together at the next frame
  boundary (datasheet 0x3212 group write). Use this for HTS, never two plain camReg writes:
  a mid-frame HTS write latched a magenta cast that a whole campaign then blamed on the line
  length (BOARD_TESTING §37). The control query string is refused at 64 chars ("Query string
  too long"), so one call carries at most 4 pairs - split larger sets and order the calls so
  no interval is invalid
- `xclkStat=1` - **ground truth**: counts XCLK and VSYNC off the pins and back-solves the
  implied pixel clock. Refused while capturing. Use this, never the computed figure
- `camPll=<csv>` - set the PLL directly; `subSample=<y>,<vts>[,<x>]` - subsample probe,
  kept as the record of a dead end (§31)

State and budgets:
- `updateFPS=1` - fpsCeil, aecMax, budgetKBs, live frameKB, govBoost, frameCapKB
- `motionStats=1`, `zoneStats`, `avgZones` - detector counters and the AEC 4x4 zone grid
- `sdBusClk` / `sdBusDiv`, `battScale` / `sagTest`, `extDVDD`, `lencFhd` (LENC A/B)
- `peerReset=1` - pulse the OTHER board's RESET through D1 / GPIO 2 for 5 s (`=<ms>` 500-10000,
  `=0` reads the pin: 1 = the other board's EN is high). Wired COM3 -> COM4 only; on a board
  with nothing on D1 it is a harmless pulse. The far board comes back POWERON (RTC ring and
  crash snapshot gone). An EN reset is NOT a power cycle: on 4 Sep 2026 COM4 came back from
  the pulse alive but handling every packet ~1 s late (TCP body at 35 B/s, its own gateway
  ping failing, the wifi supervisor flapping the link) and only a real power cycle cured it -
  the card and the radio keep their state through EN. Prefer a power cycle after a wedge
- `banding=0|50|60` - the mains banding filter, persisted with `save=1`. 0 (the default) is
  off: the AEC then spends the whole frame on exposure before gain. 50/60 select the manual
  band; `dumpCam` reports the live state on its Exposure line

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
- `hts_floor.sh` - the HTS walk. Its AEC/byte gates PASSED CORRUPT FRAMES (BOARD_TESTING §37);
  never trust a still that has not been through `still_color.py` and an eyeball
- `sclk96_probe.sh` - the in-spec clock route (0x3108 = 0x11, SCLK = mul x 4/3) ladder and HTS
  walk at 1280X960; restores 0x3108 FIRST on every exit path, copy that order anywhere 0x3108
  is written
- `route_b_verify.sh` - proves from registers, VSYNC and both still gates that a flashed image
  runs 1280X960 on route B at the ceiling (41, HTS 2156) and route A below, that the banding
  filter is off and Exposure Level -2 persisted, and that HD after it is back on 0x26
- `fps_ladder.sh` - one row per requested fps for a size: route, PIXCLK, HTS, VTS, line time,
  max exposure, the settled exposure and gain, one VSYNC count, a forced recording (delivered
  fps, frame KB, storage ms, SD kB/s) and a still through `still_color.py`. Prints the table
- `pixclk80_check.sh` - the ceiling rungs with the sensor forced onto the 80 MHz tree
- `dim_check.sh` - the exposure line, gain, zones, VSYNC, a clip and a still per rate in the dark
- `pixclk_floor_walk.sh`, `pixclk_floor_walk2.sh`, `pclk_port_probe.sh` - the SCLK-below-10 MHz
  dead end (BOARD_TESTING §37): speckle from 9.6 MHz down by either route, dead by 6.3; the DVP
  port clock and the JPEG FIFO flag exonerated. Do not re-walk without a new mechanism
- `hts_stretch.sh` - exposure by lengthening the LINE at the clock floor: HTS walked up by
  register at VTS 1968 (the AEC's range); full resolution clean to HTS 8000 = 3.1 s ceiling at
  0.32 fps. Group writes do not land at ~1 fps (the launch poll misses the frame), so it
  writes the pair plainly, high byte first when raising. `Q=` sets the campaign quality,
  `SCLK=` the tuner's clock for the size (10.13 FHDNARROW, 11.33 QSXGA - the "Tuned timing"
  line is the check), `IDX=`/`HTS0=`/`WALK=` the size and rungs, `PLL_MUL=76` moves the
  sensor to the floor after the tuner's retime (route and divider checked, the tuner's own
  0x3036 write, read back; the restore's size reload puts the tuner's value back),
  `AF_VCM_SET=B20A` places the lens for a dark run, the still is retried up to six times at 7/6 of the frame period
  (`stillTries` column) and 0x4407 is read per rung (`qs` column, the rescue's sticky
  quality). In the dark (BOARD_TESTING §37) the AEC used the whole ceiling at every rung at
  FHDNARROW and QSXGA to HTS 8191, and the gain came off 31.9x at HTS 5600 in both; the mic
  is off for the run (`micGain=0`)
- `manual_exposure.sh` - AEC and AGC off, exposure and VTS by register past 1964 lines at QSXGA
  on the floor (`POINTS="vts:lines ..."`, `GAIN=`): a control point (same exposure, doubled
  frame) validates the rig before the 2x, 3x and 3.4x steps; gain through the driver's
  `agc_gain`, the exposure only ever raised (see the manual-mode rule above)
- `awb_eval.sh` - does the AWB work during long exposures: QSXGA at fps 5, fps 1, HTS 5600,
  HTS 8191 and a manual 3932-line stage, the AWB gains (0x3400-0x3405, 0x3406) sampled every
  10 s and a still per stage with the star-chart box's R/G and B/G (`BOX=`, `EGAIN=`)
- `manual_probe.sh` - which registers take effect in manual mode: auto settled, the same values
  by hand, gain 0x3FF, exposure halved, AEC-only manual, then the driver's `agc_gain` and
  `aec` / `aec_value` with their registers read back. One still per step at FHDNARROW 1 fps
- `bench_lib.sh` `af_hold` / `af_check` / `af_resume` - the lens hold for low-rate work. The AF
  program runs continuous AF from boot; it has no pause command (only 0x03 / 0x04), a release
  sends the lens to rest, and at 1-2 fps it neither converges nor answers. Hold = focus at FHD
  10 fps until the VCM DAC (0x3602/03) settles, then the sensor MCU into reset (0x3000 bit 5);
  release restarts the program. Check the VCM before and after, always. In low light the
  program parks instead of focusing, so pass `AF_VCM_SET=<3602><3603>` from a lit hold
  (0xB20A = code 171) and the hold writes it after the reset; `af_code` decodes a pair
- `analog_probe.sh`, `analog_stages.sh` - the binned analog register dead end (BOARD_TESTING
  §37): 0x3709 moves nothing, 0x370C=0x03 scrambles colour. Reusable as an HTS A/B walk at
  VGA with any register set in REFSET, and as a stills-per-register-stage rig
- `ae_level_probe.sh` - Exposure Level x banding grid x fps: the settled exposure, gain and
  YAVG per point, all from the ring. Exposure Level -2 is the persisted default, and the
  banding filter is OFF by config default (`banding=0`; 50 or 60 re-enable it manual) since
  4 Sep 2026 - the driver's own init is manual 50 Hz, which held the AEC at two bands plus
  gain near the ceiling. Both decide how bright a still looks before the tuning does
  (BOARD_TESTING §37)
- `ui_regress.sh` - the web UI's camera controls, every `/control` key the page can send, one at
  a time at the three mainstays (HD 30, 1280X960 41, FHDNARROW 1): min / max / the live default,
  each with a register snapshot diffed against the size's baseline (`regsnap.py`: the tuner
  set from `dumpCam=1` read back off the SD log plus the core three and the control's own
  registers by `camRegRd` - the control's own expected, anything else a finding, the AEC/AWB
  live set logged; ~35 s per snapshot under the 5 s HTTP gap),
  `/status` readback, VSYNC, a still, 0x4407 and the rescue / WRN lines; the default must diff
  clean. Scenarios replay the UI's own sequences (manual exposure and gain, the AWB presets,
  colour bar across a size change, flip/mirror on the cropped sizes, the special-effect
  coupling, mid-recording fps / framesize, sharpness at q6 against the frame window). Hidden
  keys are audit-only, never sent; `DRY=1` is the HD smoke run (`CONTROLS=none` for a
  scenario-only run); `PEER=<COM3 address>` arms the peer reset; `RESTORE_ONLY=1 OUT=<run dir>`
  replays a run's restore after an abort; the exit restore replays the start-of-run `/status`
  snapshot and proves it by register before idleFps / detection / recording return. Never
  `save=1`. Full matrix plus scenarios measured **3.5 h at `MIN_HTTP_GAP=1`** (214 matrix points +
  59 scenario rows, 4-5 Sep 2026; it was ~10.5 h at the old 5 s gap). Run it with the lamp held.
  The rate gate is rate-dependent (6% below 10 fps: the VSYNC counter's window is a dozen edges at
  1 fps), the WRN gate compares only a line's tail (the RTC ring clips ageing lines from the
  front), and a key `/status` does not report is an audit note not a finding
- Parsers: `t1_point.py` (retime line + gates), `parse_avi.py`, `parse_play.py`,
  `parse_motion.py`, `parse_zones.py` (the avgZones grid: mean / min / max / YAVG / band / AEC
  state), `jfield.py` (/status field), `jpeg_dims.py`, `still_color.py` (channel ratio +
  adjacent-pixel noise: the two gates that catch a corrupt still), `regsnap.py` (register
  snapshot list / parse / diff / live). `tools/bench/README.md` carries the full inventory with
  each script's purpose and env knobs

Reference data: `FPS_RECAL_stills/sweep.csv` is the current register reference (224 points
across 9 sizes as of 4 Sep 2026, 1280X960 at HTS 2156 / ceiling 41); the displaced rows live
beside it as `sweep_20260904_hts2112_1280X960.csv` and `sweep_20260904_pre_route_b_1280X960.csv`,
and `sweep_20260828_flat_overdrive.csv` is the superseded pre-overdrive one, all kept
deliberately as measurement records.

## Tuner entry points (mjpeg2sd.cpp)

`applySensorTuning()` is the one place that runs after every `set_framesize`, in order:
`setOutputSize` (custom output) -> `applyCropWindow` (readout geometry, all sizes now,
binned included) -> `applyHtsFloor` (binned line length) -> `applyScalerClock` **or**
`applyTunedTiming` (rate) -> `applyAecLimits` (banding steps and exposure ceiling, last because
it reads back whatever landed; it also re-asserts the configured `banding` state through
`applyBanding`). `cropPreScaleW()` picks each size's target pre-scale;
`camClocks()` is the single clock decode; `senLineFactor()` returns 1 binned, 2 full-res.
