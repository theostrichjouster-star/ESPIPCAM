"""Last dumpMotionStats counters from the RTC ring on stdin -> 'bad=N stale=M'. Reading resets them."""
import sys, re
text = sys.stdin.read()
def last(pat):
    m = re.findall(pat, text)
    return m[-1] if m else "?"
print(f"bad={last(r'Bad frames discarded: (\d+)')} stale={last(r'Stale frames flushed after switch: (\d+)')}")
