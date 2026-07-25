#!/bin/bash
# flash/build.sh — build a flashable mtd2 (rootfs) image for the RR104P.
#
# Produces, in flash/out/:
#   rootfs_new.ext2   ext2 rev0 ramdisk, EXACTLY the stock geometry (13372 x 1024 B)
#   rootfs_new.gz     gzip -9 of the above
#   mtd2_new.bin      uImage-wrapped, 0xFF-padded to the full 5 MB partition  <-- FLASH THIS
#
# Run inside WSL (needs e2fsprogs >= 1.43 for `mke2fs -d`, plus debugfs/gzip/tar):
#     bash flash/build.sh
#
# WHY a rebuild rather than a patch of the stock image: the vendor's mtd2 is a gzip
# stream truncated to fit the 5 MB partition, so ~1 MB of the tail of the filesystem is
# simply absent (/bin/busybox, /mknod_console, most of /etc and /web come out empty).
# The stock image only boots because those files are re-supplied from the .tgz archives
# or never touched. We rebuild from the *live* rootfs dump (complete) + the four boot
# archives lifted out of the stock image (which do survive the truncation, verified with
# `gzip -t`), so the result is a correct, fully-CRC'd image with room to spare.
#
# Sources (all committed in dump/):
#   dump/livedump_20260723/rootfs.tar.gz  complete live rootfs (11 MB, no /dev, no /root/rec)
#   dump/mtd2_rootfs.bin                  stock flash image -> app.tgz/data.tgz/
#                                         tl_modules_r9508.tgz/kernel.tgz
#   device/dvr/dvr                        our DVR binary (built by device/dvr/build.sh)
#   flash/rootfs_overlay/                 files that override everything above
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WORK="${WORK:-$HOME/dvrflash}"
OUT="$HERE/out"

# --- content switches (see the size report at the end) ---------------------------
#  app.tgz/data.tgz = the stock DVR app (3.26 MB app.out + its data), 2.6 MB of the
#  5 MB partition and *already compressed*, so they cost that in flash 1:1. They are
#  only needed to run the vendor firmware. With INCLUDE_STOCK_APP=0 they are staged on
#  SATA instead (flash/sata/stock/) and boot.sh restores them on demand — the fallback
#  still works and the flash image drops to ~40% full.
INCLUDE_STOCK_APP="${INCLUDE_STOCK_APP:-0}"
#  /web = the dead ActiveX client this whole project replaces. ~1.1 MB, mostly PNG
#  (incompressible). Keep it only if you want the stock web server to still serve.
INCLUDE_WEB="${INCLUDE_WEB:-1}"

BLOCKS=13372          # stock: 13372 x 1024 B — the kernel ramdisk is sized for this
INODES=4096           # stock inode count
PARTSZ=$((5*1024*1024))

echo "=== 0. workspace $WORK ==="
mkdir -p "$WORK" "$OUT"
cd "$WORK"

echo "=== 1. unpack the stock flash image (for the four boot .tgz) ==="
python3 - "$REPO/dump/mtd2_rootfs.bin" <<'PY'
import sys, zlib
d = open(sys.argv[1], 'rb').read()
o = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(d[64:])
open('stock.ext2', 'wb').write(o)
print(f"    stock.ext2: {len(o)} bytes (truncated stream — expected)")
PY
rm -rf stock && mkdir stock
debugfs -R "rdump / stock" stock.ext2 >/dev/null 2>&1 || true
K=stock/lib/modules/2.6.24-rt1-hi3515v100
for f in stock/root/app.tgz stock/root/data.tgz stock/root/tl_modules_r9508.tgz "$K/kernel.tgz"; do
    gzip -t "$f" || { echo "FATAL: $f did not survive the stock truncation"; exit 1; }
    echo "    ok $(stat -c%s "$f") $f"
done

echo "=== 2. unpack the complete live rootfs ==="
rm -rf live && mkdir live
tar -xzf "$REPO/dump/livedump_20260723/rootfs.tar.gz" -C live
chmod -R u+rwX live

echo "=== 3. stage the new rootfs ==="
rm -rf stage && cp -a live stage
# runtime debris that must not be baked into flash
rm -rf stage/tmp/* stage/root/rec stage/root/DVR_CTRL_PORT.txt
# boot-time archives (rcS moves these to /tmp2 and unpacks them)
cp stock/root/tl_modules_r9508.tgz stage/root/
cp "$K/kernel.tgz" "stage/lib/modules/2.6.24-rt1-hi3515v100/"
mkdir -p stage/dev stage/proc stage/sys stage/tmp stage/tmp2 stage/mnt stage/lost+found
# /dev char nodes: staged as empty regular files, turned into device inodes in step 6.
# (debugfs's own `mknod` allocates the inode but never links it into the directory on a
# rev0 / no-filetype filesystem, so it silently produces an empty /dev — don't use it.)
for n in console ttyAMA0 ttyAMA1 ttyS000; do : > "stage/dev/$n"; done
if [ "$INCLUDE_STOCK_APP" = "1" ]; then
    cp stock/root/app.tgz stock/root/data.tgz stage/root/
    rm -rf stage/root/app.out stage/root/data      # they come back from the .tgz at boot
else
    rm -rf stage/root/app.out stage/root/data
    mkdir -p "$HERE/sata/stock"
    cp stock/root/app.tgz stock/root/data.tgz "$HERE/sata/stock/"
    echo "    stock app staged for SATA in flash/sata/stock/ (not in flash)"
fi
[ "$INCLUDE_WEB" = "1" ] || rm -rf stage/web

echo "=== 4. apply our overlay ==="
cp -a "$HERE/rootfs_overlay/." stage/
chmod 744 stage/root/run_app.sh
if [ -x "$REPO/device/dvr/dvr" ]; then
    echo "    (device/dvr/dvr exists but is NOT baked in — it lives on SATA, see flash/sata/)"
fi
# the SATA payload is a build product too: assemble it next to the image
rm -rf "$OUT/sata" && mkdir -p "$OUT/sata"
cp -a "$HERE/sata/." "$OUT/sata/"
[ -f "$REPO/device/dvr/dvr" ] && cp "$REPO/device/dvr/dvr" "$OUT/sata/dvr" && chmod +x "$OUT/sata/dvr"
[ -f "$REPO/device/dvr/dvr.conf.example" ] && cp "$REPO/device/dvr/dvr.conf.example" "$OUT/sata/dvr.conf"

echo "    staged: $(du -sh stage | cut -f1) in $(find stage | wc -l) entries"

echo "=== 5. build the ext2 (rev0, no features, 1 KiB blocks — matches stock) ==="
rm -f rootfs_new.ext2
mke2fs -q -F -r 0 -O none -I 128 -b 1024 -N "$INODES" \
       -d stage rootfs_new.ext2 "$BLOCKS"

echo "=== 6. fix ownership to root:root + create /dev nodes ==="
# mke2fs -d stamps the *builder's* uid/gid; the device is a single-root system so it
# would still work, but a rootfs that says uid 1000 is a trap for future debugging.
{
  find stage -printf '%P\n' | while read -r p; do
      [ -z "$p" ] && continue
      printf 'sif "/%s" uid 0\nsif "/%s" gid 0\n' "$p" "$p"
  done
  echo 'sif / uid 0'
  echo 'sif / gid 0'
  # /dev: only the four char nodes the stock image carries (S00devs/udev make the rest).
  # ext2 stores a device number in i_block[0] as (major<<8)|minor for the old <256 range,
  # so an empty regular file + a mode/rdev poke IS a device node.
  #   console c 5 1 -> 0x0501   ttyAMA0/ttyS000 c 204 64 -> 0xcc40   ttyAMA1 c 204 65 -> 0xcc41
  for spec in console:0x0501 ttyAMA0:0xcc40 ttyAMA1:0xcc41 ttyS000:0xcc40; do
      n=${spec%%:*}; rdev=${spec##*:}
      printf 'sif /dev/%s mode 020644\nsif /dev/%s block[0] %s\nsif /dev/%s links_count 1\n' \
             "$n" "$n" "$rdev" "$n"
  done
} > fixup.debugfs
debugfs -w -f fixup.debugfs rootfs_new.ext2 >/dev/null 2>&1
e2fsck -fy rootfs_new.ext2 >/dev/null 2>&1 || true      # -y: clean the dirty bit debugfs leaves

echo "=== 7. compress + wrap ==="
gzip -9 -n -c rootfs_new.ext2 > rootfs_new.gz
GZSZ=$(stat -c%s rootfs_new.gz)
MAX=$((PARTSZ - 64))
echo "    ext2 : $(stat -c%s rootfs_new.ext2) bytes"
echo "    gzip : $GZSZ bytes   (budget $MAX)"
if [ "$GZSZ" -gt "$MAX" ]; then
    echo "!!! TOO BIG for the 5 MB partition by $((GZSZ-MAX)) bytes."
    echo "!!! Re-run with INCLUDE_STOCK_APP=0 and/or INCLUDE_WEB=0."
    exit 1
fi
python3 "$HERE/mkuimage.py" rootfs_new.gz "$OUT/mtd2_new.bin" --pad "$PARTSZ"
cp rootfs_new.ext2 rootfs_new.gz "$OUT/"
md5sum "$OUT/mtd2_new.bin" | tee "$OUT/mtd2_new.bin.md5"

echo
echo "=== DONE ==="
echo "  flash image : $OUT/mtd2_new.bin  ($(stat -c%s "$OUT/mtd2_new.bin") bytes)"
echo "  SATA payload: $OUT/sata/  (copy to /root/rec/a1/ on the device)"
echo "  headroom    : $((MAX-GZSZ)) bytes unused in the partition"
echo "  next        : docs/FLASH.md"
