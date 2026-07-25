# PROTOCOL2.md — our own lean DVR wire protocol (device ⇄ PC)

Replaces the stock 8670 TLV protocol for our custom DVR. Design goal: **minimal device-side
work** — the device already has encoded H.264 in hand, so it just fans raw frames out a socket;
all the smarts (browser bridging, MSE, UI) live on the PC. Implemented device-side in
`device/dvr/net.h` + the stream server in `device/dvr/dvr.c`.

## Live H.264 stream — TCP port 8091

The device runs a tiny non-blocking TCP server folded into the record loop (`net_init`/`net_poll`/
`net_fanout` in `dvr.c`). Because the encoder output is shared, streaming is just an extra consumer
of the same frames written to disk — near-zero added device cost.

**Handshake (v1, deliberately trivial):**
1. Client connects to `192.168.1.108:8091`.
2. Client sends **1 byte** = channel: ASCII `'0'`..`'3'` (or raw `0`..`3`).
3. Server streams that channel's **raw Annex-B H.264**, starting at the next **keyframe**
   (a GetStream batch whose first NAL is SPS type 7 or IDR type 5), so the decoder syncs cleanly.
   Every GOP begins with SPS+PPS+SEI+IDR in-band, so no out-of-band parameter sets are needed.

**Framing:** none — it's a raw Annex-B elementary stream (`00 00 00 01` start codes). The PC side
feeds it straight to jMuxer/MSE. Current encoder output: **352×240, H.264 Constrained Baseline,
30 fps** (from the stock VENC attr; made configurable in M1).

**Backpressure:** the server writes each frame non-blocking with a tiny (~6 ms) retry budget; a
viewer that can't keep up is dropped frame-wise (or disconnected) rather than stalling recording —
live latency is never held hostage to a slow client. Up to `MAXCLI` (**12**) concurrent viewers.

**Only encoded channels stream.** The firmware creates a VENC channel for the first
`enc_channels` channels (default **1** — all the bandwidth goes to the FPV camera; see
`dvr.conf`). Subscribing to any other channel is accepted by the socket and then never
produces a frame, while consuming one of the 12 client slots. Clients should read `enc=`
from the control port's `INFO` first; `webapp2` does, and refuses the rest up front.

**Lifecycle:** server up while `dvr` runs in record mode (`net_init` skipped in display-only mode).
Clients are closed on `dvr` exit (`net_shutdown`).

**Display aspect is NOT signalled — the client must apply it.** The SPS carries no VUI
aspect_ratio_idc, confirmed 2026-07-25 by decoding a live capture: ffmpeg reports
`704x240` with no SAR/DAR, where the same content remuxed to MP4 reports
`SAR 10:11 DAR 4:3`. So a player assumes square pixels and computes 2.93:1, which renders
the picture as a short, wide strip. Every capture mode this hardware produces (704×240 and
704×288 2CIF, 704×480, 352×240 CIF) is a **4:3** display frame — 704×240 is full-width,
single-field, so it needs a 2× vertical stretch. `webapp2` handles this by sizing each cell
4:3 and letting the video fill it (`object-fit:fill` in `style.css`); the recording path
does the equivalent with ffmpeg `-aspect 4:3`. Emitting proper VUI from the encoder would
fix it for every client at once and is the better long-term fix.

## Control (planned, not yet implemented)

A second small port (or the same one, pre-stream) for: start/stop recording, list segments, fetch a
recorded segment (range). Will be documented here when built. For now, control is via the existing
serial/telnet path during development.

## PC side

`webapp2/` — a thin Node proxy: opens the device TCP stream, relays frames to the browser over a
WebSocket; the browser decodes via jMuxer + MSE (reusing `webapp/public/` + `jmuxer.min.js`). Far
smaller than the stock-protocol `webapp/server.js` (no TLV `dvr/protocol.js`, no live-ffmpeg, no
telnet). Recorded-clip transcoding, if wanted, can still run on the PC.

## Verified

2026-07-23: PC connected to `:8091`, sent `'0'`, received a keyframe-aligned Annex-B stream that
ffmpeg decoded to 352×240 H.264 30fps (123 frames / 6 s). Device streamed while simultaneously
recording all 4 channels and driving the VGA display.
