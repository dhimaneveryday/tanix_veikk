# VEIKK VK2200 Pro pen fix for an Android TV box (TX3 Max)

## The problem

When the VEIKK VK2200 Pro pen display is connected to the Android TV box:

- Moving the pen moves the cursor.
- **Tapping with the pen tip does nothing.** It doesn't click or draw.
- A normal mouse works fine.

**Status:** fixed and working (confirmed 2026-11-21).

## Equipment

| Item | Details |
|---|---|
| TV box | TX3 Max, Amlogic chip (`p281`), 32-bit ARM (`armeabi-v7a`) |
| Android | 7.1.2, Linux kernel 3.14.29, build `p281-userdebug 7.1.2 NHG47L 20190218 test-keys` |
| Root | Yes. It's a `userdebug` build, so `su` and `adb root` work. SELinux is **Permissive**. |
| Pen display | VEIKK VK2200 Pro. USB vendor `2feb`, product `1006`. Shows up as `VEIKK.INC VK2200PRO`. |
| Computer | Mac with Android SDK `adb` and NDK 28 (`~/Library/Android/sdk/ndk/28.2.13676358`) |

## Root cause

The pen display creates two input devices on the box. The one that carries the pen has `BTN_TOOL_PEN`, pressure and tilt. Its USB data contains **two tip switches**: "Tip Switch" (HID `0x42`) and "Secondary Tip Switch" (HID `0x43`). The box's old **kernel 3.14 maps both to the same key, `BTN_TOUCH`**.

So in each frame where the tip is pressed, the kernel sends:

```
MSC_SCAN 000d0042   BTN_TOUCH DOWN   <- real tip: pressed
MSC_SCAN 000d0043   BTN_TOUCH UP     <- secondary tip: not pressed
SYN_REPORT
```

Android only acts on the final state, which is **UP**, so it never sees a touch. Hovering still works, which is why the cursor moves. The pen's side button has the same problem with `BTN_STYLUS`.

Neither an Android setting nor an `.idc` config file can fix this, because the error happens in the kernel. Changing the kernel isn't practical.

## The fix: a helper program called `penfix`

`penfix` is a small native program (source: [penfix.c](penfix.c)) that runs as root in the background:

1. It finds the VEIKK pen device (vendor `2feb` with `BTN_TOOL_PEN` and `ABS_PRESSURE`) and takes exclusive control of it (`EVIOCGRAB`). Android then stops receiving the broken signals.
2. It creates a virtual device through `/dev/uinput`, named **`VEIKK VK2200PRO penfix`**. That device has the same position, pressure and tilt ranges as the real pen, plus `INPUT_PROP_DIRECT`, so Android treats it as a pen on the screen.
3. It forwards position, pressure and tilt unchanged.
4. It works out each button's real state once per frame:
   **pressed = (a DOWN arrived in this frame) OR (the kernel still says DOWN)**

   This rule covers every combination of the two switches. If both are pressed, the kernel stays DOWN and sends nothing new. If only the main tip is pressed, the kernel sends DOWN then UP in every frame. The first version used only the first part of the rule, and strokes broke up mid-line.
5. When the pen is unplugged, `penfix` waits and reconnects automatically. It writes a log to `/data/local/tmp/penfix.log`.

### Files on the box

| File on box | Purpose | Backup in this folder |
|---|---|---|
| `/system/bin/penfix` | the helper program | `penfix` |
| `/system/bin/install-recovery.sh` | starts penfix at boot. The box's `init.rc` already has a service `flash_recovery` that runs this path as root. The file didn't exist before. | `install-recovery.sh` |
| `/data/system/devices/idc/Vendor_2feb_Product_1006.idc` | `touch.deviceType = touchScreen`. Optional backup setting: not enough on its own, but harmless. | `Vendor_2feb_Product_1006.idc` |

## Files in this folder

| File | What it is |
|---|---|
| `README.md` | this document |
| `penfix.c` | source code of the helper program |
| `penfix` | the built program, ready to install (32-bit ARM, static, stripped) |
| `install-recovery.sh` | boot script that starts penfix |
| `Vendor_2feb_Product_1006.idc` | Android input config file |
| `install.sh` | installs everything from the Mac with one command |
| `uninstall.sh` | removes everything from the Mac with one command |

## Reinstalling after a firmware update or factory reset

### 1. Unlock Developer options and enable USB debugging
On the box: **Settings → About → Build number**, tap it 7 times. Then in **Developer options**, turn on **USB debugging**.

### 2. Turn on Wi-Fi ADB
This box has no "ADB over network" switch.

1. Install **Terminal Emulator for Android** on the box.
2. Type:
   ```
   su
   setprop service.adb.tcp.port 5555
   stop adbd
   start adbd
   ip addr show wlan0
   ```
3. Note the box's IP address from the `inet ...` line.

This setting is lost when the box reboots. Repeat this step whenever you need ADB again.

**If the Mac can't connect:**
- Check that the Mac and the box are on the same network.
- On one router, AP or client isolation blocked the connection completely (`ping` failed). Another network worked.
- Or use a cable: a USB-C to USB-A adapter on the Mac, plus an **A-to-A** cable into the box's port nearest the power jack. Then run `adb tcpip 5555`.

### 3. Install from the Mac
```bash
git clone git@github.com:dhimaneveryday/tanix_veikk.git
cd tanix_veikk
./install.sh <box-ip>          # e.g. ./install.sh 10.159.41.147
```
The output should end with `penfix is running` and `attached to /dev/input/eventN`.

### 4. Test
Tap with the pen tip. To see touches, turn on **Developer options → Pointer location**. The top bar should show `P:1/1` while the tip touches, and lines should draw without gaps. Turn Pointer location off afterwards.

## Manual install (without the scripts)

```bash
adb connect <ip>:5555 && adb root && adb connect <ip>:5555
adb push penfix /data/local/tmp/penfix.sys
adb push install-recovery.sh /data/local/tmp/
adb shell
# --- on the box ---
mount -o rw,remount /system
cp /data/local/tmp/penfix.sys /system/bin/penfix
cp /data/local/tmp/install-recovery.sh /system/bin/install-recovery.sh
chmod 755 /system/bin/penfix /system/bin/install-recovery.sh
chcon u:object_r:system_file:s0 /system/bin/penfix
chcon u:object_r:install_recovery_exec:s0 /system/bin/install-recovery.sh
mount -o ro,remount /system
start flash_recovery            # starts penfix now (same as at boot)
```

With no computer at all, you can do the same from Terminal Emulator after `su`. Copy `penfix` and `install-recovery.sh` to the box with a USB stick first.

## Rebuilding penfix from source

```bash
NDK=~/Library/Android/sdk/ndk/28.2.13676358
CC=$(ls $NDK/toolchains/llvm/prebuilt/*/bin/armv7a-linux-androideabi21-clang)
$CC -O2 -Wall -static -s -o penfix penfix.c
```

## Removing the fix
```bash
./uninstall.sh <box-ip>
```
Or on the box, as `su`:
```
mount -o rw,remount /system
rm /system/bin/penfix /system/bin/install-recovery.sh
mount -o ro,remount /system
```
Then reboot.

## Troubleshooting

| Check | Command (on the box as `su`, or via `adb shell`) |
|---|---|
| Is penfix running? | `pidof penfix` |
| penfix log | `cat /data/local/tmp/penfix.log` |
| Does the fixed device exist? | `getevent -pl \| grep -B1 penfix` should show `VEIKK VK2200PRO penfix` |
| What the pen really sends | `getevent -lt /dev/input/eventN`. `getevent` accepts only **one** device; with no device given it shows all. |
| How Android sees the devices | `dumpsys input` (look for `DeviceType: touchScreen` and `Last Raw Touch`) |

- **Event numbers change** after replugging (`event3`, `event4`, `event5`, ...). penfix finds the pen by vendor ID and capabilities, not by number.
- **Taps land in the wrong place:** set the box's output resolution to match the display (1920×1080 is native). The current setting, 1280×720, maps correctly.
- **The side button acts like a right-click.** That's normal. Avoid pressing it by accident while tapping.
- **Don't run `pkill -f /data/local/tmp/penfix` inside `adb shell '...'`.** It matches and kills the shell itself. Use `kill $(pidof penfix)`.

## Timeline of what was tried

1. **Added the `.idc` file with `touch.deviceType = touchScreen`.** Android loaded it and both VEIKK devices became `touchScreen`, but taps still failed.
2. **Recorded raw events with `getevent`.** Found `BTN_TOUCH DOWN` and `UP` in the same frame, caused by HID usages `0x42` and `0x43`. This was the root cause.
3. **penfix v1:** touch = "a DOWN arrived in this frame". Taps worked, but strokes broke up, because sometimes the kernel stays DOWN and sends nothing.
4. **penfix v2:** touch = "a DOWN arrived in this frame, or the kernel still says DOWN", applied to all buttons. Works well.
5. **Installed the boot hook** through `/system/bin/install-recovery.sh`, tested with `start flash_recovery`, and confirmed penfix runs automatically.
