#!/bin/bash
# Installs the VEIKK pen fix on the Android TV box over ADB.
# Usage:  ./install.sh <box-ip>        e.g.  ./install.sh 10.159.41.147
# Needs: adb on this computer, Wi-Fi ADB enabled on the box (see README.md step 2).
set -e
cd "$(dirname "$0")"
IP="$1"
if [ -z "$IP" ]; then echo "Usage: $0 <box-ip>"; exit 1; fi

adb connect "$IP:5555"
adb -s "$IP:5555" root || true
sleep 3
adb connect "$IP:5555" >/dev/null
A="adb -s $IP:5555"

$A push penfix /data/local/tmp/penfix.sys
$A push install-recovery.sh /data/local/tmp/install-recovery.sh
$A push Vendor_2feb_Product_1006.idc /data/local/tmp/Vendor_2feb_Product_1006.idc

$A shell '
set -e
# 1. helper program + boot hook in /system
mount -o rw,remount /system
cp /data/local/tmp/penfix.sys /system/bin/penfix
cp /data/local/tmp/install-recovery.sh /system/bin/install-recovery.sh
chown root:shell /system/bin/penfix /system/bin/install-recovery.sh
chmod 755 /system/bin/penfix /system/bin/install-recovery.sh
chcon u:object_r:system_file:s0 /system/bin/penfix
chcon u:object_r:install_recovery_exec:s0 /system/bin/install-recovery.sh || chcon u:object_r:system_file:s0 /system/bin/install-recovery.sh
sync
mount -o ro,remount /system

# 2. input config file (optional backup setting)
mkdir -p /data/system/devices/idc
cp /data/local/tmp/Vendor_2feb_Product_1006.idc /data/system/devices/idc/
chown -R system:system /data/system/devices
chmod 755 /data/system/devices /data/system/devices/idc
chmod 644 /data/system/devices/idc/Vendor_2feb_Product_1006.idc
restorecon -R /data/system/devices

rm -f /data/local/tmp/penfix.sys /data/local/tmp/install-recovery.sh /data/local/tmp/Vendor_2feb_Product_1006.idc

# 3. start it now, the same way the box does at boot
P=$(pidof penfix || true); [ -n "$P" ] && kill $P && sleep 1
start flash_recovery
sleep 3
'

echo "--- check ---"
$A shell 'pidof penfix >/dev/null && echo "penfix is running (pid $(pidof penfix))" || echo "penfix is NOT running"; tail -2 /data/local/tmp/penfix.log'
echo "Done. Tap the screen with the pen to test."
