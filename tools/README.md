# Test tools

Used to bring up and tune the low-latency branch on an MS9133 with a webcam
pointed at the TV. Most need root (debugfs, hidraw, module loading).

| Tool | What it does |
|---|---|
| `reload.sh [ko] [params]` | Unbind the adapter, unload and reload the module from the build tree |
| `fps.sh [seconds]` | Frame rate and per-stage timings from `ms912x_stats` over an interval |
| `latency.py [n]` | Device + display latency: sends black/white frames via `ms912x_fill` and times the change on the webcam (use with `debug_hold=1`) |
| `persist.py` | Sends one white frame and logs how long the TV keeps showing it |
| `stability.py [s] [crop]` | Logs the TV's brightness on the webcam to catch flicker |
| `msreg.py r ADDR...` / `w ADDR VAL` | Read/write device registers over the adapter's HID interface |
| `vendor_seq.py` | Replays the vendor's mode-set sequence by hand |
| `poll_d003.py` | Polls register `0xd003` |

The 1080p60 test clip (`media/`, not in git) can be regenerated with:

```sh
ffmpeg -f lavfi -i "testsrc2=size=1920x1080:rate=60,drawtext=text='%{n}':fontsize=200:fontcolor=white:box=1:boxcolor=black:x=50:y=50" \
  -t 20 -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 60 media/test1080p60.mp4
```
