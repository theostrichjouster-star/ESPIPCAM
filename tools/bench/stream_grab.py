#!/usr/bin/env python
"""Pull the first complete JPEG out of a raw MJPEG stream capture.

Why this exists: the still handler waits MAX_FRAME_WAIT (1.2 s) for the capture task to keep a
frame and then gives up, so a request lands with probability 1.2 x fps. At the rates a multi-second
exposure produces that is a coin flip - measured 6 Sep 2026, ZERO stills caught in 59 requests at
7.5 s and 10 s frames, while the VSYNC pin said the sensor was delivering all along. The stream has
no such window: it sends each frame as it arrives, so a capture merely has to be longer than one
frame period. It also runs on the httpd task, which is the branch measured safe over aborts -
unlike /sustain?download=0, which is never to be called.

  curl -s -m <secs> "http://<board>/sustain?stream=0" -o raw.bin
  python tools/bench/stream_grab.py raw.bin out.jpg

Prints "<bytes> <frames seen>" on success, exits 1 with nothing written if no complete frame is in
the capture. A truncated trailing frame is ignored rather than written - a partial JPEG passes a
byte-count gate and fails an eyeball, which is the exact failure class still_color.py exists for.
"""
import sys

SOI = b"\xff\xd8\xff"
EOI = b"\xff\xd9"


# A start-of-frame marker is what makes a JPEG an IMAGE rather than a header. The sensor really does
# emit the other kind: at a 7.5 s frame it handed over 68 bytes - JFIF header, one quantisation
# table, then end-of-image, no scan at all (measured 6 Sep 2026). That has both an SOI and an EOI, so
# a marker-pair scan calls it complete and a byte-count gate calls it small. Requiring an SOF is what
# separates a truncated frame from a real one, and it is the same failure class still_color.py exists
# for - structural checks passing on something no eye would accept.
SOF = tuple(bytes([0xFF, m]) for m in (0xC0, 0xC1, 0xC2, 0xC3, 0xC9, 0xCA, 0xCB))


def frames(buf, whole=True):
    """Yield every complete JPEG in buf, in order. whole=False also yields header-only fragments."""
    pos = 0
    while True:
        start = buf.find(SOI, pos)
        if start < 0:
            return
        end = buf.find(EOI, start + len(SOI))
        if end < 0:
            return  # trailing partial frame - the capture was cut mid-send
        end += len(EOI)
        frame = buf[start:end]
        pos = end
        if whole and not any(m in frame for m in SOF):
            continue  # header without an image: the sensor abandoned this frame
        yield frame


def main():
    if len(sys.argv) < 3:
        print("usage: stream_grab.py <raw capture> <out.jpg> [which]", file=sys.stderr)
        return 2
    raw, out = sys.argv[1], sys.argv[2]
    # which frame to keep: the LAST complete one by default. The first frame out of a stream can
    # predate the settle - the sensor is mid-retime when the stream opens - so the newest complete
    # frame is the one that matches the registers the run just wrote.
    which = sys.argv[3] if len(sys.argv) > 3 else "last"
    with open(raw, "rb") as f:
        buf = f.read()
    got = list(frames(buf))
    if not got:
        # say so explicitly: "nothing arrived" and "only truncated headers arrived" are different
        # findings, and at a long exposure the second one is the interesting one
        partial = len(list(frames(buf, whole=False)))
        print("0 0 %d" % partial)
        return 1
    keep = got[0] if which == "first" else got[-1]
    with open(out, "wb") as f:
        f.write(keep)
    print("%d %d 0" % (len(keep), len(got)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
