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
  App-only; preserves NVS (wifi creds, calibrations) and the SD card.
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

## Docs map

- `BATTERY.md` - battery deployment guide (committed)
- `BOARD_TESTING.md` - local bench notebook: boards, calibrations, measured laws
- `THERMAL_SOAK.md`, `FPS_RECAL.md` - local bench campaigns (untracked)
