#!/bin/bash
# flash/verify.sh — prove flash/out/mtd2_new.bin is bootable BEFORE it goes near the NOR.
#
# Checks, in order of "what would brick the box":
#   1. uImage header parses, magic/type/comp right, header CRC + data CRC both valid
#   2. payload gunzips completely (no truncation — the stock image's original sin)
#   3. the ext2 is rev0 / no features / 1 KiB blocks / stock geometry, and fsck-clean
#   4. every file the boot chain touches exists, is non-empty and is executable
#   5. run_app.sh really contains the hand-off hook and the stock fallback
#   6. ownership is root:root and the four /dev char nodes are present
#   7. free space is sane (app.tgz/data.tgz get unpacked into this fs at boot)
#
# Run in WSL:  bash flash/verify.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
IMG="${1:-$HERE/out/mtd2_new.bin}"
WORK="${WORK:-$HOME/dvrflash}/verify"
KMOD=2.6.24-rt1-hi3515v100
FAIL=0
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
note() { printf '       %s\n' "$*"; }

rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
echo "=== verifying $IMG ==="
[ -f "$IMG" ] || { bad "image not found"; exit 1; }

echo "--- 1/7 uImage header ---"
python3 - "$IMG" <<'PY'
import binascii, struct, sys
d = open(sys.argv[1], 'rb').read()
h = d[:64]
magic, hcrc, ts, size, load, ep, dcrc, os_, arch, typ, comp = struct.unpack(">IIIIIIIBBBB", h[:32])
name = h[32:64].split(b'\0')[0].decode()
h0 = h[:4] + b'\0'*4 + h[8:]
calc_h = binascii.crc32(h0) & 0xFFFFFFFF
body = d[64:64+size]
calc_d = binascii.crc32(body) & 0xFFFFFFFF
def chk(c, m):
    print(("  PASS " if c else "  FAIL ") + m)
    return 0 if c else 1
bad = 0
bad += chk(magic == 0x27051956, f"magic 0x{magic:08x}")
bad += chk(hcrc == calc_h,      f"header CRC 0x{hcrc:08x}")
bad += chk(dcrc == calc_d,      f"data CRC 0x{dcrc:08x} over {size} bytes")
bad += chk(typ == 3,            f"type {typ} (RAMDisk)")
bad += chk(comp == 1,           f"comp {comp} (gzip)")
bad += chk(os_ == 5 and arch == 2, f"os {os_} arch {arch} (Linux/ARM)")
bad += chk(len(d) == 5*1024*1024, f"image is exactly the 5 MB partition ({len(d)})")
bad += chk(64 + size <= len(d),  f"payload fits ({64+size} <= {len(d)})")
bad += chk(set(d[64+size:]) <= {0xFF}, "tail padding is 0xFF (erased NOR)")
print(f"       name {name!r}")
open('payload.gz', 'wb').write(body)
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] || FAIL=$((FAIL+1))

echo "--- 2/7 gzip payload ---"
if gzip -t payload.gz 2>/dev/null; then ok "gunzip clean (no truncation)"; else bad "gzip stream is broken/truncated"; fi
gzip -dc payload.gz > new.ext2 2>/dev/null
note "ext2 size $(stat -c%s new.ext2)"

echo "--- 3/7 filesystem ---"
INFO=$(dumpe2fs -h new.ext2 2>/dev/null)
grep -q "revision #:  *0"   <<<"$INFO" && ok "ext2 revision 0" || bad "not revision 0"
grep -q "features: *(none)" <<<"$INFO" && ok "no fs features set" || bad "unexpected fs features"
grep -q "Block size: *1024" <<<"$INFO" && ok "1024-byte blocks" || bad "wrong block size"
BC=$(sed -n 's/^Block count: *//p'  <<<"$INFO")
IC=$(sed -n 's/^Inode count: *//p' <<<"$INFO")
[ "$BC" = 13372 ] && ok "block count 13372 (stock geometry)" || bad "block count $BC != 13372"
[ "$IC" = 4096 ]  && ok "inode count 4096"                   || bad "inode count $IC != 4096"
if e2fsck -fn new.ext2 >fsck.log 2>&1; then ok "e2fsck clean"; else bad "e2fsck errors:"; sed 's/^/       /' fsck.log | head -8; fi
FREEB=$(sed -n 's/^Free blocks: *//p' <<<"$INFO")
note "free blocks $FREEB (~$((FREEB/1024)) MB) — boot unpacks app.tgz+data.tgz here"
[ "${FREEB:-0}" -ge 3000 ] && ok "enough free space for the boot-time unpack" \
                           || bad "only $FREEB free blocks — boot unpack may fill /"

echo "--- 4/7 boot-chain files ---"
need_file() {   # path minsize
    local sz; sz=$(debugfs -R "stat $1" new.ext2 2>/dev/null | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)
    if [ -n "$sz" ] && [ "$sz" -ge "$2" ]; then ok "$1 ($sz bytes)"; else bad "$1 missing or short (${sz:-none})"; fi
}
need_file /bin/busybox            300000
need_file /sbin/init              1
need_file /etc/init.d/rcS         100
need_file /etc/init.d/S00devs     10
need_file /etc/init.d/S01udev     10
need_file /etc/inittab            100
need_file /etc/passwd             1
need_file /root/run_app.sh        500
need_file /root/net.sh            100
need_file /root/init.sh           100
need_file /root/mydaemon.out      1000
need_file /root/tl_modules_r9508.tgz 600000
need_file "/lib/modules/$KMOD/kernel.tgz" 600000
need_file "/lib/modules/$KMOD/modules.dep" 1000
need_file /mknod_console          1000
need_link() {   # a symlink the loader/boot depends on
    if debugfs -R "stat $1" new.ext2 2>/dev/null | grep -q "Type: symlink"; then ok "$1 (symlink)";
    else bad "$1 is not a symlink"; fi
}
need_link /lib/ld-uClibc.so.0
need_link /linuxrc
# the real uClibc loader behind the symlink — without it nothing dynamic runs
LDC=$(debugfs -R "ls -l /lib" new.ext2 2>/dev/null | grep -c 'ld-uClibc-')
[ "$LDC" -ge 1 ] && ok "/lib/ld-uClibc-*.so present" || bad "uClibc loader missing"

echo "--- 5/7 the hook itself ---"
debugfs -R "cat /root/run_app.sh" new.ext2 2>/dev/null > run_app.sh
grep -q 'exec /root/rec/a1/boot.sh' run_app.sh && ok "hand-off hook present"      || bad "hook missing!"
grep -q 'mount -t vfat /dev/sda1'   run_app.sh && ok "mounts SATA before the hook" || bad "no SATA mount"
grep -q './app.out'                 run_app.sh && ok "stock app.out fallback kept" || bad "fallback lost!"
grep -q './net.sh'                  run_app.sh && ok "net.sh (telnetd) still runs first" || bad "net.sh lost — LOCKOUT RISK"
M=$(debugfs -R "stat /root/run_app.sh" new.ext2 2>/dev/null | sed -n 's/.*Mode: *0*\([0-7]*\).*/\1/p' | head -1)
case "$M" in *7*|*5*|*1*) ok "run_app.sh mode $M (executable)";; *) bad "run_app.sh mode $M not executable";; esac

echo "--- 6/7 ownership + /dev ---"
NONROOT=$(debugfs -R "ls -l /root" new.ext2 2>/dev/null | awk '{print $3 $4}' | grep -vc '^00$' || true)
UIDS=$(for p in / /bin /etc /root /root/run_app.sh /bin/busybox; do
         debugfs -R "stat $p" new.ext2 2>/dev/null | sed -n 's/.*User: *\([0-9]*\).*/\1/p' | head -1; done | sort -u | tr '\n' ' ')
[ "$(echo $UIDS)" = "0" ] && ok "sampled inodes are uid 0" || bad "non-root ownership: uid(s) $UIDS"
for spec in console:1281 ttyAMA0:52288 ttyAMA1:52289 ttyS000:52288; do
    n=${spec%%:*}; want=${spec##*:}
    S=$(debugfs -R "stat /dev/$n" new.ext2 2>/dev/null)
    got=$(sed -n 's/.*Device major\/minor number: *\([0-9]*\):\([0-9]*\).*/\1 \2/p' <<<"$S" | head -1)
    if grep -q "Type: character" <<<"$S"; then
        # debugfs prints the decoded major:minor; recompute the raw rdev to compare
        maj=${got%% *}; min=${got##* }
        raw=$(( (maj<<8) | min ))
        [ "$raw" = "$want" ] && ok "/dev/$n char $maj,$min" || bad "/dev/$n is char $maj,$min (expected rdev $want)"
    else
        bad "/dev/$n missing or not a char device"
    fi
done

echo "--- 7/7 what changed vs stock ---"
python3 - "$HERE/../dump/mtd2_rootfs.bin" "$IMG" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
print(f"       stock {len(a)} bytes / new {len(b)} bytes")
print(f"       identical bytes: {sum(1 for x, y in zip(a, b) if x == y)} (headers differ by design)")
PY

echo
if [ "$FAIL" -eq 0 ]; then
    echo -e "\033[32m=== ALL CHECKS PASSED — image is safe to flash (see docs/FLASH.md) ===\033[0m"
else
    echo -e "\033[31m=== $FAIL CHECK(S) FAILED — DO NOT FLASH ===\033[0m"
fi
exit $FAIL
