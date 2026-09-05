# Setup — continuing on another PC

This repo contains everything to keep working on the RR104P / Hi3515 DVR project:
a modern web UI (`webapp/`), the full firmware backup + reverse-engineering scripts
(`dump/`), protocol captures (`capture/`), and docs (`PROTOCOL.md`, `README.md`,
`docs/PROJECT_NOTES.md`).

## Run the web UI

Requires **Node.js** (built/tested on v14+) and **ffmpeg** (for recordings).

```sh
cd webapp
npm install                 # restores ws + jmuxer (jmuxer.min.js is already vendored in public/)
```

### Get ffmpeg (excluded from the repo — 84 MB)
The app calls `webapp/bin/ffmpeg.exe`. Easiest way to get it:

```sh
# option A: via Python (bundles a static ffmpeg)
pip install imageio-ffmpeg
python -c "import imageio_ffmpeg,shutil,os; os.makedirs('webapp/bin',exist_ok=True); shutil.copy(imageio_ffmpeg.get_ffmpeg_exe(),'webapp/bin/ffmpeg.exe')"
```
or download any static ffmpeg build and place it at `webapp/bin/ffmpeg.exe`
(on Linux/Mac use `ffmpeg` and set `FFMPEG=/usr/bin/ffmpeg`).

### Start
```sh
cd webapp
node server.js
# -> http://localhost:8090   (also served on your LAN IP:8090)
```

### Config (env vars, all optional)
| var | default | meaning |
|-----|---------|---------|
| `DVR_HOST` | `192.168.1.108` | DVR IP on the LAN |
| `DVR_PORT` | `8670` | control/media port |
| `DVR_USER` / `DVR_PASS` | `admin` / `000000` | DVR app login |
| `TN_USER` / `TN_PASS` | `root` / *(blank)* | telnet login |
| `PORT` | `8090` | web UI port |
| `FFMPEG` | `webapp/bin/ffmpeg.exe` | ffmpeg path |

The PC must be on the **same LAN** as the DVR (telnet :23, control/media :8670,
and busybox httpd :8081 which the backend auto-starts must be reachable).

## What works today
- **Live** — 4-ch H.264 in the browser (WebSocket + jMuxer), 1×1/2×1/2×2, double-click
  enlarge, fullscreen, snapshot.
- **Clips** — DVR recordings: spans (from the device log) + `.ifv` files; click to
  transcode a prefix to MP4 and play; download raw `.ifv`.
- **Files** — browse/download/delete the DVR filesystem.
- **Shell** — root shell over telnet (no serial cable).
- **Record** — start/stop DVR-side recording per channel.

## Reverse-engineering tooling (`dump/` and repo root)
Python scripts (need `pip install pefile capstone pyserial imageio-ffmpeg` as used):
- `dump/analyze_dll.py`, `dump/cmdmap.py`, `dump/cmddict.py`, `dump/reqsamples.py` —
  decode the 8670 protocol from `dump/activex_x/CAB/NetDvr2.dll` + captures.
- `deframe.py`, `parse_capture.py` — the live media stream framing.
- `unpack.py`, `ext2extract.py` — firmware: `dump/mtd2_rootfs.bin` → ext2 → files.
- `serial_bridge.py`, `capture_proxy.py`, `tftp_server.py`, `pull.py` — device I/O helpers.

### Regenerate the extracted firmware (not committed — it's regenerable)
```sh
py -3 unpack.py                 # mtd*.bin -> dump/rootfs.ext2
py -3 ext2extract.py dump/rootfs_padded.ext2 dump/rootfs_x 9303315
tar -xzf dump/live_rootfs.tar.gz -C dump/live_x     # the live rootfs (etc, bin, web…)
```

## Firmware backup (irreplaceable — committed)
`dump/mtd0_boot.bin` (1M), `dump/mtd1_uImage.bin` (2M), `dump/mtd2_rootfs.bin` (5M) —
the full 8 MB NOR flash, md5-verified against the device. See `README.md`.

## Read next
- `docs/PROJECT_NOTES.md` — full state + decisions + what's next (from the working notes).
- `PROTOCOL.md` — the reverse-engineered 8670 protocol + command map.
- `docs/ui_prototype.html` — the clickable UI design prototype.
