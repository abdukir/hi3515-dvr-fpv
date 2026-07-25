# hi3515-dvr-fpv

**Turning a dead 2011 CCTV DVR into a modern FPV ground station — by replacing its firmware with one written from scratch.**

This is a reverse-engineering project against a **NetDVR RR104P**: a 4-channel analog
security recorder built on a HiSilicon **Hi3515** SoC (ARM926EJ-S, 200 MHz, 43 MB usable
RAM, Linux 2.6.24, BusyBox 1.1.2). Its only client was an ActiveX plugin for Internet
Explorer, so as shipped the box is e-waste.

It now runs **our own firmware** — a single ~75 KB freestanding C program with no libc and
no vendor SDK — which captures analog video, encodes H.264 in hardware, records to SATA,
streams live over the network, and drives its own on-screen UI. A browser-based console
talks to it over a protocol we designed.

> ### ⚠️ This project is entirely vibecoded
>
> Effectively all of the code, reverse engineering and documentation here was produced by
> an AI agent (Claude) driving the hardware directly — reading firmware dumps, writing
> ARM ioctl replays, flashing NOR, and testing against the live device over telnet, a
> serial console and an HDMI capture card. A human set the goals and eyeballed the results.
>
> Practical consequences, stated plainly:
> - It works on **one** device, the one it was developed against. It has never run anywhere else.
> - There are no tests. "Verified" throughout these docs means *observed working on the bench*, usually once.
> - Some of it is genuinely subtle (the MPP bring-up, the freeze diagnosis). Some of it is
>   held together by comments explaining why the obvious thing doesn't work.
> - Several confident claims in the docs were **wrong and later corrected** — that history is
>   deliberately preserved in `docs/REVIEW.md` rather than tidied away.
>
> Treat it as a detailed lab notebook that happens to compile, not as a product.

> ### ⚠️ This modifies flash. You can brick your device.
>
> Milestone M4 rewrites the `mtd2` partition. There is a tested recovery path, but it
> depends on a **working serial console** — the only way back if the rootfs won't boot.
> Read [`docs/FLASH.md`](docs/FLASH.md) fully before writing anything. You can use
> everything here **without flashing at all** (see below), which is how it was developed.

---

## What it actually does

| | stock firmware | this project |
|---|---|---|
| live view | ActiveX in IE only | any modern browser, H.264 over WebSocket + MSE |
| recording | proprietary `.ifv` circular buffer | MPEG-TS files with real PTS, plays anywhere |
| video quality | ~CIF, low bitrate | 704×240 (full-width 2CIF), FIXQP QP10 ≈ 17-23 Mbps |
| control | vendor CMS on a TLV protocol | plain line protocol on TCP 8090, documented |
| on-screen UI | vendor menus | our own OSD: HUD, menus, mouse/keyboard, playback |
| glass-to-glass | — | ~130 ms on the VGA output (mostly the monitor's own lag) |

### Why bother

For **FPV** specifically: an analog video receiver plugs into a composite input, and you
get a recorder that is genuinely good at the one job that matters — capturing the video
link at high bitrate while showing it on a monitor with minimal latency. The hardware is
worth ~nothing second-hand and has a real H.264 encoder, four composite inputs, a SATA
port, and a VGA output. That is a lot of ground station for the money.

More generally it is a worked example of taking a locked, obsolete embedded Linux appliance
and making it yours: dumping flash, reverse-engineering an undocumented media pipeline,
writing a replacement init/userland, and reflashing safely with a recovery path.

---

## Hardware

- **SoC** HiSilicon Hi3515 v100 (ARMv5TE, OABI-only kernel)
- **Capture** Techwell TW2866, 4× composite, PAL/NTSC
- **Encoder/decoder** hardware H.264 (VEDU), shared between encode and playback
- **RAM** 128 MB, kernel limited to `mem=43M`
- **Flash** 8 MB NOR: `mtd0` boot (1 MB) · `mtd1` kernel (2 MB) · `mtd2` rootfs (5 MB)
- **Storage** SATA, 4 × vfat partitions mounted `/root/rec/a1..a4`
- **Out** VGA (+ composite), USB host, 100M Ethernet, RS-485 PTZ, buzzer
- **Rootfs** gzip'd ext2 **RAM disk** — `/` is volatile, which is what makes no-flash development possible

---

## Two ways to run it

### A. Without flashing (recommended — this is how it was built)

Nothing permanent happens. `/` is a RAM disk, so a power cycle restores the stock firmware
completely. You need the DVR on your LAN and telnet access (stock firmware has telnetd on,
root with a blank password).

```sh
# 1. build the firmware (WSL/Linux, bootlin armv5 uclibc toolchain — see docs/BUILD.md)
bash device/dvr/build.sh

# 2. stop the vendor app so we can own the MPP hardware (single owner only)
#    serial console:  `closewd`  then  `exit`      (disarms the watchdog, ends app.out)
#    or over telnet:  killall mydaemon.out; killall app.out

# 3. push our binary to the SATA disk and run it
py -3 tools/dvr.py push device/dvr/dvr /root/rec/a1/dvr 755
py -3 tools/dvr.py shell "/root/rec/a1/dvr 0 0 r 30 9 1 &"

# 4. check it came up
py -3 tools/dvr.py status
```

Power-cycle to undo everything.

### B. With flashing (persistent — it boots on its own)

The flash change is **one file**: `/root/run_app.sh` gains a hook that mounts SATA and
`exec`s `/root/rec/a1/boot.sh` if present, falling through to the stock `app.out` otherwise.
Everything of ours then lives on the **SATA disk**, which is writable over the network — so
this is a *one-time* flash and you never touch NOR again.

```
power on
  └ U-Boot (mtd0) → kernel (mtd1) → ext2 RAM disk (mtd2) → /etc/init.d/rcS
                                                              └ /root/run_app.sh
      net.sh (eth0 + telnetd)  ←── always runs first, so telnet is up no matter what
      init.sh (drivers)
      ├ /root/rec/a1/boot.sh exists ──→ exec it ──→ our DVR      ← the new path
      └ otherwise ──────────────────→ mydaemon.out + app.out     ← stock, untouched
```

**Read [`docs/FLASH.md`](docs/FLASH.md) first.** Short version:

```sh
bash flash/build.sh          # build the rootfs image (needs your own mtd2 dump, see below)
bash flash/verify.sh         # 40+ offline checks; refuses to bless a bad image
py -3 tools/dvr.py deploy    # stage boot.sh + dvr + config on SATA, and test them by hand
# on-device preflight: loop-checks the image with the device's own kernel and gunzip
py -3 tools/dvr.py push flash/device/preflight.sh /root/rec/a1/preflight.sh 755
py -3 tools/dvr.py shell "sh /root/rec/a1/preflight.sh /root/rec/a1/mtd2_new.bin"
py -3 tools/dvr.py flash flash/out/mtd2_new.bin --reboot
```

`tools/dvr.py flash` backs the current `mtd2` up to SATA, disarms the hardware watchdog,
writes, then **reads the flash back and compares md5 before you are allowed to reboot**.

#### You must supply your own firmware dump

This repository deliberately contains **no vendor firmware binaries** (see
[Legal](#legal-and-scope)). `flash/build.sh` needs `dump/mtd2_rootfs.bin` from *your* device
— which is the correct workflow anyway, since every unit's config blob and MAC differ:

```sh
py -3 tools/dvr.py backup dump/    # pulls mtd0/mtd1/mtd2 off the device, md5-verified
```

---

## Connecting to the device

Three independent channels, and you want all three before flashing:

| channel | what it gives you | notes |
|---|---|---|
| **Network** | telnet (root, blank password), our control port 8090, media 8091, busybox httpd 8081 | default `192.168.1.108`; our `boot.sh` sets it, configurable via `/root/rec/a1/ip` |
| **Serial console** | root shell **and** U-Boot — works with no network at all | USB-TTL, 115200 8N1. **This is the recovery path.** |
| **Capture card** | see the DVR's actual VGA output from your PC | any HDMI/VGA grabber; used to verify UI work |

Everything is driven by one CLI:

```sh
py -3 tools/dvr.py status                    # alive? what's it running?
py -3 tools/dvr.py shell "ps | head"         # telnet
py -3 tools/dvr.py ctl INFO                  # control protocol
py -3 tools/dvr.py push FILE /root/rec/a1/x  # chunked, md5-verified upload
py -3 tools/dvr.py screen shot.png           # grab the DVR's screen
py -3 tools/dvr.py serial send "id"          # root shell over the console
py -3 tools/dvr.py backup dump/              # pull all three flash partitions
py -3 tools/dvr.py flash IMG --reboot        # write mtd2, verified before reboot
py -3 tools/dvr.py restore --reboot          # put the stock firmware back
py -3 tools/dvr.py recover --test            # rehearse U-Boot + TFTP recovery
```

`tools/dvrlib.py` encodes the transport traps as behaviour rather than folklore — uploads
stage on SATA (`/` has ~1.9 MB free) and go in 1 MB chunks (busybox `nc` silently stops
around 2.5 MB), the `nc` listener gets its own telnet session (it eats the next command
otherwise), a telnet `exec` timeout is fatal (a partial reply desyncs the session forever),
and Git-Bash POSIX-path mangling is undone automatically.

---

## The web console (`webapp2/`)

```sh
cd webapp2 && npm install && node server.js     # → http://localhost:8092
```

Node v14+ compatible. Needs `ffmpeg` at `webapp/bin/ffmpeg.exe` for clip remuxing (see
[`SETUP.md`](SETUP.md)); it is **not** in the live video path.

- **Live** — H.264 straight from the device over a WebSocket into jMuxer/MSE. No transcoding.
- **Clips** — recordings listed from the device, played in-browser (lossless TS→MP4 remux,
  range-served) or **on the DVR's own monitor**, plus download and delete.
- **Monitor** — remote control for the DVR's VGA output: channel, OSD D-pad, live picture
  controls (TW2866 registers), playback transport.
- **Files / Shell** — file manager and a root shell over telnet.
- **Config** — device readout, clock sync, PAL/NTSC, buzzer toggle, and a `dvr.conf` editor.

The backend keeps **one** persistent control connection (the device only accepts four) and
polls `INFO` once for all browser tabs.

---

## How it works

```
        TW2866 (4× composite)
              │
        VI  (only the channels something drains — see below)
         ├──────────────► VO / VGA  ── the DVR's own monitor, codec-free, ~130 ms
         │                              plus an fb0 OSD layer + fb4 cursor overlay
         │
         └─► VENC ────► pump_encode() ─┬─► MPEG-TS file on SATA   (recording)
                        │              └─► TCP 8091 fan-out       (live)
              VDEC ◄────┘  (on-screen playback, shares the VEDU with VENC)

   control: TCP 8090 + /dev/ttyAMA1        PC: webapp2 (8090 · 8091 → WS · telnet · 8081)
```

The important structural decision: **capture and encode happen once, and disk / network /
display are independent consumers.** Adding a viewer costs a `write()`. That is why a
200 MHz ARM9 sustains all three at 704×240 and ~17 Mbps.

The MPP bring-up is a **byte-for-byte ioctl replay** of a trace taken from the vendor's
`app.out`, because the only obtainable HiSilicon SDK mismatches this silicon
(`VI_SetPubAttr` → `SYS_NOTREADY`). Every struct is a magic byte array with an offset
comment. It looks awful; it is unavoidable. `docs/MPP_INIT_SEQUENCE.md` is the spec.

---

## What the reverse engineering turned up

Highlights — the full record is in [`docs/`](docs/):

- **[`PROTOCOL.md`](PROTOCOL.md)** — the vendor's 8670 TLV protocol, decoded from the
  ActiveX DLL plus packet captures. Only needed for gen-1; kept because it's the only record.
- **[`docs/MPP_INIT_SEQUENCE.md`](docs/MPP_INIT_SEQUENCE.md)** — the exact ioctl sequence that
  makes video flow, including the five corrections that were the difference between
  "nothing" and "working".
- **[`docs/IFV_FORMAT.md`](docs/IFV_FORMAT.md)** — the vendor recording container and its
  time→offset index.
- **[`docs/DISPLAY_PATH.md`](docs/DISPLAY_PATH.md)** — VO/VGA bring-up, the fb0 OSD, and the
  flash-free cursor on a dedicated fb4 overlay (app.out's own trick).
- **[`docs/FRONT_PANEL.md`](docs/FRONT_PANEL.md)** — the 18-pin panel header: a 5×5 GPIO key
  matrix, the LED bank, and the buzzer (`0x20150000` bit 7, a passive transducer, so it
  plays actual tones). *The daughterboard itself is unused — its switches are worn and fire
  on their own — but the RE is preserved.*
- **[`docs/REVIEW.md`](docs/REVIEW.md)** — architecture review, defects found, and the
  ranked backlog. Includes the **live-video freeze**: idle VI channels captured into the
  shared VB pool that nothing drained, starving the channel that mattered. Diagnosed by
  A/B test against the hardware, and the section keeps the earlier, wrong analysis on
  purpose.
- **[`docs/FLASH.md`](docs/FLASH.md)** — the persistence work, including the discovery that
  the vendor's own `mtd2` is a **gzip stream truncated to fit the partition** with a wrong
  data CRC (it only boots because U-Boot has `verify=n`).

---

## Repository layout

```
device/dvr/     our firmware — dvr.c + oabi.h/net.h/ts.h/fb.h/ui.h/... , build.sh
tools/          dvr.py + dvrlib.py — one CLI for telnet/control/serial/U-Boot/capture/flash
flash/          the persistence kit: build.sh, verify.sh, mkuimage.py, boot hook, SATA payload
webapp2/        the current web console (Node + browser)
webapp/         gen-1 console that spoke the vendor protocol — kept for the 8670 RE
docs/           protocol specs, RE writeups, build/flash procedures, the review
*.py            RE tooling: unpack.py, ext2extract.py, deframe.py, tftp_server.py, ...
```

Start with [`CLAUDE.md`](CLAUDE.md) — it is the orientation document, and it is accurate
about which parts are current and which are historical.

---

## Status

Working and running from flash: cold boot to network in ~20 s, live H.264 to the browser,
manual per-channel recording to MPEG-TS, on-screen playback with frame stepping and
scrubbing, OSD menus driven by mouse/keyboard/network, buzzer feedback with melodies.

**Recording is manual, by command, deliberately** — `REC`/`STOP` only. No auto-record, no
circular retention. The point of writing this firmware was to *not* rebuild a CCTV DVR.

Known-open items are tracked honestly in [`docs/REVIEW.md`](docs/REVIEW.md) §3, including
the U-Boot TFTP recovery path being *available but unrehearsed* (it needs inbound UDP 69
allowed on the PC).

---

## Legal and scope

- **No vendor firmware is redistributed here.** The dumps this project was built from —
  U-Boot, the kernel, the rootfs, `app.out`, the HiSilicon SDK, the ActiveX DLL, the
  datasheet — are the manufacturers' copyrighted property and are **not** in this
  repository. Dump your own device (`tools/dvr.py backup`). What *is* published is our own
  code and our own descriptions of how the hardware behaves.
- One exception, declared: `flash/rootfs_overlay/root/run_app.sh` is the device's stock boot
  script with our hand-off hook inserted, kept whole so the diff against stock is auditable.
  It is ~40 lines of `mount`/`tar`/`exec` and exists purely for interoperability.
- The stock device credentials referenced in the docs (`root` with no password, `admin` /
  `000000`) are **factory defaults** for this device family, not anyone's secrets.
- Only ever run this against hardware you own.
- No affiliation with, or endorsement by, HiSilicon, Techwell, or the DVR's manufacturer.

## License

MIT for the code in this repository. See [`LICENSE`](LICENSE). This does not and cannot
grant any rights over the third-party firmware it interoperates with.
