# ms912x driver for Linux

Linux kernel driver for MacroSilicon USB to VGA/HDMI adapter.

There are three variants:
- VID/PID is 534d:6021. Device is USB 2
- VID/PID is 534d:0821. Device is USB 2
- VID/PID is 345f:9132. Device is USB 3
- VID/PID is 345f:9133. Device is USB 3

## Supported kernels

Use the branch matching the target kernel:

- Linux 5.10 LTS: `kernel-5.10`
- Linux 5.15 LTS: `kernel-5.15`
- Linux 6.1 LTS: `kernel-6.1`
- Linux 6.6 LTS: `kernel-6.6`
- Linux 6.12 LTS: `kernel-6.12`
- Linux 6.18 LTS: `kernel-6.18`
- Linux 7.2: `kernel-7.2`

`main` tracks current development.

TODOs:

- Detect connector type (VGA, HDMI, etc...)
- More resolutions
- Error handling
- Is RGB to YUV conversion needed?

## Development 

Driver is written by analyzing wireshark captures of the device.

## DKMS

Run `sudo dkms install .`


## Low-latency branch (`raul-lowlatency`)

Changes on top of upstream, tested with an MS9133 (`345f:9133`) on Hyprland:

- **Vendor mode-set sequence** (power/transfer/video ordering with delays,
  HDMI unmute at `0xfb07`): without it the MS9133 accepts frames but never
  drives the HDMI output.
- **Keepalive**: the device turns its output off after ~3 s without frames
  (the vendor driver resends every 2.5 s). The last transfer is resent after
  `keepalive_ms` of idle time.
- **`evdi_compat`**: reports the DRM driver name as `evdi` so Hyprland
  (aquamarine) drives the render-less device by importing the iGPU's linear
  buffers via PRIME (zero copy) instead of failing to create a renderer.
- **Fast conversion**: AVX2 XRGB8888 -> UYVY read directly from the
  framebuffer, split across threads (~1.7 ms per 1080p frame vs ~12.8 ms).
- **Damage clips kept across buffer flips**: partial updates instead of a
  full 4 MB frame for every change.
- **Flip pacing** (`flip_mode=2`): the page flip completes just in time for
  the next frame to be ready when the USB link frees up, so frames never queue
  behind a transfer and go stale.

Module options (`/sys/module/ms912x/parameters/`, or `options ms912x ...`
in `/etc/modprobe.d/`): `flip_mode` (0/1/2), `keepalive_ms`, `conv_threads`,
`use_avx2`, `trans_mode` (3 = double buffered, 5 = bypass; no measurable
latency gain), `evdi_compat`, `debug_hold`.

Debugfs (`/sys/kernel/debug/dri/<minor>/`): `ms912x_stats` (timings since
the last read) and `ms912x_fill` (send a solid `RRGGBB` frame, for latency
tests).

Test and measurement scripts used for this work are in [`tools/`](tools/).

Install with DKMS from a clean copy (the module then rebuilds on kernel updates):

```sh
sudo mkdir -p /usr/src/ms912x-0.2.0
sudo cp Makefile dkms.conf LICENSE README.md *.c *.h /usr/src/ms912x-0.2.0/
sudo dkms install ms912x/0.2.0
```
