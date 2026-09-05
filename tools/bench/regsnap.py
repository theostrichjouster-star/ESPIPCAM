"""Register snapshots for the UI control regression (ui_regress.sh).

Under the bench's 5 s minimum gap between HTTP requests (4 Sep 2026) a snapshot cannot be a
hundred single-register reads, so it is built from two sources:

  1. `dumpCam=1` - the firmware's own LOG_DIA dump of the clock tree, timing, geometry, exposure
     and JPEG state, read back from the SD log (`/web?log.txt`, the tail) and parsed by `dump`
     into named values: the PLL and root-divider bytes, HTS / VTS / AEC extra lines, the window
     start / end / output / offsets, subsample and binning bytes (0x3814/15, 0x3820/21), the
     scaler enable (0x5001), the AEC exposure / gain / ceiling and the night-mode and banding
     state, the JPEG encoder state. That is the whole set the tuner owns.
  2. a handful of `camRegRd` reads (the core three - 0x3503, 0x3406, 0x4407 - plus the control's
     own registers), which land in the RTC ring as "camRegRd: 0xADDR = 0xVV" and are parsed by
     `parse` (the LAST len(args) reads; a batch that came back short is reported, never filled
     from an older line).

Both write "KEY=VALUE" lines to the same snapshot file; `diff` classifies every changed key as
the control's own (expected), unexpected (a finding), or live (AEC exposure / gain, the AEC
frame extension, AWB gains - they move on their own and are logged, not gated).

  python regsnap.py dump < sd-log-tail            -> KEY=VALUE lines from the LAST clock-tree block
  python regsnap.py parse 0xADDR ... < ring       -> ADDR=VV lines for the last len(args) reads
  python regsnap.py expand 5587,5588,5381-538B    -> "0x5587 0x5588 0x5381 ..." for the shell
  python regsnap.py diff A B [expected,csv]       -> exp=..;unexp=..;live=..;missing=..  (exit 1 on unexp/missing)
  python regsnap.py live SNAP                     -> "expLines gain16 awbR awbG awbB r3503 r4407"
"""
import re
import sys

CORE = ["0x3503", "0x3406", "0x4407"]
LIVE = {"3500", "3501", "3502", "350A", "350B", "350C", "350D",
        "3400", "3401", "3402", "3403", "3404", "3405",
        "EXPLINES", "GAINX", "EXTRA"}
RD = re.compile(r"camRegRd: 0x([0-9A-Fa-f]{4}) = 0x([0-9A-Fa-f]{2})")
BLOCK = "OV5640 clock tree"
# a control's expected register may show up in the dump under a derived name: the ceiling as a
# multiplier, the timing pair as HTS / VTS, the window as its corners, 0x3A00 as the two state words
ALIAS = {"3A18": ["CEIL"], "3A19": ["CEIL"], "3A00": ["NIGHT", "BAND"],
         "380C": ["HTS"], "380D": ["HTS"], "380E": ["VTS"], "380F": ["VTS"],
         "3800": ["XST"], "3801": ["XST"], "3802": ["YST"], "3803": ["YST"],
         "3804": ["XEND"], "3805": ["XEND"], "3806": ["YEND"], "3807": ["YEND"],
         "3808": ["OUTW"], "3809": ["OUTW"], "380A": ["OUTH"], "380B": ["OUTH"],
         "3810": ["XOFF"], "3811": ["XOFF"], "3812": ["YOFF"], "3813": ["YOFF"],
         "5001": ["SCALE"]}


def norm(a):
    return a.strip().upper().replace("0X", "")


def load(path):
    d = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                d[norm(k)] = v.strip()
    return d


def expand(spec):
    out = []
    for tok in spec.split(","):
        tok = norm(tok)
        if not tok:
            continue
        if "-" in tok:
            lo, hi = tok.split("-")
            out.extend("%04X" % a for a in range(int(lo, 16), int(hi, 16) + 1))
        else:
            out.append(tok)
    return out


def hx(s):
    return "%02X" % int(s, 16)


def parse_dump(text):
    blocks = text.split(BLOCK)
    if len(blocks) < 2:
        return None
    b = blocks[-1]
    o = {}
    m = re.search(r"Frame size: (\S+), XCLK (\d+)MHz", b)
    if m:
        o["FS"], o["XCLK"] = m.group(1).rstrip(","), m.group(2)
    m = re.search(r"PLL regs: 0x3034=0x([0-9A-Fa-f]+) 0x3035=0x([0-9A-Fa-f]+) 0x3036=(\d+) 0x3037=0x([0-9A-Fa-f]+) 0x3039=0x([0-9A-Fa-f]+)", b)
    if m:
        o["3034"], o["3035"], o["3036"], o["3037"], o["3039"] = hx(m.group(1)), hx(m.group(2)), "%02X" % int(m.group(3)), hx(m.group(4)), hx(m.group(5))
    m = re.search(r"PCLK regs: 0x3108=0x([0-9A-Fa-f]+) 0x3824=(\d+) 0x460C=0x([0-9A-Fa-f]+) 0x3103=0x([0-9A-Fa-f]+)", b)
    if m:
        o["3108"], o["3824"], o["460C"], o["3103"] = hx(m.group(1)), "%02X" % int(m.group(2)), hx(m.group(3)), hx(m.group(4))
    m = re.search(r"PIXCLK ([0-9.]+)MHz", b)
    if m:
        o["PIXCLK"] = m.group(1)
    m = re.search(r"Timing: HTS (\d+) x(\d+) clocks/line, VTS (\d+) \+ (\d+) AEC extra", b)
    if m:
        o["HTS"], o["LF"], o["VTS"], o["EXTRA"] = m.group(1), m.group(2), m.group(3), m.group(4)
    m = re.search(r"Exposure: ([0-9.]+) lines = .*? gain ([0-9.]+)x \(ceiling ([0-9.]+)x\), night mode (\S+), banding ([^\r\n]+)", b)
    if m:
        o["EXPLINES"], o["GAINX"], o["CEIL"] = m.group(1), m.group(2), m.group(3)
        o["NIGHT"] = m.group(4).rstrip(",")
        o["BAND"] = m.group(5).strip().replace(" ", "_")
    m = re.search(r"JPEG: mode (\d+) \(0x4713=0x([0-9A-Fa-f]+)\), 0x4600=0x([0-9A-Fa-f]+) fixed height (\S+), VFIFO output (\d+)x(\d+), input (\S+), JFIFO overflow (\S+)", b)
    if m:
        o["JPGMODE"], o["4713"], o["4600"], o["FIXH"] = m.group(1), hx(m.group(2)), hx(m.group(3)), m.group(4).rstrip(",")
        o["VFW"], o["VFH"], o["JPGIN"], o["JFIFO"] = m.group(5), m.group(6), m.group(7).rstrip(","), m.group(8)
    m = re.search(r"Window: start (\d+),(\d+) end (\d+),(\d+) output (\d+)x(\d+) offset (\d+),(\d+)", b)
    if m:
        o["XST"], o["YST"], o["XEND"], o["YEND"], o["OUTW"], o["OUTH"], o["XOFF"], o["YOFF"] = m.groups()
    m = re.search(r"ISP: window (\d+)x(\d+) / subsample (\d+)x(\d+) = input (\d+)x(\d+), pre-scale (\d+)x(\d+), scale (\S+) \(0x5001=0x([0-9A-Fa-f]+)\)", b)
    if m:
        o["PREW"], o["PREH"], o["SCALE"], o["5001"] = m.group(7), m.group(8), m.group(9), hx(m.group(10))
    m = re.search(r"Subsample: 0x3814=0x([0-9A-Fa-f]+) 0x3815=0x([0-9A-Fa-f]+) .*? 0x3820=0x([0-9A-Fa-f]+) 0x3821=0x([0-9A-Fa-f]+)", b)
    if m:
        o["3814"], o["3815"], o["3820"], o["3821"] = hx(m.group(1)), hx(m.group(2)), hx(m.group(3)), hx(m.group(4))
    return o


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "dump":
        text = sys.stdin.buffer.read().decode("utf-8", "replace")
        o = parse_dump(text)
        if not o:
            sys.stderr.write("NODUMP\n")
            return 1
        for k in sorted(o):
            print("%s=%s" % (k, o[k]))
        return 0 if "HTS" in o and "3108" in o else 1
    if cmd == "parse":
        want = [norm(a) for a in sys.argv[2:]]
        text = sys.stdin.buffer.read().decode("utf-8", "replace")
        hits = RD.findall(text)
        got = hits[-len(want):] if want else []
        seen = {a.upper(): v.upper() for a, v in got}
        missing = [a for a in want if a not in seen]
        extra = [a for a in seen if a not in want]
        for a in want:
            if a in seen:
                print("%s=%s" % (a, seen[a]))
        if missing or extra:
            sys.stderr.write("MISSING:%s EXTRA:%s\n" % (",".join(missing), ",".join(extra)))
            return 1
        return 0
    if cmd == "expand":
        print(" ".join("0x" + a for a in expand(sys.argv[2] if len(sys.argv) > 2 else "")))
        return 0
    if cmd == "diff":
        a, b = load(sys.argv[2]), load(sys.argv[3])
        expected = set(expand(sys.argv[4])) if len(sys.argv) > 4 else set()
        for k in list(expected):
            expected.update(ALIAS.get(k, []))
        exp, unexp, live, missing = [], [], [], []
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                # a register read for one point and not the other is not a change
                continue
            if a[k] == b[k]:
                continue
            item = "%s:%s>%s" % (k, a[k], b[k])
            if k in LIVE:
                live.append(item)
            elif k in expected:
                exp.append(item)
            else:
                unexp.append(item)
        for k in ("HTS", "VTS", "3108", "3035", "3036", "3814", "3820", "5001", "3503", "3406", "4407"):
            if k not in b:
                missing.append(k)
        print("exp=%s;unexp=%s;live=%s;missing=%s" % (
            "+".join(exp) or "-", "+".join(unexp) or "-", "+".join(live) or "-", "+".join(missing) or "-"))
        return 1 if (unexp or missing) else 0
    if cmd == "live":
        # every field on its own: the raw registers when the snapshot read them, else the dump's
        # derived values, else "?" - a scenario that reads 0x3500-02 but not 0x350A/B (the
        # first dry run) must still get its exposure, and 0x3503 / 0x4407 never depend on either
        s = load(sys.argv[2])

        def pair12(hi, lo):
            return ((int(s[hi], 16) & 0x0F) << 8) | int(s[lo], 16)

        try:
            if "3500" in s and "3501" in s and "3502" in s:
                e0, e1, e2 = int(s["3500"], 16), int(s["3501"], 16), int(s["3502"], 16)
                lines = ((e0 & 0x0F) << 12) | (e1 << 4) | (e2 >> 4)
            else:
                lines = int(round(float(s["EXPLINES"])))
        except (KeyError, ValueError):
            lines = "?"
        try:
            if "350A" in s and "350B" in s:
                gain = ((int(s["350A"], 16) & 3) << 8) | int(s["350B"], 16)
            else:
                gain = int(round(float(s["GAINX"]) * 16))
        except (KeyError, ValueError):
            gain = "?"
        try:
            r, g, bl = pair12("3400", "3401"), pair12("3402", "3403"), pair12("3404", "3405")
        except (KeyError, ValueError):
            r = g = bl = "?"
        print("%s %s %s %s %s %s %s" % (lines, gain, r, g, bl, s.get("3503", "?"), s.get("4407", "?")))
        return 0 if lines != "?" else 1
    print("unknown command %s" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main())
