---
name: dvr-hi3515-project
description: "The RR104P Hi3515 DVR reverse-engineering / FPV-repurposing project — hardware, access, dump state"
metadata: 
  node_type: memory
  type: project
  originSessionId: 76e1abb0-6be7-4c89-8239-e98f51f54621
  modified: 2026-07-23T06:04:48.111Z
---

Repurposing an old 4-ch analog CCTV DVR (NetDVR **RR104P**, fw `V4.0.0-JMS502 2011`)
into an FPV analog recorder. Work dir `D:\Documents\Work\DVR`. Full details in `README.md` there.

- Platform: HiSilicon **Hi3515 v100** (ARMv5), 8 MB NOR (mapped @0x80000000), Linux 2.6.24 + BusyBox 1.1.2, gzip-ext2 **RAM-disk rootfs** (/ is NOT persistent — must reflash mtd2 to persist).
- Analog capture = **TW2866** (`tw286x_R9508.ko`); H.264 HW encoder; SATA SSD → `/root/rec/a1..a4`.
- Access: DVR at `192.168.1.108` (default net.sh=.114). **telnet root / blank password** = root shell. Serial COM21 @115200 auto-logs-in root. Watchdog: run `closewd` at app `[user]#` prompt before living in shell.
- Boot: init→rcS→run_app.sh→{net.sh, init.sh(load3515 -i), unpack app.tgz→app.out, mydaemon.out, app.out}. `run_app.sh` has a `read -t1` "run app.out y/n" hook.
- U-Boot 2011.06 stripped (no `boot`/`run`). To boot: paste 3 `cp.b` lines + `bootm 0x80100000 0xc1500000` ONE AT A TIME (drops UART input during copies).

**Dump state (DONE, md5-verified vs device):** all 3 mtd partitions in `dump/`; complete rootfs extracted to `dump/live_x/`; `app.out` + all `.ko` in `dump/rootfs_x/root/`. See [[dvr-workflow-tools]].

**GOAL (finalized):** build a **modern browser-based web UI** (live H.264 streaming in-browser + recording file download + full control/config) to replace the dead ActiveX/old-IE web UI. NOT a desktop client. Ground-station use: DVR→screen VGA, PC→screen HDMI; control DVR from PC over Ethernet.

**Architecture (recommended, PC-hosted):** PC backend service speaks the DVR's 8670 TLV control + media protocol → reimplements needed `NETDVR_*` calls, remuxes/transcodes H.264 to browser (MSE/HLS via ffmpeg on PC), serves modern web UI + file downloads. DVR stays 100% stock (no firmware risk, keeps recording). Device-hosting rejected (ARM926/43MB too weak).

**Protocol facts:** control port 8670 (`UDServerPort`), separate `MediaPort` for H.264, `StreamType`=main/sub. No RTSP/ONVIF/CGI — proprietary TLV only. The ActiveX client (`dump/activex_x/CAB/`: `NetDvr2.dll`=protocol core, `TLNetDvr.ocx`=API, `avcdec.dll`=H264 dec, `TLPlay.dll`) fully implements it. `NetDvr2.dll` exports the whole API: NETDVR_createDVR, StartRealPlay/StopRealPlay, fastPlay, Set/GetRecordState, Set/GetRecordSCH, Get/SetVideoProperty, Get/SetSubStreamParam, PtzControl, GetDeviceInfo, user mgmt, formatHdd, etc.

**PROTOCOL DECODED + PIPELINE PROVEN (done).** Full spec in `PROTOCOL.md`. Captured via `capture_proxy.py` + serving patched web UI from PC (`webserve/`, ServerIP forced to 127.0.0.1) on :8080; user loaded it in Chrome+ActiveX → logs in `capture/port8670_connN.log`. Control = TLV frames `[len][type][cmd][seq][0100][payload]` (login 0x2711 admin/000000, GetDeviceInfo 0x2713, config gets, event 0x4e27). Media = 8670 too; container `[8B prefix][per-frame: 11B pkt hdr(62 19..)][12B: LEN,FRAMENO,TS][LEN bytes Annex-B H264]`. `deframe.py` extracts clean H264 (1139 frames contiguous, 30fps, decodes in ffmpeg zero errors, real frame shows OSD "CH 02"). Sub-stream 352x240 CIF NTSC. ffmpeg via pip `imageio-ffmpeg` (no PATH ffmpeg): path in imageio_ffmpeg.get_ffmpeg_exe().

**WEB UI WORKING (live view done).** `webapp/` — Node backend (`node server.js` → http://localhost:8090). `webapp/dvr/protocol.js` (control+login, verified logs into real DVR), `webapp/dvr/mediastream.js` (streaming H264 de-framer), `webapp/server.js` (HTTP+WS, one media conn per ch), `webapp/public/` (jMuxer MSE grid, no ActiveX). Verified LIVE: pulls all 4 ch, 704x480 main/352x240 sub, decodes zero errors, live OSD timestamp confirms realtime. Media req = 100B: magic + `01 02 00 00 00 <ch> <stream>` (ch 0-3, stream 0=main). Login payload = user[12]+pass[12]+mac[18]+clientIP[4], pass=`000000`. Node is v14 (write v14-compatible). ws@7 + jmuxer@2 installed.

**COMMAND SET FULLY DECODED (static, no capture)** via NetDvr2.dll disasm (`dump/cmdmap.py`, `dump/cmddict.py`, `dump/analyze_dll.py`) — sender helper 0x10012250 `send(session,cmd,reqPtr,reqLen,respBuf,respSize,g)`. Full table in PROTOCOL.md. Key: **record SetRecordState=0x2758 / GetRecordState=0x2757 = big-endian u32 bitmask (ch N = bit N)** — VERIFIED live (start/stop toggles [1,0,1,0] etc). Also PtzControl=0x2756 `[ch,dir,speed]`, recFilesSearch=0x275a(20B), file download 0x271b/0x2779, playback 0x276d cluster, config sets 0x2730/0x2734/0x2732, reboot 0x2755. Tools installed: pefile, capstone, imageio-ffmpeg (ffmpeg vendored at webapp/bin/ffmpeg.exe).

**DESIGN chosen + REAL UI BUILT (phase 1 done).** Broadcast-dark aesthetic, cyan accent, mono readouts. Prototype artifact: https://claude.ai/code/artifact/7bc6ce80-61b0-4295-bc74-baf73d3cea77 (source scratchpad/dvr_ui_prototype.html). Real UI in `webapp/public/` (index.html/style.css/app.js) wired to backend: live 4-ch H.264 via WS+jMuxer, layouts 1×1/2×1/2×2, channel switcher, **double-click enlarge**, real fullscreen (+CSS fallback for iframes), snapshot, fps meter, **DVR-side record start/stop working** (bitmask via /api/record + /api/recordstate poll). Backend `server.js` uses SourceManager fan-out (one DVR conn per ch shared by viewers) + persistent ControlClient (protocol.js), binds 0.0.0.0 → LAN at http://192.168.1.74:8090. Running as bg task. RECORDING IS DVR-SIDE (user rejected PC-side recording; recorder.js unused).

**USER DECISIONS:** VGA UI = leave for now. Persistence = approved boot-hook reflash (not done yet). Screen use = both monitor+maybe fly. Wants FTP-like file mgr + web terminal (telnet, already running on device) + SSH later. Clips/Files/Shell/Config are stubbed in UI.

**NEXT (phase 2):** playback of DVR recordings (recFilesSearch 0x275a → list; file download 0x271b/time 0x2779 → pull → play; or remote-play stream via 0x276d); on-device busybox httpd file server + web file manager; web terminal over telnet; config get/set UI (video params 0x2733/34, bitrate); then the persistence boot-hook reflash. Latency tuning + true-fullscreen only work outside the artifact iframe (real app at localhost fine).

**PHASE 2a DONE — file manager + web terminal.** `webapp/dvr/telnet.js`: TelnetExec (login root/blank, sends `stty -echo cols 1000`, quote-split S/E markers so echoed cmd never matches output markers) + TelnetRaw (interactive bridge). Backend auto-starts busybox `httpd -h / -p 8081` on device via telnet. Endpoints: GET /api/files?path (ls -la parse), GET /api/download?path (proxy device httpd; busybox httpd has NO Range = full download only), POST /api/fs {delete|mkdir}, WS /shell (telnet bridge). WS MUST use noServer + manual `server.on('upgrade')` routing by path (two WebSocket.Server on one http server 400s otherwise). UI Files tab (browse/download/delete/mkdir + breadcrumb) + Shell tab (web terminal, ↑history, ANSI-stripped) — verified live (uname, file lists, download all work). Test helpers in webapp/: test_client.js, test_files.js, shelltest.js.

**RECORDINGS = circular buffer** of pre-allocated 128MB `.ifv` files: /root/rec/a{1..4}/fly000NN.ifv (~224 per partition). NOT per-clip mp4. Playback (Clips tab, still stubbed) needs either .ifv container RE (download a chunk, scan for `62 19` packet headers + Annex-B H264 — likely SAME framing as live, so deframe.py-able) OR the DVR playback protocol (recFilesSearch 0x275a → startPlayByTime 0x276d → media stream on 8670).

**PHASE 2b DONE — recordings/clips + shell fix.** BIG FIND: **ffmpeg has a native `.ifv` demuxer** (webapp/bin/ffmpeg.exe reads them → h264 704x480/352x240). Recordings on device: /root/rec/aN/{fly000NN.ifv (128MB circular), index00/01.bin (segment index: 4KB-ish blocks of [start_ts u32le][end_ts u32le][start_off][end_off]... — decoded but not yet used), log.txt (per-sec record log: [flag '0'/'1'][ts u32le][name32])}. Recordings are SHORT intermittent clips (sparse in the .ifv, hence ffmpeg demux errors at gaps). `dvr/recordings.js` parseLog+toSpans. Endpoints: GET /api/recordings?ch (spans from log via httpd fetch + .ifv file list via telnet ls, ANSI-stripped) ; GET /api/clip?ch&file (prepareClip: telnet `dd bs=1M count=30 | nc -l -pPORT` → pullViaNc to cache/*.ifv → ffmpeg transcode → cache/*.mp4, range-served). ffmpeg .ifv needs a SEEKABLE local file (HTTP no-range + stdin pipe both fail 'Invalid data'), hence dd+nc-to-tempfile. Clips UI: channel select, spans, file grid (recent 60), click→player modal (<video> src=/api/clip, 'preparing…' overlay), raw .ifv download. Shell newline fixed (white-space:pre-wrap in .term .body). All verified: clip served 7s mp4 in 12.6s, frame decoded.

**PHASE 2c DONE — index-based clip seek (proper).** Full `.ifv` container + index format reverse-engineered and verified live (see `docs/IFV_FORMAT.md`). Index00/01.bin = 16-byte records `[start_ts u32le][end_ts u32le][start_off u32le][end_off u32le]` at 4-byte alignment; off = GLOBAL byte offset into the concatenated 128MB `.ifv` stream (file = off/128MiB). KEY finding: ffmpeg's `-f ifv` demuxer **resyncs from a segment's start_off with NO header** (a mid-file dd at a segment boundary decodes directly; arbitrary mid-segment offsets do NOT probe → seek granularity = per index segment). New `webapp/dvr/ifvindex.js` (parseIndex/mergeSegments/segRange/ddCommand — dedup by start_off keeping widest range; byte-exact busybox dd via largest power-of-2 block dividing localStart). Backend: `/api/recordings` returns real recorded `segments` (time + byte offsets) as a timeline; `/api/clip?ch&start&end` dd's exactly that segment's bytes → ffmpeg ifv → MP4 (cap `CLIP_CAP_MB`=64, range-served). Frontend Clips tab rebuilt: timeline bar + segment cards (time/duration/size), click → plays exact span. VERIFIED end-to-end in browser (segment list, timeline, and playback of recorded footage w/ its own OSD all work). A continuous recording = one growing segment record. Note: ffmpeg fetched to webapp/bin, `npm install` run on this PC (Node v22 here, code still v14-compatible).

**PHASE 2d DONE — frame-accurate seek.** BIG finding: ffmpeg's `-f ifv` demuxer seeks ACCURATELY via its internal index — `-ss <t>` lands on the exact OSD second (verified in browser: scrub to 9:57 → OSD +9:57). So no need to parse the 28-byte per-frame table ourselves. Efficiency via a device-backed **range-proxy** `/api/ifvstream?ch&file&base&len` (seekable HTTP view of the on-device .ifv; each Range → a BACKGROUNDED `dd|nc -l &` for that window, killed on client disconnect). Backgrounding was the crux: ffmpeg opens header-read + seek-target as OVERLAPPING connections, and a foreground `nc -l` blocks the single telnet command queue → 2nd connect ECONNREFUSED. `/api/clip` gained `&t=<sec>`: `ffmpeg -ss t -f ifv -i <proxy> -t CLIP_WINDOW_SEC(1800)` → MP4; ffmpeg fetches only ~450KB header/index + seek-point bytes (no full-segment pull, any size). Player scrubber spans the whole segment: seek within loaded window = instant, outside = reload at new t. (streamDeviceRange does block-aligned dd + head-drop in Node for byte-exact start.)

**DEVICE CLEANUP (2026-07-23, user-requested):** nothing connected → stopped recording (all ch off) and DELETED all recordings: `rm` every `*.ifv` + `index*.bin` + `log*.txt` across a1..a4. Freed ~110GB (all 4 SATA partitions 99%→0%). LEFT the tiny `iflydvr` marker (4KB of 0x63) + empty `test/` dir per partition.

**⚠️ CORRECTION — deletion BROKE recording.** The `fly000NN.ifv` files ARE the DVR's PRE-ALLOCATED circular buffer that app.out records INTO; the app does NOT re-create them on demand (records the event to log.txt but writes no video → 0 .ifv, 0% used). The 99%-full state was NORMAL (fixed buffer overwritten in place; only wears the SSD while recording, which was off). So the correct wear-avoidance was simply leaving recording OFF, not deleting the buffer. Restore needs the buffer re-created.

**RESTORE PLAN (pending):** (1) user will power-restart the device (has physical access → safe vs the stripped-U-Boot auto-boot risk) to see if app.out re-initializes/formats the partitions on boot. (2) The app.out Ghidra RE should decode the disk-init/format routine + its trigger (how the `fly000NN.ifv` buffer + `iflydvr` marker get created — a 8670 cmd, missing-marker check, or boot check). NOTE: client `NETDVR_formatHdd` is NOT a simple TLV — it's a threaded async op (NetDvr2.dll: formatHdd 0x1000bc20 → wrapper 0x10013750 → 0x10013ab0, worker thread 0x10014eb0, progress status 0x7e1), so DON'T send a format message blind. After restore, verify recording writes a valid .ifv + index (frame-accurate clip pipeline already proven). Clips tab shows 0 segments until then.

**KNOWN LIMITS/NEXT:** cross-file segments (span crossing a 128MB .ifv boundary) clamped to start-file end (rare, not yet seen). Also still: config get/set UI (video 0x2733/34 bitrate/quality/res lists 0x2764/72/74), PTZ 0x2756 [ch,dir,speed], approved persistence boot-hook reflash (httpd currently restarted per-boot by backend telnet, not persistent on device). Optional: xterm.js, dropbear SSH.

---
---
name: dvr-workflow-tools
description: "How to talk to the Hi3515 DVR from the Windows PC — serial bridge, transfer method, Python path"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 76e1abb0-6be7-4c89-8239-e98f51f54621
  modified: 2026-07-22T21:53:27.819Z
---

Working with the DVR from `D:\Documents\Work\DVR` (part of [[dvr-hi3515-project]]).

- **Python**: bare `python` is a broken MS Store stub. Use `/c/Users/Abdullah/AppData/Local/Programs/Python/Python312/python.exe` (or `py -3`). pyserial installed. 7-Zip at `/c/Program Files/7-Zip/7z.exe`. No WSL/binwalk/mkimage/nc on PC.
- **Serial bridge** (`serial_bridge.py`, run in background): logs COM21→`serial.log`, sends any lines appended to `cmds.txt`. Directives: plain text = cmd+`\n`; `ENTER`; `HEX:03` (Ctrl-C); `RAW:x` (no newline). Append with `printf ... >> cmds.txt`; read with `tail -c N serial.log | cat -v`.
- **File transfer**: PC firewall blocks inbound (can't add rule w/o UAC, which failed headless). So DVR listens, PC connects OUT: on DVR `nc -l -p PORT < file &`; on PC `python pull.py 192.168.1.108 PORT outfile` (verifies md5). Reverse (PC→DVR) works too via `tftp_server.py` but needs the firewall rule.
- ext2 rev0 images: 7-Zip can't open them; use `ext2extract.py` (pure Python). uImage/gzip parsing in `unpack.py`.
