# ESPIPCAM

IP camera firmware for the Seeed Studio XIAO ESP32S3 Sense with an OV5640. Records motion-triggered or continuous video to SD card as AVI files with audio, streams live to a browser or an NVR, and updates itself over the air from GitHub Releases.

A hardware-locked fork of [s60sc/ESP32-CAM_MJPEG2SD](https://github.com/s60sc/ESP32-CAM_MJPEG2SD), retuned against real boards rather than inherited defaults.

---

## What the retuning bought

Every figure here was measured on this hardware. Frame rates are counted off the sensor's VSYNC pin and confirmed by a recorded clip.

### Sensor frame rates

Upstream ships one default rate per frame size. This build measures a ceiling per size, and any integer rate up to it can be requested.

| Frame size | Upstream default | Ceiling here |
|---|---|---|
| QVGANARROW 320x240 | not in upstream | 147 |
| VGANARROW 640x480 | not in upstream | 77 |
| HD 1280x720 | 5 | 56 |
| 1280X960 | not in upstream | 42 |
| VGA 640x480 | 20 | 39 |
| QVGA 320x240 | 30 | 39 |
| SXGA 1280x1024 | 5 | 17 |
| FHD 1920x1080 | 5 | 16 |
| UXGA 1600x1200 | 5 | 14 |
| QXGA 2048x1536 | 5 | 11 |
| QHD 2560x1440 | 5 | 8 |
| QSXGA 2560x1920 | 4 | 6 |

Six frame sizes exist here that upstream does not have. The two NARROW sizes crop the sensor array rather than scaling it down, trading field of view for rate, which is where 147 fps at QVGA comes from. `FHDMID` and `FHDFULL` are two more crops of the 5 MP array, at 12 and 9. `HDV2` is 720p read the way the datasheet specifies it, worth half a frame per second over `HD` at the top three rates and nothing below them.

### WiFi throughput, and a rebuilt core

Throughput over WiFi is the TCP window divided by the round trip time, and the window was pinned by the lwip send buffer: a compile-time constant baked into the prebuilt Arduino core at 5760 bytes. Nothing at runtime reaches it, since this core defines `SO_SNDBUF` as unimplemented and every IDF component links as a prebuilt archive, so neither a socket option nor a project `sdkconfig` can move it.

Rebuilding the core is the only route, so this repository does that and raises the buffer to 65535. Released binaries carry it. See [`tools/core/`](tools/core/) for the build and the config gate run before flashing.

| Live stream at HD | Stock core, 5760 | Rebuilt core |
|---|---|---|
| 67 KB frames, dark scene | 8.5 fps, 552 KB/s | 15.7 fps, 1121 KB/s |
| 19 KB frames, lit scene | 25 fps, ~100 frames dropped | 30 fps, 0 dropped |

The gain scales with frame size, so it is largest on the big frames a dark scene produces. At ordinary frame sizes the stream reaches the sensor's own cap and discards nothing it offers. That is what zero dropped frames means: sent plus skipped equals what the sensor produced, so the count is self-calibrating.

### Storage write speed

The write ceiling here is the 1-bit SD bus, not the card. The host clock divider is not reachable through the public driver API, so it is set through the hardware registers and persisted, reapplied after every mount.

| SD bus clock | Sustained write |
|---|---|
| 40.00 MHz, the driver default | 3.55-3.68 MB/s |
| 53.33 MHz, opt-in | 4.39 MB/s |

53.33 MHz is 7% outside the SD High Speed specification, so it ships off and `sdBusDiv` defaults to 4. Setting it to 3 buys 22% of sustained write, and the recording governor's budget moves with it from 3643 to 4458 KB/s.

It has to be qualified per physical card. A 32 GB card passed three integrity round trips at 53.33 MHz. A 256 GB SDXC in the same board was already card-limited at 40 MHz and turned intermittent at 53.33, truncating a 512 KB write at 176 KB, which is worse than failing cleanly. Bigger is not faster on this bus. 80 MHz is a hard wall where file creation fails outright and only a reboot recovers the driver. Every failure seen was a truncation or a clean error, never silent corruption, and the cards verified unharmed afterwards.

### Web UI delivery

The page is generated at build time. Sources stay commented and readable in [`src/web/`](src/web/), and the build minifies them and writes a pre-gzipped copy alongside, which the board serves with `Content-Encoding: gzip`.

| | Source | Served |
|---|---|---|
| `MJPEG2SD.htm` | 307 KB | 33 KB |
| `common.js` | 58 KB | 9 KB |
| Total | 365 KB | 42 KB |

That is 8.6x less on every page load, over the same link the video uses.

### Other measured gains over upstream

| | Upstream | Here |
|---|---|---|
| Clip playback in the browser | 21.9 fps, silent | full recorded rate, with audio |
| Serving a file off the card | 1.32 MiB/s | 1.42 MiB/s |
| Flash used | baseline | ~86 KB freed |
| Motion detection above SXGA | stops working | runs at every frame size |

The audio was in the recordings all along; the old player stepped over every audio chunk and discarded it.

### Added here

* `fpsPriority`, on by default, trades JPEG quality to hold the requested frame rate when the card cannot keep up. Measured at HD 52, alternating the setting so a drifting scene could not pick a side: 49.4 fps delivered with it on against 46.5 with it off.
* An SD governor that measures over a rolling window and decides every frame. On an 87 second steady clip that took quality writes from 96 to 0.
* Long exposure to 3.18 s. The sensor integrates correctly to at least 10 s, but frame delivery falls off a cliff between 4.0 and 4.5 s.
* Firmware update from GitHub Releases, tested end to end on hardware.

---

## Attribution and licence

ESPIPCAM is a derivative work of [s60sc/ESP32-CAM_MJPEG2SD](https://github.com/s60sc/ESP32-CAM_MJPEG2SD).

Substantially all of the core functionality, including the AVI recording engine, motion detection, audio capture, web interface, streaming and every integration below, is the work of [s60sc](https://github.com/s60sc) and the contributors credited at the end. This project repackages and retunes that firmware for one fixed board. It is not an independent implementation and would not exist without their work. If you find it useful, please star the upstream project.

Licensed under the GNU Affero General Public License v3.0, inherited from upstream. See [LICENSE](LICENSE).

> **Notice of modification (AGPL-3.0 §5a):** a modified version of ESP32-CAM_MJPEG2SD, forked at upstream v10.9.4. Modified between 21 August and 8 September 2026 by the maintainers of this repository. See [Changes from upstream](#changes-from-upstream) and the commit history.

> **Network use (AGPL-3.0 §13):** this firmware operates as a network server. If you deploy a modified version where others interact with it over a network, you must offer them the Corresponding Source of your version. The Corresponding Source for this version is at [github.com/theostrichjouster-star/ESPIPCAM](https://github.com/theostrichjouster-star/ESPIPCAM).

## What this build is

Upstream supports around twenty ESP32 and ESP32S3 camera boards, with every GPIO exposed as a web-configurable field so it can be adapted to arbitrary hardware. ESPIPCAM is the opposite: a fixed end product for one board. Board selection, the alternate camera drivers, the Ethernet stack, the companion mode and the GPIO configuration UI are all gone. Pins are hardwired, autofocus and audio are on by default, and updates arrive over the air.

The trade is deliberate. This build is smaller and simpler, but it will not run on any other board, and features that depended on user-assignable pins are no longer configurable.

## Hardware

| | |
|---|---|
| Board | Seeed Studio XIAO ESP32S3 Sense. The Sense expansion board is required, since it carries the camera connector, SD slot and microphone |
| Camera | OV5640, autofocus enabled by default |
| PSRAM | 8 MB octal (OPI), 2 MB minimum enforced at startup |
| Storage | microSD, 1-bit SD_MMC |

Pins are fixed in [`camera_pins.h`](camera_pins.h): SD card CLK/CMD/D0 on GPIO 7/9/8, PDM microphone data and clock on 41/42, camera per the XIAO Sense reference layout.

The user LED on GPIO 21 is not driven, since upstream's lamp driver sits inside `INCLUDE_PERIPH`, which is off here. 4-bit SD mode is unavailable because the expansion board only wires `D0`, so upstream's roughly 2x write speedup for 4-line SD_MMC cannot be reached without hardware modification.

The OV5640's pinout matches OV2640-designed boards, but its internal 1.5 V regulator runs hot. A heat sink helps in sustained use.

## Features

On by default: motion detection, continuous dashcam recording, audio from the PDM microphone muxed into the AVI as WAV, autofocus, live MJPEG streaming, still capture, a gallery with browser playback, SD card management with oldest-first deletion when space runs low, and over-the-air updates.

Off by default. Set the `#define INCLUDE_*` to `true` in [`appGlobals.h`](appGlobals.h):

| Flag | Feature |
|---|---|
| `INCLUDE_FTP_HFS` | Upload recordings to an FTP or HTTPS file server |
| `INCLUDE_SMTP` | Email alerts |
| `INCLUDE_TGRAM` | Telegram bot alerts |
| `INCLUDE_MQTT` / `INCLUDE_HASIO` | MQTT control and Home Assistant discovery |
| `INCLUDE_WEBDAV` | WebDAV access to the SD card |
| `INCLUDE_CERTS` | HTTPS, and verification of remote server certificates |

`INCLUDE_NEW_JPG` selects the `esp_new_jpeg` codec instead of the core's. It stays `false`: it is an ESP-IDF component rather than an Arduino one, so it adds a manual install step for every user, and since motion detection no longer decodes anything there is nothing left for it to speed up.

## Building

Requires the arduino-esp32 core v3.1.1 and the [`ESP32-OV5640-AF`](https://github.com/0015/ESP32-OV5640-AF) library. Autofocus is on by default, so the build fails clearly without it.

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi --warnings all .
```

> `PSRAM=opi` is not optional. The board's PSRAM menu defaults to *Disabled*, so a bare `--fqbn esp32:esp32:XIAO_ESP32S3` compiles without `-DBOARD_HAS_PSRAM` and produces a binary that flashes and boots, then halts with `Startup Failure: Need PSRAM to be enabled`. A correct build logs `PSRAM 8.0MB, mode OPI @ 80Mhz` at boot.

That build is fully functional and differs from the published binaries only in the lwip send buffer. To match them, point the compile at the rebuilt core per invocation rather than installing it over the stock one:

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi \
  --build-property "tools.esp32-arduino-libs.path=<rebuilt core>" \
  --build-property "runtime.tools.esp32-arduino-libs.path=<rebuilt core>" \
  --build-path build-65535 .
```

Both switches are needed, and a plain compile silently produces a stock-core image. `lwipSndBuf` in `/status` is the only thing that tells the two apart on a running board, so read it back if it matters.

### Flashing

Over the air is the normal route, and the only one that keeps the rollback image in the other slot. Arm the upload first, then POST the raw file as the request body:

```bash
curl "http://<camera>/control?startOTA=firmware.bin"
curl --data-binary "@build-65535/ESP32-CAM_MJPEG2SD.ino.bin" "http://<camera>/upload"
```

Do not use `-F`: the handler reads the body directly, so a multipart form arrives as corrupt firmware. Any name containing `.bin` is treated as an image, and anything else lands in `/data`, which is how the web files are pushed. A fresh image is confirmed only after the camera, storage and WiFi validate, so an image that fails to boot properly is reverted on the next reset.

Serial still works and preserves NVS and the SD card, but it writes the bootloader and OTA data as well as the app, which discards the known-good image in the other slot:

```bash
arduino-cli upload --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi --port COMx .
```

In the Arduino IDE select XIAO_ESP32S3 and set PSRAM to OPI PSRAM; the other defaults are already right. Note that `arduino-cli board list` reports this board as the generic `esp32:esp32:esp32_family`, since every ESP32-S3 with native USB shares the same USB IDs, so always pass the FQBN explicitly.

The web UI is generated too, so edit [`src/web/`](src/web/) and never `data/`. Run `node tools/web/build.mjs` after a change, which writes the minified and pre-gzipped files into `data/`. Those outputs are committed on purpose, because a board re-downloads them from this repository whenever its card loses them. `node tools/web/build.mjs --check` fails on a stale `data/` and should be run before committing a web change. Detail in [`tools/web/README.md`](tools/web/README.md).

## First run

On first boot the device starts an access point named ESP-CAM_MJPEG_... Connect to it, open `192.168.4.1`, and select your router and password. The configuration file is created automatically, and the web interface files download to the card's `/data` folder from this repository's `main` branch once the device has internet access. `/data` can be reloaded later from the Edit Config tab.

Settings changed on the web page are held in memory until you press Save. A reboot before saving discards them. Browser functionality is fully tested only on Chrome.

## Firmware updates

Two routes, both requiring authentication.

Over the air, under Access Settings, a Firmware update section carries a Check for Updates button. If a newer release exists, Install Update & Restart becomes available: it downloads the release asset, writes it to the OTA partition and reboots. A release must be tagged with a version parsing higher than the running `APP_VER`, where a leading `v` is optional, and must carry an asset named exactly `ESPIPCAM.bin`.

The device rejects an image under 64 KB before touching the OTA partition and aborts cleanly on a partial download. A fresh image is confirmed only after the camera, storage and WiFi validate; an unconfirmed image plus any reset reverts automatically to the previous one.

> Versions 1.0.0 through 1.0.2 carry a broken updater and cannot install anything over the air. The fault is in the updater those versions are running, so a camera on one of them needs a single manual flash to reach 1.0.3 or later. Self-update is tested and works from 1.0.3 on.

The OTA Upload tab also accepts a locally built `.bin`, as upstream does.

## Security

Every web endpoint requires HTTP Basic authentication once credentials are set, including `/control`, `/update`, `/upload`, `/status`, `/web`, the WebDAV tree and the websocket. Set a username and password under Access Settings on first run. Leaving them blank leaves the device open, which suits only a trusted network during provisioning.

Also hardened: query-string and path lengths are bounds-checked, path traversal is rejected, credential comparison is constant-time, and Telegram tokens are masked in the status JSON and saved configuration. Residual risks, accepted rather than fixed: firmware images are not cryptographically signed, so anyone who can authenticate can flash arbitrary firmware; HTTP is the default and HTTPS needs `INCLUDE_CERTS` and plenty of memory; CORS headers are permissive; FTPS is stubbed and MQTT has no TLS.

To reach the camera over the internet, forward a router port to the device's HTTP port. Set a static IP and credentials first. ISPs using [CGNAT](https://en.wikipedia.org/wiki/Carrier-grade_NAT) may make this impossible.

## Configuration

Six panels on the sidebar cover most settings. Changes are held in memory until you press Save, and network changes need a reboot.

| Panel | Holds |
|---|---|
| Camera Control | Resolution, frame rate, quality |
| Image settings | Exposure, white balance, colour, geometry, focus, mains banding filter |
| Long exposure | Stills and video below 1 fps |
| Motion detection and recording | Sensitivity, zone thresholds, capture length, dashcam interval, night switch |
| Gallery | Recordings and stills: play, download, delete |
| System | WiFi, hostname, time zone, authentication, storage, firmware update |

Everything else is under Edit Config, grouped Network, Motion, Streaming and Other: detection tuning, stream enables, SD management, MQTT, Telegram and deep sleep.

For time zone, use the dropdown or paste a value from the second column of [this list](https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv).

There is no pin configuration UI, and no Peripherals panel: every setting it held belonged to a subsystem this build does not compile, so toggling them only ever wrote a value to `configs.txt`. For the same reason there are no lamp, pan/tilt or RC controls, and no Machine Learning options.

The footer shows a Sensor field reporting what the camera is doing and at what resolution, such as `Recording (FHD)`, `Live view (FHD)` or `Detecting (FHD)`.

Logs are viewable under Show Log, held in RTC RAM as a 7 KB cyclic buffer that survives a reset, streamed over websocket, or written to SD. SD logging can slow recording.

## How recording works

Frames are buffered in PSRAM and written to the card in 32 KB blocks, a whole number of sectors, so the write count stays low. Recordings are named `YYYYMMDD_HHMMSS` plus frame size, frame rate and duration, for example `20200130_201015_VGA_15_60.avi`, in a per-day folder. `_S` marks audio, `_C` continuous.

Saving a set of JPEGs as one AVI is faster than writing individual files and replays at the right rate in ordinary players. Throughput depends heavily on card quality; a name-brand card can be several times faster than a no-name card of the same class.

When the card cannot keep up, `fpsPriority` decides what gives way. On by default, it raises JPEG compression to hold the frame rate you asked for, then walks the quality back down a step at a time once the scene allows. Off, the configured quality is kept and frames are shed instead. Either way the decision is taken on every frame but measured over a rolling window, because frames arrive in bursts and a single-frame sample reads that jitter rather than the sustained write rate.

A motion-triggered recording runs for a fixed `Capture Seconds`, default 15, and is not extended by continued movement. If movement persists when the file closes a new recording starts immediately, so sustained activity produces a series of fixed-length files.

Playback happens in the browser rather than on the board: tapping a clip fetches it and plays it locally, at full recorded rate with sound. Stills are filed on the card alongside recordings and appear in the same gallery, with cached thumbnails.

## Motion detection

Recording can be triggered by the camera detecting movement, or manually.

Detection reads the sensor's own 4x4 zone luminance grid, sixteen registers the auto-exposure engine already maintains, and compares them against the previous check. That is about twenty register reads over SCCB and a few milliseconds, with no frame decode anywhere in it. The JPEG-decode background-subtraction detector that upstream uses was removed outright.

Detection therefore runs at every frame size, where decoding at 1.3 MP and above intermittently hard hung the board and capped it at HD. The sensor stays at the capture resolution, so there is no VGA round trip on every recording and no transition frames to flush. Checks also continue through recordings, live view and NVR streams, where the old design had to suspend detection while capturing and produced a deterministic false retrigger after every motion recording. Only dashcam mode opts out.

Two gates decide whether the zones can be trusted, because a global optical event moves all sixteen at once and is not movement. While the luminance average sits inside the band where the auto-exposure engine holds exposure, the zones are stable to plus or minus one count on a static scene; outside it the AEC is re-exposing. A focus hunt moves every zone too, so only idle and focused autofocus states count.

A failed gate is indeterminate rather than negative: the reference is dropped and the motion state returned unchanged, so there is never a false edge either way. It also pauses the consecutive-trip streak rather than resetting it. Real movement perturbs the exposure continuously, so checks alternate between tripped and out of band, and on the first walk test fourteen zones moved at a delta of 52 while nothing fired, because every re-exposure had zeroed the streak.

| Control | Default | Effect |
|---|---|---|
| `Motion Sensitivity` | 8 | Maps 1-10 onto the per-zone luminance delta that counts as changed, inverted: 8 gives a delta of 4, against a measured noise floor of 1 |
| `Zones changed at once` | 2 | How many of the sixteen must change in one check |
| `Num changed checks` | 3 | Consecutive tripped checks needed to confirm |
| `Checks per second` | 5 | Check rate, so the default confirms on 0.6 s of sustained movement |
| `Night Switch` | 10 | Light level below which detection stops |
| `Capture Seconds` | 15 | How long a triggered recording runs |

Those defaults are measured: a walk across half the frame peaked at 4 consecutive tripped checks, so 3 confirms it and 5 would have missed it, while a single-zone LED blink never exceeded 1. Expect roughly 5% of delivered frames to be lost with detection on.

`Show Motion` streams the detector's own view instead of the camera image: a 96x96 grid of the sixteen zones, grey for luminance, red for a zone that moved on this check, dimmed for one masked out. `zoneMask` sets that mask and is URL-only, held in the order you see rather than the order the sensor reads, so a flip never silently moves the masked region.

The log reports both edges with the numbers needed for tuning:

```
Motion detected: 6 zones moved >= 4 (zoneCount 2, 3 consecutive), light 38%
Motion ended: 1 zones moved, below zoneCount 2
```

`/control?motionStats=1` reports the thresholds in force and the peak delta and zone count since the last request. `/control?zoneStats=1` returns the live zones and their deltas against the detector's reference as JSON.

Below `Night Switch` detection stops, and the log says so once per transition rather than silently doing nothing. The light level comes from the sensor's pre-gamma luminance average, so it measures exposure output rather than room brightness and can latch in ordinary indoor light.

## Audio

The onboard PDM microphone is enabled by default. Audio is 16-bit mono PCM at 16 kHz, stored as WAV inside the AVI.

Microphone Gain defaults to 5, where 3 is unity, higher amplifies and lower attenuates. Setting it to 0 turns audio off, since recording and the NVR audio stream are both gated on it. The speaker icon streams live microphone audio to the browser. Two-way intercom additionally needs an I2S amplifier, so it needs a free pin and a source change here. See [`audio.cpp`](audio.cpp).

## Streaming to an NVR

Streaming is MJPEG over HTTP, and performance depends on network quality. Enable the streams under Edit Config, Streaming, which exposes `/sustain?video=1` and `/sustain?audio=1`. Multiple streams need an intermediate tool such as [go2rtc](https://github.com/AlexxIT/go2rtc) to synchronise them.

The sensor stays at the capture resolution throughout and zone detection keeps running, so a connected viewer does not suspend motion recording. The audio stream does not affect the sensor.

RTSP was removed. It required an external library, and enabling it disabled the HTTP NVR streams outright by taking the same task slots.

## Optional integrations

### MQTT

Under Edit Config, Other, set broker IP, topic prefix, optional credentials, then enable. Status is published to `homeassistant/sensor/{hostname}/state`, and commands accepted on the `/cmd` channel, for example `dbgVerbose=1;framesize=7;fps=1`. With `INCLUDE_HASIO`, discovery messages create a Home Assistant [MQTT Camera](https://www.home-assistant.io/integrations/camera.mqtt/) automatically and publish an image on motion. Contributed upstream by [@gemi254](https://github.com/gemi254).

### Telegram

Enable either Telegram or SMTP, not both. Get a chat ID from [IDBot](https://t.me/myidbot) and a token from [BotFather](https://t.me/botfather), then enter both under Edit Config, Other. The bot receives motion alerts with a frame and a download link, up to 50 MB. It uses a lot of heap because of TLS.

### WebDAV

Set `INCLUDE_WEBDAV` to `true` and browse the card at `<ip_address>/webdav` from a client such as Windows File Explorer. See [`webDav.cpp`](webDav.cpp).

### HTTPS

Set `INCLUDE_CERTS` to `true`, then toggle Use HTTPS under Access Settings. See [`certificates.cpp`](certificates.cpp) for generating and installing certificates. If HTTPS is enabled with incorrect certificates the web page becomes unreachable and the certificate files must be deleted from the card by hand. Check Certs separately enables verification of remote server certificates.

## What was removed

Deleted outright and not recoverable by a flag: photogrammetry, machine learning (its classifier never compiled, carrying an inherited syntax error), RTSP, telemetry recording, the Camera Hub, BDC motor control, I2C peripherals, the auxiliary board over UART, the external heartbeat, time lapse, Ethernet, and the companion mode that ran this firmware on a second cameraless ESP32.

Still present in source but not configurable: the peripherals behind `INCLUDE_PERIPH` and `INCLUDE_DS18B20`, whose pins are all unassigned since the GPIO UI went. Enabling a flag also means restoring the corresponding rows to `appConfig` in [`appSpecific.cpp`](appSpecific.cpp) and bumping `CFG_VER`.

For documentation of these features as they work on upstream hardware, see the [upstream README](https://github.com/s60sc/ESP32-CAM_MJPEG2SD#readme).

## Changes from upstream

Forked at v10.9.4. Versioning restarted at `1.0.0`.

| Change | Detail |
|---|---|
| Motion detection | Rebuilt on the sensor's own 4x4 zone luminance grid: no frame decode, so it runs at every frame size, holds the capture resolution, and keeps checking through recordings and live views. The decode detector it replaced hung the board above 1.3 MP, forced a VGA round trip per recording, and false-retriggered after every one |
| Sensor retuning | Per-size frame rate ceilings measured on hardware rather than inherited, six new frame sizes, an 88 MHz pixel clock route, and a tuner that programs clock, line length and frame length per requested rate |
| WiFi throughput | A rebuilt arduino-esp32 core raising the lwip send buffer from 5760 to 65535, which is the only way to reach that constant, plus a 32 KB file-serving chunk |
| Storage | The SD host clock divider made settable and persistent, for 22% more sustained write on a qualified card |
| Recording | `fpsPriority` and a rolling-window SD governor; long exposure to 3.18 s; FHD recordings no longer write a header claiming 920x1080; frames left over from a resolution change no longer reach a file |
| Web UI | Rebuilt: phone card layout, gallery with in-browser playback and multi-file zip download, camera control panel, long exposure panel, SVG icon set, new palette, and a minify plus pre-gzip build step taking the page from 365 KB to 42 KB served |
| Playback | Clips play in the browser at full recorded rate with their audio, which was present in every recording upstream writes and discarded by its player |
| Board lock | From ~20 boards to XIAO ESP32S3 Sense only, and the GPIO configuration UI removed with them |
| Security | Authentication on every endpoint, bounds checks, path-traversal rejection, constant-time credential compare, token masking, OTA size sanity check |
| Code removal | Around 5,500 lines across dead branches, Ethernet and eight whole source files that compiled to nothing, freeing roughly 86 KB of flash |
| OTA | Update checking and installation from GitHub Releases, tested end to end |

Full detail is in the commit history.

## Credits

* [s60sc](https://github.com/s60sc), original author of ESP32-CAM_MJPEG2SD and of essentially all functionality here
* [@gemi254](https://github.com/gemi254), Home Assistant MQTT integration and the original setup assistant
* [@alojzjakob](https://github.com/alojzjakob), external heartbeat, since removed; see also [EspSee](https://github.com/alojzjakob/EspSee)
* [@josef2600](https://github.com/josef2600), SD_MMC 4-line mode investigation
* [@RedCanti](https://github.com/RedCanti), Ethernet support, since removed
* [@ldijkman](https://github.com/ldijkman), installation walkthrough
* Eric Nam ([@0015](https://github.com/0015)), the [OV5640 Auto Focus](https://github.com/0015/ESP32-OV5640-AF) library this build depends on

## Licence

GNU Affero General Public License v3.0, inherited from upstream. See [LICENSE](LICENSE).

Because this firmware acts as a network server, AGPL §13 applies: if you run a modified version and let others use it over a network, you must make your modified source available to them. The source for this version is at [github.com/theostrichjouster-star/ESPIPCAM](https://github.com/theostrichjouster-star/ESPIPCAM).
