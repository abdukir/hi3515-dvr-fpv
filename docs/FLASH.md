# FLASH.md — reflashing mtd2 so our DVR boots by itself (milestone M4)

> **STATUS: DONE — 2026-07-25.** mtd2 now holds our image (`b896e9af73394f7decd7fb8eaa036c65`)
> and the DVR boots our firmware from flash, unattended, in ~20 s. The return path was
> proven first by writing the stock image back and booting it. Both directions were
> exercised with a real erase+write (stock → ours → stock → ours), each verified by
> reading the flash back before any reboot. See **"What actually happened"** at the end
> for the two things that only showed up after the first boot.

Everything before this was reversible: `/` is a RAM disk, so a power cycle restored the
stock firmware and our program disappeared. M4 makes the DVR **ours at power-on**. It is the
first genuinely destructive step in this project, so this document is a procedure, not a
suggestion — read the whole thing before running anything.

Everything below is driven by **`tools/dvr.py`** (see `tools/dvrlib.py`), which owns the
telnet/serial/TFTP/capture-card plumbing:

```sh
py -3 tools/dvr.py status                  # is it alive, and what is it running
py -3 tools/dvr.py restore --reboot        # put the STOCK firmware back and boot it
py -3 tools/dvr.py flash flash/out/mtd2_new.bin --reboot
py -3 tools/dvr.py screen shot.png         # look at the DVR's own screen
py -3 tools/dvr.py recover --test          # rehearse the U-Boot recovery path
```

## What actually changes

**One file.** `/root/run_app.sh` in the flash rootfs gets a hand-off hook: mount SATA, and
if `/root/rec/a1/boot.sh` exists, `exec` it instead of `app.out`. That is the entire
functional diff. Everything our project runs — the `dvr` binary, its config, the launcher —
lives on the **SATA disk**, which is writable over the network. So this is a *one-time*
flash: after it, iterating means copying a file to `a1`, never touching the NOR again.

```
power on
  └ U-Boot (mtd0)  →  kernel (mtd1)  →  ext2 RAM disk (mtd2)  →  /etc/init.d/rcS
                                                                    └ /root/run_app.sh
        net.sh (eth0 + telnetd)  ←── always runs first, so telnet is up no matter what
        init.sh (drivers)
        wait for /dev/sda1, mount a1
        ├ /root/rec/a1/boot.sh exists ──→ exec it  ──→ our DVR         ← the new path
        └ otherwise ────────────────────→ mydaemon.out + app.out       ← stock, untouched
```

The image is **rebuilt**, not patched. The vendor's mtd2 is a gzip stream truncated to fit
the 5 MB partition — about 1 MB of the filesystem tail is simply not there (`/bin/busybox`,
`/mknod_console`, most of `/etc` and `/web` extract as empty files) and its uImage data CRC
is wrong. Our image is built from the complete live rootfs dump plus the four boot archives
lifted out of the stock image, so it is a correct, fully-CRC'd, non-truncated image that
fills 70 % of the partition instead of overflowing it.

| | stock mtd2 | ours |
|---|---|---|
| payload declared / stored | 6 317 967 / 5 242 816 (**truncated**) | 3 667 517 / 3 667 517 |
| uImage data CRC | wrong | correct |
| ext2 | rev0, 13372 × 1 KiB | identical geometry |
| free space in the fs | ~3.2 MB | ~6.4 MB |
| `/root/app.tgz` + `data.tgz` | in flash (2.6 MB) | on SATA (`a1/stock/`) |

Moving the stock app to SATA is what buys the headroom: those two files are already
compressed, so they cost their full size in flash. The fallback still works — `boot.sh`
unpacks them from SATA on demand (`touch /root/rec/a1/stock`).

## Hard prerequisites

1. **Serial console.** The *only* recovery path if the box will not boot: no rootfs means
   no Linux, no network, no telnet. USB-TTL (CH340) on **COM6**, 115200 8N1.
   ✅ **Verified** — `py -3 tools/dvr.py serial send "id"` returns a root shell, and
   CTRL-C during boot lands at the U-Boot prompt (`hilinux #`).
2. **U-Boot must be able to take a recovery image.** ✅ **Verified.** U-Boot 2011.06,
   stripped (no `boot`, no `run`), `help` gives:
   `tftp`, `bootp`, `rarpboot`, `erase`, `protect`, `flinfo`, `cp`, `cmp`, `crc32`,
   `md`/`mm`/`mw`/`nm`, `bootm`, `go`, `printenv`/`setenv`/`saveenv`, `reset`, `ping`.
   **There is no `loadb`/`loady`**, so serial image transfer is impossible and **TFTP is
   the only way in** — which needs inbound UDP 69 on the PC. That is firewall-blocked by
   default and must be opened *before* you need it:
   ```
   netsh advfirewall firewall add rule name="DVR TFTP" dir=in action=allow protocol=UDP localport=69
   ```
   Then rehearse it without erasing anything: `py -3 tools/dvr.py recover --test`
   (loads the image into RAM, CRCs it, boots Linux again).
   ⚠️ **Still open at the time of the flash** — the rule was not added, so the U-Boot
   recovery path is proven *available* but has not been exercised end to end.
3. **A manual boot works from the U-Boot prompt.** ✅ **Verified** — `UBoot.boot_linux()`
   replays `bootcmd` (three `cp.b` then `bootm`) one command at a time and Linux starts.
   This alone rescues "I interrupted the boot and now I'm stuck".
4. **Backups on the PC, verified.** `dump/mtd0_boot.bin`, `dump/mtd1_uImage.bin`,
   `dump/mtd2_rootfs.bin` — md5s in `README.md`; re-pull any time with
   `py -3 tools/dvr.py backup DIR`, which compares against those md5s for you. Only mtd2
   is touched, but if mtd0/1 are ever lost the device needs a flash programmer.
5. **The hardware watchdog must be disarmed before writing.** 10 s margin; if whoever
   holds `/dev/watchdog` stops petting it mid-erase the board resets with half a rootfs.
   `tools/dvr.py` does this for you (`quiesce()`): stock `app.out` only releases it via
   its serial debug command `closewd`, so the tool drives that over the console, then
   `exit`s app.out, then kills `mydaemon.out` (which would otherwise respawn it).
6. **Mains power that will not blink** for the ~30 s the write takes.

## Build

```sh
bash flash/build.sh          # WSL: needs e2fsprogs >= 1.43 (mke2fs -d), gzip, tar
bash flash/verify.sh         # 40+ offline checks; refuses to bless a bad image
```

Products in `flash/out/`:

| file | what |
|---|---|
| `mtd2_new.bin` | 5 242 880 B, uImage-wrapped, 0xFF-padded — **this goes in the flash** |
| `rootfs_new.ext2` | the uncompressed filesystem — used by the on-device preflight |
| `sata/` | the SATA payload: `boot.sh`, `dvr`, `dvr.conf`, `stock/app.tgz`, `stock/data.tgz` |

`verify.sh` checks the uImage header and both CRCs, that the payload gunzips *completely*,
the ext2 revision/geometry/fsck state, that every file the boot chain touches is present and
non-empty, that `run_app.sh` still contains the hook **and** the stock fallback **and**
`net.sh`, that ownership is root:root, and that the four `/dev` char nodes have the right
major/minor. It exits non-zero and says DO NOT FLASH if anything is off.

## Procedure

### Step 1 — stage the SATA payload (fully reversible)

```sh
# from the PC, over the existing telnet/httpd path
#   flash/out/sata/*  ->  /root/rec/a1/
```
Then, **before** flashing, prove the hand-off works with the flash untouched: kill the
running DVR and run `sh /root/rec/a1/boot.sh` by hand. It must bring the DVR up exactly as
`run_dvr.sh` does today. If `boot.sh` is wrong, fixing it now costs nothing.

### Step 2 — on-device preflight (still reversible)

Copy `flash/out/rootfs_new.ext2` and `flash/device/preflight.sh` to `/root/rec/a1/` and run:

```sh
sh /root/rec/a1/preflight.sh /root/rec/a1/rootfs_new.ext2
```

This loop-mounts the new filesystem **with the device's own 2.6.24 kernel**, chroots into
it and runs `busybox`, `sh` and `ls`, unpacks both boot archives, and checks the hook. It is
the closest thing to booting the image without committing to it: if the userland in there
cannot execute, you find out here instead of at a kernel panic.

### Step 3 — write it

```sh
sh /root/rec/a1/flash_mtd2.sh /root/rec/a1/mtd2_new.bin
```

The script backs the **current** mtd2 up to `a1/mtd2_before_flash.bin` first, stops the DVR
and the watchdog babysitter, writes via `/dev/mtdblock2`, then **reads the flash back and
compares md5**. Do not reboot unless it prints `FLASH VERIFIED`. If it prints a mismatch,
run it again; if it keeps failing, write the backup back with the same script.

### Step 4 — reboot and watch

Watch the serial console. Expect:

```
[boot] FPV hand-off -> /root/rec/a1/boot.sh
[boot.sh] exec dvr 0 0 r 30 9 1
[dvr] recorder starting
...
[dvr] up: display+stream on; recording idle (manual)
```

Then check from the PC: `ping`, telnet, control port 8090 (`STATUS`), stream port 8091,
and the VGA output.

**Note this is the first time our DVR initialises the MPP without `app.out` having run
first.** Every session so far started app.out, killed it, then started `dvr` on hardware
app.out had already touched. If the pipeline misbehaves on a truly cold start, this is
where it shows — and it is recoverable: `touch /root/rec/a1/stock` over telnet (telnetd is
up regardless) and reboot to get the vendor firmware back.

## Recovery, in order of severity

| symptom | fix |
|---|---|
| DVR misbehaves, network is up | `touch /root/rec/a1/stock` (run the vendor app) or `touch /root/rec/a1/noboot` (idle at a shell), reboot |
| boot.sh is broken | replace it over telnet, or over the **serial root shell** if the network is down — the hook falls through to `app.out` on its own if it's missing |
| our DVR crashes in a loop | `noboot`, then debug over telnet; the flash is not involved |
| network down but Linux is up | `py -3 tools/dvr.py serial send "ifconfig eth0 192.168.1.108"`, then fix `/root/rec/a1/ip` |
| want the stock firmware back | `py -3 tools/dvr.py restore --reboot` (from Linux — the proven path) |
| flash written but wrong | `py -3 tools/dvr.py flashback /root/rec/a1/mtd2_before_HHMMSS.bin` — every flash leaves that backup on SATA |
| **kernel panic / no boot at all** | serial → CTRL-C into U-Boot → `py -3 tools/dvr.py recover` (TFTP into RAM, `protect off`, `erase`, `cp.b`, `cmp.b`, `reset`). **Needs inbound UDP 69 open first** — see prerequisite 2 |
| stuck at the U-Boot prompt | `UBoot.boot_linux()` — replay `bootcmd` by hand (there is no `boot`/`run`) |

U-Boot environment for reference (`README.md` has the full set):

```
bootcmd=showlogo;cp.b 0x80300000 0xc1500000 0x500000;cp.b 0x80200000 0xc1a00000 0x100000;
        cp.b 0x80060000 0xc1b00000 0x80000;bootm 0x80100000 0xc1500000
mtd2 (rootfs) lives at flash 0x80300000, 0x500000 bytes
```
Paste the `cp.b` lines and `bootm` **one at a time** — U-Boot drops UART input during long
copies.

## What actually happened (2026-07-25)

The rehearsal, in order, all from Linux over telnet + serial:

| step | result |
|---|---|
| interrupt U-Boot, `help`, `printenv`, boot Linux by hand | prompt is `hilinux #`; full command list above; manual `bootcmd` replay boots fine |
| `restore --force` (rewrite the identical stock image) | write ok, readback md5 matches |
| `flash flash/out/mtd2_new.bin` (**different** content) | `9392700569…` → `b896e9af73…`, readback verified — proves a real erase+write, not a cached no-op |
| `restore --reboot` (stock back, boot it) | booted the stock UI in 12 s — **return path proven end to end** |
| `flash flash/out/mtd2_new.bin --reboot` (ours) | booted **our** firmware from flash |

`dd if=IMG of=/dev/mtdblock2 bs=64k` is all the write takes — mtdblock does the
erase/read-modify-write itself, and reading the device back is a genuine flash read, so
the md5 compare before rebooting is trustworthy.

### Two things only the first boot could tell us

Both were fixed in `flash/sata/boot.sh` — **on SATA, so neither needed a reflash**:

1. **The IP moved to 192.168.1.114.** `net.sh` sets that hard-coded default; the *stock*
   `app.out` then overrode it from its config blob in mtd0 at `0x0e0000`, which is where
   `192.168.1.108` came from. We don't run `app.out`, so nothing overrode it. `boot.sh`
   now sets the address itself — put a different one in `/root/rec/a1/ip`, or the word
   `dhcp` to use udhcpc.
2. **The serial console went silent.** `run_app.sh` `exec`s into `boot.sh`, so
   `/etc/init.d/rcS` never returns, so init never gets past `::sysinit` and never starts
   the getty in `/etc/inittab`. Since serial is the recovery path, that was a real
   regression: `boot.sh` now starts `/sbin/getty -L ttyS000 …` itself, in a respawn loop.
   (`/root/rec/a1/noconsole` disables it.)

Neither is visible before the flash, because until then `app.out` had always run first.
The general lesson for anything else that turns up: it is almost certainly something
`app.out` used to do for us, and the fix belongs in `boot.sh` on SATA, not in flash.

### Verified after the flash

`fw v2 · 704×240@30 · GOP 15 · FIXQP QP10 · NTSC · 1 encoded channel`, cold boot to
network in ~20 s, our OSD HUD on the VGA output, record start/stop → a playable clip,
live H.264 to the browser at ~21 Mbps with SPS in-band, non-encoded channels correctly
refused, telnet + serial + control port all answering, `dvr.conf` honoured from SATA.

## The boot splash (mtd0, done 2026-07-25)

The picture U-Boot shows while the board starts is a plain **800×600 JPEG in mtd0 at
offset `0x20000`** — env `jpeg_addr=0x80020000`, `jpeg_size=0x20000` (128 KiB ceiling),
and `bootcmd` begins with `showlogo`. The stock one (33 265 B, "REDrock / Sisteminiz
Başlatılıyor") is kept at `dump/bootlogo_stock.jpg`; ours is `flash/bootlogo/splash.jpg`,
generated by `tools/mklogo.py` (text is all CLI flags, so re-theming is one command).

```sh
py -3 tools/mklogo.py flash/bootlogo/splash.jpg --title "FPV GROUND STATION" \
    --subtitle "RR104P · analog video recorder" --status "sistem başlatılıyor..."
py -3 tools/dvr.py logo flash/bootlogo/splash.jpg
```

**mtd0 is the dangerous partition** — it holds U-Boot itself at `0x00000–0x1FFFF`, the
U-Boot environment at `0x40000`, and the device settings blob at `0x0E0000`. Lose U-Boot
and no serial console, no TFTP and no amount of cleverness gets the board back; it's a
programmer job. So `dvr.py logo`:

- refuses an image that doesn't fit in **one** 64 KiB erase block (`mklogo.py` targets
  56 KiB, so a 800×600 splash at q92 lands around 31 KiB);
- md5-fingerprints **all 16 blocks** of mtd0 first and copies the whole partition to SATA;
- writes exactly one block — `dd … of=/dev/mtdblock0 bs=64k seek=2`, never a full-partition
  rewrite, so U-Boot's own bytes are never erased;
- re-fingerprints all 16 blocks afterwards and fails loudly if *any* block other than
  block 2 changed.

Verified on hardware: block 2 `5b1b8b46…` → `6cebde31…`, the other fifteen bit-identical,
and the splash renders on the VGA output at boot before handing over to the DVR's live view.

## What this does not do

- **mtd0 and mtd1 are never touched.** U-Boot and the kernel stay stock. The device
  settings blob at mtd0 `0x0e0000` (see `docs/FIRMWARE_RE.md` §2) is also untouched, so the
  MAC/IP the vendor app configured still apply.
- No change to the partition table, `bootcmd`, or `bootargs`.
- The stock `mydaemon.out` watchdog babysitter is only started on the fallback path — our
  DVR handles `/dev/watchdog` itself.
