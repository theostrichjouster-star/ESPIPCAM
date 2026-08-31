# Battery operation

Operational guide for running a board on a single li-ion cell. The measurements
behind every number here are in BOARD_TESTING.md section 21 (discharge ladder,
landing timing, calibration data - a local bench notebook, deliberately not in this
repo); this file is what you need to deploy.

## Hardware

- Single li-ion cell on the XIAO BAT pads. The onboard charger tops the cell up
  whenever USB is present - USB-side current readings therefore include a charge
  offset, so deltas are the signal, not absolutes.
- 100k/100k divider from BAT+ to GND, tap on GPIO1 (D0, ADC1 ch0). Both boards carry
  one. The divider halves the cell voltage into ADC range; battScale converts the
  tap reading back to cell mV.

## Config keys

| Key | Fleet value | Meaning |
|---|---|---|
| battUse | 1 | master enable for the monitor |
| battPin | 1 | divider tap pin (GPIO number) |
| battScale | per-board | cell mV per 1000 tap mV (2000 = ideal 2:1 divider) |
| battWarnMv | 3000 | land recordings and park below this cell voltage |

Set via `/control?battScale=2062` etc., then `/control?save=1` - control alone is
RAM-only. The web UI footer shows the live reading (battV; "n/a" when the monitor is
off or no plausible cell is seen).

## Calibrate every divider individually

Nominally identical 100k pairs are not: COM4 calibrated to battScale=2062 (+3.1%
from nominal), COM3 to 2158 (+7.9%). Copying one board's battScale to another
silently shifts every threshold by resistor tolerance.

Procedure (five minutes, needs a multimeter):

1. Cell connected, board idle. Read the cell with the meter at the BAT pads.
2. Read battV from the footer or /status.
3. newScale = oldScale x (meter mV / battV mV). Set it, save it.
4. Re-read. Both boards verified within 5mV of the meter this way.

## What the monitor does

battMonitor() samples at 1Hz whether or not a recording is open. Three consecutive
samples below battWarnMv are required before acting - the cell sags under camera
load bursts, and one dip must not park the device. Readings below 2500mV are treated
as "no divider fitted" (warned once, never acted on). A solid 0mV reading is the
signature of an open divider top feed: the tap is pulled to GND through the bottom
resistor. That signature diagnosed a broken BAT+ joint remotely before the iron
came out.

Battery-side monitoring is the only warning mechanism that can exist here. The LDO
holds the 3.3V rail until the pack is nearly dead, and once the rail itself sags the
response window is milliseconds: the rail-side brownout comparator cannot warn, it
can only bury you.

## The landing

When the threshold confirms, sagShutdown() runs in task context with the recording
still open: new recordings blocked (including the record button), sensor to software
standby (the 5MP array plus its PSRAM traffic is the dominant draw), SD log synced,
an RTC marker set for the next boot to report. Measured landing: 52-63ms from trip
to closed file; a 50-cycle false-trip soak ran clean. The board then sits parked -
webserver alive, footer readable - until a power cycle onto a charged pack.

## Why 3000mV - the measured ladder

On 31 Aug 2026 the landing was deliberately disarmed and a fully charged board
recorded q10 HD 30fps continuously to destruction:

| Cell | What happened |
|---|---|
| 2900mV | clips still flawless, exact 30fps |
| ~2650mV | last complete clip closed clean |
| 2643mV | wifi died - the true operational floor |
| below | pack protection cutoff: ten brownout death-spiral boots, then silence |
| 2440mV | the chip's own BOD - never cleanly reached, pack bounce preempts it |

battWarnMv=3000 parks 357mV above the wifi floor, with li-ion cycle health as the
second reason. There is no capacity worth chasing below it - the usable range of a
li-ion cell lives above 3.0V anyway.

## The death spiral costs more than the last clip

The ten collapse boots orphaned the FAT directory of the entire day folder: 8.6GB
allocated, listing empty, every clip of the run a lost cluster chain. Recovery
worked - card in a PC, `chkdsk <drive>: /f`, which restored the directory and swept
strays into FOUND.000\*.CHK (rename .CHK to .avi to play); NEVER format - but the
lesson is the point: an armed landing protects the whole filesystem, not just the
recording in flight. Production posture never gets near spiral territory.

## Endurance notes

- ~2h of continuous q10 HD30 recording wrote only ~6GB - card economy is set by
  quality, not fps.
- Measured 5V-side draw (charge offset included, read the deltas): 225mA idle with
  the 5fps sensor idle throttle (360mA unthrottled), ~330mA motion recording, ~420mA
  streaming.
- Modem sleep is worth 70mA and allowAP=1 forfeits it (AP+STA blocks modem sleep) -
  wifiSleep=1 and allowAP=0 are fleet standard.
