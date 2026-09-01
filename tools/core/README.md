# Building a custom arduino-esp32 core

This directory is the capability to change ESP-IDF settings that are normally
frozen inside the prebuilt Arduino core. It exists because the MJPEG stream was
capped by a compile-time lwip constant, but the machinery is general: anything in
`sdkconfig` is now reachable.

| File | Purpose |
|---|---|
| `lwip-sndbuf.defconfig` | The config overlay plus full reasoning. The specification. |
| `check-sdkconfig-drift.sh` | The gate. Run before flashing anything. |
| `stock-3.1.1-qio_opi-sdkconfig.h` | Stock reference for the gate to diff against. |

## Why a rebuild is the only route

Three cheaper approaches were investigated and all three are dead ends. Do not
re-litigate them:

1. **`setsockopt(SO_SNDBUF)` cannot work.** This core's `lwip/sockets.h` reads
   verbatim `#define SO_SNDBUF 0x1001 /* Unimplemented: send buffer size */`.
   No `LWIP_SO_SNDBUF` macro exists anywhere in the tree. Only `SO_RCVBUF` has
   real get/set handling.
2. **A per-project `sdkconfig.defaults` does nothing.** `platform.txt` only
   *copies* the sdkconfig into the build directory for information; every IDF
   component links as a prebuilt `.a` (see `flags/ld_libs`). Defining a CONFIG
   macro in `build_opt.h` changes only your own translation units while
   `liblwip.a` keeps its baked-in value - an ABI mismatch, worse than nothing.
3. **PlatformIO does not help.** It consumes the same prebuilt archives.

## Build procedure

Environment: WSL2 (Ubuntu). `wsl -u root` works without a password for package
installs. **usbipd is NOT needed** - WSL only compiles; all flashing and serial
work stays on Windows.

    apt-get install -y git wget curl flex bison gperf python3 python3-pip \
      python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
      libusb-1.0-0 jq

    git clone https://github.com/espressif/esp32-arduino-lib-builder.git ~/lib-builder
    cd ~/lib-builder && git checkout 2f061cb     # commit that built stock 3.1.1
    ./build.sh -t esp32s3 -i cfea4f7c98          # installs IDF, clones components

**The first run will fail.** That is expected - see the pins below. After fixing
them, rebuild with `-s` so the updater cannot undo them:

    cd ~/lib-builder && source esp-idf/export.sh && ./build.sh -s -t esp32s3

Ubuntu 26.04 with Python 3.14 was fine for ESP-IDF v5.3, despite being far newer
than that IDF was tested against.

### The four pins (the hard-won part)

lib-builder checked out at the commit recorded in the stock core's `versions.txt`
*still* resolves its sibling repos forward - roughly 21 months, as of Sep 2026.
Each must be pinned by hand to the value in that same `versions.txt`:

| What | Pin to | Symptom if unpinned |
|---|---|---|
| `components/arduino` | `9eb7dc6f` (git checkout) | `No SOURCES given to target: __idf_arduino_tinyusb` |
| `components/arduino_tinyusb/tinyusb` | `249556360` (git checkout) | same as above |
| `esp_matter` in `main/idf_component.yml` | `"==1.3.0"` (was `"^1.3.0"`) | Matter API compile errors - the caret resolves forward to a breaking release |
| `esp32-camera` in `main/idf_component.yml` | `4335c93e` (was git `"master"`) | **silently** swaps the SCCB I2C driver and camera task stack |

The fourth is the dangerous one: it produces a *working build* that quietly
changes the camera driver underneath every measured law in BOARD_TESTING.md. It
was caught only because the gate flagged it. ESP-IDF itself pins correctly via
`build.sh -i <commit>`.

Provenance to match lives in the installed core at
`.../esp32-arduino-libs/idf-release_v5.3-cfea4f7c-v1/versions.txt`.

## The gate - run before flashing

    bash tools/core/check-sdkconfig-drift.sh <tree>/qio_opi/include/sdkconfig.h

It splits differences into the intended delta, **critical drift** (bootloader,
partition table, esptool, SPIRAM, FreeRTOS, esp_system, and - for this project -
CAMERA and SCCB), and benign noise. Non-zero exit if the intended change is
missing or anything critical moved.

A correct build differs from stock ONLY in the intended value, plus build
metadata strings (`ARDUINO_IDF_BRANCH`/`COMMIT`) and unrelated mdns/libsodium
options. **A PASS is what licenses OTA-flashing a custom-core app onto a board
whose bootloader came from the stock core**, by proving the bootloader and
partition assumptions still hold. Without a PASS, use serial (which writes
bootloader, partition table and app together) or do not flash at all.

## Using a custom core - no install, no risk to the stock one

Unpack the produced `esp32s3` tree somewhere short and space-free, then select it
per-invocation. `Arduino15` is never touched; rollback is dropping the flags.

    arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" \
      --build-property "tools.esp32-arduino-libs.path=C:\esp32libs\lwip65535" \
      --build-property "runtime.tools.esp32-arduino-libs.path=C:\esp32libs\lwip65535" \
      --build-path "$(pwd)/build-65535" .

**Verify the override took** before trusting any measurement: `platform.txt`
copies the selected tree's sdkconfig into the build directory, so
`grep CONFIG_LWIP_TCP_SND_BUF_DEFAULT <build-path>/sdkconfig` is free proof. At
runtime `/status` carries `lwipSndBuf`, `lwipWnd`, `lwipMss` and `idfVer`, so a
running board can always say which core it was built against.

Current trees on this machine: `C:\esp32libs\lwip` (45952) and
`C:\esp32libs\lwip65535` (65535).

## Making a new config change

1. Edit `configs/defconfig.esp32s3` in lib-builder - applied after
   `defconfig.common` and scoped to our chip. Confirm nothing later in the chain
   overrides it: `grep -l <SYMBOL> configs/*`.
2. Record the change and its reasoning in a `.defconfig` file here. That file is
   the specification and is what makes the core reproducible after an upgrade.
3. Rebuild with `-s`, run the gate, install to a NEW directory (keep the old tree
   for A/B), build, measure.
4. Extend the gate's critical list if the change touches a new subsystem.

## What has been changed, and what it bought

`CONFIG_LWIP_TCP_SND_BUF_DEFAULT` 5760 -> 65535. Measured on hardware with the
other board's radio silenced (full campaign in BOARD_TESTING.md):

- Stock: 25 fps, ~470 KB/s, ~100 frames skipped per 20s.
- 65535: **30.0 fps, 0 skipped** - the transport delivers every frame the sensor
  offers, so TCP is no longer the limit; the camera is.
- Transport ceiling roughly doubles, ~470-590 KB/s to ~1.15 MB/s. Both 45952 and
  65535 reach that same ceiling, which is the *radio*, not TCP.
- Cost: ~38 KB more internal RAM at peak (min-ever 87-89 KB vs 126 KB stock),
  still 2.7x above the 32 KB warning threshold. pbufs prefer PSRAM on this core,
  so the bulk lands there.

`CONFIG_LWIP_TCP_WND_DEFAULT` was deliberately NOT changed: it is the *receive*
window and does nothing for outbound streaming.

## Candidate future changes

Espressif publishes an "iperf rank" configuration for maximum throughput:
<https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/wifi-driver/wifi-performance-and-power-save.html>

Our core against it, checked 1 Sep 2026. **We have done only the TCP half.**

| Setting | Espressif iperf | Ours |
|---|---|---|
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 65 KB | 65535 (done) |
| `CONFIG_ESP_WIFI_TX_BUFFER_TYPE` | dynamic | 0 (static) |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 64 | n/a - static, 8 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | 16 | 8 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 64 | 32 |
| `CONFIG_ESP_WIFI_RX_BA_WIN` | 32 | 16 |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 65 KB | 5760 |
| `CONFIG_ESP_WIFI_IRAM_OPT` | on (+15 KB IRAM) | off |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | on (+16 KB IRAM) | off |
| `CONFIG_LWIP_IRAM_OPTIMIZATION` | on (+13 KB IRAM) | off |

### MEASURED 1 Sep evening - the buffer changes did NOT help

Items 1-3 below were built and tested on COM4 (COM3 silenced). Result: **no
improvement, and the top hypothesis is refuted.**

- **Dynamic TX buffers are IMPOSSIBLE on this board.** IDF Kconfig:
  `config ESP_WIFI_DYNAMIC_TX_BUFFER ... depends on !SPIRAM_USE_MALLOC`, and the
  camera requires PSRAM malloc. The help text says outright "If PSRAM is enabled,
  Static should be selected to guarantee enough WiFi TX buffers". **Espressif's
  iperf rank assumes a board without PSRAM, so it does not apply to us.**
- Raising STATIC TX buffers instead (8 -> 32) did nothing: marginal transport rate
  went 1.55 -> 1.41 MB/s, i.e. no gain and possibly a slight loss (frame size
  drifted 58 -> 69KB between runs, so this is model-normalized).
- RX 16/64 + `RX_BA_WIN` 32 + `TCP_WND` 65535 + `RECVMBOX` 32: inbound measured
  68-112 KB/s against ~118 KB/s historically. Run-to-run variance exceeds any
  effect. No clear gain.
- Memory: internal `int_min` UNCHANGED at ~87K, but `psram_min` fell ~87KB - so
  with `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` the extra buffers land in PSRAM, where
  they contend with camera frame buffers. That is a plausible mechanism for the
  slight regression.

### DANGEROUS - do NOT set SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n with a large send buffer

Tested 1 Sep evening because the PSRAM-contention theory above predicted a win:
put wifi/lwip buffers in fast internal RAM instead of PSRAM. **It nearly killed
the board.**

With `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n` and `SND_BUF=65535` (buffer counts
left at stock), internal free RAM under streaming load fell to
**`int_min` = 1100 bytes** - against a 32 KB warning threshold - and the largest
free block to 31 KB. Streaming went erratic: truncated connections (3.8s and
14.8s instead of 20s), runs returning 0 frames, and throughput scattered between
234 KB/s and 1.0 MB/s, all WORSE than leaving the buffers in PSRAM.

The mechanism is simple in hindsight: a 64 KB TCP send buffer means up to 64 KB
of pbufs, and with this setting off they must come from internal RAM, which this
board does not have spare. **The two settings are coupled - a large send buffer
REQUIRES PSRAM allocation on a board like this.** That is exactly why IDF
defaults it to `y` when PSRAM is enabled, and why the stock core ships it that
way.

The board never actually failed an allocation, but at 1100 bytes free the next
camera frame buffer or SD write could have. It was reverted immediately. This is
the failure the `int_min` instrumentation exists to catch, and it caught it -
instantaneous polling would have shown a comfortable `int_free` of 123 KB and
missed the squeeze entirely.

**Retested with a SMALLER send buffer to see if pbuf demand was the cause - it is
not.** `TRY_ALLOCATE=n` with `SND_BUF=45952` (down from 65535) gave
`int_min` = **908 bytes**, if anything slightly worse. Dropping ~19 KB of
worst-case pbuf demand changed nothing, which means the internal-RAM exhaustion
is NOT driven by the TCP send buffer at all - it is the wifi driver and lwip core
allocations themselves, which on this board simply have to live in PSRAM.
Throughput was also no better (model-normalized 1.42 MB/s vs 1.55 baseline).

**So `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n` is not viable here at ANY send buffer
size.** Do not retry it by tuning other values downward.

Also confirmed in the same build: `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` is inert while
`SPIRAM_USE_MALLOC=y`. Setting it produced `TX_BUFFER_TYPE=0` and
`STATIC_TX_BUFFER_NUM=8` unchanged - the dependency is on `SPIRAM_USE_MALLOC`,
which the camera requires, and is unaffected by the `TRY_ALLOCATE` setting.

**Conclusion: the ~1.3-1.5 MB/s ceiling is NOT driver buffering.** The earlier
"radio ceiling" attribution in BOARD_TESTING section 23 is vindicated. The
remaining lever is the RF environment (channel, placement, interference), not
core config. Item 4 (IRAM) was NOT tested: those optimizations speed CPU-bound
packet processing, and the skipped-frame counter shows the sender blocking on
transport rather than CPU, so it is unlikely to help.

The tree built for this test is `C:\esp32libs\lwipbufs`. It is retained for
reference only - **the recommended core remains the plain 65535 one**, which has
a proven 2x benefit and a clean soak behind it.

Original ranking, kept for context:

1. **TX buffers - the top candidate.** Espressif states the pairing rule
   explicitly: `LWIP_TCP_SND_BUF_DEFAULT` "should be configured to the value of
   `WIFI_DYNAMIC_TX_BUFFER_NUM` (KB)". We raised the TCP send buffer to 64 KB but
   left the driver on STATIC TX buffers with 8 of them - roughly 13 KB of
   driver-side buffering behind a 64 KB window. **This may well be the real
   ~1.15 MB/s ceiling that BOARD_TESTING.md section 23 attributes to ambient RF**:
   a fixed driver buffer pool produces the identical signature (same ceiling at
   both 45952 and 65535), so the existing evidence does not distinguish them.
   Set `TX_BUFFER_TYPE` dynamic + `DYNAMIC_TX_BUFFER_NUM=64` and re-measure; the
   ceiling either moves (buffers) or does not (RF). Watch memory: 64 dynamic
   buffers is ~100 KB, though `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` lets them
   prefer PSRAM.
2. **RX buffers** (16 / 64, `RX_BA_WIN` 32): inbound path, so it helps OTA upload
   speed and link stability rather than streaming out.
3. **`CONFIG_LWIP_TCP_WND_DEFAULT`** 5760 -> 65535, paired with
   `CONFIG_LWIP_TCP_RECVMBOX_SIZE` >= 32 (must be raised together). Inbound only:
   measure against OTA `.bin` upload time, not fps.
4. **IRAM optimizations**, 44 KB of internal RAM in total. Measure last, and watch
   `int_min` in `/status` against the 32 KB warning threshold.
5. `CONFIG_LWIP_WND_SCALE`: the only way past 65535 in either direction. Only
   worth it if the ceiling ever rises above what a 64 KB window sustains.

For scale: Espressif's iperf rank reaches ~74 Mbit/s TCP TX on a bare board. We
measure ~9 Mbit/s while also running a camera, JPEG encode and SD writes - so the
gap is not all recoverable, but it is large enough to be worth chasing.

Power save: the guide notes modem sleep delays RX by up to the DTIM interval and
recommends `WIFI_PS_NONE` for maximum throughput. We keep modem sleep ON anyway -
it costs 70 mA to disable (see the power ledger) and was measured to make no
difference to throughput DURING an active stream on a healthy link, because the
radio does not sleep while data is flowing. It only inflates idle RTT.

Camera options are reachable too, but treat any `CONFIG_CAMERA_*` or
`CONFIG_SCCB_*` change as invalidating the measured baselines in
BOARD_TESTING.md until re-verified.

## Maintenance

A custom core is a permanent divergence. Every arduino-esp32 upgrade invalidates
the tree and needs a fresh build against the new provenance. The defconfig files
here, the pinned versions above, and the committed stock sdkconfig make that
mechanical rather than a rediscovery. Until a rebuild happens, a routine
`arduino-cli compile` without the `--build-property` flags silently produces a
stock-core image - and `lwipSndBuf` in `/status` is the only thing that will say
so.
