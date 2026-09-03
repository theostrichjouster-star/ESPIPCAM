"""Register-tier point: parse the retime line for one request out of the RTC ring (stdin), apply
the three gates, compare with the 28 Aug sweep.csv row. Prints one CSV row. Exit: 0 pass, 1 fail,
2 no retime line for this request (caller retries)."""
import sys, re, csv

size, fps, ceil, regime, sweep = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
text = sys.stdin.read()

# The firmware labelled this clock SCLK until 3 Sep 2026 and PIXCLK after; both are the same
# number, and the old name was simply wrong (see the SYSCLK note in mjpeg2sd.cpp). Accept
# either so this still parses captures and RTC rings taken before the rename. Non-capturing
# group on purpose - a capturing one would shift every group index below it.
CLK = r"(?:PIXCLK|SCLK)"
VTS = re.compile(rf"Tuned timing (\S+): {CLK} ([\d.]+)MHz, HTS (\d+) x(\d), VTS (\d+) -> sensor ([\d.]+)fps for request (\d+), max exposure (\d+)ms")
SCL = re.compile(rf"Scaler clock (\S+): {CLK} ([\d.]+)MHz \(mul (\d+) sys_div (\d+)\), HTS (\d+) x(\d), VTS (\d+) -> sensor ([\d.]+)fps for request (\d+)")

pt = None
for m in VTS.finditer(text):
    if m.group(1) == size and int(m.group(7)) == fps:
        pt = dict(pixclk=float(m.group(2)), hts=int(m.group(3)), lf=int(m.group(4)), vts=int(m.group(5)),
                  sensor=float(m.group(6)), maxexp=float(m.group(8)))
for m in SCL.finditer(text):
    if m.group(1) == size and int(m.group(9)) == fps:
        hts, lf, vts, pixclk = int(m.group(5)), int(m.group(6)), int(m.group(7)), float(m.group(2))
        pt = dict(pixclk=pixclk, hts=hts, lf=lf, vts=vts, sensor=float(m.group(8)),
                  maxexp=min(vts - 4, 1964) * hts * lf * 1000.0 / (pixclk * 1e6))
if pt is None:
    print(f"{size},,{fps},{regime},,,,,,,NOLINE,,,,")
    sys.exit(2)

period = 952.0 / fps  # full frame period at fps x 1.05, ms
exempt = fps == ceil or (regime == "scaler" and fps <= 4)
g_rate = "OK" if (pt["sensor"] >= fps - 0.005 or exempt) else "FAIL"
g33 = "OK" if pt["maxexp"] >= min(33, period) - 1 else "FAIL"
g100 = "OK" if pt["maxexp"] >= min(100, period) - 1 else "FAIL"

match = "NOROW"
try:
    with open(sweep, newline="") as f:
        for r in csv.DictReader(f):
            if r["size"] == size and int(r["fps"]) == fps:
                diffs = []
                # sweep.csv is the stored 28 Aug reference and keeps its original SCLK_MHz
                # column heading deliberately - it is a measurement record, not something to
                # retitle after the fact. Same quantity as the PIXCLK the firmware now prints
                if abs(float(r["SCLK_MHz"]) - pt["pixclk"]) > 0.011: diffs.append(f"pixclk {r['SCLK_MHz']}")
                if int(r["HTS"]) != pt["hts"]: diffs.append(f"hts {r['HTS']}")
                if int(r["lf"]) != pt["lf"]: diffs.append(f"lf {r['lf']}")
                if int(r["VTS"]) != pt["vts"]: diffs.append(f"vts {r['VTS']}")
                if abs(float(r["sensor_fps"]) - pt["sensor"]) > 0.021: diffs.append(f"sensor {r['sensor_fps']}")
                if abs(float(r["maxExp_ms"]) - pt["maxexp"]) > 1.5: diffs.append(f"maxExp {r['maxExp_ms']}")
                match = "OK" if not diffs else "DIFF(" + "; ".join(diffs) + ")"
                break
except FileNotFoundError:
    match = "NOCSV"

rescue = "RESCUE" if re.search(r"rescue", text, re.I) else ""
print(f"{size},{fps},{regime},{pt['pixclk']:.2f},{pt['hts']},{pt['lf']},{pt['vts']},{pt['sensor']:.2f},{pt['maxexp']:.0f},{g_rate},{g33},{g100},{match},{rescue}")
ok = g_rate == "OK" and g33 == "OK" and g100 == "OK" and match == "OK" and not rescue
sys.exit(0 if ok else 1)
