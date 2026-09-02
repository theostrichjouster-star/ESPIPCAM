"""The LAST 'AVI playback stats' block from the RTC ring on stdin -> key=value lines."""
import sys, re
text = sys.stdin.read()
i = text.rfind("AVI playback stats")
blk = text[i:] if i >= 0 else ""
def g(pat, default=""):
    m = re.search(pat, blk)
    return m.group(1) if m else default
print("file=" + g(r"Playback (\S+\.avi)", "NONE"))
print("recFps=" + g(r"Recorded FPS (\d+)"))
print("playFps=" + g(r"Playback FPS ([\d.]+)"))
print("frames=" + g(r"Number of frames: (\d+)"))
m = re.search(r"Average SD read speed: (\d+) kB/s \((\d+) clusters of (\d+) bytes, (\d+) us each\)", blk)
print("readKBs=" + (m.group(1) if m else ""))
print("clusters=" + (m.group(2) if m else ""))
print("usPerCluster=" + (m.group(4) if m else ""))
print("readMs=" + g(r"Average frame SD read time: (\d+) ms"))
print("waitMs=" + g(r"Average frame SD wait time: (\d+) ms"))
print("procMs=" + g(r"Average frame processing time: (\d+) ms"))
print("delayMs=" + g(r"Average frame delay time: (\d+) ms"))
print("httpMs=" + g(r"Average http send time: (\d+) ms"))
print("busy=" + g(r"Busy: (\d+)%"))
