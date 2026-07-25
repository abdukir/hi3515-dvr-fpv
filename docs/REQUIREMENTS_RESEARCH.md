# REQUIREMENTS_RESEARCH.md — feasibility research for the FPV DVR requirements (2026-07-24)

Deep search behind the requirements: manual per-channel recording, MP4 files with date-time
names, web playback/download, PAL/NTSC handling, no-blue-screen, aspect/resolution, and external
(MCU/switch) record control. Sources: `dump/livedump_20260723/` (trace, dmesg, /proc), `dump/ghidra/app.out`,
`device/sdk/include/`, and TW2868/PCF8563 datasheet knowledge. Verdicts below feed the build plan.

## 1. Manual recording (start/stop, per-channel, any subset / all)  — ✅ EASY
Capture → encode → **VGA display + live stream stay always-on** (so you always see video). "Recording"
is just **gating whether encoded frames are written to disk**, per channel. One record-state owner
(a per-channel on/off set) fed by both the web UI and the MCU. No pipeline restart to start/stop.
- Today `dvr.c` writes every frame unconditionally; change to `if (rec_on[ch]) write…`.
- Start on a **keyframe** so each recording begins cleanly (independently decodable).

## 2. MP4 files + date-time names + web view/download  — ✅ FEASIBLE
- **Clock**: `/dev/rtc` (PCF8563, battery-backed, verified reading correct wall-clock).
  `RTC_RD_TIME=0x80247009` / `RTC_SET_TIME=0x4024700a`, 36-byte `struct rtc_time` (9×int32:
  sec,min,hour,mday,mon[0-11],year[+1900],wday,yday,isdst). → filenames `YYYYMMDD_HHMMSS_chN`.
  (Stock uses UTC; pick one convention.)
- **Container**: lossless remux verified both ways with ffmpeg `-c copy` (instant, no re-encode):
  `.h264 ↔ .ts ↔ .mp4`. So the device stores compact segments; the PC/web serves real **MP4**.
  - **Recommended device format = MPEG-TS** (`.ts`): easy to mux in freestanding C, carries real
    **PTS** (correct playback even when a noisy signal drops frames), directly playable, lossless→MP4.
    Raw `.h264` is simplest but has no timing (remux must assume fps). Native on-device `.mp4`
    (fragmented-MP4 muxer) is possible but the most C work.
- **Web playback/download**: the old `webapp/server.js` already has the pattern — ffmpeg remux +
  seekable HTTP range `<video>` playback + download. Adapt it for our segments/format.

## 3. PAL / NTSC  — ⚠️ GLOBAL only (not per-channel); auto-detect needs driver RE
- **Set standard**: `ioctl(/dev/tw_286x, 0xc00448d3, {1=NTSC | 2=PAL})` — **one global call**, not
  per-channel (trace line 25; emitter `FUN_00135204`). The same global flag also drives VB-pool
  geometry and VI capture height (240 NTSC / 288 PAL).
- **Mixed standards per channel = NO.** The 4 cameras are time-multiplexed into ONE BT.656 stream
  into a single-norm VIU (4D1). PAL(625/50) and NTSC(525/60) can't coexist on that shared stream —
  hardware architecture limit. **Enforce one standard for all 4 channels.**
- **Software override of the jumper = YES** — we issue `0xc00448d3` ourselves, so a **web-UI /
  MCU toggle can pick PAL/NTSC without touching the physical jumper**.
- **Auto-detect** (read the per-channel detected standard, TW2868 STD status reg ~0x1C): the driver
  can (dmesg `video_mode=NTSC`; app.out has a `TL_AUTO_PAL_NTSC` string) but **stock app.out never
  reads it back**. To auto-detect we must read the register via the `0xc00448d0` I2C hook — needs the
  **`.ko` register map** (see §6).
- **Display side** (VO): VGA output timing stays FIXED; only the VI norm + VO **layer canvas**
  (480→**576**) + framerate (30→25) change for PAL. Our `dvr.c` hard-codes NTSC — make it norm-aware.

## 4. Blue screen on signal loss  — ✅ LIKELY SOLVED for FPV; one live test to confirm
Three possible blue sources:
1. **app.out's "Video Loss" blue OSD** — **gone for free**: our firmware replaces app.out and never
   draws it. This is the usual "blue screen" people see.
2. **VO background** — already ~black (`0x03`), not blue (the `0x0000FF` is dead SDK-sample code).
   We'll set it to pure black (`0x00`) so pillarbox bars are black.
3. **TW2868 internal blue-substitution on total loss-of-sync** — decoders *can* force a solid raster
   when they lose sync. For a **noisy-but-present** FPV signal the decoder passes the noise (no blue).
   Only **total** loss (no cable) might substitute. If it does, disable via the TW2868 "force-blue on
   VDLOSS → pass-through" register through the `0xc00448d0` I2C hook — needs the `.ko` map (§6).
- **Net**: dropping app.out's OSD very likely gives you noise-on-weak-signal already. The OSD-chip
  board is only a fallback for total-loss. **Confirm with a live pull-the-camera test.**

## 5. Aspect ratio / resolution  — ✅ FEASIBLE
- **Pillarbox** (correct aspect, black bars) = set the VO channel window `stRect` to the aspect-fit
  rectangle; the bars are the (now-black) VO background. Exact rects: 1280×1024 → `{x0,y32,w1280,h960}`;
  1366×768 → `{x171,y0,w1024,h768}`.
- **Per-channel encode resolution** configurable via VENC attr (already have `dvr.conf`
  width/height/bitrate/gop/fps). PAL sources → taller capture (288/576).
- **Channel-switch bug found**: `display_vga()` writes the channel arg into the VO **priority**
  field, so it always shows channel 0. Fix: select the VO channel per-fd via `0x40044f53=N`
  (routing is by-index VO↔VPP↔VI). Then switching = show one channel's fullscreen window.
- **VGA modes that work**: 1024×768(8), 1280×1024(9), 1366×768(10), 1440×900(11). 720p/1080p are
  rejected over the VGA connector (VESA only). `/dev/vd 0x40085705` is an **OSD graphic bind, not a
  video mux** (earlier note was wrong) — irrelevant to video switching.

## 6. External control (MCU + switches) + clock  — ✅ ttyAMA1 or network
- **MCU link → `/dev/ttyAMA1`** (second PL011 UART @ MMIO 0x200a0000, IRQ12; node 204,65). Separate
  silicon from the debug console (ttyAMA0 @ 0x20090000) — **no console conflict**. Stock uses it
  **only for PTZ** (Pelco), so it's free for us. Likely wired to the rear **PTZ terminal as RS-485**
  via a transceiver — **verify the physical layer + baud on the board** (expect 9600 8N1). A tiny
  ASCII line protocol (`REC1`/`STOP1`/`PING`, bidirectional for status LEDs) is ideal.
- **Don't read switches on the DVR** — no usable userspace GPIO (`tl_gpio_i2c` is in-kernel only, no
  sysfs/char node). Read switches on the **MCU** (debounced) and forward over serial.
- **Network** = good secondary/remote path (web UI): add a small control port to our server.
- **Front panel / IR** (`/dev/panel` 10,138; `hi_ir`) repurposable as triggers but event format
  needs RE — lowest priority.

## Key unblock for §3-auto and §4: pull the driver `.ko`s
`tw286x_R9508.ko` (24628 B), `tl_R9508.ko`, `tl_gpio_i2c.ko` live in `/lib/modules/...` on-device only
(not in the dump). They hold the exact TW2868 register map behind `0xc00448cf/d0/d3`. Pull + RE them
to nail **per-channel standard auto-detect** and the **blue-substitution disable** bit. Everything
else can proceed without them.

## Decisions the plan needs
1. **Recording location + format**: device→`.ts`→PC-remux-to-MP4 (recommended, standalone, real
   timestamps) · device native `.mp4` (fMP4, more work) · PC-side recording (simplest MP4, needs PC up).
2. **SATA storage**: delete stock `fly*.ifv` (reclaim ~29 GB/partition, keep vfat) · reformat clean.
   Both wipe stock recordings (confirmed not needed). SATA is currently 99% full.
3. **PAL/NTSC**: software web/MCU toggle (easy, global) · + auto-detect (needs `.ko` RE).
4. **Pull the `.ko`s now** to unblock auto-detect + blue-disable RE? (recommended yes.)
