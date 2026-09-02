"""Print 'WxH bytes' for a JPEG file (SOF0/SOF2), or 'NOTJPEG bytes'."""
import sys, struct
p = sys.argv[1]
try:
    b = open(p, "rb").read()
except OSError:
    print("MISSING 0"); sys.exit(0)
n = len(b)
if n < 4 or b[:2] != b"\xff\xd8":
    print(f"NOTJPEG {n}"); sys.exit(0)
i = 2
while i + 9 < n:
    if b[i] != 0xFF: i += 1; continue
    marker = b[i + 1]
    if marker in (0xC0, 0xC1, 0xC2):
        h, w = struct.unpack(">HH", b[i + 5:i + 9])
        print(f"{w}x{h} {n}"); sys.exit(0)
    if marker == 0xD8 or 0xD0 <= marker <= 0xD7 or marker == 0x01 or marker == 0xFF:
        i += 2; continue
    seglen = struct.unpack(">H", b[i + 2:i + 4])[0]
    i += 2 + seglen
print(f"NOSOF {n}")
