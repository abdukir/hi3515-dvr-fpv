# MPP_INIT_SEQUENCE.md — exact Hi3520_MPP_V3.0.6.2 pipeline bring-up (from live ioctl trace)

## ✅ VERIFIED WORKING 2026-07-23 — `device/dvr/dvr.c` records valid H.264 from all 4 ch

Our raw-ioctl recorder replays this sequence and produces decodable Annex-B H.264 (ffmpeg:
350 frames/ch over 12 s, NTSC 30 fps; SPS/PPS/SEI/IDR all present). **Corrections found while
getting frames to actually flow — the original trace-read below was missing the start steps:**

1. **Missing start steps** (add all three, in this order, *after* creating all 4 VENC channels):
   - **VI start**: per channel `ioctl(fvi, 0x40084918, {2,chn})` then `{0,chn}` (8 B each).
   - **VENC SetChnAttr/RC**: `ioctl(fve, 0x404c4506, RC76)` — 76 B, verbatim from trace
     (`60 00 00 00|00..|60 01 00 00|f0 00 00 00|1e..|01..|..|00 ef 01 00|01..|00..|1e..|3c 00 00 00|0a..|00..|00 08 00 00|0..`).
     app.out does `0xc04c4507` (get) first, then this set; the set alone suffices.
   - **VENC StartRecvPic**: `ioctl(fve, 0x0000450a, 0)` — `_IO('E',10)`, no arg. **Without this
     the encoder never emits** (GetStream stays BUF_EMPTY forever).
2. **GetStream loop order = Query → prep → GetStream → Release** (app.out's exact order):
   `ioctl(fve,0x8010450e,q16)` (status), `ioctl(fve,0x40044512,{1})` (prep, flag 1), then
   `0xc00c450c` GetStream, then `0x400c450d` Release. Prep must be the call *immediately* before Get.
3. **packCount must be ≥ packs-per-frame.** VENC_STREAM_S = `{pstPack, packCount, seq}`; an
   I-frame is **4 packs** (SPS+PPS+SEI+IDR). Passing packCount=1 returns `0xa0068003`
   (ILLEGAL_PARAM) the moment a frame is ready. Provide space for ≥8 packs (`MAXPK=8`);
   GetStream writes the actual count back into `stream[1]` (`nout`).
4. **Frame phys→virt translation (the subtle one).** `0x800c450f` returns `{phys1, phys2, size}`
   (e.g. `0xc4b44040, 0xc37a0040, 0x1ef00`). **Map phys1 (info[0]) via `/dev/mem`** — app.out does,
   and it reads real data. **phys2 (info[1]) via `/dev/mem` reads all `0x80` filler** (the MMZ
   ownership wall). But the **pack's `w[2]` addr is phys2-relative**. So:
   `boff = pack.w[2] - info[1] + (info[0] & 0xfff)`; `frame = mmap_base + boff`;
   H.264 = `frame + 0x40` for `w[4] - 0x40` bytes (the 0x40 is the pack header; NAL start
   `00 00 00 01` sits exactly at +0x40, confirmed by raw dump).
5. Pack layout confirmed from a channel we own: `w[2]`=phys(phys2-space), `w[3]`=w[2]+0x80,
   `w[4]`=total len incl 0x40 header, `w[6]`=PTS. `w[0]/w[1]`=0 (kernel leaves virt for userspace).

**VO IS required** (or at least harmless-and-included): the recorder keeps the VO setup and it works.
Everything below is the raw trace read; apply the 5 corrections above.

---


**This is the version-perfect blueprint for our own recorder.** Since the `bluhbluh/Hi3515-SDK` is
`MPP_V1.0.0.0` but the device runs `Hi3520_MPP_V3.0.6.2` (mismatch → `HI_MPI_VI_SetPubAttr` = SYS_NOTREADY),
we **traced app.out's actual ioctls** with an `LD_PRELOAD` hook (`device/ioctl_trace.c`) and replay
them directly with raw `ioctl`s — no SDK needed, guaranteed to match the kernel. Full raw trace:
`dump/livedump_20260723/mpp_init_trace.log` (app.out's real init, 4-channel D1 H.264).

Build the recorder with the **`device/` OABI runtime** (`sys_ioctl`/`sys_open`/`sys_mmap` from `oabi.h`)
— no libmpi, no SDK. Each `ioctl(fd, cmd, arg)` below: `arg` is a pointer to the shown little-endian
bytes, EXCEPT where noted "val=" (passed by value). Device magics: `sys`='Y'(0x59), `vb`='B'(0x42),
`vo`='O'(0x4f), `vi`='I'(0x49), `grp`='G'(0x47), `venc`='E'(0x45).

## The sequence (exact, in order)

### 1. Capture chip (tl_R9508 board glue + tw286x)
```
open("/dev/tw_286x")=ftw ; open("/dev/tl_R9508")=ftl
ioctl(ftl, 0xc00456d3, (void*)0x64)          # val — board video-input profile (=hardware_type 100)
ioctl(ftl, 0xc00456ce, &{0x28})              # -> 0
ioctl(ftw, 0xc00448d8, &{0})                 # -> {1}
```
### 2. SYS + VB (video buffer pools)  — the part the V1 SDK got wrong
```
open("/dev/sys")=fsys ; ioctl(fsys, 0x5901)                       # sys open
open("/dev/vb")=fvb ;  ioctl(fvb, 0x4208)                          # vb open
ioctl(fvb, 0x4044420a, &VBCONF68)  # 68B VB pool config:
     78 00 00 00 | 00 7e 09 00 | 08 00 00 00 | 80 5f 02 00 | 50 00 00 00 | 00*... (rest 0)
     = {u32 0x78; blkSize 0x97e00; blkCnt 8; blkSize2 0x25f80; blkCnt2 0x50; ...}
ioctl(fvb, 0x4207)                                                 # vb init
ioctl(fsys, 0x40085902, &{0x10, 0})                                # sys config: align=0x10(16)
ioctl(fsys, 0x5900)                                                # sys init
```
### 3. VO (display — needed by app.out; a headless recorder MAY skip, test)
```
open("/dev/vo")=fvo
ioctl(fvo,0x40044f53,&{0x200}); ioctl(fvo,0x4f01,val 0); ioctl(fvo,0x40384f02,&VO56);
ioctl(fvo,0x4f00,val 0); ioctl(fvo,0x40244f0d,&VO36{...,0x2d0,0x1e0,...,30,19,...}); ioctl(fvo,0x4f0b,val 0)
ioctl(ftw,0xc00448d3,&{1})
```
### 4. VI (VIU) — per channel 0..3, open a fresh /dev/vi each
```
for chn in 0..3:
  open("/dev/vi")=fvi[chn]
  ioctl(fvi,0x4004492e,&{chn})                 # select dev/chn
  if chn==0: ioctl(fvi,0x4901)                 # (dev-level, chn0 only)
  if chn==0: ioctl(fvi,0x40144902,&PUBATTR20)  # VI_SetPubAttr, 20B:
             00 00 00 00 | 03 00 00 00 | 01 00 00 00 | 00 00 00 00 | 00 00 00 00
             = {enInputMode? , workmode=3(4D1), norm=1(NTSC), ...}
  if chn==0: ioctl(fvi,0x4900, val 0x80)        # VI_EnableDev
  ioctl(fvi,0x40244908,&CHNATTR36)              # VI_SetChnAttr, 36B:
             00*8 | c0 02 00 00 (w=704) | f0 00 00 00 (h=240) | 01 00 00 00 | 01 00 00 00 | 00*4 | 13 00 00 00 ...
  ioctl(fvi,0x4904, val chn)                    # VI_EnableChn
  ioctl(fvi,0x4004490a,&{0x1e})                 # framerate 30
```
### 5. Group + VENC — per channel 0..3
```
for chn in 0..3:
  open("/dev/grp")=fgrp[chn]
  ioctl(fgrp,0x40044705,&{chn}); ioctl(fgrp,0x4700); ioctl(fgrp,0x40084702,&{0,chn})  # create+bind grp
  open("/dev/venc")=fve[chn]
  ioctl(fve,0x4004451b,&{chn})                  # select chn
  ioctl(fve,0x40684500,&VENCATTR104)            # VENC_CreateChn, 104B (SAME for all chn):
     60 00 00 00 (enType=PT_H264) | 00 00 00 00 (priority) | 60 01 00 00 (w=0x160=352) |
     f0 00 00 00 (h=240) | 1e 00 00 00 (viFR=30) | 01 00 00 00 (bMain) | 00 00 00 00 |
     00 ef 01 00 (bufSize=0x1ef00) | 01 00 ...  (see trace for full 104B)
  ioctl(fve,0x800c450f,&{"/grp"...}) -> OUT{phys(4), virt/other(8)}   # get stream buffer info
  # phys = OUT[0..3] (e.g. 0xc4b44040 -> page base 0xc4b44000). Map the buffer:
  open("/dev/mem",O_RDWR|O_SYNC)=fmem  (once)
  mmap(NULL, 0x1f000, PROT_READ|WRITE, MAP_SHARED, fmem, phys_pagebase)  # stream buffer -> virt
  ioctl(fve,0x40044508,&{chn})                  # (start recv? )
  ioctl(fve,0xc0104518,&{...}); ioctl(fve,0xc0104517,&BIND16{ff*8,01,01})  # bind venc<-grp/vi
```
### 6. Record loop (already proven working)
```
loop per chn:
  ioctl(fve,0x40044512,&{1})                    # prep
  ioctl(fve,0xc00c450c,&VENC_STREAM_S)          # GetStream: {pstPack, packCnt, seq}
  # for each pack: phys=pack.w[2]; virt = bufmmap + (phys - buf_phys_base); write [virt+? .. +len]
  ioctl(fve,0x400c450d,&VENC_STREAM_S)          # ReleaseStream
```
Frame read: the pack's phys addr (`w[2]`) lies inside the mmap'd stream buffer → `virt = mmap_base +
(pack_phys - buf_phys_base)`. (This is the `FUN_0015a524` translation; it's why a bare `/dev/mem` map
of the pack addr read filler — you must map the **buffer base** from `0x800c450f` and offset into it.)

## Notes for implementation
- Use `device/oabi.h` (`sys_ioctl`, `sys_mmap2`, `sys_open`) — OABI, no libc/SDK. For by-value ioctl
  args, pass the integer directly as the 3rd arg.
- Exact struct bytes for the 68/104/56/36/20-byte configs are in the trace log — copy them verbatim
  first (get it working), then decode fields for our own specs (resolution/bitrate/GOP live in
  VENCATTR104 and the config image `0x1934`, see `docs/FIRMWARE_RE.md`).
- Run under exclusive MPP ownership: serial `closewd`+`exit` (or boot without app.out via the `n`
  prompt). Watchdog handled by `our_dvr` already.
- Re-capture the trace anytime: `LD_PRELOAD=.../ioctl_trace.so ./app.out` (per-PID logs `tr_<pid>.log`;
  the one with `0x40684500` is app.out's MPP process).
