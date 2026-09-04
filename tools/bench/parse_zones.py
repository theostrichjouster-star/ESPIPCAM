"""Parse the avgZones line out of the RTC ring (stdin) and reduce it to the numbers the HTS
floor campaign gates on.

The line looks like:
  AVG zones:  17  42  38  16 |  11  66  49   9 |   3  58 103  29 |   6  23  74  52 |  YAVG 37
  band 32..37 AEC stable, AF 0x10 idle

Why both halves matter. YAVG (0x56A1) is the weighted aggregate the AEC actually servos; the
sixteen zones (0x5691-0x56A0) are the raw grid. When the statistics engine goes blind - the
failure this campaign is looking for - the ZONES keep tracking the true scene while YAVG pins
inside its deadband, and the AEC then drives exposure from the pinned value. So neither number
alone identifies it: the pair does. That is how the 27 Aug full-resolution case was diagnosed.

YAVG is weighted, so it never equals the plain zone mean and no absolute equality test is
valid. The campaign compares both against the same size's own HTS 2060 baseline instead.

Prints: zoneMean zoneMin zoneMax yavg bandLo bandHi stable
"""
import sys, re

text = sys.stdin.read()
# last occurrence wins - the ring holds earlier points from the same walk
hits = re.findall(
    r"AVG zones:(.*?)YAVG\s+(-?\d+)\s+band\s+(\d+)\.\.(\d+)\s*(AEC stable|AEC re-exposing)?",
    text, re.S)
if not hits:
    print("NOZONES")
    sys.exit(2)

body, yavg, lo, hi, stable = hits[-1]
zones = [int(v) for v in re.findall(r"\d+", body)]
if len(zones) < 16:
    print("NOZONES")
    sys.exit(2)
zones = zones[-16:]

mean = sum(zones) / 16.0
print("%.2f %d %d %s %s %s %s" % (mean, min(zones), max(zones), yavg, lo, hi,
                                  "stable" if (stable or "").startswith("AEC stable") else "reexposing"))
