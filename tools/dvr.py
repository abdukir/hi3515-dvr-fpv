#!/usr/bin/env python3
"""dvr.py — the one command for driving the RR104P DVR.

    py -3 tools/dvr.py status                     device health in one line
    py -3 tools/dvr.py shell "ps | head"          run a command over telnet
    py -3 tools/dvr.py ctl INFO                   control-protocol command (:8090)
    py -3 tools/dvr.py push FILE /root/rec/a1/x   md5-verified upload (chunked)
    py -3 tools/dvr.py pull /root/rec/a1/x FILE   download via the device httpd
    py -3 tools/dvr.py screen shot.png            grab the DVR's VGA via the capture card
    py -3 tools/dvr.py serial watch 20            listen to the console
    py -3 tools/dvr.py serial send "ls /root"     type into the console (root shell)
    py -3 tools/dvr.py serial uboot               interrupt U-Boot (needs a power cycle)
    py -3 tools/dvr.py backup DIR                 pull mtd0/1/2 off the device
    py -3 tools/dvr.py deploy                     build-free: push dvr + boot.sh to SATA
    py -3 tools/dvr.py flash IMAGE [--reboot]     write mtd2, verified before any reboot
    py -3 tools/dvr.py restore [--reboot]         flash dump/mtd2_rootfs.bin back
    py -3 tools/dvr.py flashback [IMG]            rewrite mtd2 from a device-side backup
    py -3 tools/dvr.py recover [--test] [IMG]     LAST RESORT: restore via U-Boot + TFTP
    py -3 tools/dvr.py logo splash.jpg            replace the U-Boot boot splash
    py -3 tools/dvr.py mtd0 [--save F]            audit mtd0; flag any critical block change
    py -3 tools/dvr.py wait                       block until the DVR answers again

Env: DVR_HOST, DVR_SERIAL (COM6), DVR_CAPTURE ("USB Video"), FFMPEG.
"""
from __future__ import annotations

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dvrlib as D                                                    # noqa: E402

# Device output is arbitrary bytes (log files, ls with ANSI colour, binary-ish dumps) and
# this console is often a legacy codepage — printing a stray byte would otherwise kill the
# command with UnicodeEncodeError halfway through a flash operation.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass

REPO = D.REPO
STOCK_MTD2 = os.path.join(REPO, "dump", "mtd2_rootfs.bin")
PARTSZ = 5 * 1024 * 1024


def _hdr(t: str) -> None:
    print(f"\n=== {t} ===")


# Git Bash / MSYS rewrites anything that looks like a POSIX path in an ARGUMENT before the
# program is even started: `/root/rec/a1/dvr` arrives as `C:/Program Files/Git/root/rec/a1/dvr`.
# The device then gets told to write to "C:/Program", which fails in confusing ways (a
# listener that never starts, an "is a directory" error buried in a subshell). Undo it here
# rather than relying on everyone remembering to export MSYS_NO_PATHCONV=1.
_DEV_ROOTS = ("root", "tmp", "dev", "etc", "proc", "sys", "bin", "sbin", "usr", "var", "mnt", "lib")
_MANGLED = re.compile(r"^[A-Za-z]:[\\/].*?[\\/]((?:%s)[\\/].*)$" % "|".join(_DEV_ROOTS))


def devpath(p: str) -> str:
    """Normalise a DEVICE-side path argument, undoing MSYS mangling if present."""
    m = _MANGLED.match(p or "")
    return "/" + m.group(1).replace("\\", "/") if m else p


# ------------------------------------------------------------------------ commands
def cmd_status(_args) -> int:
    print(f"host {D.HOST}")
    i = D.info()
    if not i:
        print("  control :8090  NOT ANSWERING")
    else:
        print(f"  control :8090  fw v{i.get('ver')}  up {i.get('up')}s  {i.get('time','')}")
        print(f"  encoder        {i.get('res')} @{i.get('fps')} gop{i.get('gop')} "
              f"rc{i.get('rc')} qp{i.get('qp')}  enc={i.get('enc')}ch  {i.get('std')}")
        print(f"  state          rec={i.get('rec')} disk={i.get('disk')}MB "
              f"viewers={i.get('cli')} playing={i.get('pb')} osd={i.get('osd')}")
    try:
        with D.Telnet().connect(timeout=8) as t:
            who = t.exec("uptime; ps | grep -c '[d]vr'").replace("\n", " | ")
            print(f"  telnet :23     {who}")
            print(f"  running        {t.exec('ps | grep [/]dvr | head -2') or '(no dvr process)'}")
    except Exception as e:
        print(f"  telnet :23     FAILED: {e}")
    return 0


def cmd_shell(args) -> int:
    with D.Telnet().connect() as t:
        print(t.exec(" ".join(args), timeout=120))
    return 0


def cmd_ctl(args) -> int:
    for line in D.ctl(" ".join(args), timeout=12):
        print(line)
    return 0


def cmd_push(args) -> int:
    local, remote = args[0], devpath(args[1])
    mode = args[2] if len(args) > 2 else "644"
    t0 = time.time()
    # The device is a 200 MHz ARM9 with 5 MB free RAM; under load `nc -l` sometimes isn't
    # accepting by the time we dial. Retry the whole transfer rather than failing a deploy.
    for attempt in range(3):
        try:
            D.push(local, remote, mode)
            break
        except Exception as e:
            if attempt == 2:
                raise
            print(f"  transfer failed ({e}); retrying", file=sys.stderr)
            time.sleep(3)
    print(f"{local} -> {D.HOST}:{remote}  {os.path.getsize(local)} bytes  "
          f"md5 {D.md5_file(local)}  ({time.time() - t0:.0f}s)")
    return 0


def cmd_pull(args) -> int:
    n = D.pull(devpath(args[0]), args[1])
    print(f"{D.HOST}:{args[0]} -> {args[1]}  {n} bytes  md5 {D.md5_file(args[1])}")
    return 0


def cmd_screen(args) -> int:
    out = args[0] if args else "screen.png"
    D.screen(out)
    print(f"{out}  {os.path.getsize(out)} bytes")
    return 0


def cmd_serial(args) -> int:
    mode = args[0] if args else "watch"
    if mode == "watch":
        secs = float(args[1]) if len(args) > 1 else 15
        with D.Serial() as s:
            print(f"listening on {D.SERIAL_PORT} for {secs:.0f}s...")
            txt = s.read(secs)
        print(txt if txt else "(silent — the console only speaks when something happens)")
    elif mode == "send":
        with D.Serial() as s:
            s.send(" ".join(args[1:]))
            time.sleep(1.5)
            print(s.read(3))
    elif mode == "uboot":
        with D.Serial() as s:
            print("hold on — power-cycle the DVR now; sending CTRL-C until U-Boot answers")
            ok = s.interrupt_uboot(timeout=float(args[1]) if len(args) > 1 else 90)
            print("U-BOOT PROMPT" if ok else "no prompt seen")
            print(s.read(1)[-2000:])
    else:
        print("serial watch|send|uboot"); return 2
    return 0


def cmd_wait(args) -> int:
    what = args[0] if args else "ctl"
    ok = D.wait_for(what, timeout=float(args[1]) if len(args) > 1 else 180)
    print("up" if ok else "TIMED OUT")
    return 0 if ok else 1


def cmd_backup(args) -> int:
    out = args[0] if args else os.path.join(REPO, "dump", time.strftime("mtd_%Y%m%d"))
    os.makedirs(out, exist_ok=True)
    known = {
        "mtd0": ("mtd0_boot.bin", "e2b7b5a8b53468cc949b146a21ad699f"),
        "mtd1": ("mtd1_uImage.bin", "a6f061bbfae0bb460f33baf558b7bb77"),
        "mtd2": ("mtd2_rootfs.bin", "9392700569cead3e87726f2d8e71a3ce"),
    }
    with D.Telnet().connect() as t:
        for n, (name, want) in known.items():
            dev = f"/dev/mtdblock{n[-1]}"
            stage = f"/root/rec/a1/.bak_{n}.bin"
            t.exec(f"dd if={dev} of={stage} bs=64k 2>/dev/null; md5sum {stage}", timeout=180)
            got = t.exec(f"md5sum {stage}", timeout=180).split()[0]
            dst = os.path.join(out, name)
            D.pull(stage, dst)
            t.exec(f"rm -f {stage}")
            local = D.md5_file(dst)
            tag = "== committed backup" if local == want else "!! DIFFERS from the committed backup"
            print(f"  {n} -> {dst}  md5 {local}  (device {got})  {tag}")
    return 0


def cmd_deploy(_args) -> int:
    """Push the current SATA payload (our binary + launcher + config) to /root/rec/a1."""
    src = os.path.join(REPO, "flash", "out", "sata")
    items = [("boot.sh", "755"), ("dvr", "755"), ("dvr.conf", "644")]
    if not os.path.isdir(src):
        print(f"{src} missing — run flash/build.sh first"); return 1
    with D.Telnet().connect() as t:
        for name, mode in items:
            p = os.path.join(src, name)
            if not os.path.exists(p):
                print(f"  skip {name} (not built)"); continue
            D.push(p, f"/root/rec/a1/{name}", mode, tn=t)
            print(f"  {name} -> /root/rec/a1/{name}  md5 {D.md5_file(p)}")
        for name in ("app.tgz", "data.tgz"):
            p = os.path.join(src, "stock", name)
            if os.path.exists(p):
                D.push(p, f"/root/rec/a1/stock/{name}", "644", tn=t)
                print(f"  stock/{name} staged")
    return 0


# ----------------------------------------------------------------------- flashing
def quiesce(t: "D.Telnet") -> None:
    """Make the board safe to write flash on.

    The 10 s hardware watchdog is the real hazard: if whoever holds /dev/watchdog stops
    petting it mid-erase, the board resets with a half-written rootfs. Stock `app.out`
    owns it and only lets go via its serial debug command `closewd`; our own `dvr` writes
    the magic 'V' on exit. So: tell app.out to drop the watchdog over the serial console,
    let it exit cleanly, then clean up whatever is left over telnet.
    """
    # our DVR (if running) exits its wrapper loop and disarms the watchdog itself
    t.exec("touch /root/stop; sleep 1; killall dvr 2>/dev/null; echo ok", timeout=30)

    running = t.exec("ps | grep -c '[a]pp.out'").strip()
    if running not in ("", "0"):
        try:
            with D.Serial() as s:
                s.ser.reset_input_buffer()
                s.send("")                      # wake the [user]# debug prompt
                time.sleep(0.6)
                s.send("closewd")               # disarm the hardware watchdog
                time.sleep(1.2)
                out = s.read(1.0)
                s.send("exit")                  # app.out releases MPP and quits
                time.sleep(3.0)
                out += s.read(1.5)
                print("  serial: closewd + exit"
                      + (" (saw a response)" if "closewd" in out or "#" in out else ""))
        except Exception as e:
            print(f"  serial closewd failed ({e}) — falling back to kill")

    # mydaemon respawns app.out, so it goes first
    t.exec("killall mydaemon.out 2>/dev/null; sleep 1; killall app.out 2>/dev/null; "
           "sleep 2; sync; echo stopped", timeout=60)
    left = t.exec("ps | grep -E '[a]pp.out|[m]ydaemon|[/]dvr' | wc -l").strip()
    wd = t.exec("ls -l /proc/*/fd 2>/dev/null | grep -c watchdog || echo 0").strip()
    print(f"  processes left: {left}   /dev/watchdog holders: {wd}")


def _check_image(path: str) -> bytes:
    data = open(path, "rb").read()
    if len(data) != PARTSZ:
        raise SystemExit(f"REFUSING: {path} is {len(data)} bytes, mtd2 is exactly {PARTSZ}")
    if data[:4] != b"\x27\x05\x19\x56":
        raise SystemExit(f"REFUSING: {path} has no uImage magic")
    return data


def do_flash(image: str, reboot: bool = False, note: str = "", force: bool = False) -> int:
    data = _check_image(image)
    want = D.md5_file(image)
    remote = "/root/rec/a1/" + os.path.basename(image)

    _hdr(f"flashing mtd2 {note}")
    print(f"  image  {image}")
    print(f"  size   {len(data)}   md5 {want}")

    t = D.Telnet().connect()
    try:
        cur = t.exec("dd if=/dev/mtdblock2 bs=64k 2>/dev/null | md5sum", timeout=240).split()[0]
        print(f"  mtd2 currently: {cur}")
        if cur == want and not force:
            print("  the flash ALREADY holds this image — nothing to do (--force to write anyway)")
            if reboot:
                _reboot(t)
            return 0
        if cur == want and force:
            # Deliberately rewriting identical content: this is how you rehearse the
            # erase+write+verify+boot loop with an image you already know boots.
            print("  --force: rewriting identical content to exercise the erase/write path")

        # 1. keep an undo copy on SATA, on the device, before anything is erased
        _hdr("1/5 back up the current mtd2 to SATA")
        bak = f"/root/rec/a1/mtd2_before_{time.strftime('%H%M%S')}.bin"
        t.exec(f"dd if=/dev/mtdblock2 of={bak} bs=64k 2>/dev/null", timeout=240)
        bsz = t.exec(f"wc -c < {bak}").strip()
        bmd5 = t.exec(f"md5sum {bak}", timeout=240).split()[0]
        print(f"  {bak}  {bsz} bytes  md5 {bmd5}")
        if bsz != str(PARTSZ):
            raise SystemExit("backup is the wrong size — aborting before any write")

        # 2. get the new image onto the device
        _hdr("2/5 upload the new image")
        exists = t.exec(f"[ -f {remote} ] && md5sum {remote} || echo none", timeout=240).split()[0]
        if exists == want:
            print(f"  {remote} already present and matching")
        else:
            D.push(image, remote, "644", tn=t)
            got = t.exec(f"md5sum {remote}", timeout=240).split()[0]
            if got != want:
                raise SystemExit(f"upload verify failed ({got})")
            print(f"  {remote} md5 {got}")

        # 3. quiesce: nothing may reset the board or touch the disk while we write
        _hdr("3/5 disarm the watchdog and stop everything")
        quiesce(t)

        # 4. write
        _hdr("4/5 WRITE — do not power off")
        out = t.exec(f"dd if={remote} of=/dev/mtdblock2 bs=64k 2>&1; sync; echo rc=$?", timeout=600)
        print("  " + out.replace("\n", "\n  "))

        # 5. read the flash back and compare BEFORE any reboot
        _hdr("5/5 verify the flash")
        time.sleep(2)
        got = t.exec("dd if=/dev/mtdblock2 bs=64k 2>/dev/null | md5sum", timeout=300).split()[0]
        print(f"  flash md5 {got}\n  want      {want}")
        if got != want:
            print("\n  !! MISMATCH — DO NOT REBOOT.")
            print(f"  !! Re-run, or write the backup back:  py -3 tools/dvr.py flashback {bak}")
            return 1
        print("\n  FLASH VERIFIED")
        t.exec("rm -f /root/stop; echo ok")
        if reboot:
            _reboot(t)
        else:
            print("  (not rebooting; add --reboot, or reboot by hand when ready)")
        return 0
    finally:
        t.close()


def _reboot(t: D.Telnet) -> None:
    _hdr("rebooting")
    try:
        t.exec("rm -f /root/stop; (sleep 1; reboot) &  echo going-down", timeout=15)
    except Exception:
        pass
    t.close()
    time.sleep(8)


def cmd_flash(args) -> int:
    if not args:
        print("flash <image.bin> [--reboot] [--force]"); return 2
    return do_flash(args[0], "--reboot" in args, force="--force" in args)


def cmd_restore(args) -> int:
    return do_flash(STOCK_MTD2, "--reboot" in args, force="--force" in args,
                    note="(STOCK firmware — the return path)")


def cmd_flashback(args) -> int:
    """Write a device-resident backup image straight back into mtd2 (recovery)."""
    src = args[0] if args else "/root/rec/a1/mtd2_before_flash.bin"
    with D.Telnet().connect() as t:
        print(t.exec(f"dd if={src} of=/dev/mtdblock2 bs=64k 2>&1; sync", timeout=600))
        print("flash md5", t.exec("dd if=/dev/mtdblock2 bs=64k 2>/dev/null | md5sum", timeout=300))
    return 0


LOGO_BLOCK = 2                 # 64 KiB erase block index in mtd0 -> flash offset 0x20000
ERASE = 64 * 1024

# --------------------------------------------------------------------------- mtd0
# mtd0 is the partition that can permanently brick this board: block 0-1 are U-Boot
# itself, and there is no second bootloader, no ROM recovery and no JTAG wired out. If
# U-Boot dies the only way back is a chip programmer. So every mtd0 operation in this
# file is block-scoped, fingerprinted before and after, and refuses anything it does not
# recognise. Map (verified against dump/mtd0_boot.bin, 16 x 64 KiB):
#
#   blk 0-1   0x00000  U-Boot 2011.06 code + jpeg decoder + the custom vo/gx commands
#   blk 2     0x20000  boot splash JPEG   <- the ONLY block we ever write
#   blk 3     0x30000  zero filler
#   blk 4     0x40000  U-Boot environment (bootcmd/bootargs/ipaddr...) - `saveenv` target
#   blk 5     0x50000  zero filler
#   blk 6     0x60000  26 575 B high-entropy blob; `bootcmd` stages 0x80000 of it to
#                      0xc1b00000 for app.out. Unidentified — do not touch.
#   blk 7-13  0x70000  erased (0xFF)
#   blk 14-15 0xe0000  device settings, two copies (zlib at +0x100, length at +0x000).
#                      app.out rewrote these on "save settings"; nothing writes them now.
MTD0_PRISTINE = {          # md5 of each block in the committed dump/mtd0_boot.bin
    0: "299a5421fed869e8b48388da60e2878e", 1: "eff0d8b4bdc9ee581912f29bbe6a47ea",
    2: "5b1b8b46e2c8d9b4ed5f94bb78eb875e", 3: "fcd6bcb56c1689fcef28b57c22475bad",
    4: "8ed93626f2df29ce28e37a4dcbe4cdcc", 5: "fcd6bcb56c1689fcef28b57c22475bad",
    6: "5ed0a167455d61e0c032876c546321a2", 7: "ecb99e6ffea7be1e5419350f725da86b",
    14: "8f2a9dc045a95c3a2b42ecff4bd49fca", 15: "8f2a9dc045a95c3a2b42ecff4bd49fca",
}
CRITICAL_BLOCKS = {0: "U-Boot code", 1: "U-Boot code", 4: "U-Boot environment",
                   6: "app.out staged blob"}
BLOCK_MD5S = ("for i in $(seq 0 15); do dd if=/dev/mtdblock0 bs=64k skip=$i count=1 "
              "2>/dev/null | md5sum | cut -d' ' -f1; done")


def _mtd0_blocks(t: "D.Telnet") -> list:
    b = t.exec(BLOCK_MD5S, timeout=300).split()
    if len(b) != 16:
        raise RuntimeError(f"could not fingerprint mtd0 (got {len(b)} blocks)")
    return b


def cmd_mtd0(args) -> int:
    """Inspect mtd0 and check the blocks that must never change.

        py -3 tools/dvr.py mtd0            show the block map and flag anything odd
        py -3 tools/dvr.py mtd0 --save F   also write the whole partition to F
    """
    with D.Telnet().connect() as t:
        blocks = _mtd0_blocks(t)
        print("blk  offset    md5                               state")
        bad = 0
        for i, m in enumerate(blocks):
            want = MTD0_PRISTINE.get(i if i < 7 else (7 if 7 <= i <= 13 else i))
            note = ""
            if i in CRITICAL_BLOCKS:
                if want and m == want:
                    note = f"OK  {CRITICAL_BLOCKS[i]} (matches pristine)"
                else:
                    note = f"!!  {CRITICAL_BLOCKS[i]} DIFFERS FROM PRISTINE"; bad += 1
            elif i == LOGO_BLOCK:
                note = "boot splash" + ("  (stock)" if m == MTD0_PRISTINE[2] else "  (ours)")
            elif i in (14, 15):
                note = "device settings" + ("" if m == want else "  (rewritten by app.out)")
            print(f" {i:2d}  0x{i * ERASE:06x}  {m}  {note}")
        if bad:
            print(f"\n!! {bad} critical block(s) differ from dump/mtd0_boot.bin.")
            print("   If you did not expect that, DO NOT reboot — restore from a backup.")
        else:
            print("\nAll U-Boot / environment blocks match the committed pristine backup.")
        if "--save" in args:
            dst = args[args.index("--save") + 1]
            stage = "/root/rec/a1/.mtd0_save.bin"
            t.exec(f"dd if=/dev/mtdblock0 of={stage} bs=64k 2>/dev/null", timeout=240)
            D.pull(stage, dst)
            t.exec(f"rm -f {stage}")
            print(f"saved -> {dst}  md5 {D.md5_file(dst)}")
    return 0 if not bad else 1


def cmd_logo(args) -> int:
    """Replace the boot splash U-Boot shows while the DVR starts.

        py -3 tools/dvr.py logo splash.jpg

    The image lives in mtd0 at 0x20000 (U-Boot env `jpeg_addr=0x80020000`,
    `jpeg_size=0x20000`; `bootcmd` begins with `showlogo`). mtd0 also holds U-Boot ITSELF
    at 0x00000-0x1FFFF, its environment at 0x40000, and the device settings blob at
    0x0E0000 — so this writes exactly ONE erase block, at a seek offset, and verifies
    every other block is bit-identical afterwards. Losing U-Boot is the one failure this
    project cannot recover from without a hardware programmer.
    """
    if not args:
        print("logo <image.jpg>   (make one with tools/mklogo.py)"); return 2
    src = args[0]
    jpg = open(src, "rb").read()
    if jpg[:3] != b"\xff\xd8\xff":
        print(f"{src} is not a JPEG"); return 2
    if len(jpg) > ERASE:
        print(f"{src} is {len(jpg)} bytes; must fit one {ERASE}-byte erase block"); return 2
    block = jpg + b"\x00" * (ERASE - len(jpg))       # stock pads with zeros, not 0xFF

    stage = os.path.join(os.environ.get("TEMP", "."), "logo_block.bin")
    open(stage, "wb").write(block)
    want = D.md5_file(stage)
    print(f"  {src}: {len(jpg)} bytes JPEG -> one {ERASE}-byte block, md5 {want}")

    t = D.Telnet().connect()
    try:
        _hdr("1/4 fingerprint every mtd0 block + back it up")
        before = _mtd0_blocks(t)
        print(f"  16 blocks; block {LOGO_BLOCK} is {before[LOGO_BLOCK]}")

        # Refuse unless the block we're about to erase really is the splash. If mtd0 ever
        # shifted under us, writing "the logo block" blind could land on U-Boot.
        head = t.exec(f"dd if=/dev/mtdblock0 bs=4k skip={LOGO_BLOCK * 16} count=1 "
                      "2>/dev/null | hexdump -C | head -1", timeout=60)
        if "ff d8 ff" not in head:
            print(f"  block {LOGO_BLOCK} does not start with a JPEG (got: {head.strip()[:60]})")
            print("  REFUSING to write — mtd0 is not laid out the way this tool expects.")
            return 1
        print("  block starts with a JPEG SOI, as expected")

        # and refuse outright if any block we must never touch already looks wrong
        for i, what in CRITICAL_BLOCKS.items():
            pristine = MTD0_PRISTINE.get(i)          # NB: not `want` — that holds the
            if pristine and before[i] != pristine:   # md5 of the block we're uploading
                print(f"  block {i} ({what}) differs from the pristine backup — "
                      "refusing to write mtd0 until that is understood")
                return 1
        print("  U-Boot + environment blocks match the pristine backup")
        bak = f"/root/rec/a1/mtd0_before_{time.strftime('%H%M%S')}.bin"
        t.exec(f"dd if=/dev/mtdblock0 of={bak} bs=64k 2>/dev/null", timeout=180)
        print(f"  full mtd0 saved to {bak} "
              f"({t.exec(f'md5sum {bak}', timeout=180).split()[0]})")

        _hdr("2/4 upload the new block")
        D.push(stage, "/root/rec/a1/logo_block.bin", "644", tn=t)
        got = t.exec("md5sum /root/rec/a1/logo_block.bin", timeout=120).split()[0]
        if got != want:
            print(f"  upload verify failed ({got})"); return 1
        print(f"  ok {got}")

        _hdr("3/4 disarm the watchdog and write ONE block")
        quiesce(t)
        out = t.exec(f"dd if=/root/rec/a1/logo_block.bin of=/dev/mtdblock0 bs=64k "
                     f"seek={LOGO_BLOCK} 2>&1; sync; echo rc=$?", timeout=300)
        print("  " + out.replace("\n", "\n  "))

        _hdr("4/4 verify — the logo block changed and NOTHING else did")
        time.sleep(2)
        after = _mtd0_blocks(t)
        ok = True
        for i, (b, a) in enumerate(zip(before, after)):
            if i == LOGO_BLOCK:
                if a != want:
                    print(f"  block {i}: got {a}, wanted {want}  FAIL"); ok = False
                else:
                    print(f"  block {i}: {b} -> {a}  (the logo, as intended)")
            elif a != b:
                print(f"  block {i}: CHANGED {b} -> {a}  !! UNEXPECTED"); ok = False
        # quiesce() stops the DVR via /root/stop; clear it and let boot.sh bring it back,
        # otherwise the box sits with no DVR until someone notices (it did once).
        t.exec("rm -f /root/stop; echo ok", timeout=30)
        if ok:
            print("\n  LOGO WRITTEN — U-Boot's own blocks are untouched.")
            print("  DVR restarting (boot.sh respawns it); reboot to see the new splash.")
        else:
            print(f"\n  !! verification failed — restore with:  dd if={bak} of=/dev/mtdblock0 bs=64k")
        return 0 if ok else 1
    finally:
        t.close()


MTD2_FLASH_ADDR = 0x80300000
MTD2_FLASH_END = 0x807FFFFF
RAM_STAGE = 0xC1500000


def cmd_recover(args) -> int:
    """LAST RESORT: restore mtd2 from the bootloader, for when Linux won't boot at all.

        py -3 tools/dvr.py recover --test      load the image into RAM only (safe rehearsal)
        py -3 tools/dvr.py recover [image]     load it into RAM and write it to flash

    U-Boot 2011.06 here has no loadb/loady, so TFTP is the ONLY way to get an image in.
    That needs inbound UDP 69 open on this PC, which is firewall-blocked by default:

        netsh advfirewall firewall add rule name="DVR TFTP" dir=in action=allow ^
              protocol=UDP localport=69

    Power-cycle the DVR when prompted — bootdelay is 1 s, so we have to be hammering
    CTRL-C as it comes up.
    """
    import shutil
    import subprocess

    test = "--test" in args
    rest = [a for a in args if not a.startswith("--")]
    image = rest[0] if rest else STOCK_MTD2
    if not os.path.exists(image):
        print(f"no such image: {image}"); return 2
    _check_image(image)

    # serve the image from a scratch dir so TFTP can't reach anything else
    root = os.path.join(os.environ.get("TEMP", "."), "dvr_tftp")
    os.makedirs(root, exist_ok=True)
    name = os.path.basename(image)
    shutil.copy2(image, os.path.join(root, name))
    pc_ip = _my_lan_ip()
    print(f"serving {name} from {root} as {pc_ip}")
    srv = subprocess.Popen([sys.executable, os.path.join(REPO, "tftp_server.py"), root, "69"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(2)
    try:
        s = D.Serial()
        u = D.UBoot(s)
        print("\n>>> POWER-CYCLE THE DVR NOW <<<  (hammering CTRL-C for 120 s)")
        if not u.interrupt(timeout=120):
            print("never reached the U-Boot prompt"); return 1
        print("at the U-Boot prompt")
        u.cmd(f"setenv ipaddr 192.168.1.116")
        u.cmd(f"setenv serverip {pc_ip}")
        out = u.cmd(f"tftp 0x{RAM_STAGE:08x} {name}", timeout=240)
        print(out[-800:])
        if "Bytes transferred" not in out:
            print("\nTFTP FAILED — is inbound UDP 69 allowed on this PC? (see the help text)")
            print("Linux is still bootable from flash; bringing it back up.")
            u.boot_linux()
            return 1
        print(u.cmd(f"crc32 0x{RAM_STAGE:08x} 0x{5 * 1024 * 1024:x}", timeout=60))
        if test:
            print("--test: image is in RAM and intact; NOT writing flash. Booting Linux.")
            u.boot_linux()
            return 0
        print(u.cmd(f"protect off 0x{MTD2_FLASH_ADDR:08x} 0x{MTD2_FLASH_END:08x}", timeout=60))
        print(u.cmd(f"erase 0x{MTD2_FLASH_ADDR:08x} 0x{MTD2_FLASH_END:08x}", timeout=300))
        print(u.cmd(f"cp.b 0x{RAM_STAGE:08x} 0x{MTD2_FLASH_ADDR:08x} 0x{5 * 1024 * 1024:x}", timeout=300))
        cmp_out = u.cmd(f"cmp.b 0x{RAM_STAGE:08x} 0x{MTD2_FLASH_ADDR:08x} 0x{5 * 1024 * 1024:x}", timeout=300)
        print(cmp_out)
        ok = "differ" not in cmp_out.lower()
        print("FLASH RESTORED" if ok else "!! MISMATCH — do not reset, retry the write")
        if ok:
            u.cmd("reset", timeout=10)
        return 0 if ok else 1
    finally:
        try:
            srv.terminate()
        except Exception:
            pass


def _my_lan_ip() -> str:
    import socket as _s
    with _s.socket(_s.AF_INET, _s.SOCK_DGRAM) as k:
        k.connect((D.HOST, 9))
        return k.getsockname()[0]


CMDS = {
    "status": cmd_status, "shell": cmd_shell, "ctl": cmd_ctl, "push": cmd_push,
    "pull": cmd_pull, "screen": cmd_screen, "serial": cmd_serial, "wait": cmd_wait,
    "backup": cmd_backup, "deploy": cmd_deploy, "flash": cmd_flash,
    "restore": cmd_restore, "flashback": cmd_flashback, "recover": cmd_recover,
    "logo": cmd_logo, "mtd0": cmd_mtd0,
}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd = sys.argv[1]
    if cmd not in CMDS:
        print(f"unknown command {cmd!r}\n"); print(__doc__); return 2
    return CMDS[cmd](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
