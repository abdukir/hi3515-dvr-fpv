# DVR_DESIGN.md — our own DVR program for the RR104P (Hi3520_MPP_V3.0.6.2)

Living architecture doc for replacing stock `app.out` with our own DVR: custom recording, a lean
network protocol for a thin PC web client, and eventually our own VGA UI. Plan of record:
`~/.claude/plans/floating-chasing-goose.md`. The device stays reversible (reboot restores stock
from the RAM rootfs) until we deliberately reflash mtd2 (M4).

## Architecture

Single OABI C program, freestanding (`-nostdlib`, `device/dvr/oabi.h` + `device/crt0.S`), one
MPP owner at a time (stop `app.out` first). Source under `device/dvr/`:

- **`dvr.c`** — M0 recorder: raw-ioctl replay of `docs/MPP_INIT_SEQUENCE.md` (capture chip → SYS+VB
  → VO → VI×4 → GRP×4 → VENC×4 → per-channel GetStream/Release loop). Currently one file; splits
  into `mpp.c`/`rec.c`/`net.c`/`ui.c` as later milestones land.
- **`oabi.h`** — OABI syscall runtime (open/read/write/ioctl/mmap2/lseek/nanosleep/gettimeofday/
  mkdir/exit) + tiny freestanding helpers. Add socket syscalls in M2.
- **`build.sh`** — bootlin armv5 uClibc gcc, `-march=armv5te -static -nostdlib -ffreestanding`,
  links `../crt0.S`. Build from ext4 (`~/dvrbuild`), not `/mnt/c`.

**Data flow (single pipeline, fan-out):** the VENC GetStream loop yields Annex-B H.264 packs → each
pack is (a) appended to a segment file on SATA and (b) [M2] written to any connected live socket.
Capture/encode happens once; disk and network are cheap consumers. This keeps the device lean.

## Milestone status

- **M0 — headless recorder: ✅ DONE (2026-07-23).** Records valid H.264 from all 4 channels to
  `/root/rec/aN/`; pulled files decode in ffmpeg (350 frames/ch/12 s). VI came up with no
  SYS_NOTREADY — the SDK version-mismatch blocker is beaten via the traced raw-ioctl replay. The
  5 corrections that made frames flow are in `docs/MPP_INIT_SEQUENCE.md` ("VERIFIED WORKING").
- **VGA display: ✅ DONE (2026-07-23).** Direct `VI→VO` (codec-free) single channel fullscreen on
  the VGA output, switchable source, **verified live at ~130 ms glass-to-glass** (mostly the LCD's
  own input lag — our path is near the floor). Runs *alongside* recording+streaming. Details +
  latency levers (output mode, `SetPlayToleration` ioctl `0x40044f30`) in `docs/DISPLAY_PATH.md`.
  Args: `dvr <secs> <dispch> <r|d> <fps> <vosync> <votol_ms>`. Widescreen VGA modes work
  (1366×768=10, 1440×900=11); 720p/1080p are rejected over the VGA connector (VESA only).
- **M1 — encoder specs + segmented recording: ✅ DONE (2026-07-23).** `VENC_ATTR`(104B)/
  `VENC_RC`(76B) fields decoded (offsets: w=8,h=12,viFR=16,bufSize=28,fps=40,gop=44,bitrate=56).
  `/root/rec/dvr.conf` (key=value; `apply_cfg`/`patch_venc_attrs` in `dvr.c`) sets
  fps/gop/bitrate/width/height/seg_sec/max_segs — see `device/dvr/dvr.conf.example`. Recording is
  now **segmented** `/root/rec/aN/SSSSSS.h264` (rotates at a keyframe every `seg_sec`, each segment
  independently decodable), an **index.txt** log, and **retention** (circular buffer keeps newest
  `max_segs`). All verified with ffmpeg.
- **M2 — lean protocol + thin webapp: ✅ DONE (2026-07-23).** OABI networking via `socketcall`
  (`device/dvr/net.h`) proven; non-blocking TCP stream server folded into the record loop (port
  8091, `net_init`/`net_poll`/`net_fanout` in `dvr.c`) fans keyframe-aligned Annex-B H.264 per
  channel; drop-on-slow so recording latency is never held hostage. `webapp2/` = thin Node proxy
  (TCP→WS) + jMuxer/MSE browser viewer with channel switcher. Wire protocol in `docs/PROTOCOL2.md`.
  Verified end-to-end: browser-WS → proxy → device → ffmpeg-decodable 352×240 30 fps.
- **M3 — VGA UI (OSD/menus): remaining.** Live video on VGA is done (above); the *UI overlay*
  (menus/OSD via `/dev/fb*` + TDE, input via `/dev/mice` + `/dev/ttyAMA1`) is the remaining piece —
  needs on-device visual iteration. Region/graphics ioctls to be traced the same way (see
  `docs/DISPLAY_PATH.md` §M3). Channel-switch-on-VGA-via-input also lands here.
- **M4 — persistence: ✅ DONE (2026-07-25).** mtd2 holds our image; the DVR boots our
  firmware from flash unattended in ~20 s, with the stock `app.out` still reachable as a
  fallback (`touch /root/rec/a1/stock`). The return path was proven *first* by writing the
  stock image back and booting it. Two things only the first real boot revealed — the IP
  reverting to `net.sh`'s 192.168.1.114 (app.out used to set .108 from the mtd0 config
  blob) and the serial getty never starting (rcS never returns once we `exec` from it) —
  are both fixed in `boot.sh` **on SATA, without reflashing**. See `docs/FLASH.md`.
  Everything below is how it was prepared:

  Full kit in `flash/`,
  procedure + recovery in **`docs/FLASH.md`**. The flash diff is one file: `run_app.sh` gains a
  hook that mounts SATA and `exec`s `/root/rec/a1/boot.sh` if present, falling through to the
  stock `app.out` otherwise (and `net.sh`/telnetd always runs first, so a bad `boot.sh` can never
  lock us out). Everything of ours lives on SATA → **one reflash, ever**.
  Status: image built and 40+ offline checks pass (`flash/build.sh`, `flash/verify.sh`); the
  device's own gunzip reproduces the ext2 **byte-identical to the PC build** and the on-device
  preflight passes (`flash/device/preflight.sh`); `boot.sh` has been proven live on the device by
  running it in place of `run_dvr.sh`. Remaining gate: a working **serial console** (the only
  recovery path if the rootfs is bad) — then `flash/device/flash_mtd2.sh`, which backs up the
  current mtd2 to SATA, writes, and md5-verifies the flash *before* any reboot.

## Current single-binary feature set (`device/dvr/dvr.c`)

One ~15 KB freestanding OABI binary does **capture → encode → {record to SATA (segmented+retention),
display on VGA (low-latency), stream over TCP}** simultaneously for 4 channels, configured by
`/root/rec/dvr.conf` + argv. No SDK, no libc (links only `-lgcc` for integer-division helpers;
`raise()` stubbed). This is the core FPV ground-station recorder/monitor.

## On-device test workflow (proven M0)

1. **Serial** (observe/control): `py -3 serial_bridge.py COM5 115200` → appends to `serial.log`,
   reads commands from `cmds.txt` (`ENTER`, `HEX:03`, plain lines). Device auto-logs-in as root.
2. **Deploy**: PC connects OUT, device listens (no inbound firewall issue). Over telnet (root/blank):
   `nc -l -p 9000 > /root/dvr &`, PC sends the binary, `chmod +x`. (`$CLAUDE_JOB_DIR/tmp/deploy.py`.)
3. **Free MPP** (single owner): serial `closewd` (disable HW watchdog) → telnet `kill <mydaemon.out
   pid>` (stops respawn/reboot) → serial `exit` (app.out frees MPP cleanly; `run_app.sh` ends, no
   respawn loop). `/dev/venc|vi|sys` remain; `ps` shows app.out gone.
4. **Run**: serial `/root/dvr <secs>` (self-terminating, disarms watchdog on exit). Watch `serial.log`.
5. **Pull small files**: `uuencode <file> f` over telnet, decode with `binascii.a2b_uu` on PC —
   read with an **accumulate-loop** (telnetlib `read_until` bails early; busybox `od` lacks `-A/-t`;
   `nc -l < file` send was flaky). Validate: `ffmpeg -f h264 -i ch.h264 -c copy ch.mp4`.
6. **Restore stock**: `reboot` (RAM rootfs → app.out relaunches; `/root/dvr` gone, SATA files persist).

## Key device facts

- SATA = 4× vfat partitions mounted `/root/rec/a{1..4}` (`/dev/sda1..4`). `/root` (and `/dev/root`)
  is the volatile RAM ext2 rootfs. Boot: `run_app.sh` → `net.sh`/`init.sh` → unpack app.tgz/data.tgz
  → `mydaemon.out &` → `app.out` (respawn/watchdog is **mydaemon.out**, not a run_app loop).
- Error decode (HiSilicon): `0xa006_800e` = VENC BUF_EMPTY (errid 0xE), `0xa006_8003` = ILLEGAL_PARAM
  (errid 0x3). errid = low 13 bits.
