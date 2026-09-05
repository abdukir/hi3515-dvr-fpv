# `.ifv` recording container + index format (RR104P / Hi3515)

Reverse-engineered and verified live on 2026-07-23 (device `192.168.1.108`). This is
what the webapp's index-based clip seek relies on (`webapp/dvr/ifvindex.js`).

## On-disk layout

Each channel records to its own SATA partition, mounted `/root/rec/a{1..4}` (ch N → `a(N+1)`):

```
/root/rec/aN/
  fly00000.ifv .. fly00223.ifv   circular buffer, each EXACTLY 128 MiB (134217728 B), pre-allocated
  index00.bin, index01.bin       recording index (time -> byte offset); the two are kept in sync
  log.txt (+ log00/log01.txt)    per-event record log (coarse; parsed by dvr/recordings.js)
```

The `.ifv` files form one logical **concatenated stream** per partition. A byte offset in
the index is **global**: `fileIndex = off / 128MiB`, `localOffset = off % 128MiB`,
`file = fly<fileIndex:05d>.ifv`. Unwritten regions of a pre-allocated file are junk
(`75 d7 5d …`), so file size is not a measure of recorded content — the index is.

## `.ifv` container

A file begins with a header (GUIDs + ASCII `"This is h.264 file ver 1"`, start-timestamp
at `+0x34` as unix-LE, resolution at `+0x5c` = `704x480` LE `u16 w,h`, `"H264"`), followed
by an interleaved video-ES + per-frame table region. The internal frame framing is **NOT**
the same as the live 8670 stream (the live `62 19` packet layout does not apply here). A
per-frame table uses 28-byte records `[ts u32le][const 0x00065741][…][offset u32le][len u32le]`
with `ts` advancing ~33 ms/frame (30 fps).

**We do not parse the container ourselves.** ffmpeg has a native `ifv` demuxer
(`-f ifv`) that decodes it directly. Crucial verified property: **the demuxer resyncs
from a segment boundary and needs no file header** — a range dd'd from a segment's
`start_off` (mid-file, no header) decodes cleanly. Arbitrary mid-segment offsets do
*not* probe, so seek granularity is **per index segment**, not per byte.

## Index format (`index00.bin` / `index01.bin`)

Fixed-size file (~1.77 MiB, pre-allocated). 32-byte header, then records. The first
u32le of the header is a record counter (increments as segments are written).

A **segment record** is 16 bytes, found at 4-byte alignment throughout the file:

```
+0  start_ts  u32le   unix epoch (seconds)
+4  end_ts    u32le   unix epoch (seconds)
+8  start_off u32le   global byte offset into the .ifv stream (segment start = decodable boundary)
+12 end_off   u32le   global byte offset (segment end)
```

Records appear both in 4 KiB block bodies (`0x1000+`, 32-byte stride) and in block-trailer
slots (`0x?fe0`); **both are real footage** (verified: each decodes to video matching its
`end_ts - start_ts` duration). Empty slots are zero / `0xffff`.

Because the buffer is circular, stale records can survive whose byte tail was later
overwritten by a newer recording, and coarse "summary" records can duplicate a start
offset with a different end. Extraction handles this by:
- validating each record (`start_ts`/`end_ts` in a plausible unix window, `end_ts >= start_ts`,
  duration < 24 h, `start_off < end_off <= totalBytes`), and
- **deduping by `start_off`, keeping the widest byte range** (global start offsets are
  effectively unique per real segment).

A continuous recording is a **single, growing** segment record (a 13-minute test produced
one record whose `end_off` extended over time), so segments can be large; the clip pull is
capped (`CLIP_CAP_MB`, default 64) and ffmpeg decodes from the segment start.

## Clip retrieval + frame-accurate seek (implemented)

`GET /api/recordings?ch=N` → `{ segments[], spans[], files[] }` — segments come from the
index (exact time + byte offsets), spans from `log.txt` (independent cross-check).

`GET /api/clip?ch=N&start=<start_off>&end=<end_off>&t=<seconds>` returns a browser-playable
MP4 that begins **`t` seconds into the segment** (default 0). ffmpeg does the seek itself —
the container's internal index makes its `ifv` demuxer's `-ss` **frame-accurate** (verified:
`-ss 600` lands on the exact OSD second). Efficiency comes from a range-proxy:

- **`GET /api/ifvstream?ch&file&base=<localStart>&len=<segLen>`** — a seekable HTTP view of
  the on-device `.ifv` segment. On each `Range` request it runs a **backgrounded** device
  `dd | nc -l &` for exactly that byte window (block-aligned dd + head-remainder dropped in
  Node = byte-exact start) and streams it back; when the client disconnects we drop the
  socket, killing that dd|nc. Backgrounding matters: ffmpeg opens the header read and the
  seek-target read as **overlapping** connections, so both device listeners must be up at
  once — a foreground `nc -l` would block the single telnet command queue and the second
  connect would get ECONNREFUSED.
- `prepareClip()` runs `ffmpeg -ss <t> -f ifv -i <ifvstream-url> -t <CLIP_WINDOW_SEC> …` →
  MP4 (faststart), cached by `(ch,start,end,t)`, HTTP-Range served. ffmpeg fetches only the
  header/index (~450 KB) + the bytes at the seek point — no full-segment pull, regardless of
  recording size.

The player's scrubber spans the whole segment: seeking within the loaded window is instant
(browser), seeking outside reloads the clip at the new `t`.

## Open / future

- **Cross-file segments** (a span crossing a 128 MiB boundary) are currently clamped to the
  end of the start file; the tail would be a follow-up range. Not yet observed in practice.
- `fileIndex = off / 128MiB` is verified within `fly00000.ifv`; the cross-file mapping is
  inferred from the concatenated-stream design and not yet tested across a boundary.
- The 28-byte per-frame table (`ts`, `offset`, `len`) is partially decoded but unused — the
  ffmpeg `ifv` demuxer already seeks accurately, so we never needed to parse it ourselves.
