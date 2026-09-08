# CLAUDE.md

Fork of s60sc/ESP32-CAM_MJPEG2SD for the XIAO ESP32S3 Sense + OV5640. Two bench
boards, heavily instrumented, iterated against real hardware.

**Read BOARD_TESTING.md before any hardware work.** It is the local bench notebook -
deliberately gitignored (it holds board LAN addresses, efuse MACs, and measured
session history) - and it is the source of truth for board identities, calibration
values, and every measured law. If it is missing, ask the user for it; do not guess
board addresses. Keep this file free of IPs and MACs too: the repo is public.

## Build and flash

- **THE WEB UI IS BUILT. Edit `src/web/`, never `data/`** (6 Sep 2026, §38.28). `data/` holds the
  minified and pre-gzipped output of `tools/web/build.mjs`, and it is COMMITTED because
  `setupAssist.cpp` `checkDataFiles()` re-downloads those exact paths from this repo whenever a card
  loses them - which is what a `CFG_VER` bump does. 342KB became 42KB on the wire.
  `node tools/web/build.mjs` after any web change, `--check` before committing or uploading one
  (it fails on a stale `data/`, and on a hand edit of it). Full detail in `tools/web/README.md`
- Compile: `arduino-cli compile -e --fqbn "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" .`
- OTA (preferred): arm with `/control?startOTA=<name.bin>`, then POST the RAW body:
  `curl --data-binary "@build/esp32.esp32.XIAO_ESP32S3/ESP32-CAM_MJPEG2SD.ino.bin" http://<board>/upload`
  Never `-F`/multipart. UI/data files go through the same startOTA gate and land in /data - and the
  names to send are now `MJPEG2SD.htm.gz` and `common.js.gz`, not the plain ones.
- Rollback ladder: a fresh image boots PENDING_VERIFY and is confirmed only after
  camera + storage + wifi validate (otaConfirm); unconfirmed image + any reset =
  automatic revert. After a failed-looking deployment, check WHICH image is actually
  running before blaming the transfer. **The tell is the `otaConfirm` line on the boot
  that follows**: a fresh image boots PENDING_VERIFY and logs "OTA image confirmed
  valid" ~45 s in, while an unchanged one is already valid and `otaConfirm` returns
  silently. No confirm line on that boot means the image did NOT change, however
  convincingly the board rebooted (COM3, 5 Sep 2026: curl 56, a reboot, and the old
  image still running - §38.15).
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
- `/sustain?stream=0` - live MJPEG stream; `/control?sfile=/` - file listings (recordings AND
  saved stills since §38.20; the cached `.thm` thumbnails never appear)
- `/file?path=/20260905/x.avi` - ANY file on the card, behind auth and `pathIsSafe()`. `/web?` only
  reaches `/data`. Add `&thumb=1` for a cached 160x90 tile, generated on first request from the
  clip's MIDDLE frame and refused while capturing. **This is also the playback path now** (§38.22):
  the page fetches the whole clip through it and plays it in the browser. `CHUNKSIZE` is **32KB**
  since 5 Sep 2026 (was 4KB), worth a measured **+7.5%**: 1.317 -> 1.416 MiB/s (§38.23). That is far
  less than the third predicted, because `sendChunks` reads through `File::read()` - stdio `fread`
  with a 4KB `setvbuf` buffer - so a 32KB read is still eight 4KB refills and only the send count
  fell. **Do not grow it again**: the card does 4.75 MB/s on the playback path, so the link is the
  constraint, and the next move if any is bypassing stdio, not a bigger buffer
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
- **`/control?fileProbe=1` reports how many file slots are free.** It opens one file repeatedly
  until the mount refuses, then closes them all. Take a reading, do the suspect operation, take
  another: the difference is the leak, per operation. Healthy idle is **14 free with SD logging on,
  15 with it off** - anything less is a leak.
- **THE open-file leak was the SD log, fixed §38.21** - not the aborted browser transfer this file
  and §38.8 blamed for months. `remote_log_init_SD()` assigned NULL over a live `FILE*` and
  reopened, and `remote_log_init()` runs at boot AND on every `logType` / `sdLog` change, so one
  descriptor was abandoned per call, unbounded (measured 12/11/10/9/8). Every boot leaked two -
  the tell was "Opened SD file for logging" appearing TWICE in the boot log.
- **DO NOT use `/sustain?download=0` - it wedges the web server** (§38.21, open, unfixed). It does
  NOT need an abort: a plain completed download took HTTP down on the first try after a boot, and
  the board then answers ping and nothing else until it is reset. Four wedges, four peer resets.
  **Use `/file?path=` instead** - same file, measured safe over 50MB and over aborts, because it
  runs on the httpd task rather than the async sustain handler. `/sustain?playback=0` and
  `?stream=0` use that same task and are fine, so it is the download branch specifically.
  It is NOT the HTTP framing: rebuilding it as a correct fixed-length response wedged it too, and
  that attempt is reverted. The only build where downloads worked was one whose `LOG_ALT` tracing
  added ~4 s of `delay()` to the path, so it is a race or a resource exhaustion - note this board
  carries the custom core's 65535 lwip send buffer.
  **Never test this on COM3**: it is the peer-reset lever for COM4.
  **Nothing in the page reaches it any more** since 6 Sep 2026 (§38.25): the Download button fetches
  `/file?path=` per file. The handler is still compiled and still wedges; only the way an ordinary
  user got there is gone.
- **Every file in /data suddenly unopenable? It is the mount's open-file slots, not the card.**
  `utilsFS.cpp` `prepSD_MMC()` passes `maxOpenFiles = 15` (raised from the core's default of 5,
  before 5 Sep 2026 - earlier notes here and in §38.8 / §38.19 say five and are STALE). The SD log
  holds one, and a recording holds its AVI plus the CSV and SRT. **An aborted browser transfer does
  NOT leak a slot** - measured on COM4, 6 Sep 2026 (§38.34): three `/file?path=` downloads killed
  mid-transfer, `fileProbe` reading 14 free before and after every one. The claim that it did was
  this file's own, it predates §38.21 finding the real leak in the SD log, and it is now retracted by
  measurement rather than by argument. `sendChunks` does log
  `WARN Failed to send to browser ... ESP_ERR_HTTPD_RESP_SEND` on the abort, which is what made it a
  plausible suspect - the warning is real, the leak is not.
  Past the ceiling EVERY open fails - reads and writes, any file - until a remount, while the
  boot listing still shows the files present with their real timestamps. The 5 Sep exhaustion
  therefore reached FIFTEEN, so the leak is worse than the raised ceiling suggests and any bulk
  browsing must stay bounded (the gallery holds 2 fetches in flight). Measured on COM4 twice on 5 Sep 2026 (§38.8); COM3 took the identical uploads at the
  identical bus clock with recording OFF and never failed. **A reset cures it; do not blame the
  card and do not reach for `chkdsk` - I did, and it was wrong.** `/control?peerReset=1` on COM3 is
  the fastest recovery.
- Board acts possessed? Check SD free space first (a full card wedges as a fake wifi
  failure), then audit the host PC for orphaned automation from earlier sessions.
  All host-side polling loops must be bounded.
- **THERE IS ONE BOARD PER ADDRESS AND ONLY ONE SESSION MAY DRIVE IT.** A second session on the same
  board silently corrupts both runs, and it does not look like contention - it looks like a firmware
  bug. Measured 6 Sep 2026 (§38.32): a size-regression sweep read back framesizes it never asked
  for, the log showed the size walking through SXGA / UXGA / QXGA on its own, and a deferred "frame
  size takes effect when the recording stops" appeared that no local script had sent. Two clips in
  that sweep also showed ~25 ms of *monitoring* wait - time inside `esp_camera_fb_get`, not the card
  and not the governor - which read as a 22% rate loss at FHDNARROW and a 12% loss at QVGA. **Both
  were artefacts and neither reproduced once the board was free.** The tell to reach for first is
  the monitoring time in the closeAvi block, then `arp -a` and the host process list.
  **This includes a spawned background task**: one that verifies on hardware WILL contend, so hand
  the board over deliberately and stay off it until that session is finished. Retrospective
  diagnosis is barely possible - the RTC ring holds only ~2 minutes at recording chatter, so by the
  time a sweep finishes the evidence of who did what is already gone
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
- **RETRACTED 7 Sep 2026: there is no ~24us binned row-time floor, and no bistable magenta latch
  near one.** This said the floor was ~24us, that the cast was never seen at 24.5 and sometimes at
  23.75, and that 1280X960 therefore ran HTS 2156 for the margin. The killing comparison
  (§38.43, `hts_floor_hypothesis.sh`): a **30.83us row at HTS 1850 / 60 MHz is CORRUPT** while a
  **23.41us row at HTS 2060 / 88 MHz is CLEAN** - the clean point has a 26% SHORTER row than the
  corrupt one, so no row-time law separates them. 54 samples at 88 MHz across 24.50 / 24.00 /
  23.41us were all clean, including 18 below the stated threshold. **The two real constraints are:**
  - **HTS >= 2060.** Below the register floor the output is corrupt at ANY clock, in a mode that
    varies run to run - no still, green blow-out, magenta, column stripes - which is the undefined
    behaviour that made it look bistable. Every §37 magenta sample had HTS below 2060.
  - **PIXCLK <= 88 MHz.** 92 is MARGINAL (identical registers: clean twice in one run, magenta in
    the next), 93.33 destroys the bottom half of the frame, 96 is corrupt at every row time from
    22.00 to 23.38us. §37's verdict on 96 stands exactly as written.
  1280X960 and HD both run **HTS 2112** now. 2112 rather than 2060 for an unrelated reason: a
  1280-wide output stretches occasional frames at 2060, measured twice.
- **A whole-frame gate is not enough - use `still_bands.py` too.** At 93.33 MHz a frame whose top
  half was a perfect chart and whose bottom half was blue and yellow garbage scored ratio 0.850 and
  hdiff 3.1, inside every clean threshold, because the good half averaged the bad half back into
  range. `still_bands.py` compares quarters: ratio spread and the max/min of band SATURATION. Not
  hdiff across bands - that measures scene detail and flags clean frames.
- **The failure mode below a VTS floor is a SILENT HALVING, not a corrupt picture.** Frames stay
  complete, correctly coloured and perfectly legible and simply arrive at half rate (measured
  27.88 against 55.76). Every image gate passes it. **Any geometry or timing walk needs a RATE
  gate**, and a VSYNC count that can report NOFRAMES - grepping the ring for "VSYNC counted" alone
  returns the PREVIOUS rung's number when the board logs "VSYNC count failed" instead.
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
- **THE AF MCU'S FIRMWARE DOES NOT SURVIVE THE HOLD, so releasing the reset resumes nothing**
  (§38.33, 6 Sep 2026). Halting the MCU (0x3000 bit 5) discards its program - the library's own
  `focusInit()` re-downloads the whole `OV5640_AF_Config` blob to 0x8000+ every time for exactly
  that reason. `camFocusAuto()` used to write only 0x3000 = 0x00 and log "continuous again", so the
  Autofocus toggle looked like it worked and the lens never moved again: measured, focused at 213,
  held at 600 by hand, and still 600 after toggling back on. **The fix is the boot pair,
  `ov5640AF.focusInit()` then `ov5640AF.autoFocusMode()`** - reload, then arm - which took the lens
  600 -> 85 sweeping -> 199 settled. It is unconditional, so the toggle RE-ACQUIRES rather than
  merely resuming, which is a "find focus" button's behaviour without exposing single-shot
- **Do NOT hand-roll the AF mailbox; it reads as success and does nothing.** With no firmware
  resident, writing 0x3023 = 0x01 then 0x3022 = 0x04 got 0x3023 CLEARED - an apparent
  acknowledgement - while the lens sat at rest (VCM 42 in a lit room). `autoFocusMode()` also sends
  a CMD_MAIN 0x01 / 0x08 preamble before the 0x04 that a hand-rolled version misses. `bench_lib.sh`
  `af_resume` has the same latent gap: it releases the MCU and sends 0x04 without reloading the
  firmware, and only ever logged the status rather than checking the lens moved
- The AF re-init costs **~1 s** of SCCB blob download and runs inline on the caller's task. For the
  UI toggle that is the httpd worker deliberately: the same second on the capture task would stall
  frame delivery and hole any recording, and nothing here changes frame timing
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
  0x5183 = 0x94 on each). **Still owed: the same comparison in a dark room.** The register write is a
  clean read-modify-write of bit 7, checked either side of the toggle on 5 Sep 2026 (0x94 simple,
  0x14 advance, every other bit intact), and as an "Advanced AWB" switch the toggle reads the right way
  round: checked = advance. Only the config NAME is inverted. It is now gated on AWB being on AND the
  gains being automatic, because choosing the algorithm that computes the gains does nothing when
  nothing is computing them (§38.16)
- **The camera panel's white balance is AWB Enable, Auto WB Gains, R/G/B, Advanced AWB** since §38.16.
  **Manual AWB and AWB Mode are hidden in the page** (still reachable by `/control` for the bench):
  Manual AWB has no registers of its own - the driver implements it as `set_wb_mode(enable ? wb_mode :
  0)`, a re-apply button for the preset - and every preset measured FURTHER from neutral than auto
  (§38.5). The three gain sliders drive `awbGains` / `awbGainsAuto`, the same as the long exposure
  panel, and both panels' controls are synced through shared helpers because they write the same
  registers. Two switches that look alike mean different things: **Enable is 0x5001[0]** (does the ISP
  apply gains at all), **Auto WB Gains is 0x3406** (who computes them)
- **A `<select>` sets the panel's minimum width**, not the other way round: it will not shrink below
  its widest option, so the framesize row needed 366px inside a 320px panel and `.panel`'s
  `overflow: hidden` clipped 46px of it in silence while the open list hung outside the panel.
  `nav.menu.panel` now takes 25 buttonSize units (400px), measured - 24 still ran 8px over - with the
  narrow-phone block resetting it to 20, where labels stack and the select gets the full width anyway.
  Check a layout change by reading each row's right edge against the panel's content edge, at a
  viewport you set deliberately: the Browser pane reports `innerWidth` 0 when it is hidden and the page
  then renders in the phone layout, which reads as a pass (§38.18)
- **`maxOpenFiles` is 15, not the core's 5** (`utilsFS.cpp` `prepSD_MMC`) - raised before 5 Sep
  2026, so §38.8's and §38.19's "five slots" are stale and the exhaustion measured on 5 Sep
  reached FIFTEEN open files. The leak audit is still owed, and browsing must stay bounded
  (the gallery keeps 2 thumbnail fetches in flight, measured)
- **`showView()` reads the frame size as `split('_')[2]`**, which is bare for a recording
  (`_HD_20_12.avi`) but carries the extension for a saved still (`QSXGA.jpg`); it must be stripped
  or the viewer prefix matches no option and sizes nothing (§38.20)
- **A page that overflows on a phone can report NO overflow**: the browser widens the layout viewport
  to fit, so every right edge then sits inside `innerWidth` and an element scan comes back clean. The
  test is `document.documentElement.scrollWidth == innerWidth` AND `innerWidth == the width you set`.
  Two silent causes found this way (§38.19): `.cfgTitle`'s `grid-column: 1/5` creating three implicit
  columns in a one-column grid (Edit Config laid out 524px on a 375px phone), and `section#footer`'s
  desktop `min-width: 20 units` stretching its flex ancestors (332px on a 320px phone)
- **The `max-width: 30rem` block sits AFTER the phone card block**, so a bare declaration there wins at
  equal specificity: its old `.quick-nav { width: 44px }` made the tool tiles compute 44px inside 75px
  grid columns. Same class of fault, same day: `.tabcontent button` also matches every tile on the main
  page, because `#mainPage` carries that class
- **NEVER draw a new icon with `<rect>`, `<text>` or a bare `<svg>` - use `<path>`.** The stylesheet
  carries three BARE ELEMENT rules from the upstream project's SVG buttons, and they reach every such
  element on the page: `rect` gets `fill: var(--buttonReady); width: 100%; height: 100%; x: 0; y: 0;
  ry: 15%`, `svg` gets a fixed 8-unit width, and `text` gets a `translate(50%, 50%)`. A `<rect>` you
  draw therefore ignores its own x/y/width/height presentation attributes - CSS beats a presentation
  attribute always - and paints as one solid ready-blue block filling its viewBox. **That is what the
  Camera Tools heading's old `icon-grid` was**: four rects, each rendered full size, stacked. Not, as
  the commit that replaced it says, four small icons whose strokes merged - that reading is wrong and
  the icon was never the cause, the `rect` rule was. Every other sprite icon is paths and circles,
  which is why nothing else ever showed it. The tell is `getBBox()` returning the whole viewBox, and
  the proof is `document.styleSheets[0].disabled = true` restoring the geometry (7 Sep 2026).
  **THE SPRITE IS NOW RECT-FREE and must stay that way** - six icons were converted (`grid`->`wrench`,
  `film`, `image`, `stop`, `frame`, `mic`, `aspect`). `document.querySelectorAll('svg symbol rect')`
  returning anything but 0 is the regression test. The ONE legitimate `<rect>` left on the page is the
  SVG config button `common.js` builds around line 901: it wants that fill and that full-size box, and
  the click dispatch routes on the tag name (`e.nodeName == 'rect'` -> the id of the next text node),
  so the rule and the button must both stay. **A rect icon can also be hiding a design that only ever
  worked because of the bug**: `icon-aspect`'s frame and its corner brackets were both drawn at 4..20,
  so drawn honestly they coincided into a plain square - the frame had to be redrawn smaller. Convert,
  then LOOK at it
- **The phone card block does NOT win by coming later - it wins by matching specificity**, and the
  desktop top row is where that bites. Making the row uniform meant scoping it to `.tab .navtop
  button` (0,2,1), which silently outweighs every `.tab button` / `nav#maintoolbar button` (0,1,1)
  the `48rem` block writes: the phone kept the desktop's fixed width, radius, colours and icon size.
  **Change both ends together.** Four phone rules and the `nav#maintoolbar .tile-icon` one were
  rewritten to the same selector shape for that reason (7 Sep 2026). Two more from the same change:
  `.sep-item` alone loses to `.navtop li`, so hiding a separator needs `.navtop li.sep-item`; and a
  desktop `white-space: nowrap` must be reset to `normal` on the phone, or "Start Recording" widens
  its own grid column, since `1fr` is `minmax(auto, 1fr)` and the four Device Controls tiles stop
  matching. Read the tile widths back at 375px and 320px - unequal widths are the tell
- **The desktop top row is one flex row of equal 9-unit rectangles**, tabs left and transport right
  (`section#main`'s `margin-left: auto`), wrapping rather than clipping below ~1230px. The first tab
  is a SQUARE icon-only camera button - it used to be relabelled to the sensor part number, which
  now goes in its `title` instead, so `customButtons()` no longer calls `setActionLabel` on it.
  An icon-only button carries `aria-label`; the model in the title does not replace that
- **Only `#camera-control` is inside `#menu-top`**; the other five panels are direct children of
  `#menu-container`, so `#menu-top.menu-pinned nav.menu.panel` has only ever styled the camera one.
  Anything meant for all six targets `nav.menu.panel` (the phone sheet is `nav.menu.panel.active`)
- **`--smallThumbSize` / `--bigThumbSize` are read ONCE at load** by `common.js:58` through
  `getComputedStyle(:root)`, so redefining them inside a media query does reach the JS that places the
  range value bubble - but only at load, and they must stay plain lengths, never `calc`
- **The Focus section lives in both panels** since §38.17 (Autofocus plus Lens position, under
  Microphone Gain in the camera panel), sending `afAuto` / `afManual` through the same shared helpers,
  lens slider hidden while the AF program owns the lens. Copying it turned up a nesting bug in the
  night panel - the lens row was inside the autofocus row's `input-group`, the parent's `</div>`
  missing - which rendered acceptably and so survived three stub passes and a deployment. **When a row
  is added next to another, read the new row's parent id back out of the DOM**; a nested group is
  invisible until something hides the parent and takes its child with it
- The colour bar is not in the block `set_framesize` reloads, so **0x503D survives a size change**
  and, being persistable, can boot a board into test bars (5 Sep 2026, §38.5).
- The web page's state tests must go through `isOn()`: `/status` sends every value as a STRING and
  `"0"` is truthy in JavaScript, so `value ? a : b` takes the wrong branch on every load (it hid the
  Manual Exposure slider whenever the AEC was manual). A control the page hides or disables must not
  be sent either (`isInert()`) - a hidden select still reached the sensor. The repeating section
  markers are CLASSES, not repeated ids: `hideBuiltOut` matches class or id, and breaking that makes
  compiled-out sections silently reappear on the builds that need them hidden (5 Sep 2026, §38.11).
- **In `src/web/MJPEG2SD.htm` the static markup is OV2640's - read the sensor branch, never the HTML.**
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

## Open items (as of 7 Sep 2026)

Ordered by what would bite first. Each names where the detail lives; the inline "Still owed" notes
elsewhere in this file are the same items seen from their own subject.

1. **`/sustain?download=0` wedges the web server** (§38.21, unexplained). No abort needed, the first
   download after a boot has done it, recovery is a reset. Not reachable from the page any more
   (§38.25) but still compiled. **Never test it on COM3** - that is COM4's peer-reset lever
1a. **NEXT CAMPAIGN, baseline measured, no changes made yet: retune the 11 VISIBLE sizes, 1 fps to
   ceiling** (user's brief, 7 Sep 2026). Four goals: maximise exposure per size, eliminate dropped
   frames, keep motion detection / idle throttle / governor boost off the delivered rate, and expose
   50 / 60 / off banding to the user. **§38.37 is the opening baseline** - all 22 points (1 fps and
   ceiling for each size) read off the board. What it says:
   - **Exposure fills the SENSOR's frame at every point (98-100%), so the deficit is entirely that at
     1 fps the sensor runs faster than the request and the frame timer decimates.** As a percentage of
     the 1000 ms request: QSXGA and QHD 99%, FHDNARROW 85, FHDFULL 83, FHDMID 65, 1280X960 42, HD /
     VGANARROW / QVGANARROW 40, **VGA and QVGA 20**
   - Two causes, one hazard each, both cheap to test and neither tested: **the four clock-tuned sizes
     are pinned at the driver's VTS** (VGA/QVGA 984, FHDMID 1344, FHDFULL 1488 against 1968), worth
     2x / 1.5x / 1.3x - blocked by §10's measured halving on scaler sizes, which FHDMID and FHDFULL
     were never actually tested against; and **binned sizes cannot pass HTS ~2277**, capping HD at
     399 ms - above 2644 the line cleanly costs 2 x HTS, which would double the exposure, unwalked
   - **At the ceiling there is no exposure headroom at any size**, so goal 1 is purely a low-rate
     problem and does not fight the 88 MHz raise except at the top rung
   - **Goal 4 is nearly free**: `banding` already works, persists and is measured - it is just not on
     the page (config row group 98, URL only). One select element
   - **Goal 3 is partly done**: `idleThrottle` already stands down for a recording, stream, playback
     or still; its cost is the retime on release. Motion detection's ~4.8% delivery loss is the real item
   - **Goal 2 is DONE (7 Sep 2026)** - see open item 2 and BOARD_TESTING §38.40. All 11 sizes were
     measured for delivered rate; QHD 9->8 and QSXGA 7->6 are lowered for good, HD and 1280X960 KEEP
     their sensor ceilings because `fpsPriority` now trades quality to hold the rate, and the other
     seven were already exact. It did NOT pull against the clock work: nothing here changed a clock
   - Scale: 448 rungs for all 11 sizes at every integer fps. Narrow first, do not sweep first
1b. **PARTLY CLOSED 7-8 Sep 2026: HD and 1280X960 are on route B at HTS 2112** (§38.41-46).
   HD 52 -> **56** (88e6 / (2112 x 744), counted 56.006, a clip delivered 54.3) and 1280X960
   41 -> **42** (counted 42.343, delivered 41.4). Route B engages at a request of 49 for HD and
   38 for 1280X960; below that both keep route A and differ from the old tier only in the line.
   The 2156 line went back to 2112 because the row-time floor it protected against does not exist
   (see the retraction above). **New size HDV2** (index 30) reads the datasheet's own 720p geometry
   - 1440 array rows, ISP offset 0, VTS floor 738 - and is worth 0.46 fps at three requests and
   nothing anywhere else; soaked 25/25. sweep.csv regenerated for all three, 154/154 gates.
   **What is left of this item**: QVGA, VGA, FHDMID and FHDFULL, the four with real time headroom.
   Note the constraint below is now measured rather than assumed - the clock ceiling is 88, and the
   line floor is 2060, with row time playing no part.
   (§38.35 carries the full brief and the fresh per-size baseline). §37 established the in-spec 88 MHz
   (§38.35 carries the full brief and the fresh per-size baseline). §37 established the in-spec 88 MHz
   route (0x3108 = 0x11, VCO 440, mul 66) and it was applied to **1280X960 alone**; every other
   mainstay still runs 80.00 MHz, so +10% clock is +10% fps at unchanged HTS x VTS on arithmetic.
   **Most of it is unreachable** - see item 2: at their present ceilings 1280X960 and SXGA are at 99%
   busy and 1280X960 is already ON 88 MHz and still delivers 38.1 of 41, so a clock raise buys those
   two nothing. Target the four with real time headroom: **QVGA 15% busy, VGA 39%, FHDMID 46%,
   FHDFULL 53%.** Two hard constraints: ~90 MHz is the sensor's digital-path cliff so this is one step
   and not a walk (96 is corrupt by every route), and raising the clock SHORTENS the line at fixed
   HTS, so every binned size needs its HTS raised to hold the ~24.5 us row time that keeps the
   bistable magenta latch away - which is exactly why 1280X960 went HTS 2112 -> 2156. The open
   question worth testing rather than assuming: §31 concluded PIXCLK "helps 1280X960 ALONE" because
   its scaler pass is 1:1, which would exclude QVGA and VGA - the two sizes with the most headroom
2. **PARTLY CLOSED 7 Sep 2026 by `fpsPriority`: the governor now closes a loop on the RATE.** It
   used to ask only whether the CARD was in trouble - demand against the SD budget, and the frame
   window - and never whether the user was getting the fps they asked for. `sdGovernor`'s fourth arm
   compares delivered rate against requested (`govRingCount` over the ring's own span, so no new
   state) and pushes while under `GOV_RATE_PCT`; the relax and ease arms stand down while that
   deficit stands. `fpsPriority` (config, DEFAULT ON) also raises the boost cap from
   `GOV_MAX_BOOST` 4 to `GOV_MAX_BOOST_FPS` 14. **A/B MEASURED on COM4, toggle alternated so the
   drifting scene could not pick a side**: 1280X960 at 41 gave 39.8 on against 37.7 off, HD at 52
   gave 49.4 against 46.5, with frames 94 vs 112KB and 74 vs 88KB. HD and 1280X960 therefore KEEP
   their sensor ceilings (52, 41).
   **Three things it still does NOT do, all measured:**
   - **It does not fully reach the request** - 97% at 1280X960, 95% at HD - and the boost ends near
     its 14 cap (13.0, 12.5), so the cap is binding again. Quality lands around q23.
   - **The ramp is slow and drags the clip average**: one step per window, so 13 steps is 13s of a
     25s clip spent climbing. A proportional step (more than one when far short) is the obvious fix
     and is NOT built. The steady-state figure is therefore better than these averages suggest.
   - **It cannot help QHD or QSXGA**, whose ceilings stay lowered (8, 6). At QSXGA 7 demand is ~75%
     of budget while the size still misses the rate - the missing time is monitoring and buffering,
     which the governor cannot see - and their frame counts are near `GOV_RATE_MIN_FRAMES`.
   **Do NOT "fix" this by lowering `GOV_PUSH_PCT`**: `GOV_RELAX_PCT` is derived from it through one
   step's effect on demand, and that effect is unsettled - 1.5x at QSXGA (28 Aug) against a MEASURED
   2.9% per step at 1280X960 q14-q20 (7 Sep, `quality_buys_rate.sh`). Re-measure the step at QSXGA
   before touching either threshold.
3. **Twenty frame sizes have no frame-window protection, and three of them matter.** 7832a16
   correctly stopped `frameWindowKB` handing QHD's 800 KB cliff to every size it does not name, so
   the governor's pre-arm and the ease-down's safety gate now stand down for all of them - better
   than arming on another size's number, but it is no protection either. `frameWindowKB` names only
   VGA 266, HD 291, FHD/FHDNARROW 443, QSXGA 946, QHD 800, 1280X960 383, FHDMID/FHDFULL 443 and
   VGANARROW 266; everything else returns 0.
   **The three worth measuring are SXGA, UXGA and QXGA**, because the cliff follows the OUTPUT size
   and all three emit more than 1280X960, whose measured cliff is 383 KB - so their real cliffs are
   plausibly in the 400-700 KB band where a dark noisy scene can reach them. WQXGA and P_FHD are the
   next tier down in priority. The small sizes are fine on physics: their frames cannot grow that
   large. Closing it is `frame_window_descend.sh` per size (§20 method, §34 run).
   Lit SXGA frames measure 121 KB, so this is a dark-room risk, not a lit-room one.
   **This item is now the only record of that work**: it came from a spawned task session that has
   since been deleted, and its own owed verification is closed (lit SXGA at ceiling 17 delivered
   16.9 with the cap reading 0, boost 0, no governor writes - §38.32)
4. **The open-file leak audit** (§38.21). THE leak was the SD log and is fixed, but the 5 Sep
   exhaustion reached fifteen, so something else may still leak. `fileProbe=1` either side of a
   suspect operation is the measurement; healthy idle is 14 with SD logging on, 15 with it off
5. **The ease-down can walk to a floor the scene cannot sustain** (§38.29). At `govEaseSecs=1` a
   flickering lamp reached the configured quality, the next dark phase did not fit at it, and the
   rescue fired twice in one clip at ~4.2 s of lost frames each. Harmless at the default 10 s.
   A fix would be an adaptive floor - refuse to return to it for the rest of the clip after a
   rescue follows the walk - which would also make a fast walk safe. Not built, and a new mechanism
6. **The dark-room AWB comparison** (§38.16): simple vs advanced AWB measured neutral on the star
   chart in a LIT room and simple became the default on that basis. The dark-room half is owed
7. **The `colorbar` firmware half** (UI_REVIEW): 0x503D survives a framesize change and is
   persistable, so a board can boot into test bars. Never persist it, clear it at boot
8. **The `wb_mode` firmware gate** (UI_REVIEW): the presets are hidden in the page but still
   reachable by `/control`, and every one measured further from neutral than auto
9. **`ui_regress.sh` has not been run against any of the governor work** (§38.29-32). It exercises
   `quality`, which the governor now writes on its own, so the register snapshots could show an
   unexpected 0x4407 during the mid-recording scenarios. `DRY=1` is the smoke run, the full matrix
   is 3.5 h. Not a known fault, an ungated change
10. **A live stream has no watchdog, so a stalled one is never torn down** (7 Sep 2026, page only).
   `checkStream()` is called from `activatePlaybackButton` alone - `activateStreamButton` sets
   `view.src` and never arms it. **Measured on COM4**: a stream sat at `naturalWidth 0` with the
   request open and nothing decoding, `liveStream` still on the container and the LIVE badge still
   showing, while `/status` answered perfectly well. Nothing timed it out; only a manual stop or a
   reload cleared it. It is last in this list because the page still looks alive and nothing is lost,
   and because `markOnline`'s guard was deliberately written not to depend on it - it asks whether
   the picture decodes, so a stalled stream reads Offline rather than Online.
   **Do NOT fix it by copying the playback path.** `checkStream()` is a `while` loop on
   `view.decode()`, and decode resolves against whatever frame is already decoded rather than waiting
   for the next one - measured 200 resolves in 5 s on a 10 fps stream. On playback that spin is
   bounded by the clip; on a live stream it would run for as long as the stream does, on a phone.
   The signal it wants does not exist in the obvious places either: Chrome fires **no** `load` event
   per part for a multipart MJPEG (0 in 5 s, measured), and comparing pixels calls a static scene
   frozen. Something new is needed - a byte counter on the response, or the board reporting its own
   sent-frame count in `/status` and the page watching it stop

## Docs map

- `BATTERY.md` - battery deployment guide (committed)
- **THE LONG EXPOSURE CEILING IS 3.18 s AND IT IS A DELIVERY LIMIT, NOT THE SENSOR'S** (7 Sep 2026,
  §38.36). The sensor integrates correctly to **at least 10 s** and the array is not the limit either:
  the datasheet's "maximum exposure interval: 1964 x tROW" is the AEC ENGINE's range and equals the
  array's own 1964 rows, and it is lifted by **dummy lines** - frame length past the rows read, with
  the exposure written by hand (0x3503 = 0x01, AEC manual and AGC left auto; full manual 0x03 carries
  the two §38.5 quirks). Datasheet 4.6.2: raise the FRAME first, then the exposure. Measured at 5MP
  with the gain fixed, the sensor's own zone statistic rose 36 / 49 / 79 / 119 across 3.70 / 5.00 /
  7.51 / 10.00 s, tracking the exposure, and 5.00 s came back as a real 135 KB picture.
  **What fails is the ESP32 handing the frame over**, and it is a CLIFF: complete frames counted off
  the stream over 52 s windows, as delivered/produced, **3.18 s 0.86, 4.00 s 1.00, 4.50 s 0.00,
  5.00 s 0.00**. At 7.51 s the one thing that arrived was 68 bytes - a JFIF header and one
  quantisation table then end-of-image, no scan data. **The LINE does not matter, only the period**:
  the long regime was moved onto the size's own short line on the theory that it would help, and
  4.50 s still delivered zero - that theory is RETRACTED by measurement. A cliff at 4.0-4.5 s is the
  shape of a timeout in the driver's frame fetch, but the camera library ships precompiled and its
  source is not in the core tree, so that is a candidate and not a finding. **4.00 s measured a
  perfect 1.00 twice** and is one constant away (`NIGHT_MAX_MS`); the user's decision was to stay at
  3180. The dummy-line code and its manual exposure are in `applyTunedTiming`, unreachable through
  `nightEnter`'s clamp, kept as the record. Bench rigs: `night_ceiling_probe.sh`, `night_revert_verify.sh`
- **A no-frame state used to be unrecoverable without a reboot** (7 Sep 2026, fixed). `settleSensor()`
  runs at the END of `processFrame()`, and `processFrame()` returned early whenever
  `esp_camera_fb_get()` handed back NULL - so nothing serviced `retimePending`. Observed: a 4.5 s
  session stopped delivering and then ignored a shorter exposure, a framesize change AND its own
  exit, sitting on a 4.49 s frame while `/status` reported HD 30. **The same trap always existed for
  a dark 5MP frame that never fits the buffer**, so this was reachable before the night work. Both
  the NULL path and the oversize path now call `settleSensor()` - safe at exactly those points
  because the capture task holds no buffer, which is that function's precondition. The forced re-test
  recovered in 5 s but did not clearly reproduce the stuck state first, so the fix is argued from the
  original observation plus the code path, not from that re-test
- **Long Exposure panel** (moon button, 5 Sep 2026): `nightExp=<size>,<ms>` programs a multi-second
  frame by stretching the LINE - VTS held at `AEC_VTS_CEIL`, clock at the 10.13 MHz floor, HTS
  carrying the rest - as a branch inside `applyTunedTiming`, so every retime reproduces it and the
  capture task keeps sole ownership of sensor writes. QSXGA and FHDNARROW only (a binned size's line
  cost flips above HTS 2277). Measured: FHDNARROW 3.00 s at HTS 7724, QSXGA 3.18 s at HTS 8191, both
  agreeing with §37. `stillWaitMs()` scales the still wait to the frame (at 3 s the stock 1200 ms
  lands 4 requests in 10). `afManual=<0..1023>` / `afAuto=1` hold and release the lens. Entering
  forces idleFps and micGain to 0 and restores them on exit; nothing is persisted (§38.12).
  **A session is bound to `nightFS`, the size it was entered for, and any size or rate chosen in the
  camera panel ends it** - returning whatever the user did NOT choose (a size change gives the rate
  back, a rate change the size, the panel's toggle both). Without that binding the stretch outlived
  its size and left the sensor on a 3.17 s frame while `/status` reported HD 30: white frames and a
  starved stream. **`lf == 2` alone is not a sufficient guard** - `senLineFactor()` reads the PREVIOUS
  size mid-transition. The no-frame watchdog also has to know the sensor's period, not `FPS` (pinned
  at the timer's floor of 1), or it fires between every night frame and walks quality 12 -> 30 (§38.13).
  **The stretched line survives a same-size exit unless something puts it back**: nothing rewrites HTS
  at a full-resolution size (`set_framesize` does, but only when the size CHANGES), so a session ending
  on the size it began on left QSXGA with a 2.63 fps ceiling and a 379 ms exposure while the UI reported
  the rate asked for. `nightExit` leaves `nightHtsBase` set and the next retime consumes it, one-shot,
  which is also why it must NOT be cleared there (§38.14)
- **Manual white balance**: `awbGains=<r>,<g>,<b>` (1024 = 1.0x) writes 0x3406 = 1 and 0x3400-0x3405
  with a read-back; `awbGainsAuto=1` hands the gains back. This is the control for a green cast, NOT
  `awb`: that one clears 0x5001 bit 0 so the ISP applies no gains at all and the picture goes further
  green. Measured on a lit bench frame: green sits 13% above the other channels on auto and 15% below
  at R 1600 / B 1400, so neutral is reachable between them (§38.12). The panel exposes **all three
  channels** in R, G, B order since §38.15 - green was held at 1024 on the theory that it is the
  reference channel, which is true of the ratio and unhelpful to a user: lowering green is the direct
  cure for a green cast, and the only one that does not also brighten the frame
- **The SD governor now walks quality BACK DOWN too** (§38.29, 6 Sep 2026, firmware + page). The
  no-frame rescue used to overwrite the user's quality for good: it called `govRebaseQuality`, so a
  dark clip that had frames walked from q10 to q16 stayed at q16 after the light came on, and so did
  every clip after it, while `/status` went on reporting q10 and only a reboot or a manual write
  cured it. `govRebaseQuality` (user/config, moves the floor) and `govDegradeBase` (rescue and the
  night seed, working base only) are now separate, and `sdGovernor`'s third arm eases the base back
  one step at a time. **Measured on a real lamp**: q16 to q12 in 45 s, and the `govEase` field plus
  two new `closeAvi` lines say where a walk got to. Three things it will not do - move while a boost
  is still unwound, run during a night session, or move at all before the config replay has set the
  floor (a zero floor would walk quality off the bottom of its range)
- **The ease-down stops SHORT of the configured quality, and that is the gate working.** A step down
  grows frames ~1.5x, so it only steps when the frame that step would produce still clears the
  governor's own arming gates - which reduces to `GOV_RELAX_PCT` on the SD side and two thirds of
  `GOV_WINDOW_PCT` on the frame window, with no new constant to tune. At 5MP in the lit bench room it
  settles at q12, because q10 there predicts ~790 KB against a 756 KB pre-arm and the push arm would
  fire immediately. **q10 is simply not sustainable at 5MP in that room**; stopping at the best
  quality that holds is the answer, not a shortfall
- **`GOV_EASE_TICKS` is 10 and lowering it to speed recovery is a MEASURED mistake** (`govEaseSecs`
  exists to sweep it, not to change it). In a steady scene 1 tick is strictly better: same settling
  quality in 14 s against 45 s, no overshoot, because a 1 s window at 5 fps already holds five frames
  at the new quality. Against a FLICKERING lamp it failed - the walk reached the configured quality,
  the next dark phase did not fit at it, and the rescue fired to recover at a cost of **~4.2 s of no
  frames each time, twice in one 2.5 min clip**. The flicker does not fool the filter, it fools the
  SAMPLE: a 1 s window caught during a bright flash genuinely holds small frames, so the safety
  prediction is genuinely satisfied for that instant. The filter is what makes the measurement
  representative rather than merely favourable, and no prediction can replace it - nothing in a frame
  size says the room is about to go dark again
- **The governor MEASURES over a rolling window and DECIDES every frame** (§38.30-31, 6 Sep 2026).
  Those were one thing - a 1 Hz sample-and-reset - until then, which meant the only way to react
  sooner was to measure over less. And a per-frame MEASUREMENT is broken, because the SD write
  ceiling is a sustained property: sampling it over one frame period reads frame-arrival JITTER,
  since the sensor delivers in bursts. Measured at HD 30, adjacent frames read 2544 and 4660 KB/s
  where the true figure was 2871, straddling both thresholds and giving **96 quality writes in an
  87 s steady lit clip**, with the ease-down making **zero** progress because the churn kept zeroing
  its counter. The rolling window on the same scene gives **0 writes** and a frame average steady
  within 5%. `govWinMs=0` still reproduces the broken case on purpose, as the record
- **Because the decision is per frame, every governor threshold is a TIME, not a tick count**
  (`GOV_RELAX_MS` 2000, `govEaseSecs` in real seconds). As tick counts they only meant seconds
  because of the 1 Hz gate, and at HD 30 they would have become a thirtieth of that - which is
  exactly the §38.29 flicker fault. Boost steps are additionally spaced by one full window, so the
  ramp cannot outrun the measurement it is judged on. The **onset** of every arm is now immediate,
  which is the gain; only the repeat rate is gated
- **The measured span runs one frame period longer than `govWinMs`** (1170-1198 ms at QSXGA 5 fps
  against a 1000 ask, ~1033 at HD 30) because eviction stops at the newest sample still inside the
  window and bases the span on the one before it. Deliberate: the alternative is interpolating a
  partial frame's bytes. Every governor log line reports the span it actually measured
- **A short ease interval is now SAFE to choose** (§38.31): with the rolling window, `govEaseSecs=2`
  walked 5MP from q16 all the way to the configured q10 in 22 s with **14 writes and zero churn**,
  where reaching that speed by shortening the window cost 8 spurious writes at 5MP and broke HD
  entirely. The flicker tradeoff of §38.29 is unchanged and is still the reason the DEFAULT is 10 -
  what changed is that speed and measurement accuracy are no longer the same dial
- **Dark QSXGA steps its own quality and that is correct**: 5MP frames in the dark overrun the 983 KB
  buffer, the driver delivers nothing, and the rescue steps the sensor's quality until they fit -
  settling at q24-28, the range §37 measured. Do not suppress it; it is the reason dark 5MP works
  at all. `/status` still reports the CONFIG quality, `camLive` the sensor's. A night session at QSXGA
  now SEEDS q28 when the gain the AEC held at entry is 8x or more (a lit bench is 1-4x, this dark room
  32-64x) and restores the configured quality on exit, which turns a seven-step walk with no picture
  into one step: measured first frame clean, luma 91-132, none blown (§38.15). Seed high, not low -
  the rescue only walks quality UP, so a low seed does not save the steps, it makes them slower. **This also makes the
  stream SELECT for blown frames**: at q10 a real dark 5MP frame is ~1 MB and never arrives, while a
  blown one is 96-100 KB and always does - so one frame in three on the wire was white while the AEC
  spent most of its time in a sane range. Judge a rate fault by what did NOT arrive too (§38.14)
- **White frames at a long exposure are the sensor's AEC step engine, not our code** (§38.14, and it
  reproduces at plain QSXGA 1 fps with no night session): with the exposure filling the frame, a
  correction computed from frame N cannot reach the pixels until N+2, so the loop runs a frame behind
  itself and bounces between the fast-zone triggers - 0x3A11 (above it the AEC HALVES) and 0x3A1F
  (below it, DOUBLES), which the driver's table sets at 74/16 around a stable window of 32-37 with no
  hysteresis. The fast zone only applies in step MANUAL mode; 0x3A05[5] selects auto, [4:0] its ratio.
  `applyAecStepDamping` writes 0x3A05 0x22 (auto, ratio 2 against the driver's 16) with the fast zone
  out of reach while a night session is on, and the driver's 0x30 / 0x4A / 0x10 back when it ends.
  Night mode only - the low-fps regime has the same fault, but its rates are the fps reference's
- **A night transition needs a GAIN seed** (`applyAecGainSeed`): the exposure register does not change
  across the stretch (1964 lines either side) but the LINE grows up to 15x, so the gain the AEC holds
  is suddenly 15x wrong and it discovers that one damped step at a time. `nightEnter` reads the live
  exposure and gain BEFORE any retime - the only moment the old exposure is knowable, since the first
  of the two retimes already moves the line - and the capture task writes `gain x expMs / requestedMs`
  to 0x350A/0x350B once the timing lands. From gain 32x the first streamed frame was correctly exposed;
  from the 63.9x ceiling the AEC does not take the seed cleanly and ~6 frames are still blown (§38.14)
- `UI_REVIEW.md` - the web UI's camera controls: what the 5 Sep regression found, and every
  proposal with its measurement (committed). **All of A1-A4, B1-B3, C1-C5 and D1-D3 are done and
  deployed**; B1/B2 and part of C3 are retracted in place (I had read the static markup - see the
  sensor-branch rule above). After any UI change, `DRY=1` on `ui_regress.sh` is the smoke test and
  the full run is the gate. Still owed: the dark-room AWB comparison, the `colorbar` firmware half
  (never persist, clear at boot), the `wb_mode` firmware gate, and the open-file leak audit
- **Playback is now BUFFERED IN THE BROWSER** (§38.22, 5 Sep 2026, **page only, no firmware**): tapping a
  clip fetches it with `/file?path=` and plays it locally - a forward walk of the `00dc`/`01wb` chunks,
  frames drawn to a canvas, and the **audio played at last** (it was always in the file; `getNextFrame`
  stepped over every `01wb` chunk and threw it away). Full recorded rate with sound, against the board's
  21.9 fps silent stutter. **The audio is the clock**: chunks are scheduled on an AudioContext at their
  content time and the frame is chosen from `currentTime`, which is what gives pause, seek and sync for
  free. Read the rate from the header's rational at `0x80`/`0x84`, never the filename (29.939 vs 30,
  1.739 vs 2). **Playback starts when the remaining download is no longer slower than the remaining
  playback**, measured live - a fixed lead stalls, because the link delivers a clip more slowly than it
  plays. Two traps, both measured: **rAF is not a safe pump** (a visible page where it never fired once
  played the whole soundtrack against a frozen frame), so a 100 ms interval carries the audio, the bar,
  the end/stall checks and a drawing floor while rAF only draws; and the **AudioContext must be opened
  inside the click**, or it is born suspended and the video clock freezes with it. **What decides
  whether a clip buffers is the ALLOCATION succeeding, with a 256MB ceiling kept behind it as a
  backstop** - `CLIP_MAX_BYTES`, and the user asked for it to stay (6 Sep 2026), so do not quietly
  remove it: a successful allocation is not proof of survival, and a very large array can be granted
  and then kill the TAB when it is written, which is uncatchable where a refusal is clean. It never
  binds today - `maxFrames` 3600 at ~80KB per HD frame caps a recording near 290MB and the largest on
  the card is 157MB. What it replaced (§38.24): a fixed 64MB cap
  refused the ordinary case here, where HD recordings run 28-157MB with a 54MB median - and it refused
  in SILENCE, with no bar, no message and the badge reading LIVE because the playback button was
  active, so a clip streaming off the board was indistinguishable from a live stream with its controls
  missing. That was the bug report. A refused clip now says why and keeps the CLIP badge, and
  buffering reads "Buffering 12% 1:06 left" because a 100MB clip is over a minute of waiting
- **Gallery** (§38.20, 5 Sep 2026, firmware + page): every Get Still is now FILED on the card as
  `/YYYYMMDD/YYYYMMDD_HHMMSS_<SIZE>.jpg` - before this, stills existed only as a browser response
  and nothing had ever written a `.jpg`. The listing carries them (`listDir` takes a comma list;
  `doPlayback` is set from the extension, not from its return). Thumbnails are cached beside their
  file as `.thm`, generated on first request from a clip's MIDDLE frame; `esp_jpg_decode`'s reader
  callback streams the source off the card so nothing large is ever held in RAM, and generation is
  refused while capturing. `.thm` is invisible to `listDir`, the FTP filter and the tarball, and
  `deleteOthers()` strips it. **The bench opts out of saving** (`stillSave=0` in
  `assert_campaign_config`, restored via `CAM_KEYS`) - a full run is 214 stills
- **The gallery is SELECT then play** (§38.25, 6 Sep 2026, page only): a tap on a tile selects, and
  Start Playback opens the selection through `openFile` (buffered, stills too) and closes the sheet.
  A tap used to fetch and play, so a mis-tap started a 100MB download and nothing could be picked
  without opening it - and the two controls disagreed, the button taking the board's paced stream
  while a tile took the buffered player. **Long press (500ms, cancelled by a 10px drag) or ctrl/cmd
  click ticks tiles** for Download and Delete, which then act on the set one file at a time with a
  gap; Start Playback and File Upload are disabled while a set is ticked because both act on one
  file. **Day folders tick too** (§38.26): a set is files OR folders and never both (`pickKind`),
  because a folder delete takes everything inside it and `/file?path=` cannot fetch a folder, so
  Download goes unavailable for a folder set. Folders are tiles with a folder glyph and every file
  tile carries its byte count; the listing's own `/` row is no longer a tile but the small **Up
  button** on the left of the Tile view row, tile view only, hidden at the root. Two traps found here: **selecting a file answers `{}`** (the
  board records the name and lists nothing), so rebuilding the grid from that empty answer wiped the
  tiles just tapped - `getFiles` now keeps the grid when a listing is empty; and the action row must
  NOT be `id="buttons"`, the id `addButtons()` injects above it, or `placePlaybackButton` drops Start
  Playback among Save Settings and Reboot ESP. That pair is hidden in the Gallery sheet on a phone
- **The page's Download button now uses `/file?path=`, not `/sustain?download=0`** (§38.25). That
  closes the one route by which an ordinary user could wedge the web server. It is also the only
  route that can serve a SET - the sustain download is stateful, one selected file. **What is lost:
  the tarball** `downloadFile()` builds from the clip plus its `.csv` and `.srt`; a file fetched now
  is the file itself, and the others are on the card and fetch the same way. The wedge itself is
  still unexplained and `/sustain?download=0` is still not to be called
- **The gallery's percent bar had never worked** (§38.26, 6 Sep 2026). `updateStatus()` writes a
  status value only into `text`, `DIV.displayonly`, `INPUT`, `TD` and `SELECT`, and `#progressBar` is
  a `PROGRESS` element, so the board's `progressBar` arrived on every poll and was discarded. It is
  driven from `processStatus` now, by three sources with `xferBusy` saying which owns it. **The
  upload half is dormant on this build**: `progressBar` sits inside `#if INCLUDE_FTP_HFS` and that
  flag is false, so the board never sends it and File Upload does nothing at all. A **download** gets
  true per-byte progress by reading the response body in the page, with **no size ceiling** (the
  user's decision) - the 256MB one is playback's, where decoded frames sit on top of the buffer - and
  a catch that hands the URL to the browser's own downloader if the bytes cannot be held, so the file
  always arrives and only the bar is lost
- **A hidden element's auto margin pushes nothing**, and **`60vh` is the LARGE viewport on a phone**
  (§38.26). The first left the Tile view pair stuck at the left whenever the Up button was hidden -
  `justify-content: flex-end` on the row is the fix. The second is why the grid's own `max-height`
  could not stop the action row being pushed off the bottom: the bar and the four buttons are a
  sticky footer now, and the grid no longer scrolls inside the sheet
- **Chrome sticks a sticky element to its scroll container's CONTENT box, not its padding box**
  (§38.27, measured). `bottom: 0` therefore parked the gallery's footer exactly the sheet's 32px
  `padding-bottom` above the screen - 780 against an 812 viewport - with a strip of grid showing
  under it. The offset cancels that padding and the two share `--sheetPadBottom` so they cannot
  drift. **Do NOT blame the phone's URL bar**: I did, set the sheet to `100dvh`, and the panel being
  `content-box` made it 852 tall inside 812 and pushed the footer BELOW the fold - worse than the bug
- **Several files download as ONE zip** (§38.27), STORE, built in the page - no CDN, because this
  board's LAN is often offline. CRC summed per chunk as it arrives, timestamps from the filenames,
  refusal past 4GB (no ZIP64), fallback to one file at a time on any failure. **Select All** is files
  only, never day folders, and is disabled at the root; the panel is **Playback & Download** and the
  File Upload button is gone with it (`INCLUDE_FTP_HFS` is false, it never did anything here)
- **Phone layout** (§38.19, 5 Sep 2026, page only): below `48rem` the page is a column of cards - app
  header, Device Controls, a live "viewfinder" card, Camera Tools, System Status - driven from ONE
  `@media (max-width: 48rem)` block on the same DOM, because `updateStatus()` matches a status key to
  the element whose id equals it and a value can therefore exist only once. Settings panels become
  full-screen sheets (`nav.menu.panel.active`, with `body.sheet-open` as the switch), the phone's Back
  gesture closes them through a pushed history entry, and Start Playback is MOVED into the Gallery
  sheet at runtime, never cloned. A phone does not get the OV5640 tab, Show Log, OTA Upload or Start
  Playback (the user's choice), so the app header is the way home. Everything is built to 48px touch
  targets - verify by scripting each element's box at 375px, not by looking. The desktop layout is
  unchanged apart from the body font stack and real icons where `➤` / `▢` used to be
- **The phone's two cards are FOUR and EIGHT tiles, and three of them move at runtime** (7 Sep 2026).
  Device Controls is Record / Start Stream / Get Still / Listen; Camera Tools is Camera, Night Mode,
  Motion, Gallery, Controls, Access, Edit Config, Status. `placePhoneTiles()` moves Edit Config into
  the tools card and Listen out of it, reversing both on the `48rem` media-query change, exactly as
  `placePlaybackButton` does - moved, never cloned, for the same duplicate-id reason. **Edit Config
  gains `quick-nav` on the way in and loses it on the way out**: the phone tile styling is written
  for `.tab .navtop button` and `.quick-nav`, and once it leaves the tab row neither reaches it. Its
  `.tablinks` class stays, which is what keeps the click dispatch sending it to `openTab`.
  `#statusCard` is last in the markup so Edit Config can be inserted BEFORE it and Status stays
  bottom right. A repositioning is driven by the media-query event, so a script that measures
  immediately after setting a viewport reads the OLD arrangement - settle first, or the pass is a race
- **A tile whose phone label is shorter carries two spans**, `.tile-label` and `.tile-short`, with
  `hasShort` on the button choosing between them. Record / Stop on a phone, Start Recording / Stop
  Recording on the desktop. It cannot be one span relabelled at a breakpoint, because
  `setActionLabel()` rewrites that text every time the button toggles and a label picked at load
  would go stale on the first press - so `setActionLabel` writes both, falling back to the full text
- **System Status is hidden on a phone until the Status tile asks for it** (`body.status-open`,
  remembered in `localStorage` per browser, every access in try/catch). It must be hidden in the
  media block and NOT with the page's own `hide()`, which writes an inline `display: none` that would
  follow the element to the desktop - where that same `section#footer` is the permanent bottom strip.
  The chevron that used to collapse the card is gone with its `statusToggle` branch and `.collapsed`
  rules; a fixed-position footer reports `offsetParent === null`, so do not test it for visibility
  that way
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
- `updateFPS=1` - fpsCeil, aecMax, budgetKBs, live frameKB, govBoost, govEase, frameCapKB
- `govEaseSecs=<1..60>` - the SD governor's ease-down interval in ticks (~seconds) per quality
  step back toward the configured value. Bench knob, RAM only, default 10. **Do not lower it to
  make recovery faster** - measured, see the ease-down entry below
- `govWinMs=<0..5000>` - the length of the SD governor's rolling measurement window, default 1000.
  NOT the decision interval any more: a decision is taken on every frame regardless. 0 collapses the
  window to a single frame, which is measured broken and kept only as the record (§38.30). Bench
  knob, RAM only. `govWrites` in `updateFPS` and the `closeAvi` line are the churn metric
- `fpsPriority=0|1` - **which gives way when the card cannot keep up: quality or the frame rate.**
  Default ON and persisted. ON, `sdGovernor`'s rate arm pushes JPEG quality (boost cap
  `GOV_MAX_BOOST_FPS` 14, not 4) while the rolling window delivers under `GOV_RATE_PCT` of the
  request, and the relax and ease arms stand down while that deficit stands. OFF is the pre-7 Sep
  behaviour: the user's quality is kept and frames are shed. Measured A/B in open item 2. It cannot
  help a size whose demand sits under `GOV_PUSH_PCT` while it still misses the rate (QHD, QSXGA)
- `motionStats=1`, `zoneStats`, `avgZones` - detector counters and the AEC 4x4 zone grid
- `sdBusClk` / `sdBusDiv`, `battScale` / `sagTest`, `extDVDD`, `lencFhd` (LENC A/B)
- `peerReset=1` - pulse the OTHER board's RESET through D1 / GPIO 2 for 5 s (`=<ms>` 500-10000,
  `=0` reads the pin: 1 = the other board's EN is high). Wired COM3 -> COM4 only; on a board
  with nothing on D1 it is a harmless pulse. The far board comes back POWERON (RTC ring and
  crash snapshot gone). An EN reset is NOT a power cycle: on 4 Sep 2026 COM4 came back from
  the pulse alive but handling every packet ~1 s late (TCP body at 35 B/s, its own gateway
  ping failing, the wifi supervisor flapping the link) and only a real power cycle cured it -
  the card and the radio keep their state through EN. Prefer a power cycle after a wedge
- `fileProbe=1` - **how many of the mount's open-file slots are free.** Opens one file over and
  over until the mount refuses, reports the count as JSON, closes them all. Read only. Healthy
  idle is 14 with SD logging on, 15 with it off; take a reading either side of a suspect
  operation and the difference is the leak, in slots (§38.21)
- `stillSave=0|1` - whether Get Still is filed on the card as well as previewed. Default on;
  `assert_campaign_config` turns it off for a bench run and the exit restore puts it back
- `banding=0|50|60` - the mains banding filter, persisted with `save=1`. 0 (the default) is
  off: the AEC then spends the whole frame on exposure before gain. 50/60 select the manual
  band; `dumpCam` reports the live state on its Exposure line

Destructive or dangerous:
- `/sustain?download=0` - **DO NOT USE, it wedges the web server** (§38.21, open). Not a control,
  but it belongs on this list: no abort needed, the first download after a boot has done it, and
  recovery is a reset. `/file?path=` fetches the same file safely
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
- `night_ceiling_probe.sh` - **where a long exposure stops DELIVERING frames.** Walks the night
  slider and counts complete frames off the stream per rung, reporting delivered/produced against the
  period the registers imply. Holds the stream across each change deliberately: a session that has
  stopped delivering cannot be retimed or left, so without a consumer the walk cannot proceed - and a
  viewer is the realistic case anyway. `MSLIST` sets the rungs, `WIN` the window
- `night_revert_verify.sh` - the panel at its 3.18 s ceiling through the page's own path, that a
  longer request is clamped rather than refused, and that a board forced into a no-frame state climbs
  out without a reboot. The last part passed but did not clearly reproduce the stuck state first
- `night5s_verify.sh` - the 5 s attempt, kept as the record: it PASSED every register and arithmetic
  check (HTS 8191, VTS 3093, 3089 lines, 1141 dummy lines) and failed on delivery, which is how the
  4.0-4.5 s cliff was found
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
- `gov_size_regress.sh` - **the SD governor across frame sizes, lit room** (§38.32). A 30 s clip at
  each size's ceiling, gated against the 2-3 Sep lit q10 figures in `frameData`. **It gates the
  governor only** - rescues, `govWrites` against the boost taken, and a boost above the recorded one
  only when the demand did not justify it. **Delivered fps is reported, never gated**: at these
  ceilings it is storage-time bound, storage time follows frame size, frame size follows the room,
  so gating it reports the scene as a code regression (my first version did, and flagged four sizes
  on a governor that was clean at all seven). `SIZES_LIST` overrides the set, `GOVWIN` the window for
  an A/B, `DUR` the clip length. The restore is on an **EXIT trap**, because `bench_lib`'s `preflight`
  and `ctl` call `exit` directly and an abort otherwise leaves `micGain` and `stillSave` at 0 - which
  then becomes the NEXT run's captured restore point. Measured clean at 8 sizes twice, 6 Sep 2026
- `fps_sustain_descend.sh` - **what rate each size actually DELIVERS at its ceiling**, and therefore
  what the ceiling should be. Walks DOWN from the ceiling, jumping straight to the delivered rate
  (if it carried 38.1 of 41 it can carry ~38) rather than stepping, so QVGANARROW is a handful of
  rungs not 147. `CONFIRM` clips must pass before a rate is accepted and 2 is the minimum that
  works - QVGANARROW passed 147 once at 146.5 then failed at 144.3. Enforces the settle rule
  itself (`SETTLE`, 240s): run at 207s it read busy 99% with 22ms of MONITORING time where the
  settled board reads 15% and 0. `TOL`, `DUR`, `MAXRUNGS`, `MINFREEGB`, `SIZES_LIST`
- `quality_buys_rate.sh` - what one JPEG quality step is WORTH: a ladder of base qualities at one
  size and rate. Measured 1280X960 q14->q20 at 2.9% of frame size per step, storage 24->19ms,
  busy 86->70% - against the 1.5x per step `GOV_STEP_GROWTH` assumes from a QSXGA measurement
- `fps_priority_ab.sh` - the `fpsPriority` A/B. **Alternates the toggle rather than running it in
  blocks**, because the scene drifts: an hour of failing light moved 1280X960's delivered rate by
  3fps on its own, and blocks would confound the room with the setting
- `hts_floor_hypothesis.sh` - **the rig that killed the row-time floor.** Walks HTS below 2060 at
  TWO clocks: 88 MHz (where both models predict failure, so it can only falsify) and 60 MHz,
  where every row is 30-34us and the models predict OPPOSITE outcomes. That second arm is the
  whole experiment. Every HTS change goes through the 0x3212 group write without exception
- `hd_clock_96.sh` / `hd_row_time.sh` - the clock cliff, walked properly. The first moves the PLL
  multiplier at fixed geometry, the second PINS the clock and moves HTS, which is what separates
  a clock limit from a row-time one. Neither stops at the first bad rung: a cliff should be shown
- `magenta_reverify.sh` - the cast, re-tested with a POSITIVE CONTROL (a row SHORTER than the one
  said to fail). Single-byte atomic HTS writes throughout, analog registers read at both ends
- `hd_window_bisect.sh` / `hd_vts_floor.sh` / `hd_vts_isp.sh` - which write stops frame delivery,
  where the VTS floor really is, and whether an ISP block owns the blanking (LENC does not; the
  defect-pixel cancellers own 2 lines). All three carry a RATE gate, without which the half-rate
  mode reads as a pass
- `hdv2_soak.sh` - HDV2's soak, and it COUNTS rather than looks: the risk is a silent halving, not
  a bad picture. 25 state entries, temperature logged, recordings interleaved
- `tuner_table.sh` - the whole fps/PIXCLK/HTS/VTS table for a size, read off the board from the
  `Tuned timing` line in the RTC ring. Far cheaper than a VSYNC count per rung; registers only,
  so the ceiling still gets a count and a gated still
- `hd88_probe.sh` / `hd_hts_count.sh` / `ceiling_delivery.sh` / `hd_vs_hdv2.sh` - the 88 MHz probe
  with its quality ladder, the honoured-line check, delivered rate at a ceiling, and the HD/HDV2
  A/B. The last ALTERNATES and gates its verdict on the within-size spread: means alone called a
  1.40 fps gap a win when the spread inside one size was 4.70
- `still_bands.py` / `contact_sheet.py` - the per-band gate that catches damage confined to part of
  a frame, and a labelled grid of every still in a run so the eyeball gate costs one look
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
- `stream_grab.py` - pull one complete JPEG out of a raw MJPEG capture. **The instrument for any
  rate below ~1 fps**: the still handler waits `MAX_FRAME_WAIT` (1.2 s) for a kept frame and gives up,
  so a request lands with probability 1.2 x fps - ZERO stills in 59 requests at 7.5 and 10 s frames,
  while the VSYNC pin said the sensor was delivering. The stream has no such window. **It requires a
  start-of-frame marker, not just SOI/EOI**: the sensor really does emit a 68-byte header-only frame,
  which a marker-pair scan calls complete and a byte gate calls small. Reports complete and truncated
  counts separately. **Proven not to disturb the sensor** with `idleFps` 0 - a VTS written by register
  survived an 8 s stream open byte for byte; with `idleFps` non-zero the throttle release retimes and
  WOULD rewrite it, so campaign config's `idleFps=0` is load-bearing wherever this is used
- `bench_lib.sh` `CTL_TRIES` / `CTL_RETRY_S` (default 2 attempts 5 s apart, exactly as before) -
  raise them on a flaky LAN. The board's own supervisor takes 60-121 s to restart a station and it
  roams between two APs on this SSID, so a transient roam looks EXACTLY like a wedge: two failures,
  then `peer_reset` - which is a POWERON that wipes the RTC ring, the one place the pre-crash tail
  lives. The default escalation destroys the evidence needed to tell the two apart
- `bench_lib.sh` `AF_DIRECT=1` with `AF_VCM_SET` - place the lens by register and skip the AF program.
  For a DARK run it loses nothing (the program cannot focus in low light and the hold overwrites its
  answer anyway) and it drops a size change, a settle and ~20 mailbox polls: the flakiest stretch of
  any dark campaign's setup, and where a run aborted on 6 Sep
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
