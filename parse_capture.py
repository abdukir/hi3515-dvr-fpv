#!/usr/bin/env python3
"""Reconstruct and decode the DVR 8670 TLV protocol from a capture log.

Usage: py -3 parse_capture.py capture/port8670_conn1.log [--video]
"""
import re
import sys

path = sys.argv[1]
video = "--video" in sys.argv

# --- parse the hexdump log back into two ordered byte streams (per direction) ---
c2s = bytearray()
s2c = bytearray()
cur = None
for line in open(path, encoding="utf-8", errors="replace"):
    m = re.match(r"--- (C->S|S->C) conn#\d+ len=(\d+)", line.strip())
    if m:
        cur = c2s if m.group(1) == "C->S" else s2c
        continue
    hm = re.match(r"\s*[0-9a-f]{6}\s+((?:[0-9a-f]{2} )+)", line)
    if hm and cur is not None:
        cur.extend(bytes.fromhex(hm.group(1).replace(" ", "")))

TYPE = {0x0000: "REQ", 0x0002: "RSP", 0x0001: "EVT"}


def find_first_frame(buf):
    """Skip any preamble; return offset where a plausible length-framed msg starts."""
    for off in range(0, min(len(buf), 64)):
        if off + 4 <= len(buf):
            ln = int.from_bytes(buf[off:off + 4], "big")
            if 12 <= ln <= 4_000_000 and off + ln <= len(buf) + 4:
                # sanity: type field looks valid
                if off + 6 <= len(buf) and int.from_bytes(buf[off + 4:off + 6], "big") in TYPE:
                    return off
    return 0


def parse_frames(buf, label):
    off = find_first_frame(buf)
    if off:
        print(f"  [{label}] {off}-byte preamble: {buf[:off].hex()}")
    print(f"  [{label}] {len(buf)} bytes total")
    n = 0
    while off + 12 <= len(buf):
        ln = int.from_bytes(buf[off:off + 4], "big")
        if ln < 12 or off + ln > len(buf):
            print(f"  [{label}] @{off}: bad len {ln}, stopping ({len(buf)-off} bytes left)")
            break
        typ = int.from_bytes(buf[off + 4:off + 6], "big")
        cmd = int.from_bytes(buf[off + 6:off + 8], "big")
        seq = int.from_bytes(buf[off + 8:off + 10], "big")
        r = int.from_bytes(buf[off + 10:off + 12], "big")
        payload = buf[off + 12:off + ln]
        n += 1
        if not video or n <= 6 or len(payload) < 200:
            pv = payload[:48]
            asci = "".join(chr(x) if 32 <= x < 127 else "." for x in pv)
            print(f"  [{label}] {TYPE.get(typ,hex(typ))} cmd=0x{cmd:04x} seq=0x{seq:04x} r=0x{r:04x} "
                  f"plen={len(payload):<6} {pv.hex()[:64]}  {asci}")
        off += ln
    print(f"  [{label}] parsed {n} frames, {len(buf)-off} trailing bytes")
    return n


print(f"=== {path} ===")
print("C->S:")
parse_frames(c2s, "C>S")
print("S->C:")
parse_frames(s2c, "S>C")

# command histogram
def hist(buf):
    off = find_first_frame(buf); h = {}
    while off + 12 <= len(buf):
        ln = int.from_bytes(buf[off:off+4], "big")
        if ln < 12 or off+ln > len(buf): break
        cmd = int.from_bytes(buf[off+6:off+8], "big")
        typ = int.from_bytes(buf[off+4:off+6], "big")
        if typ == 0x0000:
            h[cmd] = h.get(cmd, 0)+1
        off += ln
    return h
print("\nC->S command histogram:", {hex(k): v for k, v in sorted(hist(c2s).items())})
