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

None of these are measured. Each needs its own one-variable experiment.

- `CONFIG_LWIP_TCP_WND_DEFAULT` with `CONFIG_LWIP_TCP_RECVMBOX_SIZE` (>= 32,
  must be raised together): speeds **inbound** transfers, i.e. the OTA POST.
- `CONFIG_LWIP_WND_SCALE`: the only way past 65535 in either direction. Only
  worth it if the radio ceiling rises above what a 64 KB window sustains.
- `CONFIG_ESP_WIFI_IRAM_OPT` / `RX_IRAM_OPT` / `CONFIG_LWIP_IRAM_OPTIMIZATION`:
  cost 20-30 KB of internal RAM against a 32 KB warning threshold, and only pay
  off at packet rates this board has not reached.
- `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM` (8) and `TX_BA_WIN` (6): the next
  suspects if the ~1.15 MB/s ceiling proves not to be ambient interference.
- Camera options are reachable too, but treat any `CONFIG_CAMERA_*` or
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
