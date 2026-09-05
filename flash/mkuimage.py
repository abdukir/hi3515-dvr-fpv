#!/usr/bin/env python3
"""mkuimage.py — wrap a payload in a U-Boot legacy image header (no `mkimage` needed).

The RR104P rootfs partition (mtd2) holds a uImage-wrapped gzip'd ext2 ramdisk:

    [64-byte uImage header][gzip stream]

Stock header (dump/mtd2_rootfs.bin):
    name  'rootfs_hs3515.ext2.gz.uboot'   os=5 (Linux)  arch=2 (ARM)
    type  3 (RAMDisk)  comp=1 (gzip)  load=0  ep=0
    NOTE: the stock image declares 6317967 payload bytes but only 5242816 fit in the
    5 MB partition — the vendor truncated the gzip stream and its data CRC is wrong.
    Images written by this script are *correct*: declared size == stored size and the
    data CRC matches, so `bootm` verifies cleanly either way.

Usage:
    mkuimage.py <payload> <out.uimg> [--name NAME] [--pad SIZE] [--type 3] [--comp 1]

`--pad SIZE` pads the finished image to SIZE bytes with 0xFF (erased NOR state), so the
result can be written straight over a whole mtd partition.
"""
import argparse
import binascii
import struct
import sys
import time

MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM = 2


def build(payload, name, load=0, ep=0, typ=3, comp=1, timestamp=None):
    if timestamp is None:
        timestamp = int(time.time())
    dcrc = binascii.crc32(payload) & 0xFFFFFFFF
    nm = name.encode()[:31].ljust(32, b"\x00")
    hdr = struct.pack(
        ">IIIIIIIBBBB",
        MAGIC, 0, timestamp, len(payload), load, ep, dcrc,
        IH_OS_LINUX, IH_ARCH_ARM, typ, comp,
    ) + nm
    hcrc = binascii.crc32(hdr) & 0xFFFFFFFF          # header CRC computed with hcrc == 0
    hdr = hdr[:4] + struct.pack(">I", hcrc) + hdr[8:]
    return hdr + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload")
    ap.add_argument("out")
    ap.add_argument("--name", default="rootfs_hs3515.ext2.gz.uboot")
    ap.add_argument("--pad", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--type", type=int, default=3)
    ap.add_argument("--comp", type=int, default=1)
    ap.add_argument("--load", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--ep", type=lambda s: int(s, 0), default=0)
    a = ap.parse_args()

    payload = open(a.payload, "rb").read()
    img = build(payload, a.name, a.load, a.ep, a.type, a.comp)
    if a.pad:
        if len(img) > a.pad:
            sys.exit(f"ERROR: image is {len(img)} bytes, does not fit in {a.pad}")
        img += b"\xff" * (a.pad - len(img))
    open(a.out, "wb").write(img)

    hdr = img[:64]
    _, hcrc, _, size, _, _, dcrc = struct.unpack(">IIIIIII", hdr[:28])
    print(f"{a.out}: {len(img)} bytes "
          f"(header 64 + payload {size} + pad {len(img) - 64 - size})")
    print(f"  name  : {a.name}")
    print(f"  hcrc  : 0x{hcrc:08x}   dcrc: 0x{dcrc:08x}")
    if a.pad:
        pct = 100.0 * (size + 64) / a.pad
        print(f"  fill  : {pct:.1f}% of the {a.pad} byte partition")


if __name__ == "__main__":
    main()
