"""The LAST 'AVI recording stats' block from the RTC ring on stdin -> key=value lines."""
import sys, re
text = sys.stdin.read()
i = text.rfind("AVI recording stats")
blk = text[i:] if i >= 0 else ""
def g(pat, default=""):
    m = re.search(pat, blk)
    return m.group(1) if m else default
print("file=" + g(r"Recorded (\S+\.avi)", "NONE"))
print("duration=" + g(r"AVI duration: (\d+) secs"))
print("frames=" + g(r"Number of frames: (\d+)"))
print("reqFps=" + g(r"Required FPS: (\d+)"))
print("actFps=" + g(r"Actual FPS: ([\d.]+)"))
print("avgBytes=" + g(r"Average frame length: (\d+) bytes"))
print("storageMs=" + g(r"Average frame storage time: (\d+) ms"))
print("sdKBs=" + g(r"Average SD write speed: (\d+) kB/s"))
print("boost=" + g(r"max quality boost (\d+)", "0"))
print("rescues=" + g(r"No-frame rescue fired (\d+)", "0"))
print("busy=" + g(r"Busy: (\d+)%"))
