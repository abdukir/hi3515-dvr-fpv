# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **Note for the public repo:** `dump/` (vendor firmware images, `app.out`, the HiSilicon SDK,
> the ActiveX DLL, the SoC datasheet) and `capture/` (raw 8670 protocol logs) are **not**
> published — they are the manufacturers' property. The docs below reference them freely
> because they exist in the private working tree. Recreate `dump/` from your own device with
> `py -3 tools/dvr.py backup dump/`; that is what `flash/build.sh` needs.

## What this is

Reverse-engineering and repurposing an old 4-channel analog CCTV DVR (**NetDVR RR104P**,
HiSilicon **Hi3515 v100** SoC, fw `V4.0.0-JMS502 2011`) into an FPV analog recorder / ground
station.

**The project moved on from "PC talks to stock firmware" to "we wrote the firmware."** Two
generations exist side by side; know which one you are in:

| | gen 1 (historic) | gen 2 (**current**) |
|---|---|---|
| on the DVR | stock `app.out` | **`device/dvr/dvr.c`** — our own freestanding OABI program |
| wire protocol | the vendor's 8670 TLV (`PROTOCOL.md`) | ours: control 8090, media 8091 (`docs/CONTROL_PROTOCOL.md`, `docs/PROTOCOL2.md`) |
| PC console | `webapp/` | **`webapp2/`** |
| VGA output | app.out's UI | our OSD: HUD, menus, USB mouse/keyboard, on-screen playback |

`webapp/` and `PROTOCOL.md` stay because that 8670 RE is the only record of the vendor
protocol, but **new work goes in `device/dvr/` and `webapp2/`.** Start with
`docs/DVR_DESIGN.md` (milestone map), **`docs/REVIEW.md`** (architecture review, open
defects, ranked backlog) and `docs/FLASH.md` (persistence).

**The DVR boots OUR firmware from flash** (mtd2 reflashed 2026-07-25, `docs/FLASH.md`).
The stock `app.out` is still on board as a fallback: `touch /root/rec/a1/stock` + reboot.
Everything of ours lives on the **SATA disk** (`/root/rec/a1/`: `boot.sh`, `dvr`,
`dvr.conf`), so iterating never needs another reflash — just `py -3 tools/dvr.py deploy`.
This project spans two PCs; paths in older notes (`D:\Documents\Work\DVR`) refer to a
prior machine.

## Driving the device: tools/dvr.py

One CLI for every transport — telnet, our control protocol, serial console, U-Boot, the
HDMI capture card pointed at the DVR's VGA output, and flashing:

```sh
py -3 tools/dvr.py status                    # alive? what's it running?
py -3 tools/dvr.py shell "ps | head"         # telnet
py -3 tools/dvr.py ctl INFO                  # control protocol :8090
py -3 tools/dvr.py push F /root/rec/a1/F     # chunked, SATA-staged, md5-verified
py -3 tools/dvr.py screen shot.png           # SEE the DVR's screen — use this to verify UI work
py -3 tools/dvr.py serial send "id"          # root shell over the console (works with no network)
py -3 tools/dvr.py deploy                    # push dvr + boot.sh + config to SATA
py -3 tools/dvr.py flash IMG --reboot        # write mtd2, verified before any reboot
py -3 tools/dvr.py restore --reboot          # stock firmware back
py -3 tools/dvr.py recover --test            # rehearse U-Boot+TFTP recovery
py -3 tools/dvr.py mtd0                      # audit mtd0; U-Boot lives there, check before writing
py -3 tools/dvr.py logo splash.jpg           # boot splash (tools/mklogo.py generates one)
```

Device diagnostics worth knowing (`dvr.py ctl <cmd>`): **`INFO`** carries `pps=` (encoder
frames/s), `still=` (seconds the VGA video layer hasn't advanced), `heals=`/`froze=`;
**`VOSIG`** answers "is the video layer moving"; **`RELIVE [HARD]`** forces the freeze heal;
**`PANEL`** shows recent front-panel/IR key codes; **`PGPIO`** reads the raw key-matrix GPIOs.

**Live feed freezing?** Root-caused 2026-07-25: idle VI channels drained the shared VB
pool. Only bring up channels something consumes (`vi_all=0`, now the default). `INFO pps=`
is the reliable liveness signal — *not* `still=`/`vo_frame_sig()`, which reports a frozen
video layer as healthy. Never auto-restart on a stall: a respawn keeps the MPP drivers'
state and loops forever. See `docs/REVIEW.md` §2b.

**Recording is manual, by command, deliberately** — `REC`/`STOP` only. No auto-record, no
circular retention: the point of this firmware is that it is not a CCTV DVR. See
`docs/REVIEW.md` §4b before adding anything that records on its own.

`tools/dvrlib.py` holds the transport traps as behaviour, not folklore: uploads stage on
SATA (`/` has ~1.9 MB free) and go in 1 MB chunks (busybox `nc` dies past ~2.5 MB), the
`nc` listener gets its own telnet session (it eats the next command otherwise), and a
telnet `exec` timeout is fatal (a partial reply desyncs the session forever).

## Our DVR firmware (device/dvr/) — the main codebase

One ~70 KB freestanding OABI binary, no libc and no SDK: capture → encode → {record to
SATA as MPEG-TS, stream over TCP, drive the VGA output with an OSD}, for 4 captured
channels (`enc_channels` of them encoded, default 1). MPP bring-up is a byte-for-byte
ioctl replay of a trace from `app.out` (`docs/MPP_INIT_SEQUENCE.md`) because the only
findable SDK mismatches the silicon — **do not "clean up" those magic byte arrays.**

```sh
# build in WSL from ext4 (NOT /mnt/c — the old-gcc path breaks on DrvFs)
bash device/dvr/build.sh          # bootlin armv5 uclibc gcc, -nostdlib, links ../crt0.S
```
Deploy: device listens (`nc -l -p PORT > f`), PC connects out — see `docs/DVR_DESIGN.md`.
Two traps when transferring files this way: `/` is a 12.5 MB RAM disk with ~1.9 MB free (so
stage on SATA, not `/tmp`), and busybox 1.1.2 `nc` stops accepting data around 2.5 MB per
connection (so chunk it). Both silently truncate.

## The persistence kit (flash/)

`flash/build.sh` → `flash/verify.sh` → `flash/device/preflight.sh` → `flash/device/flash_mtd2.sh`.
Read **`docs/FLASH.md`** before touching any of it. Serial console is a hard prerequisite:
it is the only recovery path for a bad rootfs.

## The old web UI (webapp/) — gen 1, kept for the 8670 RE

```sh
cd webapp
npm install          # ws + jmuxer (jmuxer.min.js is vendored in public/)
node server.js       # -> http://localhost:8090, also on LAN IP:8090
```

Requires **Node.js v14+** (write v14-compatible JS — no optional chaining in server code) and
**ffmpeg** at `webapp/bin/ffmpeg.exe` (excluded from repo, 84 MB — see `SETUP.md` to fetch it).
The PC must be on the **same LAN** as the DVR. All config is via env vars (`DVR_HOST` default
`192.168.1.108`, `DVR_USER`/`DVR_PASS` = `admin`/`000000`, `TN_USER`/`TN_PASS` = `root`/blank,
`PORT`, `FFMPEG`) — see `CFG` in `server.js` and the table in `SETUP.md`.

There is no test framework and no lint config. `webapp/test_client.js` (and other `test_*.js`
helpers) are ad-hoc scripts run directly with `node` against a live DVR — not a suite.

### Backend architecture (`webapp/server.js` + `webapp/dvr/`)

The DVR speaks a proprietary TLV protocol over TCP **8670** for both control and media (there is
**no RTSP/ONVIF/CGI**). The backend maintains persistent connections and fans them out to browsers:

- **`dvr/protocol.js`** — `ControlClient`: one long-lived control connection. Sends a 20-byte
  preamble (16-byte magic + `01 01 00 00`), logs in (cmd `0x2711`), then request/reply framing
  `[len u32be][type u16be][cmd u16be][seq u16be][0x0100][payload]`, matched by `seq`. Auto-reconnects.
  Implements record start/stop via `GetRecordState 0x2757` / `SetRecordState 0x2758` (a big-endian
  u32 **bitmask**, channel N = bit N — read-modify-write). Also builds the 100-byte media request.
- **`dvr/mediastream.js`** — `MediaStream`: opens a *separate* 8670 connection per stream, sends
  `mediaRequest(ch, stream)`, and de-frames the media container into clean Annex-B H.264 frames.
  Container = `[8B prefix][per frame: 11B pkt hdr starting 62 19][12B LEN/FRAMENO/TS][LEN bytes]`.
  Resyncs on the `62 19` marker. This runs entirely in JS — ffmpeg is **not** in the live path.
- **`dvr/channelsource.js`** — `SourceManager`/`ChannelSource`: exactly one `MediaStream` per
  `(channel, stream)`, shared by all viewers. Caches SPS/PPS so new joiners' decoders init
  immediately; drops the DVR link ~5 s after the last viewer leaves.
- **`dvr/telnet.js`** — `TelnetExec` (login root/blank, `stty -echo`, S/E marker command framing)
  for file ops, and `TelnetRaw` for the interactive web terminal. On connect the backend also
  starts busybox `httpd -h / -p 8081` on the device for file downloads.
- **`dvr/recordings.js`** — `parseLog`/`toSpans` for the device record log; clip retrieval.

`server.js` wires these into HTTP + two WebSocket endpoints. **WebSockets use `noServer` + a
manual `server.on('upgrade')` router by path** (`/stream` for H.264, `/shell` for the terminal) —
two `WebSocket.Server` instances on one HTTP server otherwise 400. Key HTTP APIs: `/api/record`
+ `/api/recordstate` (DVR-side recording), `/api/files` + `/api/fs` + `/api/download` (file
manager via telnet + device httpd), `/api/recordings` + `/api/clip` (transcode a recording prefix
to MP4). `webapp/recorder.js` is **unused** — the user chose DVR-side recording, not PC-side.

### Recordings / clips (the tricky part)

Recordings are a **circular buffer** of pre-allocated 128 MB `.ifv` files at
`/root/rec/a{1..4}/fly000NN.ifv` (~224 per SATA partition; channel N → partition `a(N+1)`), plus
`index00/01.bin` (segment index: `[start_ts u32le][end_ts u32le][start_off][end_off]…`) and
`log.txt` (per-second record log). **ffmpeg has a native `.ifv` demuxer.** Clips play by:
`dd`-ing a prefix off the device → `nc` to a temp file → ffmpeg `.ifv`→MP4 (range-served). ffmpeg's
`.ifv` demuxer needs a **seekable local file** (HTTP-no-range and stdin-pipe both fail), which is
why the flow copies to a tempfile first. Current limit: only a 30 MB prefix is pulled; efficient
full-timeline seek via `index00.bin` is the open TODO.

### Frontend (`webapp/public/`)

`index.html` / `style.css` / `app.js` + vendored `jmuxer.min.js`. Broadcast-dark aesthetic, cyan
accent. Live 4-ch H.264 over the `/stream` WebSocket → jMuxer → `<video>`; layouts 1×1/2×1/2×2,
channel switcher, double-click enlarge, fullscreen, snapshot, fps meter. Tabs: Live, Clips, Files,
Shell, Record. Note true-fullscreen and low latency only work in the real app at localhost, not in
an embedded artifact iframe.

## The current web console (webapp2/)

```sh
cd webapp2 && npm install && node server.js     # -> http://localhost:8092
```
Same Node v14-compatible rules as `webapp/`, and it reuses `webapp/bin/ffmpeg.exe` for
TS→MP4 remuxing of clips. Structure:

- **`dvr/control.js`** — `DvrControl`: ONE persistent connection to the device control port
  8090, with a command queue and in-order reply matching (the device is strictly sequential
  per connection and only accepts 4 control clients — do not go back to connect-per-command).
  `parseInfo`/`parseList` turn `INFO`/`LIST` into objects.
- **`dvr/telnet.js`** — `TelnetExec` (marker-framed one-shot commands), `TelnetRaw` (web
  shell), `putFile` (out-of-band `nc` upload). `exec()` frames a **single line**; a heredoc
  wedges the session. An `exec()` timeout is treated as fatal and drops the socket — a
  partial resolve would desync every later command permanently.
- **`server.js`** — HTTP + the `/stream` and `/shell` WebSockets (`noServer` + a manual
  `upgrade` router by path; two `WebSocket.Server` on one HTTP server otherwise 400).
  `INFO` is polled once and cached for all tabs. `/stream` refuses channels ≥ `enc_channels`
  with WS code 4404 instead of leaving a dead socket holding a device slot.
- **`public/`** — Live grid (jMuxer/MSE), Clips (play in browser *or* on the DVR's own
  monitor, download, delete), Monitor (VGA channel, OSD D-pad, picture, playback transport),
  Files, Shell, Config (device readout + `dvr.conf` editor).

## The device: access & firmware (context for RE work)

- **Access:** DVR at `192.168.1.108` (default net.sh `192.168.1.114`). **`telnet` root / blank
  password = root shell.** Serial console COM21 @ 115200 8N1 auto-logs-in as root. The rootfs is a
  gzipped-ext2 **RAM disk** — `/` is volatile, NOT persistent; persisting anything requires
  reflashing mtd2. Disable the hardware watchdog before living in the shell (`closewd` at the app's
  `[user]#` debug prompt, then `exit`).
- **Flash (8 MB NOR @ 0x80000000):** mtd0 = boot/U-Boot (1 MB), mtd1 = uImage kernel (2 MB),
  mtd2 = rootfs (5 MB). Full md5-verified backups are committed in `dump/` (the only
  irreplaceable artifacts here). `dump/live_x/` is the complete rootfs pulled from RAM;
  `dump/rootfs_x/root/app_x/app.out` is the main DVR binary that implements the 8670 protocol.

### RE tooling (Python, repo root + dump/)

Ad-hoc scripts, run with `py -3` / `python`. Deps as needed: `pip install pefile capstone
pyserial imageio-ffmpeg`. **Bare `python` may be a broken MS Store stub** — prefer `py -3`.

- Protocol RE: `dump/analyze_dll.py`, `dump/cmdmap.py`, `dump/cmddict.py`, `dump/reqsamples.py`
  decode the 8670 command set from the ActiveX `dump/activex_x/CAB/NetDvr2.dll` + captures.
- Live media framing: `deframe.py`, `parse_capture.py`.
- Firmware: `unpack.py` (U-Boot/gzip images → `rootfs.ext2`), `ext2extract.py` (pure-Python ext2
  rev0 extractor that tolerates the intentionally truncated tail — 7-Zip can't read these).
- Device I/O: `serial_bridge.py` (background COM21 logger + `cmds.txt` command queue → `serial.log`),
  `capture_proxy.py`, `tftp_server.py`, `pull.py`.

**Transfer that works with no firewall change:** the PC firewall blocks inbound, so the DVR listens
and the PC connects out — on DVR `nc -l -p PORT < file &`, on PC `py -3 pull.py 192.168.1.108 PORT out`.

## Conventions & gotchas

- Node backend targets **v14** — keep it v14-compatible.
- `capture/*.log` are raw protocol captures; `PROTOCOL.md` is the authoritative decoded spec —
  update it when new commands are decoded.
- The DVR must be reachable on the LAN for the backend to do anything; without it, endpoints return
  503 (control/shell not connected) rather than crashing.
- Live H.264 de-framing is hand-rolled in JS and resync-tolerant; when touching `mediastream.js`,
  preserve the `62 19` resync and the `00 00 00 01` frame-start validation.
