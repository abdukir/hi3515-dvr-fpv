'use strict';
// Parse the DVR recording index (index00.bin / index01.bin) into recorded segments,
// and map a segment to the exact .ifv file + byte range that holds it.
//
// Reverse-engineered + verified live (see docs/IFV_FORMAT.md):
//  - The index holds 16-byte records scattered at 4-byte alignment:
//      [start_ts u32le][end_ts u32le][start_off u32le][end_off u32le]
//    ts = unix epoch (seconds); off = GLOBAL byte offset into the channel's
//    concatenated .ifv stream (fly00000.ifv, fly00001.ifv, ... each IFV_SIZE bytes).
//  - Records appear both in the 4 KB block bodies (0x1000+) and in block-trailer
//    slots (0x?fe0); both are real. Circular-buffer reuse leaves some stale entries
//    whose byte tail was overwritten, so entries are deduped by start offset (keeping
//    the widest range) and any decode simply stops at the first corrupt frame.
//  - A segment dd'd at its start_off decodes directly with `ffmpeg -f ifv` — the
//    demuxer resyncs from a segment boundary, so NO .ifv header splice is needed.
//    (Arbitrary mid-segment offsets do NOT probe, hence segment-level granularity.)

const IFV_SIZE = 128 * 1024 * 1024;          // 134217728 — every fly000NN.ifv is exactly this
const TS_MIN = 0x60000000, TS_MAX = 0x80000000; // plausible unix window (~2021..2038)
const MAX_SEG_SECONDS = 24 * 3600;           // reject implausibly long records

function fileName(fileIndex) {
  return 'fly' + String(fileIndex).padStart(5, '0') + '.ifv';
}

// Parse one index buffer into raw records. `totalBytes` bounds valid offsets.
function parseIndex(buf, totalBytes) {
  totalBytes = totalBytes || 224 * IFV_SIZE;
  const out = [];
  if (!buf || buf.length < 0x30) return out;
  for (let o = 0x20; o + 16 <= buf.length; o += 4) {
    const t1 = buf.readUInt32LE(o), t2 = buf.readUInt32LE(o + 4);
    if (t1 <= TS_MIN || t1 >= TS_MAX || t2 < t1 || t2 - t1 > MAX_SEG_SECONDS) continue;
    const o1 = buf.readUInt32LE(o + 8), o2 = buf.readUInt32LE(o + 12);
    if (o1 >= o2 || o2 > totalBytes || o2 - o1 > totalBytes) continue;
    out.push({ startTs: t1, endTs: t2, startOff: o1, endOff: o2 });
  }
  return out;
}

// Merge records from one or more index buffers into deduped, time-sorted segments.
// Dedup key = start offset (global, effectively unique per real segment); when two
// records share it, keep the one covering the widest byte range.
function mergeSegments(records) {
  const byStart = new Map();
  for (const r of records) {
    const ex = byStart.get(r.startOff);
    if (!ex || r.endOff > ex.endOff) byStart.set(r.startOff, r);
  }
  return [...byStart.values()]
    .sort((a, b) => a.startTs - b.startTs || a.startOff - b.startOff)
    .map((r) => ({ ...r, dur: r.endTs - r.startTs, bytes: r.endOff - r.startOff, ...segRange(r) }));
}

// Map a segment's global byte range to a concrete .ifv file + local byte range.
// A segment that crosses a 128 MB file boundary is clamped to the end of its start
// file (cross-file spans are rare; the tail can be fetched as a follow-up segment).
function segRange(seg) {
  const fileIndex = Math.floor(seg.startOff / IFV_SIZE);
  const localStart = seg.startOff - fileIndex * IFV_SIZE;
  const endFile = Math.floor((seg.endOff - 1) / IFV_SIZE);
  const spansFiles = endFile !== fileIndex;
  const localEnd = spansFiles ? IFV_SIZE : seg.endOff - fileIndex * IFV_SIZE;
  return { fileIndex, file: fileName(fileIndex), localStart, localEnd, length: localEnd - localStart, spansFiles };
}

// Build a byte-exact busybox `dd` command for a local byte range. Old busybox dd has
// no skip_bytes, so pick the largest power-of-two block size that divides the start
// offset exactly (guaranteeing an exact segment-boundary start), then read in blocks.
// Over-reads at most (blk-1) bytes at the tail, which ffmpeg ignores.
function ddCommand(ifvPath, localStart, length, port) {
  let blk = 65536;
  while (blk > 1 && localStart % blk !== 0) blk >>= 1;
  const skip = localStart / blk;
  const count = Math.ceil(length / blk);
  return `dd if=${ifvPath} bs=${blk} skip=${skip} count=${count} 2>/dev/null | nc -l -p ${port}`;
}

module.exports = { IFV_SIZE, parseIndex, mergeSegments, segRange, ddCommand, fileName };
