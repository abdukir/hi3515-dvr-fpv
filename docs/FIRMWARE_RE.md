# FIRMWARE_RE.md — device-side teardown of the RR104P / Hi3515 DVR

Running notes on the on-device firmware: `app.out`, config storage, the record pipeline,
and the boot chain. Companion to `PROTOCOL.md` (network protocol) and `docs/PROJECT_NOTES.md`.
Findings are labelled **CONFIRMED** (observed on the live device or cross-checked) vs
**INFERRED** (static reasoning only).

Primary Ghidra target: `app.out` (ARM v5 little-endian ELF, ~3.2 MB, stripped uClibc/BusyBox
era). Load as ARM:LE:32:v5. The live copy is `dump/livedump_20260723/rootfs.tar.gz :: root/app.out`.

---

## 1. Live system map (CONFIRMED 2026-07-23)

- **Kernel:** Linux 2.6.24-rt1-hi3515v100 (`#264 Wed Sep 8 2010`), armv5tejl. No `/proc/kallsyms`,
  no `/proc/config.gz`.
- **Rootfs:** ext2 **RAM disk** mounted `/` — volatile. Rebuilt each boot from mtd2. Device nodes
  in `/dev` (tmpfs) recreated by `/mknod_console`.
- **SATA:** 4 vfat partitions `/dev/sda1..4` → `/root/rec/a1..a4` (27.9 GB each, ~99% full of
  `fly000NN.ifv` recordings). Channel N → partition `a(N+1)`.
- **Flash (mtd, NOR):** `mtd0 boot` 1 MB, `mtd1 uImage` 2 MB, `mtd2 rootfs` 5 MB (erasesize 64 KB).
  Live md5 of mtd1/mtd2 == committed `dump/mtd1_uImage.bin`/`mtd2_rootfs.bin`. **mtd0 differs**
  from the committed backup — see §2.
- **Processes:** `mydaemon.out` (pid 595, watchdog/keepalive) + `app.out` (pid 597 main, ~30
  worker threads). `app.out` open fds (`proc/app597_fd.txt`) show the whole media stack:
  `/dev/tw_286x` (analog capture), `/dev/vi`, `/dev/venc` (H.264 encoder), `/dev/vo`, `/dev/vd`,
  `/dev/vdec`, `/dev/grp`, `/dev/tde`, `/dev/fb0/2`, `/dev/mem`, and **`/dev/mtd0` (fd 21)**.
- **Listening TCP:** `80` ASP web, `8670` DVR TLV (control+media), `8081` busybox httpd (webapp),
  `23` telnet, plus undocumented `6623`, `101`, `102` (purpose TBD).
- **Kernel modules** (`proc/modules.txt`): HiSilicon MPP set (`hi3515_venc/h264e/viu/vou/vpp/
  group/chnl/sys/base/md/...`), `tw286x_R9508` + `tl_R9508` + `panel_r9508` (TW2866 capture),
  `mmz` (media memzone), `hidmac`, `hiether`, `hifb`, `hiwdt` (**hardware watchdog**),
  `tl_gpio_i2c` (bit-banged i2c), `rtc_pcf8563`, `ahci/libata/sd_mod` (SATA), `usb_storage`, vfat/fat/msdos.

## 2. Config storage & persistence (objective #2 — CONFIRMED, layout partial)

The persistent settings are **not** in `/root/data/config.txt` (that's cosmetic UI colors +
`model=RR104P`), and there is **no i2c EEPROM** (no `/dev/i2c*`, no eeprom sysfs) and **no
dedicated config mtd partition**. Settings live in **mtd0 at `0x0e0000`** as a length-prefixed
**zlib** blob:

```
0x0e0000  u32 LE  compressed length (observed 0x043c = 1084)
0x0e0004  0xFF ... padding to 0x0e0100
0x0e0100  zlib stream (78 9c ...)  ->  18066 bytes decompressed
```

`app.out` keeps `/dev/mtd0` open (fd 21) and rewrites this 64 KB erase-block on "save settings",
which is exactly (and only) where live mtd0 diverged from the old backup (44 diff ranges, all in
`0x0e0000..0x0e04b3`). Decompressed (`dump/livedump_20260723/flash/mtd0_config_decompressed.bin`)
it contains, as fixed-width records: channel names `CH 01..CH 04` + per-channel schedule bitmaps,
MAC (`XX:XX:XX:XX:XX:XX` — redacted, it is this unit's own), user table (`admin` + password
strings, stored in the clear), and
identity block `NetDVR` / `RR104P` / `V4.0.0-JMS502 Aug 12 2011`.

### Config codec — fully decoded from `app.out` (Ghidra, CONFIRMED)

The config code is compiled from `../source/config.c`. All functions below are **renamed in the
Ghidra DB**. The `/dev/mtd0` base offset `0xe0000` is hard-coded in `cfg_flash_write`/`cfg_flash_read`.

**On-flash layout** (mtd0, TWO redundant copies — primary + self-healing backup):
```
0x0e0000  primary : [u32 LE complen][u8 flag] header, padded to +0x100, then zlib stream (complen bytes)
0x0f0000  backup  : identical format (written when primary is saved / to re-sync a stale copy)
```

**RAM config image** = 18066 (0x4692) bytes, header then data:
```
+0x00 u16 version = 0x0100     +0x04 u32 magic = 0x013219CD
+0x02 u8  valid   = 1          +0x38 u32 declen = 0x4692 (18066)
                               +0x3c u32 checksum
+0x40.. payload (channel names @ +0x41, schedules, MAC, users, identity ...)
```
**Checksum** (`cfg_checksum`, verified reproduces `0x4b871` on our dump) = 32-bit additive sum of
payload bytes `[0x40 .. 0x40+0x4652)` plus header bytes `[0x00 .. 0x3c)` (skips the checksum field).

**Function map** (config subsystem):
| addr | name | role |
|---|---|---|
| `0x25da0` | `cfg_LoadConfig` | read+inflate primary → verify → copy live; cross-check/re-sync backup; fall back to backup if primary bad |
| `0x25c70` | `cfg_ReadInflateCopy` | read 5-byte header (complen) @base+off, read stream @+0x100, `zlib_uncompress` → 18066 B |
| `0x25a4c` | `cfg_verifyconfig` | check version/valid/magic/declen==0x4692/checksum |
| `0x25a00` | `cfg_checksum` | additive checksum (above) |
| `0x25f88` | `cfg_WriteConfigThread` | background thread; waits on a dirty-flag (mutex+usleep), erases `0xe0000` (+`0xf0000`), writes both copies |
| `0x25b4c` | `cfg_WriteConfigCopy` | store checksum in header, `zlib_compress2` the 18066 B, write header@base + stream@base+0x100 |
| `0x25958` | `cfg_flash_write` | `lseek(fd, off+0xe0000); write; fdatasync; sync` (bound off+len < 0x20001) |
| `0x2585c` | `cfg_flash_read` | `lseek(fd, off+0xe0000); read` |
| `0xd9e4c` | `zlib_compress2` | static zlib 1.2.3 deflate wrapper |
| `0xda898` | `zlib_uncompress` | static zlib 1.2.3 inflate wrapper |
| `0x2d918` | (hw probe) | sums `/dev/mtd%d` sizes via `MEMGETINFO`; board-detect ioctl (`'V'`,0xc9) → 128 vs 256 MB |

**To edit settings offline:** inflate primary → patch payload (`>=0x40`) → fix `checksum`@0x3c →
deflate → write `[complen][flag]`+stream to mtd0 `0xe0000` **and** `0xf0000` (md5-verify, keep backup).
The 18066-byte payload struct layout (field offsets per protocol `Set*` command) is the remaining
sub-task — decode incrementally against `PROTOCOL.md`.
- **Persistence implication:** editing settings = patch struct → deflate → write length + stream
  to mtd0 `0x0e0000`. Doable offline against a pulled mtd0, then reflash (md5-verify). Safer than
  touching mtd2.

## 3. Boot chain (CONFIRMED — `dump/livedump_20260723/boot/`)

`U-Boot (mtd0) → uImage (mtd1) → ext2 RAM rootfs (mtd2) → /etc init (rcS) → /root/run_app.sh`:

```
run_app.sh: cd /root; ./net.sh; ./init.sh
            rm kernel/tl_modules; mv /tmp2/app.tgz .; tar -zxf app.tgz   # app.out extracted here
            mv /tmp2/data.tgz .; tar -zxf data.tgz
            hostname 192.168.1.255
            echo "run app.out(y/n)?"; read -t 1 -n 1 char   # <-- 1s injection hook
            ./mydaemon.out &
            [ "$char" != n ] && ./app.out
```

`app.tgz`/`data.tgz` arrive in `/tmp2` from an earlier stage (packed in mtd2). **Least-risky
mod hook (planned):** reflash mtd2 with a `run_app.sh` that, before `./app.out`, sources a script
off SATA (`/root/rec/aN/…`) — iterate on SATA thereafter without further flashing. Reflash recipe
+ U-Boot env quirks: TBD in this doc as the persistence work proceeds.

## 4. Record pipeline (objective #3 — in progress)

`tw286x` (analog in) → HiSilicon VIU → VENC (H.264) → `.ifv` files on SATA. Container starts with
`62 19` packet headers + Annex-B H.264 (see `webapp/dvr/mediastream.js`, `webapp/dvr/recordings.js`).
Per partition: pre-allocated 128 MB `fly000NN.ifv` circular buffer + `index00.bin`/`index01.bin`
(segment index, 1.77 MB) + `log.txt` (binary op/audit log: 4-byte ts + event + fixed-width name;
a1 only). Fresh full metadata set captured under `dump/livedump_20260723/rec_meta/`.
`.ifv` container internals → `docs/IFV_FORMAT.md` (if it grows).

**Ghidra (partial):** path format `rec/%c%d/fly%05d.ifv` @ `0x1d9388` (also `fly%05d.ifv`,
export paths `/tmp/%s.ifv`, `myusb/%s.ifv`). `rec_resolve_ifv_by_time 0x89d84` maps a timestamp to
an `.ifv` file+offset by walking the in-RAM record-state array: **2 disk groups × 4 channels**
(group stride `0x8e4`, per-channel struct `0x234` B, holding the loaded segment index). `%c` =
`'a'+group` so the firmware supports a 2nd disk ("b" group); our unit only populates "a" (a1..a4).
**MPP pipeline (Ghidra — entry points for reimplementation).** app.out drives the standard
HiSilicon Hi3515 **MPP** API (statically linked; the library lives in the `0x15xxxx` region — that's
why exports show `HI_MPI_*`-style helpers). Verbose logs name the source files and exact SDK calls:
- **Capture:** `FUN_0013527c` opens `/dev/tw_286x` (TW2866 analog-in) and `/dev/mmz_userdev` (MPP
  memzone). (`lib_*` sources.)
- **Encode:** `tl_venc_start = FUN_0013b3b4` (`lib_venc.c`) — the H.264 encoder: configures the
  channel (`HI_MPI_VENC_SetChnAttr`), sets rate-control (`HI_MPI_VENC_Get/SetH264eRcPara`), GOP
  (`tl_venc_set_gop`), then pulls NALs in a loop (`HI_MPI_VENC_GetStream`/`ReleaseStream`) and writes
  the `.ifv` container. `tl_venc_minor_start` = the substream encoder.
- **Preview/display:** `lib_preview.c` (`HI_MPI_VO_SetChnAttr` …), **audio:** `lib_audio.c`
  (`HI_MPI_AENC_GetStream`).

**To write our own recorder** (see `docs/BUILD.md` for the proven build path): replicate the standard
MPP flow — `HI_MPI_SYS_Init` → VB pool → capture (tw2865/VI) → `HI_MPI_VENC_CreateChn(&VENC_CHN_ATTR)`
→ `StartRecvPic` → loop `GetStream`→write→`ReleaseStream`. Two routes: (a) link the Hi3515 **MPP SDK**
(`libmpi.a` + headers, external — must be the OABI build) against our OABI runtime; or (b) reverse the
raw `ioctl`s `tl_venc_start` issues on `/dev/venc`,`/dev/vi`,`/dev/mmz_userdev` and call them directly
via `sys_ioctl`/`sys_mmap` (no SDK). Encoder params (res/fps/bitrate/GOP) come from the config video
block at image `0x1934` (§2). This MPP mapping is the main remaining RE effort.

**VENC stream-pull kernel ABI (decoded from `HI_MPI_VENC_GetStream` = `FUN_0015c5f0`).** This is the
crux for our own recorder — how to get encoded H.264 out of the hardware. Per VENC channel (0-0x3f),
open its `/dev` node, then `ioctl` (magic `'E'` = 0x45):
| ioctl | _IOC | purpose |
|---|---|---|
| `0x4004451b` | `_IOW('E',27,4)` | select/attach channel (arg = chn u32) |
| `0x40044512` | `_IOW('E',18,4)` | query / prepare stream (arg = flag) |
| `0xc00c450c` | `_IOWR('E',12,12)` | **GetStream** → `VENC_STREAM_S` |
| (release) | `_IO('E',...)` | ReleaseStream after use (paired) |

`VENC_STREAM_S` = `{ VENC_PACK_S *pstPack; u32 u32PackCount; u32 u32Seq; ... }`. Each **`VENC_PACK_S`
is 44 B (0xb words)**: `{ u32 u32PhyAddr; u8 *pu8Addr; u32 u32Len; u32 u32PhyAddr2; u32 u32Len2; u32
PTS ... }`. The actual H.264 (Annex-B) bytes are at `addr + 0x40` for `len - 0x40` bytes — the 0x40-B
pack header is the origin of the `.ifv` `62 19` framing. Physical MMZ addresses are converted to user
pointers via the mmap'd VENC buffer (`FUN_0015a524`/`FUN_0015a4e8` = phys→virt).

**MVP recorder — status (`device/venc_probe.c`, `device/venc_rec.c`).** ✅ **PROVEN 2026-07-23:** our
own OABI binary (`device/` runtime, no SDK/libc) opens `/dev/venc`, `ioctl 0x4004451b` (bind chn),
`ioctl 0x40044512` (prep), then `ioctl 0xc00c450c` (GetStream) with a **caller-provided pack buffer**
(`stream.pstPack`) and pulls live packs **alongside running app.out, no conflict** — e.g. ch0:
`packs=1 seq=212906  pack[0] len=75 phy=0x7f1c0300`. So frame **metadata** (count/len/PTS/phys addr)
is fully readable from our program. Raw pack fields: `w[0]`=phys of 0x40 header, `w[4]`=total len
(data = `phys+0x40 .. phys+w[4]`), plus a 2nd segment (`w[2]/w[3]`) for ring-buffer wrap.

⏳ **Remaining (reading the frame BYTES) — corrected findings 2026-07-23:**
1. **Phys addr is `w[2]` (and `w[3]` for the 2nd/wrap segment), NOT `w[0]`.** Re-reading
   `HI_MPI_VENC_GetStream`: it feeds `piVar7[2]`/`piVar7[3]` into the translator and *writes* the virt
   result into `w[0]`/`w[1]`. So the raw `VENC_PACK_S` from the ioctl is:
   `w[2]`=phys seg1, `w[3]`=phys seg2, `w[4]`=total len (incl 0x40 hdr); data = `phys+0x40 .. +w[4]`.
   (Our first probe mislabeled `w[0]`; dump all 11 words to confirm before mapping.)
2. **MMZ mmap recipe = `FUN_0016c734`:** `mmap(NULL, roundup(len+(phys&0xfff)), PROT_READ|PROT_WRITE,
   MAP_SHARED, mmz_fd, phys & 0xfffff000)`, then `virt = ret + (phys & 0xfff)`; `mmz_fd =
   open("/dev/mmz_userdev")` (`FUN_0016c7cc`/`FUN_0016c8e0`). Our attempt failed because we mapped the
   wrong field (`w[0]`); retry with `w[2]`. If it still MAP_FAILs, the region may need an mmz
   register/alloc ioctl first (reverse `FUN_0016c7cc`).
3. **ReleaseStream is REQUIRED** — GetStream without it leaves the channel stateful; repeated
   un-released grabs make later GetStreams block (observed) and pile up zombie readers. Reverse
   `HI_MPI_VENC_ReleaseStream` (function adjacent to GetStream; paired `'E'` ioctl, likely
   `_IOW('E',13,12)` = `0x400c450d`) and call it after each GetStream.

**RESOLVED (2026-07-23) — the full VENC cycle works from our own program:**
- `ReleaseStream = FUN_0015c8cc` → `ioctl 0x400c450d` (`_IOW('E',13,12)`, same stream struct). REQUIRED
  after each GetStream; without it the channel stalls and spawns zombie readers.
- Prep flag: `0` = wait for a frame (we poll); `1` = non-blocking (returns `0xa006800e` = buf-empty).
  app.out passes 1 because it `select()`s the venc fd first.
- **Raw `VENC_PACK_S`** (dumped live): `w[2]`=phys addr of frame data (in MMZ), `w[3]`=phys of 2nd/wrap
  segment, `w[4]`=total len (incl 0x40 pack header); H.264 = `phys+0x40 .. phys+w[4]`. (`w[0]`/`w[1]`
  are where the MPI *writes* translated virt addrs; `w[6]` is unrelated.)
- **Physical memory map** (`/proc/iomem`): DDR base = **`0xc0000000`**; Linux gets 43 MB
  (`0xc0000000–0xc2afffff`, `mem=43M`); the **MMZ is reserved DDR above that** (`0xc2b00000+`), which
  is why frame phys addrs look like `0xc37b3bc0`.

⛔ **The memory-ownership wall (the real remaining boundary):** a *foreign* process cannot read
app.out's encoder frames. `/dev/mem` maps the reserved MMZ but returns **filler `0x80`** (not backed as
System RAM). `/dev/mmz_userdev` only maps blocks the caller **allocated itself** — the MMZ API is
alloc-then-map: `open("/dev/mmz_userdev")` → `ioctl 0xc04c6d0a` (MMZ-ALLOC, magic `'m'`=0x6d, 76-B
struct) → returns a phys you own → then mmap it. You can't map someone else's block.

➡️ **This redirects to the real goal:** own the pipeline. For a true custom DVR we want our program to
**create its own VENC channel** (`HI_MPI_VENC_CreateChn` → we allocate the MMZ VB pool → we can read our
own frames → we can drop app.out). So the next phase is reversing the **encode setup**: `HI_MPI_SYS_Init`,
VB pool (`HI_MPI_VB_*`), `VI`/tw2865 bring-up, `VENC_CreateChn(VENC_CHN_ATTR_S)`, `StartRecvPic` — the
`FUN_0015xx` MPP wrappers + their ioctls, decoded the same way. Proven runtime + full VENC pull/release
ABI (`device/venc_probe.c`, `device/venc_rec.c`) are the foundation; the MVP "steal app.out's frames"
route is a dead end due to MMZ isolation.

CONFIRMED DEAD END (2026-07-23): tried `/dev/mem` cached, `/dev/mem` **uncached (O_SYNC)**,
`/dev/venc`, and `/dev/mmz_userdev` mmaps of the frame phys (`w[2]`, e.g. `0xc37bb340`) — **all read
filler `0x80`**. `/dev/mem` cannot back the reserved MMZ region for a foreign process. So a recorder
MUST own the encoder channel.

### Encode-setup phase (started) — `HI_MPI_VENC_CreateChn` = `FUN_0015ab88`
The path to owning a channel (and a mappable buffer). CreateChn(`chn, VENC_CHN_ATTR_S*, 0`):
1. `open("/dev/venc")` → `ioctl 0x4004451b` (select chn).
2. **`ioctl(fd, 0x40684500, &attr)`** = `_IOW('E',0,104)` — **create; `VENC_CHN_ATTR_S` = 104 B**
   (resolution / bitrate / GOP / profile / RC mode — layout TBD).
3. **`ioctl(fd, 0x800c450f, &info)`** = `_IOR('E',15,12)` — returns the stream-buffer `{phys, size, …}`.
4. `open(<node>, O_RDWR|O_CREAT|O_SYNC=0x1042)` then **`mmap(NULL, size, PROT_RW, MAP_SHARED, fd,
   phys & ~0xfff)`** — maps THIS channel's buffer (uncached). Because *we* created it, *we* can read it.
5. `ioctl(fd, 0x4501)` (`_IO('E',1)`) on error path (destroy/reset).
**Encoder attr construction** (from `tl_venc_start`, the fields fed into CreateChn): width/height
(from the requested resolution), **fps** = `0x1e` (30/NTSC) or `0x19` (25/PAL) by video standard,
**bitrate** ← config `+8`, **GOP** ← config `+0xc` (via `FUN_0013b178`), a stream-type flag
(main=`param2==2`), buffer-size = `w*h*2` (or `*3/4` per a mode bit), plus constants (`local_3e8=10`,
`local_3dc=2`). The attr passed is `{u32=0x60; VENC_ATTR_S *; …}` — CreateChn copies it into the
104-B ioctl struct. (These encoder params also live in the config video block @ image `0x1934`, §2.)

Then: `HI_MPI_VENC_RegisterChn(group)` (bind to a VeGroup), `SetChnAttr`, `HI_MPI_VENC_StartRecv`
(`tl_venc_open FUN_0013b208`), then the GetStream/Release loop we already have. The encoder also needs a
**video source** feeding it: VI/tw2865 capture (`FUN_0013527c`, `'H'` ioctls) → VB pool
(`HI_MPI_VB_*`) → VeGroup → chn. Remaining work: decode `VENC_CHN_ATTR_S` (104 B) + the VI/VB/group
setup ioctls (same method: decompile the `FUN_0015xx` MPP wrappers). This yields a **fully standalone
capture→encode→record** owned by our code — the real custom-DVR foundation (and the same VO/framebuffer
path, `lib_preview.c`/`/dev/vo`, gives us our own VGA UI).

NOTE: a **reboot** gives a clean slate after test runs (RAM rootfs restores from mtd2; nothing
persistent is ever touched — flash backups are committed).

Then the loop is: GetStream → for each pack, copy `[phys+0x40 .. +w[4]]` from the mapped MMZ → append
to our own file (our container/naming/segmentation/retention = "our specs"). The **capture+encode
setup** (`FUN_0013527c` tw2865/VI `'H'` ioctls, `tl_venc_start FUN_0013b3b4` VENC CreateChn) still
needs reversing for a fully-standalone recorder; the MVP above leverages stock capture/encode. Encoder
params (res/fps/bitrate/GOP) ← config video block @ image `0x1934` (§2).

**Capture front-end ABI (`FUN_0013527c`).** Opens `/dev/tw_286x` (magic `'H'`=0x48) and
`/dev/mmz_userdev`; configures the TW2866 via `ioctl 0xc00448d7` / `0xc00448d8`
= `_IOWR('H',215/216,68)` (68-byte video-format/timing structs), plus a board-control ioctl on
magic `'V'`=0x56 (`0xc00456ce`; same family as the 128/256 MB board-detect `0xc00456c9`).

**Known kernel ioctl magics so far:** `'E'`=0x45 VENC, `'H'`=0x48 TW2866 capture, `'V'`=0x56 board
control, plus `/dev/mmz_userdev` (MMZ physical-memory alloc). The full chain also needs VI/VPSS/VB
setup + `VENC_CreateChn` (VENC_CHN_ATTR_S, ~100 B nested) — each reversible by the same method
(decompile the `HI_MPI_*` wrapper in the `0x15xxxx` MPP lib → read its `ioctl(fd,cmd,&struct)`).

**Reality check / recommended path.** Reversing every MPP ioctl + struct from scratch is a large
multi-pass effort. The efficient route to a full custom DVR is to link the **Hi3515 V100 MPP SDK**
(`libmpi.a` + `mpi_*.h`, the API app.out was built against) — but it must be the **OABI** build to
link with `device/`'s runtime. Then all DVR *logic/specs* (record policy, container, scheduling,
UI, network) is our own clean code; only the hardware bring-up uses the SDK. The pure-RE route
(this section's ioctl tables) remains viable as a fallback / for SDK-free operation.

Record-segment helpers: `rec_index_alloc_segment 0x8738c`, `FUN_00087bc0`, `FUN_00087c58`.
`rec_index_alloc_segment` reveals the **`index0N.bin` format**: `0x200`-byte header then `0x10`-byte
segment records (start_ts, end_ts, start_off, end_off — u32le each; matches `webapp/dvr/recordings.js`).
The per-channel struct (`0x234` B) holds both index copies at `+0x208` (index00) and `+0x20c`
(index01) and writes them together via `FUN_0008606c` — **`index00.bin`/`index01.bin` are dual
redundant copies**, same scheme as the config's primary/backup. Counters: `+0x10` capacity,
`+0x14` write head, `+0x18` used/tail, `+0x218` valid flag.

## 5. Tooling

- `dump/dev.py` — telnet exec helper (root/blank, quote-split marker framing). `py -3 dump/dev.py "cmd" ...`.
- `dump/capture_state.py` — re-runs the full state capture (edit `OUT` path; `import dev`).
- Transfer: stage to SATA `a1` (310 MB free) → `md5sum` → pull `http://192.168.1.108:8081/<abs path>`
  (httpd docroot `/`, **no range support**) → verify → `rm`. Keeps the PC firewall out of the loop.

## 6. Network servers (Ghidra, partial)

`app.out` runs several TCP servers (see `dump/livedump_20260723/state/netstat.txt`): `80`, `8670`,
`101`, `102`, `6623`. Multiple `accept()`/`bind()` sites; those decoded so far:

- **`proto_aux_server_port101` `0x0000f918`** — a select-loop server, up to 20 client slots (0x30 B
  each), default port **101** (`param_1==0` → `0x0065`). Speaks a **TLV protocol** distinct from the
  main 8670 0x27xx set (this is the CMS/matrix "SendToHost" family; likely the same code is bound to
  102/6623 with different args). Framing decoded:
  - **Request** = `[8-byte header][records…]`; parser `proto_aux_parse_tlv 0x0000ee9c`. Record tag is
    a u16 at hdr+8. Tags: `0x28` hello (payload checks version `0x00070003` = v3.7), **`0x29` login**
    (32-byte user+pass → looked up in the config user table; returns privilege), `0x33` ctrl,
    `0x40` per-channel stream request (channel byte, must be <4), `0x48` (36-byte payload).
  - **Reply** = `[u32 BE total-len][records…]`; builder `proto_aux_build_reply 0x0000f5f4`,
    per-record encoder `FUN_0000f218`. Login-OK emits records `0x28`,`0x46`,`0x41`.
- **`proto_relay_server_6623` `0x0001e0a4`** — port **6623** (`0x19DF`). Accepts a client, sends a
  3-byte handshake, `recv` ≤256 B, and **relays** between the client socket and a second upstream
  socket — a stream relay/proxy (mobile / P2P push). Handler `FUN_0001deb4`.

**Auth uses the shared config user table** (so does the 8670 protocol): `cfg_get_user_count 0x26188`
= `u32 @ payload+0x41b9`; `cfg_get_user 0x26158` = `payload + 0x41bd + i*0x4c`.

### Config payload struct — known field offsets (in the 18066-byte decompressed image)

Live image is in `.bss` at **`0x598341`** (manager struct at `0x59833c`; `image = manager+5`).
Handlers load that base and index by field offset; several field blocks located below (offsets into
the 18066-byte image, verified against `dump/livedump_20260723/flash/mtd0_config_decompressed.bin`):

| offset | size | field | value in our dump |
|---|---|---|---|
| `0x0000` | u16 | version | `0x0100` |
| `0x0002` | u8  | valid | `1` |
| `0x0004` | u32 | magic | `0x013219CD` |
| `0x0038` | u32 | declen | `0x4692` (18066) |
| `0x003c` | u32 | checksum | additive (see §2) |
| `0x0041` | — | channel-name / schedule block | `CH 01`..`CH 04` |
| **`0x1934`** | 4×`0x37` | **video/encode params** per channel | see below |
| **`0x1fc8`** | ~0x30 | **network block** | see below |
| `0x41b9` | u32 | user_count | `1` |
| `0x41bd` | 0x4c/ea | user[] records | `admin`… |
| `0x441f` | 32 | device name | `NetDVR` |
| `0x4449` | 32 | model | `RR104P` |
| `0x4469` | 12 | MAC (no separators) | `001020182468` |
| `0x4489` | 32 | firmware version string | `V4.0.0-JMS502 Aug 12 2011` |

**user record (`0x4c`=76 B):** `+0x00` name(12) · `+0x0c` password(12) · `+0x18` perm-bitmap A(16,
e.g. `111111111111111`) · `+0x28` perm-bitmap B(16) · `+0x38` MAC-bind(18, `00:00:00:00:00:00`=any) · tail(2).

**video/encode params** (`0x1934` + ch·`0x37`, ch 0-3; accessor `FUN_02b078`): `+0x01` channel name
(`CH 0N`, ~33 B) · `+0x22` u8 flag (1 for ch0 / "enabled"?) · `+0x29` u8 fps (`0x19`=25/PAL,
`0x1e`=30/NTSC; app.out auto-flips this by video standard) · remaining bytes = res/bitrate/GOP (TBD).

**network block** (`0x1fc8`): `+0x00` MAC string(18, ASCII `XX:XX:XX:XX:XX:XX`) · `0x1fda` IP(4) ·
`0x1fde` control port(2, BE = `0x21de`/8670) · `0x1fe0` netmask(4) · `0x1fe4` gateway(4) ·
`0x1fe8` DNS(4) · `0x1ff6` HTTP port(2, =80). (`0x1fc0`: four u16 `0,1,2,3` = channel ids.)

With header + these blocks known, **IP / port / channel names / video fps / users / identity can be
edited offline** in the decompressed image, then re-checksummed + deflated + written to mtd0 (§2).

## Environment / toolchain
Ghidra 12.1.2 + GhidraMCP (`mcp__ghidra__*`). Function renames land in the Ghidra DB (`config.c` +
network funcs named so far). **Bridge caveat:** the MCP HTTP timeout is 5 s, so very large functions
(e.g. `main` @ `0x18a8c`, and likely the 8670 dispatch) time out on decompile — bump the timeout in
`bridge_mcp_ghidra.py` (or disassemble in chunks) to reach them.

## 7. Main 8670 protocol (0x27xx) — server + dispatch (Ghidra, CONFIRMED)

Startup path (in `main`): sets the control port (from `DVR_CTRL_PORT.txt`/argv), calls
`FUN_00079a40(port)` = **8670 server** (binds the control socket, `listen` backlog 427, plus a
2nd socket; inits a **256-slot × 19-byte client table**), then spawns two worker threads via
`FUN_00116e24` (a `pthread_create` wrapper w/ sched priority + stack size): **control `0x774ac`**
and **media `0x79254`**. `main` also registers the command handler via `FUN_00076974`
(`*DAT_000769ac = handler`) and starts the aux servers (`proto_aux_server_port101` etc.) as threads.

**The registered command handler is `FUN_00059840`** — one ~98 KB function (`0x59840`–`0x60f4c`)
that is the whole 0x27xx command set. Dispatch = a **binary-search tree** on the u16 command
(`mov r3,#0x2700; add r3,#X; cmp cmd,r3; beq handler / bgt higher`). Extracted in full
(`dump/ghidra/dispatch_8670.json`) — **36 commands → in-function handler addresses**:

| cmd | handler | cmd | handler | cmd | handler |
|---|---|---|---|---|---|
| **0x2711** login✓ | 0x598dc | 0x2727 | 0x5a804 | 0x273e | 0x59f04 |
| 0x2712 | 0x5e87c | 0x2729 | 0x5e4c8 | 0x2741 | 0x5ef3c |
| 0x2715 | 0x5be8c | 0x272a | 0x5ec18 | 0x2742 | 0x59bb8 |
| 0x271c | 0x5bd1c | **0x272b** net✓ | 0x59d64 | 0x2743 | 0x5ce7c |
| 0x271e | 0x5e188 | 0x272c | 0x5d2f0 | 0x2749 | 0x5bbcc |
| 0x271f | 0x5ee58 | **0x2732** substrm✓ | 0x5b900 | 0x274a | 0x5fea8 |
| 0x2720 | 0x5a424 | 0x2738 | 0x5d010 | 0x274b | 0x5a65c |
| 0x2721 | 0x5cf80 | 0x2739 | 0x5fc74 | 0x274c | 0x5d540 |
| 0x273a | 0x5a254 | 0x273b | 0x5e13c | 0x274f | 0x5ce18 |
| 0x2755 | 0x5a9a4 | **0x2757** GetRecState✓ | 0x5df08 | **0x2758** SetRecState✓ | 0x5eef0 |
| 0x2759 | 0x59e34 | 0x275a | 0x5c908 | 0x2771 | 0x5b7d8 |
| 0x4e15 | 0x5f148 | 0x4e1c | 0x5a544 | 0x4e1d | 0x5dd78 |

✓ = semantics confirmed vs `PROTOCOL.md` (login calls `cfg_get_user_count`; 0x2757/0x2758 =
record-state get/set; 0x272b = network; 0x2732 = substream). The other 30 handlers are located
but not yet individually decoded — each is a sub-block inside `FUN_00059840` at the listed address.
A **second handler cluster** (`FUN_0004f7ec`, registered by `FUN_00050bcc`) likely holds commands
not in this tree (e.g. video-params 0x2734, users 0x274e, playback 0x276d, file 0x271b/0x2779).

**Extraction method / tooling note:** the GhidraMCP bridge hard-codes a 5 s HTTP timeout, so `main`
and this 98 KB handler can't be decompiled through it. Work around it by hitting the plugin's HTTP
server **directly**: `http://127.0.0.1:8080/{decompile_function,disassemble_function,xrefs_to,
xrefs_from,searchFunctions,strings,rename_function_by_address,...}` (see `scratchpad/gh.py`), and
disassemble indirectly-referenced thread routines (not auto-analyzed by Ghidra) with **capstone**
straight from `dump/ghidra/app.out` (ELF phdr vaddr→offset). This is how the dispatch table above
was recovered.

## Open questions for the Ghidra pass
1. **Decode the 30 unnamed 0x27xx handlers** (addresses above) + the 2nd handler cluster
   (`FUN_0004f7ec`) — map each to its `Set*`/`Get*` semantics and payload, completing `PROTOCOL.md`.
2. Rest of the config payload struct: video params, network (IP/DHCP), record schedule bitmap format,
   PTZ — decode field offsets against each `Set*` command (§2, extends the table above).
3. Ports `102` and the second copy of the aux server; confirm which port `6623`'s upstream targets.
4. Exact `.ifv` write path + `index0N.bin` record format (§4).

## 8. Capture driver (`/dev/tw_286x`) — TW286x decoder RE (CONFIRMED, static)

Vendor CAPTURE modules recovered from the flash rootfs (NOT present post-boot; `run_app.sh` `rm`s
`/root/tl_modules_r9508`). Extraction: `unpack.py`→truncation-tolerant gunzip of `mtd2_rootfs.bin`
(payload gzip is intentionally short → decompress with `zlib.decompressobj(-MAX_WBITS)`, tolerate the
tail) → `rootfs.ext2` (9.3 MB) → `ext2extract.py` → `/root/tl_modules_r9508.tgz` (644 KB, intact) →
`tar xzf`. Yields `our_modules/{tw286x_R9508.ko (41240 B, not 24628), tl_R9508.ko (16024 B),
tl_gpio_i2c.ko (9096 B), panel_r9508.ko, enc.ko, hiwdt.ko, rtc_pcf8563.ko, hi_ir.ko}`. These are ARM
OABI REL modules that **keep symbol names**; disassembled with the WSL `arm-buildroot-linux-uclibcgnueabi-`
binutils. **Note the `/lib/modules/.../tl_modules_r9508.tgz` copy is a 45-B stub — use the `/root` one.**

**Board silicon = single 4-ch TW2866/TW2865 core, `chip_type=1`, I2C 7-bit `0x28`** (8-bit
wr `0x50` / rd `0x51`) on the bit-banged GPIO bus (`tl_gpio_i2c`). Confirmed by
`dump/livedump_20260723/state/dmesg.txt`: `encoder chip type=1` / `start tw2866 !!! video_mode=NTSC`.
`init_module` probes `tw2865_byte_read(0xFF)==0xC8` (product ID) → chip_type 1; if slave `0x52`'s
`0xFF` is also `0xC8` → chip_type 7 (TW2868 = two cores, 8-ch). The driver is multi-decoder (tw2865/
tw2868/rn6266/lg1702/nvp1108/nvp1104b/mik2455/mp280x/sph8528); ONLY the tw2865 path applies here.
`tw2865_byte_write(r,v)`=`gpio_i2c_write(0x50,r,v)`; `tw2865_byte_read(r)`=`gpio_i2c_read(0x50,r)`.

### ioctl handler `tw2864a_ioctl` @ `.text 0xbac` (magic `'H'`=0x48, all args = 20-byte struct)

| ioctl | nr | @ | meaning |
|---|---|---|---|
| `0xc00448d3` | 0xd3 | 0xf38 | **SET video standard**: copies u32, calls `tw2864_device_video_init(arg)`; **arg 1=NTSC, 2=PAL** (0→PAL). |
| `0xc00448cf` | 0xcf | 0xde0 | **register READ** (status poll). switch on **word[1]** selector; result→word[0]. |
| `0xc00448d0` | 0xd0 | 0xfe8 | **register WRITE**. switch on word[1] selector. |
| `0xc00448d8` | 0xd8 | 0xfd4 | **get chip_type** (returns 1). trace: `IN 00.. OUT 01..`. |
| `0xc00448d7` | 0xd7 | 0xebc | detect/print current standard (per chip_type; ch1 path 0x10d0). |
| `0xc00448d5` | 0xd5 | 0xca0 | pca9555 GPIO expander (I2C 0x40) read/write. |

**Register R/W struct (20 B, 5×u32 LE)** for `0xcf`/`0xd0` — verified from read sub-fn0 @0x1108 &
write sub-fn0 @0x13b0:
```
word[0] u32  READ: result out (reg value). WRITE: unused.
word[1] u32  selector (which decoder). 0 = tw2865 chip0 @0x50  <-- OUR board
word[2] u32  register address (low byte used)
word[3] u32  value to write   (WRITE only, low byte)
word[4] u32  pad
```
selector map: **0=tw2865@0x50**, 1=tw2865 slave1@0x52, 2/3=rn6266 c0/c1, 6/7=tw2868 c0/c1 (page reg
0x40), 8/9=lg1702, 10=sph8528. **To poke any TW2866 register from freestanding C:** open `/dev/tw_286x`,
`ioctl(fd,0xc00448d0,&{0,0,REG,VAL,0})`; read: `ioctl(fd,0xc00448cf,&{0,0,REG,0,0})` then read word[0].
> ⚠ The boot ioctl_trace shows app.out calling `0xcf` with visible `IN: 03 00 00 00` — that is **word[0]
> (the unused/result slot), NOT the selector** (which is word[1], off by 4). The earlier "sub-func 3"
> note was a misread of the truncated 4-byte trace. For our board the selector is always 0.

### SET-standard programming (`tw2864_device_video_init` @0x164)
Reprograms the TW2866 by rewriting per-channel timing at regs `0x00-0x0F` of each channel base
(`0x00,0x10,0x20,0x30`) via `tw2865_write_table` (also blindly writes slave `0x52` for the absent 2nd
core — NAK'd, harmless), then common backend tables (all standard-independent). The **only
standard-specific payload** is the 16-byte per-channel table (regs 0x00-0x0F), std = timing/scale:
```
reg:   00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F   (TW2864 map:
PAL :  00 00 64 15 80 80 00 12 18 20 05 D0 00 10 01 03    02=CONTRAST 03=SHARP 04/05=SAT_UV
NTSC:  00 00 64 11 80 80 00 02 12 F0 0C D0 00 00 00 7F    07=CROP_HI 08=VDELAY 09=VACTIVE
diff:            ^        ^  ^  ^  ^        ^  ^  ^        0A=HDELAY 0B=HACTIVE 0D=VSCALE
                                                          0E=SCALE_HI 0F=HSCALE)
```
i.e. NTSC VACTIVE=0xF0 (480i), PAL VACTIVE=0x20 + SCALE_HI MSB (576i). `tbl_ntsc_tw2865_common`
@`.data 0x92`, `tbl_pal_tw2865_common` @`.data 0x82`. `initial_reg`/`initial_ntsc_reg`/`initial_pal_reg`
belong to sph8528/rn6266, **not** our chip.

### Detected-standard / per-channel loss (auto-detect)
`chip_type==1` detect (video-format handler 0x10d0, init_module 0x6c):
- **reg `0xFD` low nibble = per-channel video-PRESENT** (bit N = ch N; `(0xFD & 0x0F)==0` ⇒ "TW2866
  input no video"). This is the register for PAL/NTSC-present + loss auto-detect.
- **reg `0x00` bit0 = standard** (0 ⇒ "TW2866 input NTSC", 1 ⇒ "TW2866 input PAL"). Per-channel status
  (VDLOSS/HLOCK/VLOCK/SLOCK) is the standard TW286x reg `ch*0x10 + 0x00`, all readable via `0xcf` sel 0.
- Read them live: `ioctl(fd,0xc00448cf,&{0,0,0xFD,0,0})` → word[0] & 0x0F = present mask.

### Blue-screen / VDLOSS pass-through (LIVE: disconnected ch = solid blue ~RGB(30,40,190))
**`tl_R9508` path RULED OUT.** `screen_control` (@0xa68) just writes 1 bit to SoC **GPIO reg
`0x20180004`** via `hs3515_wr` when `hardware_type==0x68` — a physical screen/backlight/relay, not the
digital frame. Its ioctl is `_IOW('V',0xd5,4)=0x400456d5` (arg u32 0/1). The whole `/dev/tl_R9508`
`'V'` set is board I/O only: screen, `sys_software_reset`, `buzz_control`, `board_R9508_init`,
`get_video_format`(RO), `rs485_control`, `alarm_in/out`, `power_control`, and a **raw SoC mem rd/wr**
(TL_DBG_MEM_RW → `hs3515_rd/hs3515_wr`, useful generic primitive but SoC-side, not the I2C decoder).

**The blue is in the TW2866, and `tw2864_device_video_init` writes NO blue-enable bit** — so it is a
TW2866 **power-on default** (auto-mute to blue on loss-of-sync), not decodable further from this driver.
Fix = poke a TW2866 register via `ioctl(0xc00448d0, &{0,0,REG,VAL,0})`, **per-channel `REG = ch*0x10 +
off`** so the working camera channel is untouched. Best empirical candidates (decode a frame after each):
1. **Saturation U/V** `ch*0x10+0x04` & `+0x05` (driver sets 0x80): write `0x00` → kills the blue CHROMA
   of the mute frame → expect gray/black instead of blue. Safest first test.
2. **Per-ch CONTROL** `ch*0x10+0x0C` (driver=0x00): sweep bits — likely holds the VLOSS output-mode.
3. Global backend/system regs the driver touches: `0xC8-0xDF`, `0xF0-0xFB` (encoder/mute area).
Diagnostic: read `ch*0x10+0x00` (per-ch status: VDLOSS/HLOCK/VLOCK/SLOCK) + reg `0xFD` (present nibble)
to confirm the lost channel. True "pass noise" may be impossible (no sync ⇒ no video); "blue→black" via
(1) is the realistic win. Exact auto-mute bit needs the TW2864/2865 datasheet or the poke sweep above.

**Artifacts:** extracted modules saved to the job scratch dir (`tmp/tw286x_R9508.ko`, `tl_R9508.ko`,
`tl_gpio_i2c.ko`, plus `ko_all/`). Disassembly evidence: `tw2864a_ioctl.asm`, `video_init.asm`,
`helpers.asm`, `init_and_strings.asm` in the same dir.
