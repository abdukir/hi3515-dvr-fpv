# CONTROL_PROTOCOL.md — DVR record/control command protocol

Our custom DVR firmware (`device/dvr/dvr.c`) exposes one command set over two transports that
both drive a single record-state owner (`rec_want[]`). Capture, VGA display, and the live H.264
stream are always on; these commands only control **recording** (and clock/standard).

## Transports
- **TCP control port `8090`** — the web UI (and any tool). On connect the server sends `DVR READY\n`;
  then send newline-terminated commands, one reply line each. Up to 4 concurrent clients.
- **Serial `/dev/ttyAMA1` (9600 8N1, raw)** — the external MCU with the physical switches. Same
  line protocol. Bidirectional so the MCU can poll `STATUS` and mirror record state on an LED.
  (Physical layer on the rear PTZ terminal is likely RS-485 — confirm on the board; the MCU reads
  the debounced switches and sends `REC`/`STOP` lines.)

## Commands (case-insensitive keyword; `\n` terminated)

Every reply is ONE line except `LIST`, which is `LIST <n>` then n rows then `END`.
`INFO ver=` carries the protocol version (currently **2**) so a client can tell what the
firmware supports — `webapp2` reads it at startup and lights up features accordingly.

| Command | Reply | Effect |
|---|---|---|
| `PING` | `PONG` | liveness check |
| `REC <n>` / `REC ALL` | `OK` | start recording channel n (0..3) or all |
| `STOP <n>` / `STOP ALL` | `OK` | stop recording channel n or all |
| `STATUS` | `STATUS rec=<4> std=<NTSC\|PAL\|AUTO>` | per-channel record state: `1`=recording, `w`=armed (waiting for keyframe), `0`=off |
| `STD PAL\|NTSC\|AUTO` | `OK` / `ERR recording` | set the (global) video standard — applied by the pipeline (Phase 3). **Refused while recording**, same reason as `SHOW`: it takes effect by respawning |
| `TIME <Y> <M> <D> <h> <m> <s>` | `OK`/`ERR` | set the RTC clock (fixes dated filenames); the web "sync clock" button pushes PC time here |
| `SHOT` | `SHOT <w>x<h> fmt=..` | grab the VO output frame (VGA screenshot) to `/root/rec/shot.yuv` |
| `SHOW <ch>` | `SHOW <ch>` / `ERR recording` | switch which VI channel (0..3) the VGA displays (also a "Channel" menu item). The pipeline restarts to re-latch VI→VO, so this takes a few seconds — and **is refused while recording**, because the respawn would end the capture without saying so |

### Status &amp; inventory (protocol v2 — added so the web UI needs no telnet for these)

| Command | Reply | Effect |
|---|---|---|
| `INFO` | see below | one-line machine-readable device state — the web UI's heartbeat |
| `LIST` | `LIST <n>` · `R <path> <bytes> <key>` × n · `END` | every `.ts` recording across `a1..a4`, **newest first**, max 128. `<key>` is an opaque 15-char sort key (numbered files start `9`, legacy dated ones `2`, so numbered always sort above). Selection is insert-in-order, so the cap drops the OLDEST — it used to truncate in directory order before sorting, which silently hid the newest recordings once there were more than the cap on disk. |
| `DEL <path>` | `OK` / `ERR path` / `ERR recording` / `ERR playing` | delete one recording. Refuses anything that isn't `/root/rec/aN/*.ts`, and refuses entirely while recording or playing back |
| `PIC` | `PIC bright=.. contrast=.. hue=.. sat=..` | read the TW2866 picture registers for the displayed channel |
| `PIC <BRIGHT\|CONTRAST\|HUE\|SAT> <0..255>` | same | set one, live. `SAT` writes both U and V |
| `PIC RESET` | same | restore the neutral defaults (fixes the pink/green cast after a standard change) |

### Buzzer (main board — the front-panel daughterboard is unused, see `docs/FRONT_PANEL.md`)

| Command | Reply | Effect |
|---|---|---|
| `SND` | `SND on` / `SND off` | report the menu-feedback setting |
| `SND <0\|1>` | same | turn menu beeps off/on; persisted to `/root/rec/snd` so it survives a restart. Also on the OSD **Settings > Sound** row and the web Config tab |
| `BUZZ [ms]` | `OK` | one beep, non-blocking (default 120 ms) |
| `TONE <hz> [ms]` | `TONE <hz>Hz <ms>ms` | square wave at a given pitch; **blocks** while sounding, capped at 400 ms |

### Capture chip (TW2866) diagnostics

| Command | Reply | Effect |
|---|---|---|
| `TWSTD` | `TWSTD ch1=NTSC/HSV ch2=nosignal ... using=NTSC` | per-channel decoder state: detected standard, then H/S/V for horizontal, subcarrier and vertical lock (`-` = not locked), or `nosignal` when VDLOSS is set. `using=` is what the pipeline is configured for. **This is the first thing to read when there is no picture** — it separates "no signal reaching the chip" from "chip locked but the pipeline is set to the wrong standard". |
| `REG <r> [v]` | `REG <r>=<v>` | read/write one TW2866 register (decimal). `REG 0` is channel 1's status byte |

`INFO` also carries `det=` (what the chip currently detects: `NTSC`/`PAL`/`none`) and `lock=` (a
bitmask of channels with signal), so a standard mismatch is visible rather than guessed.

The buzzer is a passive transducer (pitch is controllable), on block `0x20150000` bit 7 —
from `buzz_control()` in `tl_R9508.ko`. Record start/stop play short melodies and menu
navigation plays distinct move/enter/back tones; `INFO` reports the setting as `snd=`.

```
INFO ver=2 up=382 enc=1 std=NTSC res=704x240 fps=30 gop=15 rc=3 qp=10 br=12288
     disk=27928 ch=0 rec=0000 recmb=0,0,0,0 recsec=0,0,0,0 cli=0 pb=0 osd=0
     packs=11811 time=20260725_041249
```

| field | meaning |
|---|---|
| `ver` | control-protocol version (`DVR_VER` in `dvr.c`) |
| `up` | seconds since this DVR process started |
| `enc` | how many channels have an encoder (`enc_channels`) — the others cannot be streamed or recorded |
| `res` `fps` `gop` `rc` `qp` `br` | live encoder settings (`rc` 3 = FIXQP, 0 = VBR-capped) |
| `disk` | free MB on the recording partition |
| `ch` | VI channel currently on the VGA output |
| `rec` | per channel `1` recording / `w` armed / `0` off |
| `recmb`, `recsec` | size and elapsed time of each channel's current recording |
| `cli` | live stream viewers connected to :8091 |
| `pb`, `osd` | a clip is playing on the monitor / the on-screen menu is open |
| `time` | the device RTC. The firmware also pushes this onto the kernel clock at startup, so file mtimes are right; recordings are numbered rather than dated, so naming no longer depends on it |

### On-screen playback transport (a clip on the DVR's own monitor)

`PLAY <path>` decodes a recording full-screen on the VGA output and does not reply until
playback ENDS — so send it on its own connection (webapp2 uses `sendDetached`). Everything
below drives it from any other connection, and maps 1:1 to the on-screen control bar.

| Command | Reply | Effect |
|---|---|---|
| `PLAY <path>` | `PLAY done` / `PLAY err` | play a recording on the monitor; blocks until finished |
| `PBSTOP` | `OK` | stop and return to live |
| `PBPAUSE` / `PBPLAY` / `PBTOGGLE` | `OK` | pause, resume, toggle |
| `PBSTEP` / `PBBACK` | `OK` | one frame forward / back (pauses) |
| `PBREPLAY` | `OK` | restart from the beginning |
| `PBSEEK <permille>` | `OK` | jump to a position, 0..1000 = start..end |
| `PBSPEED <n>` | `OK` | pacing divisor: `1`=1/4x `2`=1/2x `4`=1x `8`=2x `16`=4x `32`=8x |

**Speeds above 1x play keyframes only and SEEK between them.** Reading a clip straight
through cannot beat the demuxer — measured ~1.7x on a 32 Mbps recording even when only
keyframes are decoded, because every byte still has to be read and de-packetised. So
fast-forward jumps instead: show a keyframe, seek past the content to skip, show the next.
The target position is anchored to the wall clock rather than stepped by a fixed amount, so
a slow step simply makes the next jump longer and the speed self-corrects. Measured
clip-seconds per real second: **0.50 / 0.99 / 1.98 / 4.08 / 7.80**.

(1x itself used to run at 0.62x: the loop waited a whole frame interval and *then* did
~20 ms of read/demux/feed work, so the real period was delay+work. Pacing now runs against
an advancing deadline, which absorbs the work.)

**Seeking with the arrows.** The progress bar is a navigable stop on the control bar, after
the speed button. `UI M` on it *arms* seek mode: `-`/`+` then jump through the clip instead
of moving the highlight, and `UI M` or `UI X` hands navigation back. Repeated presses within
700 ms double the step, 1% up to 10%, so a tap nudges and a burst crosses the recording; the
step resets after a pause and the bar shows it live as `SEEK +/- N%`. While armed the bar is
outlined and playback holds on the frame you land on.

`INFO` carries `pbs=<cur>/<total>` (seconds), `pbspd=` (the divisor) and `pbseek=1` while
armed, so a client can render its own transport without guessing.


### On-screen UI / OSD (VGA overlay — Milestone M3)
The OSD is a 1280×1024 ARGB1555 graphics layer (`/dev/fb0`) composited over the VGA video: a compact
status HUD (channel, `REC`, RTC clock) plus a navigable menu (Settings / Playback / Logs / Reboot).
The HUD auto-boots and its clock ticks live. The menu is driven by four keys that will map to the
MCU's physical buttons — `-`/`+` (prev/next), `M` (menu/enter), `X` (exit/back) — plus a dedicated
hardware REC switch that drives `REC`/`STOP` directly.

| Command | Reply | Effect |
|---|---|---|
| `UI <key>` | `UI open=.. menu=.. sel=.. edit=..` | feed one key: `UP`/`-`, `DOWN`/`+`, `ENTER`/`MENU`/`M`, `EXIT`/`BACK`/`X`. Opens the menu from the HUD, navigates, toggles values, and adjusts numeric picture settings (edit mode). Renders + dumps `/root/rec/fb.raw`. |
| `UICLOSE` | `OK` | close the menu (HUD only) |
| `UIPAINT` | `OK` | re-render the OSD now (refresh clock/state) + dump `fb.raw` |
| `UISHOW <0\|1>` | `OK` | show/hide the OSD graphics layer over the video |
| `UIFB` | `UIFB <w>x<h> virt=.. bpp=.. stride=.. len=..` | report fb0 geometry (diagnostic) |
| `MOUSE <x> <y> [btn]` | `MOUSE x=.. y=.. open=.. menu=.. sel=..` | inject a mouse event at absolute (x,y); btn 1=left 2=right (test without hardware) |
| `MSENS <n>` | `MSENS <n>` | mouse pointer speed in 1/8 units (8 = 1:1, default 12 ≈ 1.5×) |
| `UIFB4` | `UIFB4 fd=.. <w>x<h> stride=..` | cursor-overlay (fb4) open state (diagnostic) |

**Cursor** = a jet sprite on a **dedicated double-buffered overlay layer `/dev/fb4`** (app.out's method,
`docs/DISPLAY_PATH.md` §8): moving it draws into the hidden back buffer and `FBIOPAN_DISPLAY`-flips, so
the menu/video layer (fb0) is never touched → no flash, and only fully-drawn frames scan out → no tear.
Mouse read (`mouse.h`) drops PS/2 overflow-saturated packets (byte0 bit6/7 — the random jumps) and uses
a 1/8-px sub-pixel accumulator so slow motion is smooth.

**USB mouse:** a real USB mouse drives the menus too (until the MCU buttons are wired). The device
kernel has mousedev+usbhid; the dvr `mknod`s `/dev/input/mice` (c 13 63) and reads 3-byte PS/2
packets non-blocking. Move → an arrow cursor (drawn with a saved-background so it moves without a
full re-render) + hover-highlights the row; **left-click** = enter/toggle (numeric picture rows show
`- NNN +` and click left/right of the value to −/+); **right-click** = back/exit. NOTE: a high-power
gaming mouse (e.g. Logitech G502) can brown out the board's Ethernet PHY (link drops) — use a
powered USB hub, or expect mouse XOR network at once.

**USB keyboard:** this kernel has no evdev, so the keyboard lands on the foreground VT; the dvr
opens `/dev/tty0`, switches it to `K_MEDIUMRAW` and reads raw keycodes (`keyboard.h`).
Arrows/Enter/Space/Esc map to the same four menu keys as everything else, plus:

| key | effect |
|---|---|
| **R** | **start/stop recording, from anywhere** — live view, inside a menu, or during playback |

`R` is a field hotkey, not a menu key: one press, no navigation, and the buzzer's start/stop
melodies confirm it without looking at the monitor (the HUD also shows `armed` immediately, since
recording itself begins at the next keyframe). It targets the channel on screen when that channel
has an encoder, otherwise ch0 — with `enc_channels=1` the other three cannot be recorded at all.

Picture controls in the menu poke the **TW2866** per-channel registers live (base `ch*0x10`: +01
brightness, +02 contrast, +04/05 saturation, +06 hue). Standard PAL/NTSC and Display fill/pillarbox
are also togglable from the menu (same effect as `STD` / the config). Verified end-to-end on-device
2026-07-24 by reading `/dev/fb0` back (pixel-accurate) — see `docs/DISPLAY_PATH.md` for the fb0
ARGB1555 / 1280×1024 bring-up that made a full-screen overlay possible.

Recording actually starts/stops at the next **keyframe** (so each file begins with SPS/PPS/IDR),
usually within one GOP (~1 s). Files: `/root/rec/aN/NNNN_chC_MMmSSs.ts` — MPEG-TS with real PTS
taken from the encoder's own clock — served/downloaded as MP4 by the web UI.

**Why numbered and not dated.** The RTC keeps real time across power-off, but the kernel clock
booted at 1970 and nothing synced the two, so recordings carried a plausible name and a 1970/1980
mtime; and any RTC error would sort the newest recording into the middle of the list. A counter
cannot drift and sorts lexicographically, so largest number == newest. The duration is in the name
because that is what you want when picking a clip out of a list; it is only known at STOP, so the
file is written as `NNNN_chC_REC.ts` and renamed on close — **a leftover `_REC` file means that
recording was cut short.** (The firmware now also sets the kernel clock from the RTC at startup, so
file dates are correct as well; the naming no longer depends on it either way.)

**Display aspect.** The encoder emits 704x240 (NTSC) or 704x288 (PAL) — one full-width field — and
its SPS carried no VUI, so players assumed square pixels and drew a 2.93:1 strip. The firmware now
rewrites the SPS to carry an Extended_SAR (5:11 / 6:11, exactly half the ITU-R BT.601 values because
we hold half the lines), so both recordings and the live stream display as 4:3 everywhere.

## Examples
```
$ nc 192.168.1.108 8090
DVR READY
TIME 2026 7 24 1 42 21
OK
REC 0
OK
STATUS
STATUS rec=1000 std=NTSC
STOP 0
OK
```

## Notes
- Verified over TCP 2026-07-24 (PING/TIME/REC/STATUS/STOP; TIME corrected the RTC → correct
  dated filename). Serial path implemented; test with the MCU or a TX↔RX loopback.
- The record-state owner is shared, so web and MCU stay consistent; `STATUS` is the source of truth.
