#!/bin/bash
# Gate a custom-built arduino-esp32 core against the stock one.
#
# The premise of the custom core is that it reproduces the stock 3.1.1 build with
# exactly ONE value changed. This checks that premise. It is what licenses putting a
# custom-core image on a board: if options outside the intended delta moved, then
# lib-builder has drifted from what shipped and assumptions about bootloader and
# partition compatibility - which the OTA path depends on - are void.
#
# Usage: check-sdkconfig-drift.sh <new-sdkconfig.h> [stock-reference.h]

set -u
NEW="${1:?usage: check-sdkconfig-drift.sh <new-sdkconfig.h> [stock-reference.h]}"
STOCK="${2:-$(dirname "$0")/stock-3.1.1-qio_opi-sdkconfig.h}"

[ -r "$NEW" ]   || { echo "cannot read new sdkconfig: $NEW"; exit 2; }
[ -r "$STOCK" ] || { echo "cannot read stock reference: $STOCK"; exit 2; }

# The delta we asked for. Anything else is drift.
INTENDED='CONFIG_LWIP_TCP_SND_BUF_DEFAULT'

# Options where a silent change would invalidate flashing a custom-core app onto a
# board whose bootloader came from the stock core, or would change memory layout.
# CAMERA/SCCB are here because this is a camera firmware: every measured law in
# BOARD_TESTING.md (HTS/VTS timing, applySensorTuning, zone detection register reads)
# sits on the camera driver and its SCCB backend, so a driver swap invalidates the
# whole measured baseline. Found the hard way - esp32-camera is pulled from git
# "master" and silently resolved 21 months forward, switching the SCCB I2C driver
# and the camera task stack until it was pinned to a contemporary commit
CRITICAL='CONFIG_BOOTLOADER_|CONFIG_PARTITION_TABLE_|CONFIG_SPIRAM|CONFIG_FREERTOS_|CONFIG_ESP_SYSTEM_|CONFIG_ESPTOOLPY_|CONFIG_IDF_TARGET|CONFIG_CAMERA_|CONFIG_SCCB_'

DIFF=$(diff <(sort "$STOCK") <(sort "$NEW") | grep -E '^[<>]' | sed 's/^[<>] //' | sort -u)

if [ -z "$DIFF" ]; then
  echo "NO DIFFERENCES AT ALL - the overlay did not take effect. Check that the"
  echo "defconfig override was applied and that you are reading the right memory variant."
  exit 1
fi

echo "=== intended delta ==="
grep -E "$INTENDED" <<< "$DIFF" || echo "  (ABSENT - the override did NOT take effect)"

echo
echo "=== critical drift (blocks flashing if present) ==="
CRIT=$(grep -E "$CRITICAL" <<< "$DIFF" || true)
if [ -n "$CRIT" ]; then echo "$CRIT"; else echo "  none"; fi

echo
echo "=== other drift (review, usually benign) ==="
OTHER=$(grep -vE "$INTENDED|$CRITICAL" <<< "$DIFF" || true)
if [ -n "$OTHER" ]; then echo "$OTHER"; else echo "  none"; fi

echo
if ! grep -qE "$INTENDED" <<< "$DIFF"; then
  echo "VERDICT: FAIL - intended change absent"; exit 1
elif [ -n "$CRIT" ]; then
  echo "VERDICT: FAIL - critical options drifted; do not flash until understood"; exit 1
else
  echo "VERDICT: PASS - only the intended change plus benign drift"; exit 0
fi
