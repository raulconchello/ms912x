#!/usr/bin/env python3
"""
Measure display latency of the ms912x adapter with a webcam pointed at the TV.

The driver's debugfs hook sends solid black/white frames directly and reports
the CLOCK_MONOTONIC time the USB transfer completed. The webcam frames carry
CLOCK_MONOTONIC timestamps too (uvcvideo), so
    latency = first camera frame showing the new color - USB transfer done.
This includes the camera's own delay, which is constant, so use it to compare
configurations. Run as root while the TV content is static.
"""
import glob, os, random, re, subprocess, sys, tempfile, time, statistics

N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
CROP = sys.argv[2] if len(sys.argv) > 2 else "320:180:480:260"  # w:h:x:y in 1280x720

fill = glob.glob('/sys/kernel/debug/dri/*/ms912x_fill')
if not fill:
    sys.exit('ms912x_fill not found')
fill = fill[0]

cam = subprocess.Popen(
    ['ffmpeg', '-hide_banner', '-loglevel', 'info', '-f', 'v4l2', '-ts', 'abs',
     '-input_format', 'mjpeg', '-video_size', '1280x720', '-framerate', '30',
     '-i', '/dev/video0', '-copyts', '-vf', f'crop={CROP},format=gray,showinfo',
     '-f', 'null', '-'], stderr=(log := tempfile.TemporaryFile('w+')), text=True)
time.sleep(5)  # camera warm-up / auto exposure

events = []
color = 'ffffff'
for i in range(N):
    color = '000000' if color == 'ffffff' else 'ffffff'
    with open(fill, 'w') as f:
        f.write(color)
    with open(fill) as f:
        t_done = int(f.read()) / 1e9
    # camera timestamps are wall clock (ffmpeg converts); convert ours
    t_done += time.time() - time.monotonic()
    events.append((t_done, color == 'ffffff'))
    time.sleep(random.uniform(0.35, 0.65))
time.sleep(1.5)
cam.terminate()
cam.wait()
log.seek(0)
err = log.read()

frames = []
for line in err.splitlines():
    m = re.search(r'pts_time:([\d.]+).*?mean:\[(\d+)', line)
    if m:
        frames.append((float(m.group(1)), int(m.group(2))))
if not frames:
    sys.exit('no camera frames parsed:\n' + err[-2000:])

lo = min(v for _, v in frames); hi = max(v for _, v in frames)
mid = (lo + hi) / 2
lat = []
for t_done, white in events:
    for ts, v in frames:
        if ts > t_done + 0.3:
            break  # no transition seen: skip
        if ts > t_done and ((v > mid) if white else (v < mid)):
            lat.append((ts - t_done) * 1000)
            break
span = frames[-1][0] - frames[0][0]
print(f'camera frames={len(frames)} fps={len(frames)/span:.1f} levels black~{lo} white~{hi}')
print('latencies ms:', ' '.join(f'{x:.0f}' for x in lat))
if lat:
    print(f'n={len(lat)} mean={statistics.mean(lat):.1f} median={statistics.median(lat):.1f} '
          f'min={min(lat):.1f} max={max(lat):.1f}')
