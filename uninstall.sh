#!/bin/bash
# Removes the VEIKK pen fix from the Android TV box over ADB.
# Usage:  ./uninstall.sh <box-ip>
set -e
IP="$1"
if [ -z "$IP" ]; then echo "Usage: $0 <box-ip>"; exit 1; fi
adb connect "$IP:5555"
adb -s "$IP:5555" root || true
sleep 3
adb connect "$IP:5555" >/dev/null
adb -s "$IP:5555" shell '
P=$(pidof penfix); [ -n "$P" ] && kill $P
mount -o rw,remount /system
rm -f /system/bin/penfix /system/bin/install-recovery.sh
sync
mount -o ro,remount /system
rm -f /data/system/devices/idc/Vendor_2feb_Product_1006.idc /data/local/tmp/penfix.log
'
echo "Removed. Unplug and replug the pen display (or reboot the box)."
