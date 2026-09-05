# DISPLAY_PATH.md — VGA/CVBS video display on the RR104P (for the low-latency ground-station screen)

Goal: show live analog video on the **VGA output** with the lowest possible latency (codec-free),
single channel fullscreen, switchable between the 4 inputs, while recording runs in parallel. This
is decoded from the same `dump/livedump_20260723/mpp_init_trace.log` that gave us M0. The display
path has **no H.264 in it** — `VI → VPP → VO` is all hardware (VPP = scaler, ~1 frame), so it is
inherently low-latency; the win is simply *not* routing the screen through encode/decode.

## The two physical outputs (decoded from `/dev/vo` `0x40384f02` + `0x40244f0d`)

app.out drives **both** outputs at once:
- **VO dev 0** (first `/dev/vo`, fd=8 in trace): `enIntfType=0` **CVBS**, `enIntfSync=1` **NTSC**,
  layer **720×480** — the BNC/composite jack (SD monitor).
- **VO dev 1** (fd=23): `enIntfType=2` **VGA**, `enIntfSync=9` **1280×1024_60**, layer **1280×1024**
  — **the ground-station VGA screen.** Your monitor must accept **1280×1024 @ 60 Hz**.

`0x40384f02` (56 B) = VO_PUB_ATTR: `{u32 bgColor?, enIntfType, enIntfSync, …}` (dev1 = `03,02,09,…`).
`0x40244f0d` (36 B) = video-layer attr: `{…, canvasW, canvasH, dispW, dispH, refresh 0x1e, 0x13, -1}`
(dev1 = `…,0x500(1280),0x400(1024),0x2d0,0x1e0,…`).

## VO device / layer / channel ioctls (magic 'O' = 0x4f)

| ioctl | sz | meaning |
|---|---|---|
| `0x40044f53` | 4 | early per-handle setup (dev0=`0x200`, dev1=`0`) — call first on each vo handle |
| `0x00004f01` | 0 | (dev-level) |
| `0x40384f02` | 56 | **SetPubAttr** (intf type/sync — VGA vs CVBS) |
| `0x00004f00` | 0 | **Enable dev** |
| `0x40244f0d` | 36 | video-layer attr (canvas/display size) |
| `0x00004f0b` | 0 | enable layer |
| `0x401c4f1f` | 28 | **channel attr = window** `{chn, x, y, w, h, 1, 0}` — the display rectangle |
| `0x00004f1d` | 0 | **enable channel** |

Stock uses **four 360×240 windows** (a 2×2 mosaic): e.g. tile3 `{1, x=0x168, y=0xf0, w=0x168, h=0xf0}`.
**For single-cam fullscreen: one channel `{0, 0, 0, 1280, 1024, 1, 0}`** — that's the whole change.

## VPP (scaler) — `/dev/vpp`, magic 0x50

Per camera: `0xc03c5002` (60 B) **CreateChn** (`{1, chn, 0, 0x28,0xc,0xe4,0x14, 8, 0x80, 0x14, …}`,
returns a handle 0x44/0x48/0x4c/0x50), then `0x400c5007` (12 B) and `0x4014500d` (20 B) configure the
scaler geometry. `0x40045004` (4 B) = start. For one fullscreen channel we need **one** VPP channel
scaling the selected VI to 1280×1024 (not four to tiles).

## The routing / binding — IMPLICIT BY INDEX (important)

There is **no `SYS_Bind` / VI-side / VO-side bind ioctl** in the trace — the VI fds only do capture
calls. So the vendor driver routes **by channel index**: VPP chn N ← VI chn N (via the VB pool),
VO chn N ← VPP chn N. The **`/dev/vd`** device (`0x40085705`, 8 B, values `{4,0}` and `{1,2}`) sets
the output mux (which source/layer feeds which physical output). This is the one area to confirm on
the monitor: set up VI + VPP(chn) + VO(dev1 fullscreen chn) with matching indices and video should
appear; the VD mux picks VGA.

## Plan to bring it up (iterate on-device with the monitor, like M0)

1. Keep M0 recording (VI×4 + VENC×4) running — display is an added consumer, not a replacement.
2. Add VO **dev 1 (VGA 1280×1024)** setup (replay fd=23 sequence verbatim), one **fullscreen** VO
   channel, one VPP channel scaling the selected VI, VD mux to VGA.
3. Test on the monitor: expect the selected camera fullscreen. **Switch channel** = reconfigure the
   VPP/VO channel source index (or the VD mux) — no re-init, so switching is instant and latency is
   identical per channel.
4. If nothing shows: re-trace app.out **with a camera connected** (real signal) and dump the VD/VPP
   structs — the proven M0 method — to pin the exact mux/route.

## Open items to resolve on-device
- **VO device selection**: how a `/dev/vo` open maps to dev0(CVBS) vs dev1(VGA) — sequential open
  order, or the `0x40044f53` value, or the pub-attr. Confirm by enabling only dev1 and checking VGA.
- **VD mux** (`0x40085705`) exact meaning of `{4,0}`/`{1,2}` — which selects VGA + fullscreen source.
- Whether one VPP channel can scale D1→1280×1024 fullscreen, or VO does the upscale.

---

# The OSD graphics layer: /dev/fb0 at 1280x1024 16bpp (RE of app.out, 2026-07)

RE of `dump/ghidra/app.out` to answer: **why does HiFB reallocate fb0 to 2.6 MB (1280x1024x2)
for app.out, but stay at 829440 B (720x576x2) for us?** Function `FUN_00095bac` (called once from
`main` @ `0x18ca0`) is the entire fb bring-up. Trace: `dump/livedump_20260723/mpp_init_trace.log`
lines 113-135 (fb0 = fd 27, fb2 = fd 25).

## 1. The observed stock sequence (verbatim, in order)

Everything below happens **after** the VGA VO output is already programmed to 1280x1024. In the
trace, immediately before the fb block (lines 100-114):

```
open /dev/vo (=23, the VGA dev)
ioctl 0x40044f53  {0}                       # per-handle early setup
ioctl 0x00004f01                            # dev-level
ioctl 0x40384f02  {enIntfType=2(VGA), enIntfSync=9(1280x1024_60), ...}   # VO_SetPubAttr (56B)
ioctl 0x00004f00                            # enable VO dev
ioctl 0x40244f0d  {canvasW=0x500(1280), canvasH=0x400(1024), 0x2d0,0x1e0, ...}  # video-layer attr (36B)
ioctl 0x00004f0b                            # enable video layer
```

Then `FUN_00095bac` runs:

```
open /dev/vd  (=24)
ioctl 0x40085705  IN 8B = 04 00 00 00 00 00 00 00     # {u32=4, u32=0}   graphic bind/mux
ioctl 0x40085705  IN 8B = 01 00 00 00 02 00 00 00     # {u32=1, u32=2}
  -- fb2 (SD/CVBS graphics, 720x480) branch, then fb0: --
open /dev/fb0 (=27)
ioctl 0x400c465d  (FBIOPUT_ALPHA_HIFB, 12B) = 01 00 00 00 | 00 00 00 00 | 00 ff 00 00
ioctl 0x00004600  (FBIOGET_VSCREENINFO)      # read current var FIRST
ioctl 0x00004601  (FBIOPUT_VSCREENINFO)      # write modified var (1280x1024x16 ARGB1555)
mmap(NULL, 0x280000=2621440, PROT_READ|WRITE, MAP_SHARED, fb0, 0)   # 2.6 MB  <-- the win
memset(buf, 0, 0x280000)
ioctl 0x00004600  (FBIOGET_VSCREENINFO)      # re-read
ioctl 0x00004606  (FBIOPAN_DISPLAY)          # pan (yoffset=0)
ioctl 0x00004602  (FBIOGET_FSCREENINFO)      # later, read fix info
```

There is **no HiFB show/colorkey ioctl on fb0** -- the overlay is shown implicitly once the layer is
bound and vscreeninfo is set; compositing is done purely with the alpha config (below).

## 2. The exact FBIOPUT_VSCREENINFO app.out writes

app.out does **FBIOGET_VSCREENINFO first**, then overwrites **only these fields**, leaving
everything else (xoffset, yoffset, grayscale, nonstd, activate, height, width, pixclock, all
margins, hsync/vsync_len, sync, vmode, rotate) **exactly as GET returned them**:

| field            | offset | value app.out writes            | source (app.out global) |
|------------------|--------|---------------------------------|-------------------------|
| `xres`           | 0      | **1280**                        | `*0x304b90`             |
| `yres`           | 4      | **1024**                        | `*0x304b8c`             |
| `xres_virtual`   | 8      | **1280** (= xres)               | copies xres             |
| `yres_virtual`   | 12     | **1024** (= yres, single buf)   | copies yres             |
| `bits_per_pixel` | 24     | **16**                          | literal 0x10            |
| `red`   (off,len,msb)  | 32 | **{10, 5, 0}**              | `@0x304bd8`             |
| `green` (off,len,msb)  | 44 | **{5, 5, 0}**               | `@0x304bcc`             |
| `blue`  (off,len,msb)  | 56 | **{0, 5, 0}**               | `@0x304bc0`             |
| `transp`(off,len,msb)  | 68 | **{15, 1, 0}**              | `@0x304be4`             |

**Pixel format is ARGB1555, NOT RGB565.** red bit10-14, green bit5-9, blue bit0-4, alpha bit15.
This is the single most important correction to our current attempt (we were setting RGB565
red 11/5, green 5/6, blue 0/5). RGB565 has no alpha channel, so the per-pixel OSD compositing
below cannot work with it.

mmap length app.out uses = `yres * xres * 2` = 1280*1024*2 = 0x280000. It does **not** double-buffer
(yres_virtual == yres).

Note the static `.data` defaults are 1024x768 (`*0x304b90=1024`, `*0x304b8c=768`); they are
overwritten at runtime by `FUN_0006335c` (see section 4) before `FUN_00095bac` runs.

## 3. The alpha / compositing config (FBIOPUT_ALPHA_HIFB, 0x400c465d, 12 bytes)

`HIFB_ALPHA_S` = `{ BOOL bAlphaEnable; BOOL bAlphaChannel; u8 u8Alpha0; u8 u8Alpha1; u8 u8GlobalAlpha; u8 u8Reserved; }`

app.out writes for fb0 (and fb2): `bAlphaEnable=1, bAlphaChannel=0, u8Alpha0=0x00, u8Alpha1=0xFF,
u8GlobalAlpha=0, u8Reserved=0`  (bytes `01 00 00 00 00 00 00 00 00 ff 00 00`).

Combined with ARGB1555 this gives the OSD blend rule:
- pixel alpha-bit = 0 -> uses Alpha0 = 0x00 -> **fully transparent** (video shows through)
- pixel alpha-bit = 1 -> uses Alpha1 = 0xFF -> **fully opaque** (graphics shown)

So draw opaque graphics as `0x8000 | rgb555`, and clear/transparent regions as `0x0000`. This is a
per-pixel key via the top bit; **no colorkey ioctl is used.**

## 4. Where 1280x1024 comes from -- the VO-resolution / fb-size link (FUN_0006335c)

`FUN_0006335c(param_1 = res_index, param_2 = vga_flag)` is called from `main` @ `0x18c0c`
**before** `FUN_00095bac`. It writes BOTH the fb0 resolution globals AND the VO enIntfSync in
lockstep. For `param_2 != 0` (VGA/HD output):

| param_1 | fb0 xres (`*0x304b90`) | fb0 yres (`*0x304b8c`) | internal code | -> VO enIntfSync |
|---------|------------------------|------------------------|---------------|------------------|
| **0**   | **1280**               | **1024**               | 0xB           | **9** (1280x1024_60) |
| 1       | 1024                   | 768                    | 9             | 8 (1024x768_60)  |
| 2       | 800                    | 600                    | 7             | 7 (800x600_60)   |

The internal code is mapped to the SDK `VO_INTF_SYNC` enum stored at `*(cfg+0x1bc)`, then
`FUN_001397c0()` is called, which builds a 56-byte VO_PUB_ATTR + 36-byte layer attr from that enum
(`FUN_00139264`) and applies them (`FUN_0013961c` -> the `0x40384f02` + `0x40244f0d` ioctls).
**=> The VGA VO output resolution and the fb0 canvas are the same knob, set together, VO applied
first.** fb0 is the HD/VGA-VO graphics overlay; fb2 is the SD/CVBS-VO graphics overlay (hardcoded
720x480, `0x2d0`); fb1 = 2nd HD graphics layer; fb4 = HD hardware cursor.

## 5. THE STEP WE ARE MISSING

Our failing test configures **fb0 in isolation** while the VGA VO output/graphics-layer is still at
the SD default (720x576). On this HiFB, `FBIOPUT_VSCREENINFO` **accepts `xres`/`yres`
cosmetically (they are just stored and echoed back by GET), but the mmappable buffer size
(`fix.smem_len`) is driven by `xres_virtual`*`yres_virtual`*bpp/8, and HiFB clamps the *virtual*
dimensions to the attached VO graphics-layer canvas.** With the layer still 720x576, the virtual
dims clamp back to 720x576 -> smem_len stays 720*576*2 = **829440 B**, exactly our symptom, even
though the reported `xres`/`yres` read 1280x1024.

**Fix / required order for our freestanding code:**

1. **Program the VGA VO output to 1280x1024 FIRST** (VO dev = VGA): `/dev/vo`
   `0x40044f53{0}` -> `0x4f01` -> `0x40384f02{enIntfType=2, enIntfSync=9, rest 0}` (56B) ->
   `0x4f00`(enable dev) -> `0x40244f0d{canvasW=1280, canvasH=1024, dispW=720?, dispH=480?, 0x1e,0x13,-1}`
   (36B) -> `0x4f0b`(enable layer). (This is our existing VO dev1 bring-up from M0 -- it must run
   before fb0, not after.)
2. **Graphic bind via /dev/vd**: open `/dev/vd`, `ioctl(0x40085705, {u32 4, u32 0})` then
   `ioctl(0x40085705, {u32 1, u32 2})`. (Replay verbatim -- exact semantics unconfirmed; likely
   binds the graphics layers to their VO outputs / sizes the fb canvas.)
3. **Then** open `/dev/fb0` and do: PUT_ALPHA(sec 3) -> **GET_VSCREENINFO** -> modify only the sec 2
   fields (crucially set `xres_virtual=1280` AND `yres_virtual=1024`, ARGB1555 bitfields, bpp=16;
   DO NOT zero the rest of the struct) -> PUT_VSCREENINFO -> mmap(2621440) -> memset -> PAN_DISPLAY.

### Freestanding C skeleton (raw ioctls)

```c
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOPAN_DISPLAY     0x4606
#define FBIOPUT_ALPHA_HIFB  0x400c465d
#define VD_GRAPHIC_BIND     0x40085705

/* --- step 1: VGA VO @1280x1024 must already be up (see M0 VO dev1) --- */

/* --- step 2 --- */
int vd = open("/dev/vd", O_RDWR);
unsigned int b1[2] = {4,0}; ioctl(vd, VD_GRAPHIC_BIND, b1);
unsigned int b2[2] = {1,2}; ioctl(vd, VD_GRAPHIC_BIND, b2);

/* --- step 3 --- */
int fb = open("/dev/fb0", O_RDWR);
unsigned char alpha[12] = {1,0,0,0, 0,0,0,0, 0x00,0xFF,0,0}; /* en=1,chan=0,a0=0,a1=255 */
ioctl(fb, FBIOPUT_ALPHA_HIFB, alpha);

struct fb_var_screeninfo v;
ioctl(fb, FBIOGET_VSCREENINFO, &v);      /* READ FIRST, then patch in place */
v.xres = 1280;  v.yres = 1024;
v.xres_virtual = 1280;  v.yres_virtual = 1024;   /* <-- drives smem_len */
v.bits_per_pixel = 16;
v.red.offset=10; v.red.length=5; v.red.msb_right=0;   /* ARGB1555 */
v.green.offset=5;  v.green.length=5;  v.green.msb_right=0;
v.blue.offset=0;   v.blue.length=5;   v.blue.msb_right=0;
v.transp.offset=15; v.transp.length=1; v.transp.msb_right=0;
ioctl(fb, FBIOPUT_VSCREENINFO, &v);

unsigned char *p = mmap(0, 1280*1024*2, PROT_READ|PROT_WRITE, MAP_SHARED, fb, 0);
memset(p, 0, 1280*1024*2);               /* 0x0000 = transparent everywhere */
ioctl(fb, FBIOGET_VSCREENINFO, &v);
ioctl(fb, FBIOPAN_DISPLAY, &v);
/* draw: pixel = 0x8000 | (r5<<10)|(g5<<5)|(b5) for opaque; 0x0000 for see-through */
```

## 6. On-device tests to disambiguate (uncertain bits)

- **Discriminator for sec 5:** after PUT_VSCREENINFO, log `v.xres_virtual`/`v.yres_virtual` returned
  by a following GET. If they come back **720/576** (clamped) -> confirms the VO-canvas-clamp theory:
  the VO layer must be 1280x1024 first. If they read **1280/1024** but smem is still 829440 ->
  it is a HiFB smem_len bug and the `/dev/vd` bind (step 2) or layer attach is what actually grows it.
- **`/dev/vd 0x40085705 {4,0}/{1,2}` semantics** are unconfirmed (driver not in Ghidra). Replay
  verbatim; if fb0 grows without it, it is optional.
- Whether the VO **layer canvas** (`0x40244f0d` canvasW/H) rather than the VO **pub-attr sync**
  is the field HiFB reads for the fb canvas -- set both to 1280x1024 to be safe.

## 7. Bonus -- how app.out draws the OSD

app.out uses the **TDE 2D engine** (`/dev/tde`, fd 26): `0x40047401` (get handle) ->
`0x407c7405` (124-byte bit-blit descriptor referencing the fb0 phys addr @1280x1024, ARGB1555) ->
`0x40107409` -> `0x4004740a` (submit/flush). i.e. OSD bitmaps are hardware-blitted into fb0. For our
purposes **direct mmap writes to the fb work equally well** (no TDE required); TDE is only a speed
optimization for large blits/fills.

## 8. How app.out does the USB MOUSE + CURSOR (RE of app.out, 2026-07)

Source: Ghidra project DVRRE (app.out) + boot ioctl trace
`dump/livedump_20260723/mpp_init_trace.log`. Goal: fix our cursor FLASH-on-move and
erratic movement. Bottom line: **app.out does NOT use a HiFB hardware-cursor register.
It puts the pointer on a DEDICATED full-size overlay framebuffer (fb4 HD / fb1 SD),
separate from the menu layer (fb0), and that overlay is DOUBLE-BUFFERED + flipped with
FBIOPAN_DISPLAY.** That is what kills the flash: cursor motion never touches the menu
layer, and the layer it does touch is never the one being scanned out.

### 8.1 Layer map (from the trace, this box = NTSC)
| node | mmap size | geometry (ARGB1555) | role | gets SHOW/ALPHA re-set? |
|------|-----------|---------------------|------|----|
| fb0  | 0x280000  | 1280x1024 x2        | HD OSD / menu (main graphics)      | no |
| fb2  | 0x0a8c00  | 720x480 x2          | SD OSD / menu                      | no |
| fb1  | 0x151800  | 720x480, **double-buffered** | SD **cursor/top overlay** | YES |
| fb4  | 0x500000  | 1280x1024, **double-buffered** (2x 0x280000) | HD **cursor/top overlay** | YES |

Only fb1 and fb4 get the extra `FBIOGET_SHOW_HIFB (0x80044666)` + `FBIOPUT_ALPHA_HIFB
(0x400c465d, tail 00 ff 78 78)` after mmap — they are the overlay layers composited
*above* fb0/fb2. The cursor (a 32x32 sprite) is drawn INTO the 1280x1024 overlay; the
whole layer is dedicated to cursor + transient popups.

### 8.2 Mouse input  (FUN_000974bc OpenMouseFd, FUN_000984e0 event loop)
- Device: **`/dev/mice`** opened `open(path, O_RDONLY)` (blocking; the loop uses
  `select()` with a 200 ms timeout, fd-set = {mouse fd, keypad/IR pipe fd}).
- Read: **`read(fd, &pkt, 20)`** — a **20-byte `struct mousedev_motion`**, NOT the 3/4-byte
  PS/2 packet. Layout (from field use): `{ timeval time(8); int32 dz(8..11); uint32
  buttons(12..15); uint32 pos(16..19) }` where `pos = (x & 0xffff) | (y << 16)` are
  **ABSOLUTE, already-accumulated, already-clamped** screen coords. buttons bit0=L bit1=R;
  dz<0/>0 = wheel up/down.
- **There is NO acceleration curve in app.out.** The delta->absolute accumulation and the
  screen clamp are done by the DVR's *custom kernel mousedev driver*. app.out only applies a
  fixed **linear scale** (FUN_001cd174/268/57c/920 = int->double, multiply, double->int) to
  map the driver's coord space onto the OSD's (0x2d0=720 wide x N*0x30 tall). => not directly
  reusable by us: we read RAW `/dev/input/mice` (relative 3-byte PS/2), so our accumulation &
  feel are OUR problem (see 8.5).

### 8.3 Cursor bitmap  (`data/pics/cursor.bits`, loader FUN_00098288)
- **32x32, ARGB1555, 2 bytes/px** (cursor object: width@+8=0x20, height@+0xc=0x20,
  pixels = malloc(0x800)=2048=32*32*2).
- File format: 4x u32 header `[?][?][rowCount=32][rowBytes=64]`, then 32 rows of 64 bytes.
- Transparency: source colour **0x001F** = transparent. Loader rewrites every pixel: if
  value != 0x001F it sets the top bit (`|= 0x8000`, ARGB1555 alpha=1 = opaque); transparent
  pixels keep alpha=0. The blitter (FUN_0009780c) then copies **only pixels with bit15 set**
  (`byte & 0x80`), i.e. per-pixel alpha keying, no restore-under of individual pixels needed.

### 8.4 The flicker-free draw pipeline  (FUN_00097d64 -> FUN_00097904 / erase FUN_00097dc4)
Per mouse move, cursor at (x,y):
1. `FUN_0009780c(sprite, dstbuf, x, y)` — SW blit the 32x32 sprite into the overlay's *back*
   buffer, copying only alpha-set pixels.
2. TDE 2D engine composites/copies buffers: `FUN_001cb828`=TDE2_BeginJob,
   `FUN_001cba58/bc54/bb7c`=Bitblit/QuickCopy (save-under + draw), `FUN_001cc160`=EndJob,
   `FUN_001cc1e0`=WaitForDone. (TDE = hardware 2D; optional for us.)
3. **`FUN_000957b8(fb, bufidx)` = FBIOPAN_DISPLAY flip**: `ioctl(fb,0x4600,&v)` (GET
   VSCREENINFO) -> `v.yoffset = bufidx * v.yres` -> `ioctl(fb,0x4606,&v)` (PAN_DISPLAY).
   `FUN_00095a50` toggles bufidx 0<->1. This is the classic double-buffer flip; the newly
   drawn buffer is only shown once complete => **no half-drawn / no-cursor frame ever scans
   out => no flash.**

There is **no** FBIOPUT_CURSOR / HIFB_CURSOR_S / 0x4608 anywhere. Confirmed software cursor
on a hardware overlay layer, double-buffered.

### 8.5 What to change in device/dvr (fb.h + mouse.h) to fix our FLASH + aim
Our flash = we draw the cursor straight onto the single-buffered, being-scanned-out fb0 with
`fbreal_px` and erase by memcpy from the shadow: both race the CRTC. Two fixes, in order of
fidelity to app.out:

**FIX A (recommended, = app.out): dedicated cursor overlay.** Bring up a SECOND HiFB layer
above fb0 and host the cursor there, so cursor motion never touches the menu:
- `open("/dev/fb1"` (SD) or `"/dev/fb4"` (HD), `O_RDWR)`; GET/PUT VSCREENINFO to ARGB1555 at
  the screen size (for double-buffer set `yres_virtual = 2*yres`); mmap.
- Enable per-pixel alpha + show: `FBIOPUT_ALPHA_HIFB (0x400c465d, 12B: [0]=1 enable,
  [9]=0xff)` and `FBIOPUT_SHOW_HIFB (0x40044665, u32=1)`. (Our fb.h already has these
  constants; note the trace *reads* show via `FBIOGET_SHOW_HIFB 0x80044666 _IOR('F',102,4)`.)
- Clear the overlay to 0x0000 (transparent) so only the sprite shows; draw sprite; if
  double-buffering, PAN_DISPLAY-flip (yoffset = bufidx*yres) instead of drawing in place.
- ON-DEVICE TEST NEEDED: confirm fb1/fb4 actually enumerate & enable on our kernel after our
  VO/HiFB bring-up (they exist on the stock boot per the trace, but our MPP init may not
  register all layers). If only fb0 exists, use Fix B.

**FIX B (single layer): double-buffer fb0 itself.** Set `yres_virtual = 2*yres`, mmap 2x,
draw the full frame (menu + cursor) into the back half, then `FBIOPAN_DISPLAY` with
`yoffset = back*yres`. No direct-to-scanout writes => no flash. Heavier (re-blit menu each
frame) but needs no extra layer. This is exactly FUN_000957b8.

**Aim / erratic movement (mouse.h):** unrelated to app.out (its kernel driver hides it). Ours:
(1) `g_msens` multiplies integer deltas by 3 => 3px quantised steps; use a **sub-pixel
accumulator** (accumulate dx*sens into a fixed-point remainder, move by the integer part) for
smooth slow motion. (2) We ignore the PS/2 **overflow bits** (byte0 bit6=X-ovf, bit7=Y-ovf):
when set, that delta is saturated garbage — **drop the packet** (or clamp) instead of applying
it, which removes the big random jumps. (3) Our 3-byte resync on `bit3` is correct; keep it.
