"""Contact sheet of a run's stills, so the eyeball gate costs one look instead of fifty.

The channel gates in still_color.py are a SCREEN, not the verdict: the HTS floor campaign's own
byte and AEC gates passed magenta, green-blown and confetti frames, and the gates that replaced
them were themselves derived from a handful of samples. A grid of every still, labelled with the
condition that produced it, is what lets a person see a cast or a stripe pattern that no scalar
caught - and see it in context, next to the frames that ought to look identical.

Rows are grouped by the label prefix before the first underscore run, so a walk of one variable
reads across a row. The label under each tile carries the filename stem and, when a CSV of
still_color.py output is passed, its channel ratio and hdiff.

Usage:
  python contact_sheet.py <out.jpg> <dir-with-stills> [csv-with-ratio-column] [tile-width]
"""
import csv
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: python -m pip install pillow")

LABEL_H = 22


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    outPath, srcDir = sys.argv[1], sys.argv[2]
    csvPath = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != "-" else None
    tileW = int(sys.argv[4]) if len(sys.argv) > 4 else 320

    stats = {}
    if csvPath and os.path.exists(csvPath):
        for r in csv.DictReader(open(csvPath)):
            # key on the same fields the run used to name its files
            key = "%s_hts%s_c%s_s%s" % (r.get("size"), r.get("hts"), r.get("cycle"), r.get("shot"))
            stats[key] = (r.get("ratio", "?"), r.get("hdiff", "?"), r.get("verdict", ""))

    files = sorted(f for f in os.listdir(srcDir) if f.lower().endswith(".jpg"))
    if not files:
        sys.exit("no stills in " + srcDir)

    # group into rows by CONDITION, so a row is everything taken at one setting and the eye
    # compares across it. Cycle and shot are both within-condition repeats, so strip at the
    # cycle marker when there is one - eighteen rows of three is unreadably tall where six rows
    # of nine reads at a glance
    rows, order = {}, []
    for f in files:
        stem = os.path.splitext(f)[0]
        if "_c" in stem:
            group = stem.rsplit("_c", 1)[0]
        elif "_s" in stem:
            group = stem.rsplit("_s", 1)[0]
        else:
            group = stem
        if group not in rows:
            rows[group] = []
            order.append(group)
        rows[group].append(f)

    # a run that provoked a failure leaves ZERO-BYTE stills behind, and those are data: drop them
    # from the grid but say so, rather than letting one unreadable file abort the whole sheet
    tileH = None
    thumbs = {}
    dropped = []
    for f in list(files):
        try:
            im = Image.open(os.path.join(srcDir, f))
            im.thumbnail((tileW, tileW))      # keeps aspect; height follows the source
            thumbs[f] = im.convert("RGB")
            tileH = max(tileH or 0, im.size[1])
        except Exception:
            dropped.append(f)
            files.remove(f)
            for g in rows:
                if f in rows[g]:
                    rows[g].remove(f)
    order = [g for g in order if rows[g]]
    if not files:
        sys.exit("every still in %s failed to decode" % srcDir)
    if dropped:
        print("skipped %d unreadable still(s): %s" % (len(dropped), ", ".join(dropped)))
    cols = max(len(rows[g]) for g in order)

    sheetW = cols * tileW
    sheetH = len(order) * (tileH + LABEL_H)
    sheet = Image.new("RGB", (sheetW, sheetH), (24, 24, 28))
    d = ImageDraw.Draw(sheet)

    for r, group in enumerate(order):
        for c, f in enumerate(rows[group]):
            x, y = c * tileW, r * (tileH + LABEL_H)
            sheet.paste(thumbs[f], (x, y))
            stem = os.path.splitext(f)[0]
            ratio, hdiff, verdict = stats.get(stem, ("", "", ""))
            label = stem
            if ratio:
                label = "%s  ratio %s  hdiff %s%s" % (stem, ratio, hdiff,
                                                      "  " + verdict.upper() if verdict and verdict != "clean" else "")
            colour = (255, 120, 120) if verdict and verdict != "clean" else (210, 210, 210)
            d.text((x + 4, y + tileH + 5), label[:70], fill=colour)

    sheet.save(outPath, quality=88)
    print("%s  %dx%d, %d stills in %d rows" % (outPath, sheetW, sheetH, len(files), len(order)))


main()
