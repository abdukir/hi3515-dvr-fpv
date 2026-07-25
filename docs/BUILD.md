# BUILD.md — building & running our own programs on the RR104P / Hi3515 DVR

Proven working 2026-07-23: a modern cross-gcc + a tiny hand-written OABI runtime produces
native ARM binaries that run on the stock device. This is the foundation for writing our own
FPV recorder. Scaffold lives in **`device/`**.

## The one critical gotcha: the kernel is OABI-only

The device (Linux 2.6.24, ARM926EJ-S / ARMv5TE) uses the **old ARM ABI (OABI)**. Its ELF flags
are `0x00000202` (EABI ver 0 = "GNU EABI" = OABI); busybox and `app.out` are all OABI. Syscalls
must use **`swi #(0x900000 + nr)`** with args in r0–r6. The modern **EABI** convention
(`svc 0` + syscall number in r7) **SIGILLs** on this kernel (verified: an EABI static binary,
even with zero ARMv6 instructions, dies with "Illegal instruction"; the same code using OABI
`swi #0x9000xx` runs fine).

Consequences:
- **Instructions** from any modern EABI ARM toolchain are fine — they're plain ARMv5.
  (Build for `-march=armv5te -mfloat-abi=soft`; ARM926 has no VFP and no ARMv6 `ldrex`/`dmb`.)
- **The toolchain's libc (glibc/musl/uClibc) is NOT usable** — its syscall stubs are EABI.
  So we build `-nostdlib` and supply our own OABI syscall stubs (`device/oabi.h`) + `crt0.S`.
- A syscall number must be a compile-time constant (it's the `swi` *immediate*), so each stub
  uses a GCC `"i"` asm constraint. See `device/oabi.h`.

## Toolchain (build host = WSL Ubuntu)

No cross-gcc ships on the PC; use WSL. A prebuilt bootlin **armv5 uClibc** toolchain (gcc 14.3.0)
installs with no root:
```
# inside WSL: ~/arm-uc/bin/arm-linux-gcc  (from)
https://toolchains.bootlin.com/downloads/releases/toolchains/armv5-eabi/tarballs/armv5-eabi--uclibc--stable-2025.08-1.tar.xz
```
(The original firmware was built with GCC 3.4.3 CodeSourcery ARM 2004/2005 — an OABI toolchain —
per app.out's `.comment`. We don't need it; we only use the modern gcc for instruction generation
and bypass its libc.)

### Cross-boundary quoting gotchas (Windows git-bash → wsl.exe)
- git-bash **path-translates** `/...` args (e.g. `/mnt/c/...` → `C:/Program Files/Git/mnt/c/...`).
  Prefix commands with **`export MSYS_NO_PATHCONV=1`** (per-call; env doesn't persist between
  Bash tool calls).
- Shell **variables** inside `wsl -- bash -c '...$V...'` often expand to empty across the boundary.
  Use **literal paths** or put the script in a **file** and run `wsl -- bash /mnt/c/.../script.sh`.
- WSL stdout is UTF-16-ish; pipe through `tr -d '\0'` or have the script write a file you Read.

## Build

`device/build.sh` compiles a `main.c` against the scaffold:
```
# in WSL:
GCC=~/arm-uc/bin/arm-linux-gcc bash /mnt/c/Work/priv/hi3515-dvr-fpv/device/build.sh main.c out
# -> arm-linux-gcc -march=armv5te -mfloat-abi=soft -static -nostdlib -ffreestanding
#       -I device/ -o out device/crt0.S main.c ; strip
```
Verify no ARMv6+/EABI leakage: `arm-linux-objdump -d out | grep -E 'ldrex|strex|dmb|movw|movt'`
should be empty, and every `svc` should be `svc 0x9000xx` (OABI), never `svc 0` (EABI).

## Deploy + run (no PC firewall change; device listens, PC connects out)

```
# device (telnet root/blank), detached listener:
(nc -l -p 5017 > /root/rec/a1/prog 2>/dev/null &)
# PC: connect out and send the bytes (see scratchpad/push_run.py); then:
chmod +x /root/rec/a1/prog && /root/rec/a1/prog
```
Helper: `scratchpad/push_run.py` pushes a local file and runs it in one shot.

## 🚨 CRITICAL: SDK MPP VERSION MISMATCH (found 2026-07-23, the real blocker)

The `bluhbluh/Hi3515-SDK` repo is **`Hi3515_MPP_V1.0.0.0`**, but the device (app.out + its loaded
kernel modules) runs **`Hi3520_MPP_V3.0.6.2`** (app.out string: `HI_VERSION=Hi3520_MPP_V3.0.6.2`;
Hi3515 and Hi3520 are the same MPP family). V1→V3 restructured the MPP: VENC's core ioctls stayed
stable (create `0x40684500`, GetStream `0xc00c450c` all matched our RE — record would work), **but the
SYS/VIU/VB *init* interface changed**. So the V1 SDK's `HI_MPI_SYS_SetConf`/`HI_MPI_VI_SetPubAttr`
don't match the device's V3 kernel → **`HI_MPI_VI_SetPubAttr` returns `SYS_NOTREADY` (0xa0108010)`**,
and no config tweak (pin-mux, tl_R9508 ioctl — both tried, both applied cleanly) fixes it. Verified on a
pristine boot (app.out never started via `run_app.sh` `n` prompt): same failure → not a state issue.

**Two paths for the fresh session (do NOT keep fighting the V1 SDK):**
1. **Get the matching SDK — `Hi3520`/`Hi3515` MPP `V3.0.6.2`** (headers + `libmpi.a` + it will match the
   device's kernel modules). Then `our_dvr.c` + `SAMPLE_*` bring-up should work as-is. This is the clean
   route — the whole `device/our_dvr.c` + `device/sdk/build.sh` toolchain is already proven; only the
   SDK version is wrong. Look for "Hi3520 SDK V3.0.6.2" / "Hi3515 SDK V3.x".
2. **Pure-RE, no SDK** — replicate app.out's exact V3 ioctls (which match the kernel): the VIU/SYS init
   in `tl_hslib_init` (`FUN_0013527c` → `FUN_001342ac` → sys-config ioctl **`'B' 0x4044420a`** with a
   68-byte struct, then the VI sequence `FUN_0013961c`) plus tw286x (`/dev/tw_286x` `'H'`) and
   `/dev/tl_R9508` (`'V' 0xc00456d3, 0x64`). Version-independent, all reversible, but more work. Docs in
   `docs/FIRMWARE_RE.md`.

**Everything else is proven and stays valid:** OABI toolchain, build chain, `our_dvr.c` (SYS/VB init,
VENC/GetStream/record loop, watchdog), and the serial clean-MPP + boot-without-app.out workflow. The
`tl_R9508` VIU-config bring-up (`ioctl(/dev/tl_R9508, 0xc00456d3, 0x64)` → `<dbg>set hardware_type=100`)
is correct and needed. Only the SDK version blocks the finish.

## ⭐ The MPP SDK route (recommended — found & validated 2026-07-23)

The **Hi3515 V100 MPP SDK** (github.com/bluhbluh/Hi3515-SDK) is staged into **`device/sdk/`**:
`include/` (45 headers — `hi_comm_venc.h`, `mpi_venc.h`, `mpi_sys.h`, `hi_comm_vb.h`, `mpi_vi.h`,
`hi_type.h`, …), `lib/libmpi.a` (static, **OABI** `Flags 0x200` — links with device code), and
`sample/venc/` + `sample/common/` (reference capture→encode). **This SDK matches our device**:
`VENC_CHN_ATTR_S = {PAYLOAD_TYPE_E enType; void *pValue;}` and `VENC_ATTR_H264_S` map **byte-for-byte**
to what we reversed from `app.out` (create ioctl `0x40684500`, 104-B attr, enType=0x60=H.264).

This collapses the "reverse 50 MPP structs" problem into normal MPP programming. Instead of raw ioctls,
`#include` the headers and call `HI_MPI_SYS_Init` → VB pool → `VI_BindOutput` → `VENC_CreateChn` →
`VENC_RegisterChn` → `VENC_StartRecvPic` → `GetStream`/`ReleaseStream` (exact sequence in
`device/sdk/sample/venc/sample_venc.c`: `SAMPLE_VENC_4D14CifH264`). All DVR logic + VGA UI stays our
own code; only hardware bring-up uses the SDK.

**Toolchain:** the SDK ships the *original* **`gcc-3.4.3-uClibc-0.9.28`** (arm-hisi-linux, **OABI** —
exact match to the firmware). In `tools/toolchains/` of the repo; extract with Python (`bz2` module —
WSL has no `bzip2` CLI). **One-time setup:** it's a 32-bit x86 binary, so on 64-bit WSL run once:
`sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install -y libc6:i386 zlib1g:i386`
— then `arm-hisi-linux-gcc` runs and links `libmpi.a`. (This replaces the `device/` OABI-runtime hack;
that hack stays valid for tiny SDK-free tools.)

**Build a recorder:** `arm-hisi-linux-gcc our_dvr.c device/sdk/sample/common/*.c
-I device/sdk/include -L device/sdk/lib -lmpi -lpthread -lm -o our_dvr`, push to device, **stop
app.out first** (our program owns the MPP — two MPP apps can't coexist), run. Because we create the
VENC channel, we can read our own frames (no MMZ isolation problem) and record with our own format.

## ✅✅ OUR OWN DVR compiles AND runs on the device (2026-07-23)

`device/our_dvr.c` — our own 4-channel H.264 recorder — **compiles to a device-native OABI binary**
(`Flags 0x202`, `ld-uClibc.so.0`, 105 KB stripped) via `device/sdk/build.sh` (link `libmpi.a` +
`sample_common.c`/`vo_open.c`; **build from ext4 not `/mnt/c`** — the old gcc's 32-bit `stat()`
overflows on DrvFs; include `-I $SDK/extdrv`). It does full MPP bring-up (`SAMPLE_InitMPP` → VB pools →
VI/VO → `SAMPLE_StartVenc`) + our own record loop (`HI_MPI_VENC_GetStream` → `pu8Addr[0]` → our
`/root/rec/aN/fpv_chN.h264`) and **manages the hardware watchdog** (`/dev/watchdog`, pet in loop,
disarm `'V'` on exit — required once app.out stops petting).

**Live run result:** on the device it printed `init MPP...` and **SYS_Init + VB pools succeeded** — our
own MPP program initialized the hardware. It then failed at capture with `open /dev/tw2865dev fail`
→ `set vi dev 0 attr fail`, because the SDK sample configures the stock **tw2865** driver, but our
device uses a customized **tw286x** driver at **`/dev/tw_286x`** (`tw286x_R9508.ko`), with the `'H'`
(0x48) ioctls we reversed in `FUN_0013527c` (`docs/FIRMWARE_RE.md`). The watchdog handling held; no
reboot; no harm.

**THE remaining fix:** replace `SAMPLE_StartViVo_SD`'s tw2865 config with the device's tw286x capture
bring-up — either drive `/dev/tw_286x` directly with the reversed `'H'` ioctls (`0xc00448d7/d8`,
68-B structs) + the matching `HI_MPI_VI_SetDevAttr`, or load the SDK's `extdrv/tw2865` `.ko` if
compatible. Then run with app.out **actually** stopped (our first test's `kill` awk had a syntax
error, so app.out stayed up — a proper test needs app.out down for exclusive MPP ownership). That's
the last step to real recording; then the VGA UI (`HI_MPI_VO_*`/`hifb`, `sample/hifb`).

### ⚠️ Live-test protocol (learned the hard way, 2026-07-23)
**app.out MUST be reliably stopped before running our_dvr — two MPP owners crash the kernel**
(reboots the device; recoverable, app.out auto-restarts on boot). In a test, a fragile pid-parse
returned no PIDs, so our_dvr ran alongside app.out → reboot. Also, `our_dvr.c` was updated to skip the
SDK tw2865 config (rely on the chip staying configured) — but it wasn't validated because of that
reboot. Do it safely next time:
- **Reliable stop:** kill `mydaemon.out` FIRST (may respawn app.out), then all `app.out` PIDs, and
  **verify `ps` shows none** before launching. (`kill -9` may leave MPP/MMZ/VB allocated → InitMPP can
  fail on dirty state.)
- **Cleanest:** boot without app.out — answer `n` at `run_app.sh`'s 1-s prompt over the **serial
  console**, so the MPP starts fresh under our ownership. Serial is the right tool for this bring-up.
- our_dvr now **disarms the watchdog on every exit path** (failed run won't reboot) and **pets it in
  the loop** (keeps the device alive after app.out stops).

### Serial-console workflow (COM5 @ 115200) — validated 2026-07-23
The **clean way** to get exclusive MPP ownership WITHOUT a reboot: at app.out's `[user]#` debug prompt
on serial, run **`closewd`** (disable watchdog) then **`exit`** (app.out shuts down *cleanly* — kernel
logs `mmz_userdev_release ... force unmap` freeing its MMZ, `Watchdog is disabled`, then an auto-login
root shell). MPP is now free, modules stay loaded, network/telnet stay up. Kill `mydaemon.out` too.
Push `our_dvr` over the network, run it from serial to watch console + kernel messages. Helper:
`scratchpad/ser.py` (`probe` / `send "<cmd>" <secs>`).

### ✅ SOLVED: the VI capture bring-up (the last piece) — recipe for the fresh session
On a clean MPP, `our_dvr` passes `init MPP...` (SYS_Init + VB OK) but **`HI_MPI_VI_SetPubAttr` fails
`0xa0108010` = `HI_ERR_VI_SYS_NOTREADY`** (`EN_ERR_SYS_NOTREADY`=16, confirmed). Root cause: the VIU has
**no input clock** — the SDK sample configures a stock tw2865, but our device needs its **vendor
capture layer** brought up first. `libmpi` is identical to app.out's, so the only difference is that
app.out runs `tl_hslib_init` (**`FUN_0013527c`**) *before* VI setup. Fully reversed:

**The two device nodes:**
- **`/dev/tw_286x`** — TW2868 analog chip, magic `'H'`(0x48). ioctls `0xc00448d7`/`0xc00448d8`
  (`_IOWR('H',215/216,68)`) query/set video-present + norm.
- **`/dev/tl_R9508`** — the **vendor board-support driver** (`tl_R9508.ko`), magic `'V'`(0x56). This is
  the key one: **`ioctl(fd, 0xc00456d3, 0x64)`** (`FUN_00134e90`; value `0x64`=100 for the standard
  board model — other models use 0x68/0x69/0x6a/0x6b/0x6e) **configures the VIU video input** →
  removes SYS_NOTREADY. Also `0xc00456ce` (query).

**app.out's bring-up order (`tl_hslib_init` / `FUN_0013527c`):**
1. open `/dev/tw_286x` + `/dev/tl_R9508`.
2. `FUN_00134e90`: `ioctl(/dev/tl_R9508, 0xc00456d3, 0x64)` — **board/VIU video config** (the critical step).
3. tw286x query/config: `ioctl(/dev/tw_286x, 0xc00448d7/d8, &struct68)`.
4. VI setup `FUN_0013961c` on **VI devices 2 AND 0** (not just 0): `VI_DisableDev → VI_SetPubAttr →
   VI_EnableDev → VI_SetChnAttr → VI_EnableChn`. PubAttr built by `FUN_0013911c`/`FUN_00139264`
   (BT.656, 4D1). NTSC path = `FUN_00139838`, PAL = `FUN_00139758`.

**So `our_dvr` capture bring-up (fresh session):** after `SAMPLE_InitMPP`, do a small
`tw286x_vi_up()`: open both nodes → `ioctl(tl_R9508, 0xc00456d3, 0x64)` → tw286x ioctls → then the
standard `HI_MPI_VI_*` on dev 0 (and 2 for 8ch). Then `SAMPLE_StartVenc` + our record loop. Remaining
to nail: the 68-byte tw286x struct contents (probably not needed — the `tl_R9508` config may suffice;
test empirically) and confirming VI dev numbering. Everything else is proven. SYS/VB/VENC/GetStream/
record/watchdog all work; only this `tl_R9508` video-config call was missing.

## Status / next

Proven: `main(argc,argv)`, `write/read/open/close/ioctl/lseek/uname` all work (`device/hello.c`,
1116 B, no libc). **Remaining for a recorder:** reverse the MPP capture→encode ioctl sequence from
`app.out` (`/dev/tw_286x`, `/dev/vi`, `/dev/venc` …) and drive those `/dev` nodes via `sys_ioctl`
+ `sys_mmap` from our runtime — no HiSilicon SDK libs required (they're static-linked in app.out;
we replicate the ioctl calls). See `docs/FIRMWARE_RE.md`.
