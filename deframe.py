#!/usr/bin/env python3
"""De-frame the DVR media stream -> clean H.264 elementary stream.

Container:  [8-byte stream prefix]  then per frame:
            [11-byte packet header (starts 0x62 0x19)]
            [12-byte frame header: LEN u32be, FRAMENO u32be, TS u32be]
            [LEN bytes of Annex-B H.264]
"""
import re
import sys

def load_s2c(path):
    s2c = bytearray(); cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"--- (C->S|S->C) conn#\d+ len=(\d+)", line.strip())
        if m: cur = (m.group(1) == "S->C"); continue
        hm = re.match(r"\s*[0-9a-f]{6}\s+((?:[0-9a-f]{2} )+)", line)
        if hm and cur: s2c.extend(bytes.fromhex(hm.group(1).replace(" ", "")))
    return bytes(s2c)


def deframe(d):
    """Return (h264_bytes, frame_list). Robust to resync via the 62 19 marker."""
    out = bytearray(); frames = []
    # locate first packet header
    off = d.find(b"\x62\x19")
    while off >= 0 and off + 23 <= len(d):
        # frame header follows the 11-byte packet header
        fh = off + 11
        LEN = int.from_bytes(d[fh:fh + 4], "big")
        FN = int.from_bytes(d[fh + 4:fh + 8], "big")
        TS = int.from_bytes(d[fh + 8:fh + 12], "big")
        payload = d[fh + 12:fh + 12 + LEN]
        if LEN <= 0 or LEN > 2_000_000 or not payload.startswith(b"\x00\x00\x00\x01"):
            nxt = d.find(b"\x62\x19", off + 2)     # resync
            if nxt < 0: break
            off = nxt; continue
        out += payload
        frames.append((FN, TS, LEN))
        off = fh + 12 + LEN
        # next packet header should start with 62 19; tolerate small drift
        if d[off:off + 2] != b"\x62\x19":
            nxt = d.find(b"\x62\x19", off)
            if nxt < 0 or nxt - off > 64: break
            off = nxt
    return bytes(out), frames


if __name__ == "__main__":
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.replace(".log", ".h264")
    d = load_s2c(src)
    h264, frames = deframe(d)
    open(dst, "wb").write(h264)
    fns = [f[0] for f in frames]
    tss = [f[1] for f in frames]
    print(f"frames={len(frames)}  h264={len(h264)} bytes -> {dst}")
    if frames:
        print(f"  frameno range {fns[0]}..{fns[-1]}  contiguous={fns==list(range(fns[0],fns[-1]+1))}")
        if len(tss) > 1:
            deltas = [tss[i+1]-tss[i] for i in range(len(tss)-1)]
            avg = sum(deltas)/len(deltas)
            print(f"  avg TS delta={avg:.1f} ms -> ~{1000/avg:.1f} fps")
