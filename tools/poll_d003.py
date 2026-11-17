import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msreg as m
fd = os.open(m.find_dev(), os.O_RDWR)
t0 = time.monotonic(); vals = []
while time.monotonic() - t0 < 1.0:
    vals.append((time.monotonic() - t0, m.read(fd, 0xd003)))
changes = [(round(t*1000,1), v) for i, (t, v) in enumerate(vals) if i == 0 or v != vals[i-1][1]]
print(len(vals), 'reads;', len(changes), 'changes;', changes[:12])
