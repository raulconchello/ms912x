#!/usr/bin/env python3
"""Log the TV's mean brightness (camera, 30 fps) for N seconds; report changes."""
import re, subprocess, sys, tempfile, time
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 6
crop = sys.argv[2] if len(sys.argv) > 2 else '320:180:480:260'
log = tempfile.TemporaryFile('w+')
cam = subprocess.Popen(['ffmpeg', '-hide_banner', '-f', 'v4l2', '-input_format', 'mjpeg',
    '-video_size', '1280x720', '-framerate', '30', '-i', '/dev/video0', '-t', str(secs + 2),
    '-vf', f'crop={crop},format=gray,showinfo', '-f', 'null', '-'], stderr=log, text=True)
cam.wait(); log.seek(0)
vals = [(float(m.group(1)), int(m.group(2))) for m in
        (re.search(r'pts_time:([\d.]+).*?mean:\[(\d+)', l) for l in log.read().splitlines()) if m]
vals = vals[30:]  # skip warm-up
ys = [v for _, v in vals]
print(f'frames={len(ys)} min={min(ys)} max={max(ys)} mean={sum(ys)/len(ys):.1f}')
jumps = [(round(t - vals[0][0], 2), a, b) for (t, b), (_, a) in zip(vals[1:], vals) if abs(b - a) > 12]
print('jumps (t, from, to):', jumps[:20])
