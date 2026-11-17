#!/usr/bin/env python3
"""Send one white frame (driver hook) and log how long the TV keeps showing it."""
import glob, re, subprocess, sys, tempfile, time
fill = glob.glob('/sys/kernel/debug/dri/*/ms912x_fill')[0]
color = sys.argv[1] if len(sys.argv) > 1 else 'ffffff'
log = tempfile.TemporaryFile('w+')
cam = subprocess.Popen(['ffmpeg', '-hide_banner', '-f', 'v4l2', '-input_format', 'mjpeg',
    '-video_size', '1280x720', '-framerate', '30', '-i', '/dev/video0',
    '-vf', 'crop=320:180:480:260,format=gray,showinfo', '-f', 'null', '-'], stderr=log, text=True)
time.sleep(4)
t0 = time.time()
open(fill, 'w').write(color)
time.sleep(float(sys.argv[2]) if len(sys.argv) > 2 else 8)
cam.terminate(); cam.wait(); log.seek(0)
prev = None
for line in log.read().splitlines():
    m = re.search(r'pts_time:([\d.]+).*?mean:\[(\d+)', line)
    if m:
        t, v = float(m.group(1)) - t0, int(m.group(2))
        bucket = 'bright' if v > 60 else 'dark'
        if bucket != prev:
            print(f't={t:+.2f}s mean={v} -> {bucket}')
            prev = bucket
