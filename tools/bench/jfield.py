import sys, re, json
# /status occasionally carries a stray control character; strip before parsing
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
txt = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", "", raw)
try:
    d = json.loads(txt)
except Exception:
    print("")
    sys.exit(0)
v = d.get(sys.argv[1], "")
print("" if v is None else v)
