"""Per-band channel statistics: the gate that catches corruption confined to PART of a frame.

WHY THIS EXISTS. still_color.py averages over the whole frame, and on 7 Sep 2026 that passed a
frame whose top half was a perfect test chart and whose bottom half was blue and yellow banded
garbage. Whole-frame ratio 0.850 and 1.010, hdiff 3.1 and 3.2 - inside every clean threshold the
project uses, because the good half averaged the bad half back into range. It was found by looking
at a contact sheet, which is the third time this project has learned the same lesson: byte counts
passed corrupt frames, then whole-frame channel means passed them, and now whole-frame means pass
them again when the damage is regional.

A readout that degrades as the frame progresses is the expected shape for a clock or data-rate
failure, so the bottom bands go first. Comparing bands against EACH OTHER also removes the scene:
a real picture varies between bands too, but its channel RATIO does not swing by a factor of two,
and its adjacent-pixel difference does not jump tenfold.

Reports per band: mean R/G/B, ratio G/avg(R,B), and mean SATURATION, max(R,G,B) - min(R,G,B).
Then the spread across bands, which is the actual gate:

  ratioSpread   max-min of the band ratios.       A clean frame here sits under ~0.20.
  satRatio      max/min of the band saturations.  A clean frame sits under ~3.

Saturation, not adjacent-pixel difference. hdiff was the obvious second metric and it is WRONG
across bands: it measures detail, and detail is a property of the scene, so a chart band full of
line gratings against a band of blank card differs by 2.5-4x on a perfectly good frame. It flagged
three known-clean stills on its first outing. Saturation works because the corruption these clocks
produce is violently coloured - blue, magenta, yellow - against a chart that is mostly grey, and
because it catches a yellow-green band that ratioSpread misses entirely: in yellow, R and G both
rise, so G / avg(R,B) barely moves while the picture is plainly ruined.

0.20 rather than 0.15 on the spread because a 4:3 size sees more of the room per band: three
known-clean 1280X960 stills measured 0.121, 0.152 and 0.156 where every clean HD still sat at
0.019-0.026. The saturation ratio carries the discrimination those miss - known-clean tops out at
2.42 and the yellow-banded frame reads 3.13.

Both thresholds are starting points calibrated on this bench's chart, not laws. Gate against the
same size's own clean baseline, and LOOK AT THE FRAME - this is a better screen, not a verdict.

Usage: python still_bands.py <file.jpg> [bands]   ->  a line per band, then a SPREAD line
       python still_bands.py <file.jpg> --terse   ->  "ratioSpread hdiffRatio verdict"
Exit 2 if the file does not decode.
"""
import sys
import warnings

warnings.simplefilter("ignore")  # Pillow's getdata notice would land in every campaign log

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: python -m pip install pillow")


def bandStats(im, y0, y1):
    crop = im.crop((0, y0, im.size[0], y1))
    px = list(crop.getdata())
    n = len(px)
    if not n:
        return None
    r = sum(p[0] for p in px) / n
    g = sum(p[1] for p in px) / n
    b = sum(p[2] for p in px) / n
    ratio = g / ((r + b) / 2) if (r + b) else 0.0
    # mean saturation: how far from grey this band sits, which is what corruption inflates
    sat = sum(max(pp) - min(pp) for pp in px) / n
    return r, g, b, ratio, sat


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    terse = "--terse" in sys.argv
    nBands = 4
    for a in sys.argv[2:]:
        if a.isdigit():
            nBands = int(a)
    try:
        im = Image.open(path).convert("RGB")
    except Exception as e:
        print("decode failed: %s" % e)
        sys.exit(2)
    # a modest downscale keeps this quick without changing a regional cast or a stripe pattern
    if im.size[0] > 640:
        im = im.resize((640, max(1, im.size[1] * 640 // im.size[0])))
    h = im.size[1]
    out = []
    for i in range(nBands):
        s = bandStats(im, h * i // nBands, h * (i + 1) // nBands)
        if s:
            out.append(s)
    if not out:
        sys.exit(2)
    ratios = [s[3] for s in out]
    sats = [s[4] for s in out]
    ratioSpread = max(ratios) - min(ratios)
    satRatio = (max(sats) / min(sats)) if min(sats) > 0 else 999.0
    verdict = "clean"
    if ratioSpread > 0.20 or satRatio > 3.0:
        verdict = "BANDED"
    if min(ratios) < 0.60 or max(ratios) > 1.35:
        verdict = "CAST"
    if terse:
        print("%.3f %.2f %s" % (ratioSpread, satRatio, verdict))
        return
    for i, (r, g, b, ratio, sat) in enumerate(out):
        print("  band %d/%d  R %5.1f  G %5.1f  B %5.1f  ratio %.3f  sat %5.1f"
              % (i + 1, nBands, r, g, b, ratio, sat))
    print("SPREAD ratio %.3f (clean < 0.20), sat max/min %.2f (clean < 3.0) -> %s"
          % (ratioSpread, satRatio, verdict))


main()
