# PLAYBACK_PATH.md — VDEC → VO playback pipeline (from stock `app.out` RE)

How the stock DVR app decodes a recorded H.264 stream and shows it full-screen on the
VGA/VO. Reverse-engineered from `dump/ghidra/app.out` (Ghidra project DVRRE). This is the
missing half of our own firmware: live is `VI → VO` (codec-free); playback is `VDEC → VO`.

**SoC MPP reality:** the app is *not* raw-ioctl; it links the HiSilicon MPP userland (MPI). But
the MPI wrappers are tiny and each is one `open(node)` + one/two `ioctl()`. We extracted the node
paths, the exact `ioctl` request codes, and the struct bytes — everything needed to talk to the
same kernel drivers directly from freestanding OABI C, exactly like we did for VENC.

The vendor library files are `lib_vdec.c` / `lib_vdev.c` (VDEC), plus `HI_MPI_VI_BindOutput` /
`HI_MPI_VDEC_BindOutput` glue. Function addresses below are in `app.out`.

---

## 0. Device nodes and ioctl magic

| Module | Node        | `open` flags | ioctl magic |
|--------|-------------|--------------|-------------|
| VDEC   | `/dev/vdec` | `O_RDWR` (2) | `'D'` = 0x44 |
| VO     | `/dev/vo`   | `O_RDONLY`(0)| `'O'` = 0x4f |
| VI     | `/dev/vi`   | `O_RDONLY`(0)| `'I'` = 0x49 |

All decode with the standard Linux `_IOC` layout `dir[31:30] size[29:16] type[15:8] nr[7:0]`.
The app opens **one fd per channel** and keeps an fd table; the first ioctl on a fresh fd is
always the "attach this fd to channel N" call.

---

## 1. VDEC ioctls (`/dev/vdec`, magic `'D'`)

| Request      | `_IOC`                | Meaning (our name)              | arg |
|--------------|-----------------------|---------------------------------|-----|
| `0x40044414` | `_IOW('D',0x14,4)`    | **AttachChn** (fd ↔ channel)    | `u32 chnId` |
| `0x40204400` | `_IOW('D',0x00,0x20)` | **CreateChn**                   | `VDEC_CHN_ATTR_S` (32 B) |
| `0x4401`     | `_IO('D',0x01)`       | **DestroyChn** / reset          | none |
| `0xc0204405` | `_IOWR('D',0x05,0x20)`| **GetChnAttr** (read-back)      | 32 B (checked before send) |
| `0x40084406` | `_IOW('D',0x06,8)`    | **BindOutput** (VDEC→VO)        | `{u32 voDev, u32 voChn}` |
| `0x40104409` | `_IOW('D',0x09,0x10)` | **SendStream**                  | `VDEC_STREAM_S` (16 B) |
| `0x8008440c` | `_IOR('D',0x0c,8)`    | GetStreamBuf phys+size (mmap)   | `{u32 phys, u32 size}` |
| `0x4004440d` | `_IOW('D',0x0d,4)`    | Set send mode/blocking          | `u32` (0 or 1) |
| `0x80184410` | `_IOR('D',0x10,0x18)` | **Query** (input-buf status)    | `VDEC_CHN_STAT_S` (24 B) |
| `0x4411`     | `_IO('D',0x11)`       | **StartRecvStream**             | none |
| `0x4412`     | `_IO('D',0x12)`       | **StopRecvStream**              | none |

Wrapper fns: CreateChn `FUN_00157a80`, StartRecv `FUN_00159430`, StopRecv `FUN_001595f8`,
DestroyChn `FUN_00157ed8`, SendStream `FUN_00158cc4`, Query `FUN_001597c0`, BindOutput
`FUN_001580d0`.

### `VDEC_CHN_ATTR_S` (32 bytes) — exact bytes app.out sends for H.264

| off  | value (main / sub-window)        | meaning (best guess) |
|------|----------------------------------|----------------------|
| 0x00 | `0x60`                           | `enType` = `PT_H264` |
| 0x04 | `picW*picH*2` (e.g. 0xCA800)     | `u32BufSize` stream buf (×3/2 if a cfg bit set) |
| 0x08 | `0`                              | reserved / mode |
| 0x0c | `0x2D0` (720) / `0x160` (352)    | `u32PicWidth` |
| 0x10 | `0x1E0` (480) or `0x240` (576) / `0xF0`|`0x120` | `u32PicHeight` |
| 0x14 | `2`                              | ref-frame count / dec mode |
| 0x18 | `1`                              | priority / chroma flag |
| 0x1c | `0`                              | reserved |

(Widths/heights are D1: 720×480 NTSC, 720×576 PAL; sub-windows use CIF 352×240/288.)

### Create sequence (per channel)
```
fd = open("/dev/vdec", O_RDWR);
ioctl(fd, 0x40044414, &chnId);            // attach
ioctl(fd, 0x40204400, &attr);             // CreateChn (attr above)
ioctl(fd, 0x8008440c, &physsz);           // get stream buf; app then mmap()s it (optional for us)
ioctl(fd, 0x4411);                        // StartRecvStream
```
The `mmap` of the stream buffer is **not required** to feed by copy (see §2).

---

## 2. Feeding the stream — `SendStream`

### `VDEC_STREAM_S` (16 bytes)

| off  | field     | note |
|------|-----------|------|
| 0x00 | `u8 *pAddr` | **user-space virtual pointer** to the H.264 bytes (must be non-NULL) |
| 0x04 | `u32 len`   | length |
| 0x08 | `u64 pts`   | app passes 0 |

`pAddr` is a **plain userspace pointer** — proven because app.out sends its 8-byte trailer from a
*stack* address. The driver `copy_from_user`s it. So we feed a normal buffer; no MMZ/phys needed.

### Per-access-unit send
```
ioctl(fd, 0x4004440d, &mode);   // mode=0
ioctl(fd, 0xc0204405, attr32);  // GetChnAttr (app checks enType==0x60)
ioctl(fd, 0x40104409, &stream); // SendStream: {pAddr, len, pts=0}
```
Then, for H.264 only, app.out **immediately sends an 8-byte trailer** as a second `SendStream`:
```
u8 aud[8] = { 0x00,0x00,0x01,0x09, 0x10, 0x00,0x00,0x01 };  // Annex-B AUD (nal 0x09) + next start code
```
This is the classic "flush the current frame out now" trick — without an explicit end-of-frame
marker VDEC waits for the *next* frame's start code before outputting, adding one frame of latency.
Feed one whole access unit (SPS+PPS+IDR, or one P/I slice AU) per SendStream, then the AUD.

**Feed one AU at a time**, not NAL-by-NAL (the trailer defines the AU boundary). Since our `.ts`
is our own muxer (PES = one frame), demux TS→AU is trivial: concatenate the NALs of one PES.

### Pacing (§ "rate")
No PTS pacing in app.out. It is **buffer backpressure + VO display rate**:
```
loop:
   ioctl(fd, 0x80184410, &stat);      // Query 24-byte status
   if (input_buf_fill >= threshold)   // stat fields, offsets ~0x00 and ~0x10
       usleep(4000); goto loop;       // wait
   SendStream(next AU) + AUD;
```
Real-time playback happens automatically: the **bound VO consumes decoded frames at its own
frame rate / vsync**, which backpressures VDEC's output queue, which backpressures VDEC's input
buffer, which makes `Query` report "full" and the feeder sleep. Speed control = set the VO channel
frame rate (a `HI_MPI_VO_SetChnFrameRate`, magic `'O'`; string present, ioctl not resolved here).

---

## 3. VDEC → VO binding and the VI/VDEC swap  ← the crux

This vendor does **NOT** use `HI_MPI_SYS_Bind`. Every source has its own *BindOutput* that points
at a VO `(dev, chn)`:

* **VI** (live): `HI_MPI_VI_BindOutput` `FUN_00161048` / `UnBindOutput` `FUN_00161288`
* **VDEC** (playback): `HI_MPI_VDEC_BindOutput` `FUN_001580d0`

### VI ioctls (`/dev/vi`, magic `'I'`)
| Request      | `_IOC`             | meaning | arg |
|--------------|--------------------|---------|-----|
| `0x4004492e` | `_IOW('I',0x2e,4)` | AttachChn | `u32 = viChn | (viDev<<8)` |
| `0x40084918` | `_IOW('I',0x18,8)` | **BindOutput → VO** | `{u32 voDev, u32 voChn}` |
| `0x40084919` | `_IOW('I',0x19,8)` | **UnBindOutput** | `{u32 voDev, u32 voChn}` |

### VO ioctls (`/dev/vo`, magic `'O'`)
| Request      | `_IOC`                | meaning | arg |
|--------------|-----------------------|---------|-----|
| `0x40044f53` | `_IOW('O',0x53,4)`    | AttachChn | `u32 = voChn | (voDev<<8)` |
| `0x401c4f1f` | `_IOW('O',0x1f,0x1c)` | SetChnAttr | `VO_CHN_ATTR_S` (28 B) |
| `0x4f1d`     | `_IO('O',0x1d)`       | **EnableChn** (show) | none |
| `0x4f29`     | `_IO('O',0x29)`       | **DisableChn** (hide)| none |

`VO_CHN_ATTR_S` (28 B): `{u32 priority=1; s32 x; s32 y; u32 w; u32 h; u32 bDeflicker=1; u32 =0/1}`
— i.e. window rectangle for the mosaic. For a single full-screen playback use one channel at
`x=0,y=0,w=720,h=480/576`.

### How the swap works
Live setup (`vio_bind_vi2vo_all` `FUN_00139b4c`): for each camera it calls **VI_BindOutput twice**
— to **VO dev 2** and **VO dev 0**, `voChn = window index`. So the two physical outputs mirror the
same picture; `voChn` picks the mosaic window.

Playback (`tl_vdec_open` `FUN_0013f8c0`) refuses unless preview is closed
(`"you should close preview first"`), then per window:
```
VO SetChnAttr + EnableChn (dev2 & dev0, chn=win)
VDEC CreateChn + StartRecvStream
VDEC_BindOutput(vdecChn, voDev=2, voChn=win)   // FUN_001580d0(chn, 2, chn)
VDEC_BindOutput(vdecChn, voDev=0, voChn=win)   // FUN_001580d0(chn, 0, chn)
```
Teardown (`tl_vdec_close` `FUN_001406e8`): `VO_DisableChn(dev2)`, `VO_DisableChn(dev0)`, flush,
`StopRecvStream`+`DestroyChn` (DestroyChn implicitly drops the bind). Then live is re-established by
re-running the preview path (VI_BindOutput + VO_EnableChn).

### Recipe for us (single full-screen swap)
```
// --- enter playback ---
VI_UnBindOutput(viDev, viChn, voDev, voChn);   // detach live VI from our VO window
VDEC: open, AttachChn, CreateChn, StartRecvStream
VDEC_BindOutput(vdecChn, voDev, voChn);        // ioctl 0x40084406 {voDev, voChn}
VO_EnableChn(voDev, voChn);                    // if not already enabled
feed AUs (§2) ...

// --- back to live ---
VDEC StopRecvStream + DestroyChn;              // drops VDEC→VO bind
VI_BindOutput(viDev, viChn, voDev, voChn);     // ioctl 0x40084918 {voDev, voChn}
```

### ⚠ VO device-number discrepancy — needs on-device check
app.out drives **VO dev 0 and dev 2**; our `device/dvr/dvr.c display_vga()` uses **VO dev 1**.
VO device numbering is fixed by the kernel driver, so this differs. **Bind VDEC to the exact same
`(voDev, voChn)` our live `VI_BindOutput`/VO channel already uses** — for us that is whatever our
working live path targets (dev1/chn0). Do not blindly copy app.out's dev 0/2. Confirm by testing
which VO dev actually paints the VGA (we already know: our dev1 works for live).

---

## 4. Playback control (pause / seek / speed)

* **Pause:** `CMD_VDEC_PAUSE` is a **no-op in this firmware** (`"CMD_VDEC_PAUSE not apply"`). App
  pauses by **stopping the feed** (stop reading the file / stop SendStream). The last decoded
  frame stays latched on the VO. Same for STEP/RESUME.
* **Flush / seek** (`tl_vdec_flush` `FUN_001404fc`): `StopRecvStream`, then poll `Query` every
  40 ms until the status counters stop draining (stable ~3 iterations or both 0), then `DestroyChn`.
  For seek: jump the file to the next **IDR/SPS**, then re-`StartRecvStream` and feed from there
  (or fully re-init the channel). Because H.264 needs an IDR to restart cleanly, seek granularity
  is GOP-sized — our muxer should keep frequent IDRs.
* **Speed:** VO `SetChnFrameRate` (magic `'O'`); slower/faster = change how fast the bound VO
  releases frames, which repaces the whole chain via the backpressure in §2.

---

## 5. Online (bind) vs GetFrame — recommendation

app.out uses **online/bind mode**: after `VDEC_BindOutput`, the **kernel delivers decoded frames
straight to the VO** — there is *no* `GetFrame` from VDEC and *no* manual VO frame queueing in
userspace. This is the **lower-effort** path for us and the one to implement: we only feed the
bitstream and the VDEC/VO drivers handle decode→display + pacing internally.

---

## Quick reference — full ordered recipe (freestanding C)

```
/* one-time */
vdec = open("/dev/vdec", O_RDWR);
ioctl(vdec, 0x40044414, &chn);                 /* attach chn */
ioctl(vdec, 0x40204400, &attr /*32B, §1*/);    /* CreateChn PT_H264 720x480/576 */
ioctl(vdec, 0x4411);                           /* StartRecvStream */

/* swap VO from live VI to VDEC */
/* on /dev/vi fd: */ ioctl(vi, 0x40084919, &(u32[2]){voDev,voChn});  /* VI UnBindOutput */
ioctl(vdec, 0x40084406, &(u32[2]){voDev, voChn});                    /* VDEC BindOutput */
/* on /dev/vo fd: */ ioctl(vo, 0x4f1d);                              /* EnableChn (if needed) */

/* feed loop, one access unit at a time */
for each AU {
    while (ioctl(vdec,0x80184410,&stat), input_full(stat)) usleep(4000);
    ioctl(vdec, 0x4004440d, &(u32){0});
    ioctl(vdec, 0xc0204405, attr32);
    ioctl(vdec, 0x40104409, &(struct{u8*p;u32 len;u64 pts;}){au_ptr, au_len, 0});
    ioctl(vdec, 0x40104409, &(struct{...}){aud8, 8, 0});   /* 00 00 01 09 10 00 00 01 */
}

/* back to live */
ioctl(vdec, 0x4412);                           /* StopRecvStream */
ioctl(vdec, 0x4401);                           /* DestroyChn (drops bind) */
/* on /dev/vi fd: */ ioctl(vi, 0x40084918, &(u32[2]){voDev,voChn});  /* VI BindOutput */
```

## RESOLVED on device (2026-07-24) — playback now works ✅

The freeze-instead-of-play symptom was **the VO channel staying latched to its (now-unbound) VI
source**. Bind-only (VI_UnBind → VDEC_BindOutput) is **not** enough: VDEC decodes fine (SendStream
rc=0, decode-frame counter climbs) but the VO never pulls the decoded pictures — they pile up in
VDEC's output queue and the screen holds the last live frame. The fix is to **cycle the VO channel
through disable→enable** so it re-sources from VDEC, exactly as stock does across two functions:

* **stop-preview** (`FUN_001377a0` case 4): VI_UnBindOutput all channels (dev2 & dev0), then VO
  **DisableChn `0x4f1e`** (`FUN_00166634`) on every VO channel of dev2 & dev0.
* **vdec-open** (`FUN_0013f8c0` → `FUN_00139b18` → `FUN_001398d0`): per window, VO **SetChnAttr
  `0x401c4f1f`** (28-B rect) + VO **EnableChn `0x4f1d`** on dev2 & dev0, then VDEC CreateChn +
  StartRecvStream + `VDEC_BindOutput` to (dev2,win) and (dev0,win).

Our `play_file()` now does, per VO device {2,0}: `AttachChn(win|dev<<8)` → `0x4f1e` (disable) →
`0x401c4f1f` (SetChnAttr full-screen rect) → `0x4f1d` (enable), then VDEC BindOutput. Verified via
VO GetScreenFrame: the VGA shows the decoded recording, click-to-stop returns to live cleanly.

Resolved specifics:
1. **VO dev number**: our live VI binds to **dev 2 AND dev 0** (not dev 1) — the "2nd open = dev1"
   comment was misleading; the VGA is dev 0 (dev 2 is the mirror). Bind VDEC to **both 2 and 0**.
2. `VDEC_CHN_ATTR_S` values (0x60 / w·h·2 / w / h / 2 / 1) work as-is; CreateChn rc=0.
4. We **do** need `SetChnAttr`+`EnableChn` on the VO window — but the critical part is the preceding
   **DisableChn `0x4f1e`**. Enable-only on an already-enabled channel is a no-op and does not switch
   the source. Disable→(SetChnAttr)→Enable is what works.

## Still uncertain / future
3. `VDEC_CHN_STAT_S` (24 B) exact field offsets — we currently pace by PTS (§ pacing below) instead
   of the input-buffer-full backpressure test, which works well; refine only if needed.
5. `StartRecvStream` before/after `BindOutput` — current order (create→StartRecv→VO cycle→bind)
   works.
```
