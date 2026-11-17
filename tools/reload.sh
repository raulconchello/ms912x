#!/bin/sh
# Reload the ms912x module from the build tree (run as root)
KO=${1:-$(dirname "$(readlink -f "$0")")/../ms912x.ko}
shift 2>/dev/null
for i in /sys/bus/usb/drivers/ms912x/*:*; do [ -e "$i" ] && echo "$(basename $i)" > /sys/bus/usb/drivers/ms912x/unbind; done
sleep 1
rmmod ms912x 2>/dev/null
for n in 1 2 3 4 5; do lsmod | grep -q '^ms912x ' || break; sleep 1; rmmod ms912x 2>/dev/null; done
insmod "$KO" "$@"
