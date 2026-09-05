#!/bin/sh
# flash/device/flash_mtd2.sh — write a new rootfs into mtd2 FROM the running device.
#
# This is the moment of truth. Everything before it is reversible; this is not (except
# by re-flashing the backup, which needs the serial console if the box won't boot).
#
# Why from Linux instead of U-Boot: we can read the flash back and byte-compare BEFORE
# rebooting. If the compare fails we just write again — the old image is already gone
# either way, but we never reboot onto an image we haven't verified in situ.
#
# Deploy:  put mtd2_new.bin + this script in /root/rec/a1/, run over telnet:
#            sh /root/rec/a1/flash_mtd2.sh /root/rec/a1/mtd2_new.bin
# Restore: same script with dump/mtd2_rootfs.bin.
set -u
IMG="${1:-/root/rec/a1/mtd2_new.bin}"
DEV=/dev/mtdblock2
PARTSZ=5242880

echo "=== mtd2 writer ==="
[ -f "$IMG" ] || { echo "FATAL: $IMG not found"; exit 1; }
SZ=`wc -c < "$IMG"`
echo "image      : $IMG ($SZ bytes)"
[ "$SZ" -eq "$PARTSZ" ] || { echo "FATAL: image must be exactly $PARTSZ bytes"; exit 1; }
head -c 4 "$IMG" | od -An -tx1 | grep -q "27 05 19 56" || { echo "FATAL: not a uImage"; exit 1; }
echo "uImage magic: ok"

echo "--- 1. back up the CURRENT mtd2 to SATA (so this is always undoable) ---"
BAK=/root/rec/a1/mtd2_before_flash.bin
dd if=$DEV of=$BAK bs=64k 2>/dev/null
BSZ=`wc -c < $BAK`
echo "saved $BAK ($BSZ bytes) md5 `md5sum $BAK | cut -d' ' -f1`"
[ "$BSZ" -eq "$PARTSZ" ] || { echo "FATAL: backup is the wrong size, aborting"; exit 1; }

echo "--- 2. stop everything that could touch the disk or the watchdog ---"
touch /root/stop            # our boot.sh / run_dvr.sh loop exits
sleep 2
killall dvr 2>/dev/null
killall app.out 2>/dev/null
killall mydaemon.out 2>/dev/null
sleep 2
sync

echo "--- 3. WRITE (do not power off) ---"
WANT=`md5sum "$IMG" | cut -d' ' -f1`
echo "target md5 : $WANT"
dd if="$IMG" of=$DEV bs=64k 2>&1
sync
sleep 2

echo "--- 4. read back and compare ---"
GOT=`dd if=$DEV bs=64k count=80 2>/dev/null | md5sum | cut -d' ' -f1`
echo "flash md5  : $GOT"
if [ "$GOT" = "$WANT" ]; then
    echo
    echo "=== FLASH VERIFIED — mtd2 now holds the new image ==="
    echo "    rm /root/stop, then 'reboot' when you are ready."
    echo "    If it does not come back: serial console -> U-Boot -> restore"
    echo "    $BAK (also in dump/mtd2_rootfs.bin on the PC)."
    rm -f /root/stop
    exit 0
else
    echo
    echo "!!! MISMATCH — the flash does NOT match the image. DO NOT REBOOT."
    echo "!!! Re-run this script; if it keeps failing, write the backup back:"
    echo "!!!     sh $0 $BAK"
    exit 1
fi
