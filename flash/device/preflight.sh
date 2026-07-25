#!/bin/sh
# flash/device/preflight.sh — run ON THE DVR, before flashing anything.
#
# Proves the image that is about to be written is intact *on the device*, using only what
# the device has. Specifically it re-does, with the DEVICE's own gunzip and md5sum, the
# whole unwrap the bootloader+kernel will do at boot:
#
#     mtd2_new.bin  --strip 64B uImage header-->  gzip stream  --gunzip-->  ext2 ramdisk
#
# and then checks the ext2 superblock fields the kernel actually reads.
#
# NOT tested here: loop-mounting the image. This kernel (2.6.24-rt1-hi3515v100) has no
# loop block driver — `/proc/devices` has no `loop` and only `cryptoloop.ko` ships in
# kernel.tgz — so a chroot test is impossible on-device. That coverage comes from
# flash/verify.sh on the PC, which walks the same filesystem with debugfs (a complete
# ext2 implementation) and checks every file the boot chain touches.
#
# Deploy:  put mtd2_new.bin + this script in /root/rec/a1/
# Run:     sh /root/rec/a1/preflight.sh /root/rec/a1/mtd2_new.bin [expected-ext2-md5]
set -u
IMG="${1:-/root/rec/a1/mtd2_new.bin}"
WANT_EXT2="${2:-}"
D=`dirname "$IMG"`
EXT="$D/rootfs_from_bin.ext2"
EXPECT_SIZE=13692928          # stock geometry: 13372 blocks x 1024 B
FAIL=0
ok()  { echo "  PASS $*"; }
bad() { echo "  FAIL $*"; FAIL=`expr $FAIL + 1`; }

# Little-endian field readers. busybox 1.1.2's `od` has no -A/-j/-N/-t (see CLAUDE.md),
# so seek with dd and let `od -d` print unsigned 16-bit decimal words. Leading zeros are
# stripped by awk's +0.
le16() { dd if="$2" bs=1 skip="$1" count=2 2>/dev/null | od -d | head -1 | awk '{print $2+0}'; }
le32() { dd if="$2" bs=1 skip="$1" count=4 2>/dev/null | od -d | head -1 | awk '{printf "%d", ($2+0)+($3+0)*65536}'; }
u8()   { dd if="$2" bs=1 skip="$1" count=1 2>/dev/null | od -d | head -1 | awk '{print $2+0}'; }

echo "=== preflight — $IMG ==="
[ -f "$IMG" ] || { echo "image not found"; exit 1; }
SZ=`wc -c < "$IMG"`
echo "  size $SZ bytes   md5 `md5sum "$IMG" | cut -d' ' -f1`"
[ "$SZ" -eq 5242880 ] && ok "exactly the 5 MB mtd2 partition" || bad "wrong size for mtd2 ($SZ)"

echo "--- 1/5 uImage header ---"
head -c 4 "$IMG" | hexdump -C | grep -q "27 05 19 56" && ok "uImage magic" || bad "not a uImage"
# ih_size is big-endian at offset 12
DECL=`head -c 16 "$IMG" | hexdump -C | head -1 | cut -c40-51 | tr -d ' '`
echo "       declared payload size (be32): 0x$DECL"
TYP=`u8 30 "$IMG"`
CMP=`u8 31 "$IMG"`
[ "$TYP" = "3" ] && ok "image type 3 (RAMDisk)" || bad "image type $TYP (expected 3)"
[ "$CMP" = "1" ] && ok "compression 1 (gzip)"   || bad "compression $CMP (expected 1)"

echo "--- 2/5 the DEVICE's own gunzip unwraps it completely ---"
rm -f "$EXT"
dd if="$IMG" bs=64 skip=1 2>/dev/null | gunzip > "$EXT" 2>/dev/null
ESZ=`wc -c < "$EXT" 2>/dev/null`
if [ -z "$ESZ" ]; then bad "gunzip produced nothing"; exit 1; fi
[ "$ESZ" -eq "$EXPECT_SIZE" ] && ok "ext2 is exactly $EXPECT_SIZE bytes (nothing truncated)" \
                              || bad "ext2 is $ESZ bytes, expected $EXPECT_SIZE"
EMD5=`md5sum "$EXT" | cut -d' ' -f1`
echo "       ext2 md5 $EMD5"
if [ -n "$WANT_EXT2" ]; then
    [ "$EMD5" = "$WANT_EXT2" ] && ok "matches the ext2 built on the PC" \
                               || bad "does NOT match the PC's ext2 ($WANT_EXT2)"
else
    echo "       (pass the PC's rootfs_new.ext2 md5 as arg 2 to compare end-to-end)"
fi

echo "--- 3/5 ext2 superblock (the fields the kernel reads) ---"
MAGIC=`le16 1080 "$EXT"`
REV=`le32 1100 "$EXT"`   # s_rev_level is at sb+0x4C, i.e. byte 1100
LOGBS=`le32 1048 "$EXT"`
BLOCKS=`le32 1028 "$EXT"`
INODES=`le32 1024 "$EXT"`
FREEB=`le32 1036 "$EXT"`
[ "$MAGIC" = "61267" ] && ok "magic 0xEF53"                  || bad "magic $MAGIC (not ext2)"
[ "$REV" = "0" ]       && ok "revision 0 (as stock)"          || bad "revision $REV (stock is 0)"
[ "$LOGBS" = "0" ]     && ok "block size 1024"                || bad "log_block_size $LOGBS (stock 0 = 1024)"
[ "$BLOCKS" = "13372" ]&& ok "13372 blocks (stock geometry)"  || bad "block count $BLOCKS"
[ "$INODES" = "4096" ] && ok "4096 inodes"                    || bad "inode count $INODES"
echo "       free blocks $FREEB (boot unpacks app.tgz/data.tgz into this fs)"

echo "--- 4/5 the SATA payload the new run_app.sh will hand off to ---"
[ -x "$D/boot.sh" ] && ok "$D/boot.sh present and executable" \
                    || bad "$D/boot.sh missing — the box would boot the stock app.out"
[ -x "$D/dvr" ]     && ok "$D/dvr present and executable" \
                    || bad "$D/dvr missing — boot.sh would fall back to app.out"
[ -f "$D/dvr.conf" ] && ok "$D/dvr.conf present" || echo "       (no dvr.conf — built-in defaults)"
grep -q 'exec /root/rec/a1/boot.sh' "$D/boot.sh" 2>/dev/null && true   # no-op, boot.sh is the target

echo "--- 5/5 current flash, for the record ---"
echo "       mtd2 now: `dd if=/dev/mtdblock2 bs=64k 2>/dev/null | md5sum | cut -d' ' -f1`"
echo "       (dump/mtd2_rootfs.bin on the PC is 9392700569cead3e87726f2d8e71a3ce)"

rm -f "$EXT"
echo
if [ "$FAIL" -eq 0 ]; then echo "=== PREFLIGHT PASSED — safe to run flash_mtd2.sh ==="
else echo "=== $FAIL PREFLIGHT FAILURE(S) — DO NOT FLASH ==="; fi
exit $FAIL
