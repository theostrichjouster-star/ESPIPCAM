# ESPIPCAM

IP camera firmware for the **Seeed Studio XIAO ESP32S3 Sense** with an OV5640 camera. Records motion-triggered or continuous video to SD card as AVI files with audio, and streams live to a browser, an NVR, or an RTSP client.

---

## Attribution

**ESPIPCAM is a derivative work of [s60sc/ESP32-CAM_MJPEG2SD](https://github.com/s60sc/ESP32-CAM_MJPEG2SD).**

Substantially all of the core functionality — the AVI recording engine, motion detection, audio capture, web interface, streaming, and every integration described below — is the work of **[s60sc](https://github.com/s60sc)** and the upstream contributors credited at the end of this document. This project is a repackaging of that firmware for a single fixed board; it is not an independent implementation, and it would not exist without their work. If you find this useful, please star the upstream project.

This program is free software licensed under the **GNU Affero General Public License v3.0**, inherited from upstream. See [LICENSE](LICENSE).

> **Notice of modification (AGPL-3.0 §5a):** This is a modified version of ESP32-CAM_MJPEG2SD, forked at upstream version 10.9.4. Modifications were made on **21–22 August 2026** by the maintainers of this repository. See [Changes from upstream](#changes-from-upstream) for what was changed, and the commit history for full detail.

> **Network use (AGPL-3.0 §13):** This firmware operates as a network server — it serves a web interface over HTTP. If you deploy a modified version of it on a device that others interact with over a network, you must offer those users the Corresponding Source of your modified version. The Corresponding Source for this version is published at **[github.com/theostrichjouster-star/ESPIPCAM](https://github.com/theostrichjouster-star/ESPIPCAM)**.

---

## What this build is

Upstream ESP32-CAM_MJPEG2SD is a general-purpose firmware supporting around twenty different ESP32 and ESP32S3 camera boards, with every GPIO exposed as a web-configurable field so it can be adapted to arbitrary hardware.

ESPIPCAM is the opposite: a **fixed end-product for one board**. The board selection logic, the alternate camera drivers, the Ethernet stack, the auxiliary-board companion mode, and the GPIO configuration UI have all been removed. Pins are hardwired to the XIAO Sense layout, autofocus and audio are on by default, and firmware updates are delivered over the air from GitHub Releases.

The trade-off is deliberate: this build is simpler and smaller, but it will not run on any other board, and the peripheral features that depended on user-assignable pins are no longer configurable — see [Inherited features that need source changes](#inherited-features-that-need-source-changes).

## Hardware

| | |
|---|---|
| Board | Seeed Studio XIAO ESP32S3 **Sense** (the Sense expansion board is required — it carries the camera connector, SD slot and microphone) |
| Camera | OV5640, autofocus supported and enabled by default |
| PSRAM | 8 MB octal (OPI). Minimum 2 MB enforced at startup |
| Storage | microSD, 1-bit SD_MMC mode |

Pin assignments are fixed in [`camera_pins.h`](camera_pins.h) and are not user-configurable:

| Function | GPIO |
|---|---|
| SD card CLK / CMD / D0 | 7 / 9 / 8 |
| PDM microphone data / clock | 41 / 42 (`I2S_SCK` = -1, i.e. PDM mode) |
| Onboard user LED (used as the "lamp") | 21 |
| Camera | per the XIAO Sense reference layout |

**4-bit SD mode is not available.** The XIAO Sense expansion board only wires `D0`, so the roughly 2× write speedup that upstream documents for 4-line SD_MMC on ESP32S3 cannot be achieved here without hardware modification.

## Features

Enabled by default:

* **Motion detection by camera** — see [Motion detection](#motion-detection)
* **Continuous recording** — time-lapse or dashcam style
* **Audio recording** from the onboard PDM microphone, muxed into the AVI as WAV
* **OV5640 autofocus**
* Live MJPEG streaming to browser, and still capture
* Playback of recordings in the browser
* SD card management, including automatic deletion of oldest recordings when space runs low
* [Firmware updates over the air](#firmware-updates) from GitHub Releases

Optional, off by default — set the corresponding `#define INCLUDE_*` to `true` in [`appGlobals.h`](appGlobals.h):

| Flag | Feature |
|---|---|
| `INCLUDE_FTP_HFS` | Upload recordings to an FTP or HTTPS file server |
| `INCLUDE_SMTP` | Email alerts |
| `INCLUDE_TGRAM` | [Telegram bot](#telegram-bot) alerts |
| `INCLUDE_MQTT` / `INCLUDE_HASIO` | [MQTT](#mqtt) control and Home Assistant discovery |
| `INCLUDE_RTSP` | [RTSP streaming](#streaming-to-an-nvr) (requires the ESP32-RTSPServer library) |
| `INCLUDE_WEBDAV` | [WebDAV](#webdav) access to the SD card |
| `INCLUDE_EXTHB` | [External heartbeat](#external-heartbeat) |
| `INCLUDE_CERTS` | [HTTPS](#https) and remote certificate checking |
| `INCLUDE_NEW_JPG` | Faster JPEG codec, uses more memory |

## Building

Requires the **arduino-esp32 core v3.1.1 or later**, and the [`OV5640_Auto_Focus_for_ESP32_Camera`](https://github.com/0015/ESP32-OV5640-AF) library (autofocus is on by default, so the build fails clearly without it).

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi --warnings all .
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi --port COM3 .
```

> **`PSRAM=opi` is not optional.** The board's PSRAM menu defaults to *Disabled* in `boards.txt`, so a bare `--fqbn esp32:esp32:XIAO_ESP32S3` compiles without `-DBOARD_HAS_PSRAM` and produces a binary that flashes and boots but then halts with `Startup Failure: Need PSRAM to be enabled`. Verify with `arduino-cli compile --fqbn ... --show-properties | grep build.defines`, and confirm on the device — a correct build logs `PSRAM 8.0MB, mode OPI @ 80Mhz` at boot.

In the Arduino IDE, select **XIAO_ESP32S3** and set **PSRAM: OPI PSRAM**. The remaining defaults are already correct for this board: 8 MB flash, QIO 80 MHz, *Default with spiffs (3MB APP/1.5MB SPIFFS)*, USB hardware CDC with CDC-on-boot enabled, 240 MHz.

`arduino-cli board list` reports this board as the generic `esp32:esp32:esp32_family`, because every ESP32-S3 with native USB shares the same USB IDs (`303A:1001`). That is expected — always pass the FQBN explicitly rather than relying on detection.

No board selection is needed in code — there is no `CAMERA_MODEL_*` choice to make.

## First run

On first boot the device starts a WiFi access point named **ESP-CAM_MJPEG_...**. Connect to it and open `192.168.4.1` to select your router and enter its password.

The configuration file is created automatically, and the web interface files are downloaded to the SD card `/data` folder from this repository's `main` branch once the device has internet access. The `/data` folder can be reloaded later with the **Reload /data** button on the **Edit Config** tab, or over WebDAV.

Browser functionality is fully tested only on Chrome.

## Firmware updates

Two routes, both requiring authentication:

**Over the air from GitHub Releases.** Under the **Access Settings** sidebar there is a **Firmware update** section with a **Check for Updates** button. If a newer release exists, **Install Update & Restart** becomes available; it downloads the release asset, writes it to the OTA partition, and reboots.

For this to work, a release in [this repository](https://github.com/theostrichjouster-star/ESPIPCAM/releases) must:

* be tagged with a version that parses higher than the running `APP_VER` — a leading `v` is optional, e.g. `v1.0.1`
* carry an asset named **exactly** `ESPIPCAM.bin`

The device rejects an image smaller than 64 KB before touching the OTA partition, and aborts cleanly on a partial download, so a failed update cannot damage the running firmware. If the repository has no releases yet, the check reports that rather than erroring.

**Manual upload.** The **OTA Upload** tab still accepts a `.bin` built locally, as upstream does.

> Firmware update over the air has been verified by compilation and UI testing only. It has not yet been exercised against a real published release.

## Security

Every web endpoint requires HTTP Basic authentication once credentials are set — `/control`, `/update`, `/upload`, `/status`, `/web`, the WebDAV tree and the websocket included. **Set a username and password under Access Settings on first run**; leaving them blank leaves the device open, which is only appropriate on a trusted network during provisioning.

Also hardened: query-string and path lengths are bounds-checked, path traversal is rejected, credential comparison is constant-time, and Telegram and heartbeat tokens are masked in the status JSON and saved configuration.

Known residual risks, accepted rather than fixed:

* Firmware images are **not cryptographically signed**. Anyone who can authenticate can flash arbitrary firmware.
* HTTP is the default; HTTPS requires `INCLUDE_CERTS` and is memory-hungry.
* CORS headers are permissive.
* FTPS is stubbed, not implemented, and MQTT has no TLS.

If the device is reachable from the internet, see [Port forwarding](#port-forwarding) and set credentials first.

## Configuration

Most behaviour is changed from the main web page, which is largely self-explanatory. Settings persist only after pressing **Save**; network and peripheral changes need a reboot via **Reboot ESP**.

* **Access Settings** — WiFi, hostname and mDNS, time zone, FTP/HTTPS, SMTP, authentication, HTTPS toggles, and firmware update
* **Motion Detect & Recording** — motion sensitivity, time lapse, dashcam length, minimum frames
* **Edit Config → Motion / Streaming / Other** — detection tuning, stream enables, SD management, MQTT, Telegram, heartbeat
* Recording parameters: `Resolution`, `Frame Rate`, `Quality`

For time zone, use the dropdown or paste a value from the second column of [this list](https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv).

There is **no pin configuration UI** — it was removed along with multi-board support.

Logs are viewable under the **Show Log** tab, held in RTC RAM (7 KB cyclic, default), streamed over websocket, or written to SD. SD logging can slow recording.

## How recording works

Frames are buffered in PSRAM and written to SD in sector-aligned chunks to minimise write count. Recordings are named `YYYYMMDD_HHMMSS` plus frame size, frame rate and duration — e.g. `20200130_201015_VGA_15_60.avi` — and stored in a per-day `YYYYMMDD` folder. Suffixes mark the type: `_S` audio, `_M` telemetry, `_T` time lapse, `_C` continuous.

Saving a set of JPEGs as one AVI is faster than writing individual files and replays at the correct frame rate in ordinary media players. Throughput depends heavily on SD card quality — a genuine name-brand card can be several times faster than a no-name card with the same class marking.

> Frame-rate benchmarks for this board are not yet published. Upstream's figures were measured on an OV2640 AI Thinker board and do not transfer to the OV5640 on XIAO Sense; real numbers will be added once the tuning work is done.

## Motion detection

Recording can be triggered by the camera itself detecting movement, or manually with **Start Recording**.

JPEGs are sampled 1-in-N and decoded to small grayscale or RGB bitmaps, which are compared against the previous sample. The small size smooths out artefacts and keeps the comparison cheap. Detection runs at 1-in-2 while looking for movement, dropping to 1-in-10 once recording has started to keep capture overhead low.

<img align="right" src="extras/motion.png" width="200" height="200">

* `Motion Sensitivity` — higher is more sensitive
* `Show Motion` — with **Start Stream**, displays how movement is being detected, for calibration. Changed pixels show red.
* `Min Frames` — recordings shorter than this are discarded

Motion detection is enabled by default and can be switched off under **Motion Detect & Recording**. It is unavailable above SXGA due to JPEG decoder limits.

## Audio

The onboard PDM microphone is enabled by default and its pins are applied automatically. Audio is 16-bit mono PCM at 16 kHz, stored as WAV inside the AVI.

**Microphone Gain** on the web page controls level, defaulting to 5. A value of 3 is unity gain; higher amplifies, lower attenuates. Setting it to **0 turns audio off** - recording, the NVR stream and RTSP audio are all gated on it. The speaker icon streams live microphone audio to the browser.

The intercom feature — two-way audio between the device and a browser — additionally requires an I2S amplifier, which needs a free pin and therefore a source change on this build. Browser microphone access has security constraints; see [`audio.cpp`](audio.cpp).

## OV5640

For recording, frame sizes above `FHD` should be used for stills only, due to memory limits. Recordable frame rates at the highest sizes:

| Frame size | FPS |
|---|---|
| QXSGA | 4 |
| WQXGA | 5 |
| QXGA | 5 |
| QHD | 6 |
| FHD | 6 |
| P_FHD | 6 |

Note that the OV5640's pinout matches OV2640-designed boards but its internal 1.5 V regulator runs hot; a heat sink helps in sustained use.

## HTTPS

Set `INCLUDE_CERTS` to `true`, then toggle **Use HTTPS** under Access Settings. See [`certificates.cpp`](certificates.cpp) for generating and installing certificates, and for importing the server certificate into the browser to avoid trust warnings.

If HTTPS is enabled with incorrect certificates the web page becomes unreachable, and the certificate files must be deleted from the SD card manually.

**Check Certs** separately enables verification of remote server certificates, protecting outbound connections against man-in-the-middle attacks.

## MQTT

Under **Edit Config → Other**, set the broker IP, topic prefix, optional user and password, then enable. It connects automatically on ping success.

Status is published to `homeassistant/sensor/{hostname}/state`, e.g. `{"MOTION":"ON", "TIME":"10:07:47.560"}`. Commands can be published to the `/cmd` channel, e.g. `dbgVerbose=1;framesize=7;fps=1`.

With `INCLUDE_HASIO`, discovery messages create a Home Assistant [MQTT Camera](https://www.home-assistant.io/integrations/camera.mqtt/) device automatically and publish an image on motion. Contributed upstream by [@gemi254](https://github.com/gemi254).

<a href="extras/hasio_device.png"><img src="extras/hasio_device.png" width="500" height="350"></a>

## Telegram bot

Enable either Telegram or SMTP, not both. Get your chat ID from [IDBot](https://t.me/myidbot) and a bot token from [BotFather](https://t.me/botfather), then enter both under **Edit Config → Other**. The bot receives motion alerts with a frame from the recording and a command link to download it (max 50 MB). This feature uses a lot of heap because of TLS.

<img src="extras/telegram.png" width="500" height="500">

## External heartbeat

Contributed upstream by [@alojzjakob](https://github.com/alojzjakob); see also [EspSee](https://github.com/alojzjakob/EspSee).

Lets multiple cameras behind one dynamic IP be reached without DDNS, by POSTing a JSON heartbeat every 30 seconds to a server you control. Configure the receiver domain, URI, port and optional auth token under **Edit Config → Other**.

## Streaming to an NVR

HTTP or RTSP, but not both at once. Streaming performance depends on network quality, and improves with motion detection switched off, since a recording takes priority and can make streams stutter.

**RTSP** requires the [ESP32-RTSPServer](https://github.com/rjsachse/ESP32-RTSPServer) library, version 1.3.1 or above, and `INCLUDE_RTSP` set to `true`. Enable video, audio and subtitle streams under **Edit Config → Streaming**, then save and reboot. Connect to `rtsp://<camera_ip>:<RTSPport>`, or with credentials `rtsp://<user>:<pass>@<camera_ip>:<RTSPport>`.

**HTTP** streaming is available when `INCLUDE_RTSP` is `false`, exposing `/sustain?video=1`, `/sustain?audio=1` and `/sustain?srt=1`. Multiple streams need an intermediate tool such as [go2rtc](https://github.com/AlexxIT/go2rtc) to synchronise them.

## WebDAV

Set `INCLUDE_WEBDAV` to `true` and browse the SD card at `<ip_address>/webdav` from a WebDAV client such as Windows File Explorer. See [`webDav.cpp`](webDav.cpp) for other platforms.

<img src="extras/webdav.png" width="600" height="300">

## Camera hub

Enable `Show Camera Hub tab` under **Edit Config → Other** to monitor other cameras running this firmware. Add each by IP; click an image to open that camera's page. Addresses are stored in browser local storage, not on the device.

## Port forwarding

To reach the camera over the internet, forward a port on your router to the device's HTTP port and use `your_router_external_ip:port`.

![Port forwarding](extras/portForward.png)

Set a static IP for the device, and **set authentication credentials first**. Note that ISPs using [CGNAT](https://en.wikipedia.org/wiki/Carrier-grade_NAT) may make port forwarding impossible.

## Inherited features that need source changes

Removing the GPIO configuration UI left several upstream features present in the source but **not configurable at runtime**. Their pin variables default to `0` or `-1` and there is no longer any way to set them from the web interface, so they will not function unless you assign pins in code and rebuild.

| Feature | Flag | Why it needs a source change |
|---|---|---|
| Peripherals — PIR, buzzer, relay, servos, battery voltage, DS18B20, wake pin | `INCLUDE_PERIPH`, `INCLUDE_DS18B20` | All pins unassigned |
| Auxiliary board over UART | `INCLUDE_UART` | `uartTxdPin` / `uartRxdPin` unassigned |
| I2C devices — BMP280, MPU6050, SSD1306, etc. | `INCLUDE_I2C` | `I2Csda` / `I2Cscl` unassigned |
| Telemetry recording | `INCLUDE_TELEM` | Depends on I2C |
| Remote control of an RC vehicle | `INCLUDE_PERIPH`, `INCLUDE_MCPWM` | Depends on peripheral pins |
| Photogrammetry turntable | `INCLUDE_PGRAM` | Depends on peripheral pins |

Machine learning (`INCLUDE_TINYML`) is a separate case: the classifier function in [`motionDetect.cpp`](motionDetect.cpp) contains a syntax error and has never compiled. It is preserved as inherited from upstream, but enabling the flag will not build.

The auxiliary-board *companion* mode — running this firmware on a second, cameraless ESP32 — was removed entirely and is not recoverable by configuration.

For documentation of these features as they work on upstream hardware, see the [upstream README](https://github.com/s60sc/ESP32-CAM_MJPEG2SD#readme).

## Changes from upstream

Forked at upstream **v10.9.4**. Versioning was restarted at `1.0.0` for this project.

| Change | Detail |
|---|---|
| Board lock | Reduced from ~20 supported boards to XIAO ESP32S3 Sense only. Removed the auxiliary/side-alarm companion build modes |
| Security hardening | Authentication enforced on every endpoint, buffer bounds checks, path-traversal rejection, constant-time credential compare, token masking, OTA size sanity check |
| GPIO UI removal | All pin configuration fields removed from the web interface |
| Web UI | New colour palette, SVG icon set replacing emoji, mobile breakpoint with 44 px touch targets |
| Dead code removal | Removed unreachable macro branches, orphaned declarations, and Ethernet support entirely — around 1,800 lines, reducing flash use by roughly 86 KB |
| Autofocus and audio | Both enabled by default for this hardware |
| OTA | Added update checking and installation from GitHub Releases |

Full detail is in the commit history.

## Credits

* **[s60sc](https://github.com/s60sc)** — original author of ESP32-CAM_MJPEG2SD, and of essentially all functionality in this firmware
* [@gemi254](https://github.com/gemi254) — Home Assistant MQTT integration, and the original setup assistant
* [@alojzjakob](https://github.com/alojzjakob) — external heartbeat
* [@rjsachse](https://github.com/rjsachse) — shared I2C design, and the ESP32-RTSPServer library
* [@josef2600](https://github.com/josef2600) — SD_MMC 4-line mode investigation
* [@RedCanti](https://github.com/RedCanti) — Ethernet support (since removed from this build)
* [@ldijkman](https://github.com/ldijkman) — installation walkthrough
* **Eric Nam** ([@0015](https://github.com/0015)) — the [OV5640 Auto Focus for ESP32 Camera](https://github.com/0015/ESP32-OV5640-AF) library, which this build depends on

## Licence

GNU Affero General Public License v3.0, inherited from upstream. See [LICENSE](LICENSE) for the full text.

Because this firmware acts as a network server, AGPL §13 applies: if you run a modified version and let others use it over a network, you must make your modified source available to them. The source for this version is at [github.com/theostrichjouster-star/ESPIPCAM](https://github.com/theostrichjouster-star/ESPIPCAM).
