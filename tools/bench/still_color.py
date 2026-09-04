"""Channel statistics of a still, the gate that separates a clean readout from a corrupt one.

The HTS floor campaign (3-4 Sep 2026) passed corrupt frames through its byte-count and AEC
gates: a mid-frame timing write latched a magenta cast (G at ~0.42 of R and B) that the AEC
happily servoed, and a too-short row blew the highlights green before the frame collapsed.
Both are invisible to size and dimension checks and obvious in the channel means, which is
what this prints:

  ratio  G / mean(R, B)     clean stills sit at 1.02-1.14, the latched cast at 0.40-0.45,
                            green blow-out at 1.25 and above
  gsat   % of pixels with G >= 250   clean 0-2, green blow-out 3.5-6.6
  rsat   % of pixels with R >= 250   clean 0-1.5, magenta cast 10-15
  hdiff  mean absolute luma difference between horizontally adjacent pixels, full resolution.
         Channel means alone passed a frame of pure random noise (SCLK 96 probe, 4 Sep: ratio
         0.895, and the still was confetti) because noise averages to grey. Clean 1280x960
         stills sit at 2.0-2.6, the first green-highlight corruption at 3.3, random noise at
         31, alternating-column stripes at 121. Smaller outputs run higher (QVGA 5.8), so
         gate it against the same size's own baseline, not an absolute

Usage: python still_color.py <file.jpg>  ->  "W H bytes meanR meanG meanB ratio gsat rsat hdiff"
Exit 2 if the file does not decode. Pillow is required.
"""
import os
import sys
import warnings

warnings.simplefilter("ignore")  # Pillow's getdata deprecation notice would land in the campaign log
try:
    from PIL import Image
except ImportError:
    print("NOPIL")
    sys.exit(2)

if len(sys.argv) != 2:
    print("usage: still_color.py <file.jpg>")
    sys.exit(2)
path = sys.argv[1]
try:
    im = Image.open(path).convert("RGB")
except Exception:
    print("NODECODE")
    sys.exit(2)
w, h = im.size
# every 4th pixel in each axis is plenty for means and saturation fractions
small = im.resize((max(1, w // 4), max(1, h // 4)))
r = g = b = 0
gsat = rsat = 0
n = 0
for R, G, B in small.getdata():
    r += R
    g += G
    b += B
    if G >= 250:
        gsat += 1
    if R >= 250:
        rsat += 1
    n += 1
r /= n
g /= n
b /= n
ratio = g / ((r + b) / 2.0) if (r + b) > 0 else 0.0
from PIL import ImageChops, ImageStat
luma = im.convert("L")
hdiff = ImageStat.Stat(ImageChops.difference(luma.crop((0, 0, w - 1, h)), luma.crop((1, 0, w, h)))).mean[0]
print("%d %d %d %.1f %.1f %.1f %.3f %.2f %.2f %.1f" % (
    w, h, os.path.getsize(path), r, g, b, ratio, 100.0 * gsat / n, 100.0 * rsat / n, hdiff))
