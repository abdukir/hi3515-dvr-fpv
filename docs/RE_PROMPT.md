# Prompt for a dedicated firmware-RE Claude Code terminal

Copy everything below the line into a fresh Claude Code session opened in
`C:\Work\priv\hi3515-dvr-fpv`. Connect the Ghidra MCP server first (so the
`mcp__ghidra__*` tools are available), then paste.

---

You are picking up a firmware reverse-engineering effort on an old 4-channel analog
CCTV DVR (**NetDVR RR104P**, HiSilicon **Hi3515 v100**, ARMv5TE / ARM926EJ-S, 32-bit
little-endian, Linux 2.6.24 + BusyBox). I own this hardware; this is authorized RE of
my own device. The end goal is to **modify the DVR firmware itself** so it works the
way I want as an **FPV analog recorder / ground station** — instead of only driving it
from the outside over its proprietary network protocol.

## First: orient yourself (do this before touching Ghidra)

Read these, in order — they are the accumulated state of the project, do not re-derive them:
1. `CLAUDE.md` — repo overview and conventions.
2. `README.md` — hardware, flash layout, boot chain, access, dump inventory.
3. `PROTOCOL.md` — the reverse-engineered 8670 network protocol + command map (already
   ~30 commands decoded statically from the Windows client DLL).
4. `docs/PROJECT_NOTES.md` — full running state, decisions, and what worked.
5. `SETUP.md` — how the tooling/extraction scripts are run.

The existing network-side RE is thorough. Your job is the **device side**: the ARM
binaries and firmware images, which have NOT been disassembled yet.

## What's in the dump (all under `dump/`, md5-verified against the device)

- `dump/mtd0_boot.bin` (1 MB) — U-Boot 2011.06 + boot logo jpeg + a config blob.
- `dump/mtd1_uImage.bin` (2 MB) — Linux 2.6.24 kernel (uImage-wrapped).
- `dump/mtd2_rootfs.bin` (5 MB) — uImage-wrapped gzip **ext2 RAM-disk** rootfs (`/` is
  volatile — NOT persistent; persisting a change means reflashing mtd2).
- `dump/live_rootfs.tar.gz` — the **complete** rootfs pulled live from device RAM
  (more reliable than the flash extraction, whose tail is intentionally truncated).
  The main app lives inside it, nested: `lib/modules/2.6.24-rt1-hi3515v100/app.tgz`
  → unpack that to get **`app.out`** (the main DVR binary, ARM ELF, ~3.26 MB — the
  primary Ghidra target) plus `run_app.sh`, `init.sh`, `net.sh`, `mydaemon.out`.
- `dump/activex_x/CAB/` — the Windows client: `NetDvr2.dll` (protocol core, already
  largely decoded), `TLPlay.dll`, `avcdec.dll` (H.264 decoder), `TLNetDvr.ocx`. Use as
  a **cross-reference** to confirm/extend command semantics you find in `app.out`.
- Kernel drivers (`.ko`) are in the rootfs: `tw286x_R9508.ko` (TW2866 4-ch analog
  capture), `hi3515_venc.ko` / `hi3515_h264e.ko` (H.264 HW encoder), plus HiSilicon
  MPP modules (`mmz.ko`, `hidmac.ko`, `hiether.ko`, `hi_mci.ko`).

**The extracted rootfs is NOT present on this machine yet — regenerate it first:**
```sh
# Windows: prefer `py -3` (bare `python` may be a broken MS Store stub)
py -3 unpack.py                                      # mtd*.bin -> dump/rootfs.ext2
py -3 ext2extract.py dump/rootfs_padded.ext2 dump/rootfs_x 9303315
mkdir -p dump/live_x && tar -xzf dump/live_rootfs.tar.gz -C dump/live_x
# then extract the nested app bundle to get app.out:
tar -xzf dump/live_x/lib/modules/2.6.24-rt1-hi3515v100/app.tgz -C <scratch>
```
Confirm `app.out` is an ARM LE ELF before loading it into Ghidra (`file`, or check the
`7f 45 4c 46` magic + `e_machine=0x28` ARM). If ext2extract's numbers differ from the
above, read `SETUP.md` / the script headers — don't guess.

## Tools you have

- **Ghidra via MCP** (`mcp__ghidra__*`) — your main disassembly/decompilation engine.
  Load `app.out` as ARM v5 little-endian. It's a stripped BusyBox-era uClibc binary;
  expect no symbols. Lean on strings, syscall patterns, and the known protocol to anchor
  functions.
- **The live device** is on the LAN at `192.168.1.108` — **telnet, user `root`, blank
  password = root shell** (also busybox `httpd` on :8081 that the webapp starts). Use it
  to *validate* static findings dynamically: `cat` config files, `strings` on-device,
  read `/proc`, inspect the running `app.out`'s open files/sockets, dump EEPROM/mtd,
  observe behavior when you change a config byte. **Caution:** there is a hardware
  watchdog (10 s) — run `closewd` at the app's `[user]#` debug prompt before living in
  the shell, and **never write to flash** without an md5 verify plan; the `dump/mtd*.bin`
  backups are irreplaceable. Prefer reading over writing; sandbox experiments on the
  SATA drive (`/root/rec/…`), not in flash.
- Python RE scripts already in the repo: `dump/analyze_dll.py`, `dump/cmdmap.py`,
  `dump/cmddict.py` (client-DLL decoding), `unpack.py`, `ext2extract.py`.

## Reverse-engineering objectives (roughly in priority order)

1. **`app.out` command dispatch** — find the 8670 request handler and its command table.
   Map every `cmd` (0x27xx / 0x4e27 etc.) to its handler, request/reply struct layout,
   and side effects. Goal: **complete PROTOCOL.md** — fill the "?" rows, nail down every
   config *set* payload format (video params 0x2734, rec params 0x2730, substream 0x2732,
   network 0x272b, users 0x274e), PTZ 0x2756, playback cluster 0x276d…, file download
   0x271b/0x2779. Confirm against `NetDvr2.dll` where possible.

2. **Config storage & how the app reads it** — where do settings live (the mtd0 config
   blob? an EEPROM via i2c? files on SATA?) and in what format. Find how IP/resolution/
   bitrate/channel names/record schedule are persisted and loaded at startup. Goal: be
   able to **change settings by editing storage directly**, without the Windows client.

3. **Capture → encode → record pipeline** — how `app.out` uses the HiSilicon MPP +
   `tw286x` + `venc` to go from analog-in to the `.ifv` files at `/root/rec/aN/`. Decode
   the `.ifv` container fully (we know it starts with `62 19` packet headers + Annex-B
   H.264; `dump/ifv_head.bin`, `dump/rec_index00.bin`, `dump/rec_log.txt` are samples —
   also see `webapp/dvr/recordings.js`). Goal: understand it well enough to **script or
   replace** recording (e.g. continuous FPV capture, custom naming, or a smaller custom
   capture→encode→file program using the same MPP calls).

4. **Boot chain & the cleanest modification/persistence hook** — trace
   `rcS → run_app.sh → {net.sh, init.sh, mydaemon.out, app.out}`. `run_app.sh` already
   has a `read -t 1` "run app.out (y/n)?" hook and there's a `[user]#` debug prompt.
   Identify the **least-risky injection point** (the plan on file: reflash mtd2 with an
   edited `run_app.sh` that sources a script off the SATA drive, so we can iterate
   without ever reflashing again). Document the exact reflash recipe (mkimage/gzip wrap,
   U-Boot `protect off`/`erase`/`cp.b`, md5 verify) and the U-Boot env quirks
   (stripped: no `boot`/`run`; paste `cp.b` lines one at a time).

5. **U-Boot & kernel (`mtd0`, `mtd1`)** — enough to safely reflash and to know the
   boot args / logo / any config there. Lower priority unless persistence work needs it.

## How to work

- **Validate statically-derived claims against the live device** whenever you can — a
  hypothesis about a config offset or command format is cheap to confirm over telnet.
  Label each finding CONFIRMED (observed on device or cross-checked in the DLL) vs
  INFERRED (decompiler only).
- Work in focused passes; don't try to boil the ocean of a 3.26 MB binary. Anchor on the
  known protocol commands and strings, expand outward from there.
- **Do no harm:** read-only by default. No flash writes, no `app.out` kills that could
  brick a recording session, without an explicit backup+verify+rollback plan stated first.

## Deliverables (write findings down as you go — don't leave them only in chat)

- Update **`PROTOCOL.md`** as command handlers are decoded (it's the authoritative spec).
- Create **`docs/FIRMWARE_RE.md`** — a running teardown of `app.out`: dispatch table,
  key functions (with Ghidra addresses), config storage format, the record pipeline, and
  the modification/persistence plan with the exact reflash recipe.
- Create **`docs/IFV_FORMAT.md`** if the `.ifv` container work grows beyond a paragraph.
- Note anything that changes the project's direction so the main terminal can pick it up.

Start by reading the orientation docs, regenerating the extracted rootfs + `app.out`,
then loading `app.out` into Ghidra and locating the 8670 command dispatch. Report what
you find before making any on-device or on-flash changes.
