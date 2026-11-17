#!/usr/bin/env python3
"""Read/write MS912x/MS913x xdata registers through the adapter's HID interface (hidraw)."""
import fcntl, os, sys, glob, struct

def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (ord(t) << 8) | nr
def HIDIOCSFEATURE(n): return _IOC(3, 'H', 0x06, n)
def HIDIOCGFEATURE(n): return _IOC(3, 'H', 0x07, n)

def find_dev():
    for p in glob.glob('/sys/class/hidraw/hidraw*/device/uevent'):
        if 'HID_ID=0003:0000345F' in open(p).read().upper().replace('0000345F', '0000345F'):
            return '/dev/' + p.split('/')[4]
    for p in glob.glob('/sys/class/hidraw/hidraw*/device/uevent'):
        if '345F' in open(p).read().upper():
            return '/dev/' + p.split('/')[4]
    sys.exit('adapter hidraw not found')

def feature(fd, payload):
    buf = bytearray([0]) + bytearray(payload)  # report id 0
    fcntl.ioctl(fd, HIDIOCSFEATURE(len(buf)), bytes(buf))

def get_feature(fd):
    buf = bytearray(9); buf[0] = 0
    fcntl.ioctl(fd, HIDIOCGFEATURE(len(buf)), buf)
    return buf[1:]

def read(fd, addr):
    feature(fd, [0xb5, addr >> 8, addr & 0xff, 0, 0, 0, 0, 0])
    return get_feature(fd)[3]

def write(fd, addr, val):
    feature(fd, [0xb6, addr >> 8, addr & 0xff, val, 0, 0, 0, 0])

if __name__ == '__main__':
    fd = os.open(find_dev(), os.O_RDWR)
    cmd = sys.argv[1]
    if cmd == 'r':
        for a in sys.argv[2:]:
            a = int(a, 16); print(f'{a:#06x} = {read(fd, a):#04x}')
    elif cmd == 'w':
        a, v = int(sys.argv[2], 16), int(sys.argv[3], 16); write(fd, a, v)
        print(f'{a:#06x} <- {v:#04x}; now {read(fd, a):#04x}')
