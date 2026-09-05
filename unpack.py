#!/usr/bin/env python3
"""Parse U-Boot images from the flash dump and decompress the rootfs."""
import gzip
import hashlib
import os
import struct
import sys

DUMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dump")

UIMAGE_MAGIC = 0x27051956
ARCH = {2: "ARM"}
OS = {5: "Linux"}
IMG_TYPE = {2: "Kernel", 3: "RAMDisk", 5: "Multi"}
COMP = {0: "none", 1: "gzip", 2: "bzip2"}


def md5(b):
    return hashlib.md5(b).hexdigest()


def parse_uimage(data, label):
    if len(data) < 64:
        print(f"{label}: too small")
        return None
    magic, hcrc, htime, size, load, ep, dcrc, os_, arch, typ, comp = struct.unpack(
        ">IIIIIIIBBBB", data[:32]
    )
    if magic != UIMAGE_MAGIC:
        print(f"{label}: no uImage header (first4={data[:4].hex()})")
        return None
    name = data[32:64].split(b"\x00")[0].decode("latin-1")
    print(f"{label}: uImage")
    print(f"    name    : {name}")
    print(f"    type    : {IMG_TYPE.get(typ, typ)}  os={OS.get(os_, os_)} arch={ARCH.get(arch, arch)}")
    print(f"    comp    : {COMP.get(comp, comp)}")
    print(f"    datasize: {size} bytes")
    print(f"    load/ep : 0x{load:08x} / 0x{ep:08x}")
    return {"size": size, "comp": comp, "payload_off": 64, "name": name}


def scan_gzip(data):
    """Find gzip streams (magic 1f 8b 08) in a blob."""
    offs = []
    i = 0
    while True:
        i = data.find(b"\x1f\x8b\x08", i)
        if i < 0:
            break
        offs.append(i)
        i += 3
    return offs


def main():
    for fn, label in [
        ("mtd0_boot.bin", "BOOT"),
        ("mtd1_uImage.bin", "UIMAGE"),
        ("mtd2_rootfs.bin", "ROOTFS"),
    ]:
        path = os.path.join(DUMP, fn)
        if not os.path.exists(path):
            print(f"{label}: MISSING {path}")
            continue
        data = open(path, "rb").read()
        print(f"\n=== {label} ({fn}, {len(data)} bytes, md5={md5(data)}) ===")
        info = parse_uimage(data, label)
        print("    gzip streams at offsets:", [hex(o) for o in scan_gzip(data)[:8]])

        if label == "ROOTFS" and info and info["comp"] == 1:
            payload = data[64 : 64 + info["size"]]
            try:
                ext2 = gzip.decompress(payload)
            except Exception as e:
                print("    gunzip(payload) failed:", e, "- trying raw gzip scan")
                offs = scan_gzip(data)
                ext2 = gzip.decompress(data[offs[0]:]) if offs else b""
            out = os.path.join(DUMP, "rootfs.ext2")
            open(out, "wb").write(ext2)
            print(f"    -> decompressed ext2: {len(ext2)} bytes -> {out}")
            print(f"       ext2 md5={md5(ext2)}")
            # peek at superblock magic (0xEF53 at offset 1080)
            if len(ext2) > 1082:
                mg = struct.unpack("<H", ext2[1080:1082])[0]
                print(f"       ext2 magic @1080 = 0x{mg:04x} ({'OK ext2/3' if mg==0xEF53 else 'NOT ext2'})")


if __name__ == "__main__":
    main()
