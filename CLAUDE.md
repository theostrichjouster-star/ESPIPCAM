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
- Serial diagnostics: 115200 baud with DtrEnable/RtsEnable false (proven
  non-resetting on these boards).
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
- `BOARD_TESTING.md` - local bench notebook: boards, calibrations, measured laws
- `THERMAL_SOAK.md`, `FPS_RECAL.md` - local bench campaigns (untracked)
