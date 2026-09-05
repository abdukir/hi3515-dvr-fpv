#!/usr/bin/env python3
"""Minimal ext2 (rev 0/1, 1K block) extractor - no mounting, pure Python.

Walks the inode tree from root and writes files/dirs/symlinks to an output
directory. Flags any file whose data blocks fall in a truncated region.
"""
import os
import struct
import sys

IMG = sys.argv[1] if len(sys.argv) > 1 else r"D:\Documents\Work\DVR\dump\rootfs.ext2"
OUT = sys.argv[2] if len(sys.argv) > 2 else r"D:\Documents\Work\DVR\dump\rootfs_x"
REAL_LEN = int(sys.argv[3]) if len(sys.argv) > 3 else None  # bytes actually present

data = open(IMG, "rb").read()
if REAL_LEN is None:
    REAL_LEN = len(data)

# --- superblock ---
sb = data[1024:1024 + 1024]
s_inodes_count = struct.unpack("<I", sb[0:4])[0]
s_blocks_count = struct.unpack("<I", sb[4:8])[0]
s_first_data_block = struct.unpack("<I", sb[20:24])[0]
s_log_block_size = struct.unpack("<I", sb[24:28])[0]
s_blocks_per_group = struct.unpack("<I", sb[32:36])[0]
s_inodes_per_group = struct.unpack("<I", sb[40:44])[0]
s_magic = struct.unpack("<H", sb[56:58])[0]
s_rev_level = struct.unpack("<I", sb[76:80])[0]
s_inode_size = struct.unpack("<H", sb[88:90])[0] if s_rev_level >= 1 else 128
BS = 1024 << s_log_block_size
assert s_magic == 0xEF53, "not ext2"

n_groups = (s_blocks_count + s_blocks_per_group - 1) // s_blocks_per_group
print(f"ext2 rev{s_rev_level} bs={BS} groups={n_groups} inodes={s_inodes_count} "
      f"inode_size={s_inode_size} real_bytes={REAL_LEN}/{len(data)}")

truncated_files = []


def block(n):
    off = n * BS
    return data[off:off + BS]


def block_present(n):
    return n == 0 or (n + 1) * BS <= REAL_LEN


# --- block group descriptors: start in block right after superblock ---
bgd_block = s_first_data_block + 1
bgd = data[bgd_block * BS: bgd_block * BS + n_groups * 32]
inode_tables = []
for g in range(n_groups):
    e = bgd[g * 32:g * 32 + 32]
    bg_inode_table = struct.unpack("<I", e[8:12])[0]
    inode_tables.append(bg_inode_table)


def read_inode(ino):
    g = (ino - 1) // s_inodes_per_group
    idx = (ino - 1) % s_inodes_per_group
    off = inode_tables[g] * BS + idx * s_inode_size
    raw = data[off:off + s_inode_size]
    i_mode = struct.unpack("<H", raw[0:2])[0]
    i_size = struct.unpack("<I", raw[4:8])[0]
    i_links = struct.unpack("<H", raw[26:28])[0]
    i_blocks = struct.unpack("<I", raw[28:32])[0]
    blocks = list(struct.unpack("<15I", raw[40:40 + 60]))
    return {"mode": i_mode, "size": i_size, "links": i_links,
            "iblocks": i_blocks, "blocks": blocks, "raw": raw}


def iter_indirect(bnum, level, out, needed):
    if bnum == 0 or len(out) * BS >= needed:
        return
    if not block_present(bnum):
        out.append(("MISSING", bnum))
        return
    ptrs = struct.unpack(f"<{BS // 4}I", block(bnum))
    for p in ptrs:
        if len(out) * BS >= needed:
            break
        if level == 1:
            out.append(("B", p))
        else:
            iter_indirect(p, level - 1, out, needed)


def file_data(inode, path):
    size = inode["size"]
    out = []
    b = inode["blocks"]
    for i in range(12):
        if len(out) * BS >= size:
            break
        out.append(("B", b[i]))
    iter_indirect(b[12], 1, out, size)
    iter_indirect(b[13], 2, out, size)
    iter_indirect(b[14], 3, out, size)
    buf = bytearray()
    missing = False
    for kind, bn in out:
        if kind == "MISSING":
            missing = True
            break
        if bn == 0:
            buf += b"\x00" * BS
        elif block_present(bn):
            buf += block(bn)
        else:
            missing = True
            break
    if missing:
        truncated_files.append(path)
    return bytes(buf[:size])


def read_dir(inode):
    raw = file_data(inode, "<dir>")
    entries = []
    off = 0
    while off < len(raw):
        if off + 8 > len(raw):
            break
        e_ino, rec_len, name_len, ftype = struct.unpack("<IHBB", raw[off:off + 8])
        if rec_len == 0:
            break
        name = raw[off + 8: off + 8 + name_len].decode("latin-1")
        if e_ino != 0 and name not in (".", ".."):
            entries.append((name, e_ino, ftype))
        off += rec_len
    return entries


count = {"dir": 0, "file": 0, "link": 0, "other": 0}


def walk(ino, relpath):
    inode = read_inode(ino)
    mode = inode["mode"]
    typ = mode & 0xF000
    dst = os.path.join(OUT, relpath) if relpath else OUT
    if typ == 0x4000:  # dir
        count["dir"] += 1
        os.makedirs(dst, exist_ok=True)
        for name, cino, ft in read_dir(inode):
            walk(cino, os.path.join(relpath, name) if relpath else name)
    elif typ == 0x8000:  # regular file
        count["file"] += 1
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(file_data(inode, relpath))
    elif typ == 0xA000:  # symlink
        count["link"] += 1
        if inode["size"] < 60 and inode["iblocks"] == 0:
            target = bytes(struct.pack("<15I", *inode["blocks"]))[:inode["size"]].decode("latin-1")
        else:
            target = file_data(inode, relpath).decode("latin-1")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst + ".symlink", "w") as f:
            f.write(target)
    else:
        count["other"] += 1  # device nodes, fifos, sockets - skip content


os.makedirs(OUT, exist_ok=True)
walk(2, "")  # root inode = 2
print("extracted:", count)
if truncated_files:
    print(f"\n{len(truncated_files)} file(s) hit the truncated region (blocks beyond {REAL_LEN}):")
    for p in truncated_files:
        print("   ", p)
else:
    print("no files hit the truncated region - all file data is intact.")
