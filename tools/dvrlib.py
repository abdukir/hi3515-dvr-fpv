#!/usr/bin/env python3
"""dvrlib.py — one library for talking to the RR104P DVR.

Everything this project needs to reach the device, in one place, with the traps that
cost us hours already baked in as behaviour rather than folklore:

  * telnet exec is marker-framed and SINGLE-LINE — a heredoc wedges the session, and a
    timeout that returns partial output desyncs every later command, so a timeout is
    fatal here and the caller reconnects.
  * file upload stages NEXT TO the destination on SATA: `/` is a 12.5 MB ext2 RAM disk
    with ~1.9 MB free, so /tmp silently truncates anything bigger.
  * upload goes in 1 MB chunks: busybox 1.1.2 `nc` stops accepting data around 2.5 MB
    in a single connection, again silently.
  * the `nc -l` listener gets its OWN telnet session: a backgrounded nc inherits its
    shell's stdin and forwards the next command we type to the peer, which kills the
    transfer once we have half-closed the socket.
  * transfers are PC-connects-out because the PC firewall blocks inbound.

Transports: telnet (:23), our control protocol (:8090), the media stream (:8091),
busybox httpd (:8081), the serial console (COM6, 115200 8N1, auto-login as root), and
the HDMI capture card looking at the DVR's own VGA output.
"""
from __future__ import annotations

import hashlib
import os
import random
import re
import socket
import subprocess
import sys
import time

HOST = os.environ.get("DVR_HOST", "192.168.1.108")
CTL_PORT = int(os.environ.get("DVR_CTL_PORT", "8090"))
STREAM_PORT = int(os.environ.get("DVR_STREAM_PORT", "8091"))
FILE_PORT = int(os.environ.get("DVR_FILE_PORT", "8081"))
SERIAL_PORT = os.environ.get("DVR_SERIAL", "COM6")
SERIAL_BAUD = int(os.environ.get("DVR_SERIAL_BAUD", "115200"))
CAPTURE_DEV = os.environ.get("DVR_CAPTURE", "USB Video")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FFMPEG = os.environ.get("FFMPEG", os.path.join(REPO, "webapp", "bin", "ffmpeg.exe"))


def md5_file(path: str) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------- telnet
def _strip_iac(buf: bytes, sock: socket.socket) -> bytes:
    """Answer telnet option negotiation with a flat refusal, return the payload."""
    out, i = bytearray(), 0
    while i < len(buf):
        if buf[i] == 0xFF and i + 2 < len(buf):
            cmd, opt = buf[i + 1], buf[i + 2]
            if cmd == 0xFD:                       # DO   -> WONT
                sock.sendall(bytes([0xFF, 0xFC, opt]))
            elif cmd == 0xFB:                     # WILL -> DONT
                sock.sendall(bytes([0xFF, 0xFE, opt]))
            i += 3
        else:
            out.append(buf[i])
            i += 1
    return bytes(out)


class Telnet:
    """A logged-in busybox shell. exec() runs ONE line and returns its output."""

    def __init__(self, host: str = HOST, user: str = "root", password: str = ""):
        self.host, self.user, self.password = host, user, password
        self.sock: socket.socket | None = None
        self.dead = False

    def connect(self, timeout: float = 15.0) -> "Telnet":
        s = socket.create_connection((self.host, 23), timeout=timeout)
        s.settimeout(0.4)
        self.sock = s
        stage, acc, deadline = "login", b"", time.time() + timeout
        while time.time() < deadline:
            try:
                d = s.recv(4096)
            except socket.timeout:
                d = b""
            if d:
                acc += _strip_iac(d, s)
            txt = acc.decode("latin-1")
            if stage == "login" and "ogin:" in txt:
                s.sendall(self.user.encode() + b"\n"); stage, acc = "pass", b""
            elif stage == "pass" and "assword:" in txt:
                s.sendall(self.password.encode() + b"\n"); stage, acc = "wait", b""
            elif stage in ("pass", "wait") and re.search(r"[#$]\s*$", txt):
                # wide terminal (no 80-col wrap), no echo, empty prompt
                s.sendall(b'stty -echo cols 1000 rows 60 2>/dev/null; PS1=""; export PS1\n')
                time.sleep(0.4)
                try:
                    s.recv(65536)
                except socket.timeout:
                    pass
                return self
        raise RuntimeError("telnet login timed out")

    def exec(self, cmd: str, timeout: float = 20.0) -> str:
        if self.sock is None or self.dead:
            raise RuntimeError("telnet session unusable")
        r = random.randrange(10 ** 9)
        start, end = f"S{r}S", f"E{r}E"
        # quote-split in the source so the echoed command can never contain the marker
        line = f"echo 'S'{r}'S'; {cmd}; echo 'E'{r}'E'\n"
        self.sock.sendall(line.encode("latin-1"))
        acc, deadline = b"", time.time() + timeout
        while time.time() < deadline:
            try:
                d = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            acc += _strip_iac(d, self.sock)
            txt = acc.decode("latin-1")
            i = txt.find(start)
            j = txt.find(end, i + len(start)) if i >= 0 else -1
            if i >= 0 and j >= 0:
                return txt[i + len(start):j].replace("\r", "").strip("\n")
        # Never return partial output: the late reply would be read as the NEXT command's
        # answer and every later exec() would be off by one, silently and permanently.
        self.dead = True
        self.close()
        raise TimeoutError(f"telnet exec timed out: {cmd[:60]}")

    def close(self) -> None:
        if self.sock:
            # Ask the remote shell to exit first. Just dropping the socket leaves a `-sh`
            # behind on the device every time; they pile up on a 43 MB box and eventually
            # make new sessions (and the `nc -l` transfers that need them) flaky.
            try:
                self.sock.sendall(b"exit\n")
                time.sleep(0.15)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def __enter__(self): return self
    def __exit__(self, *a): self.close()


# ----------------------------------------------------------------------- file upload
CHUNK = 1 << 20


def push(local: str, remote: str, mode: str = "644", progress=True, tn: Telnet | None = None) -> None:
    """Copy a local file to the device, md5-verified. See the module docstring for why
    this is chunked, staged on SATA, and uses a second telnet session."""
    data = open(local, "rb").read()
    want = hashlib.md5(data).hexdigest()
    rdir = remote.rsplit("/", 1)[0] or "/tmp"
    port0 = random.randrange(9000, 9400)
    tmp = f"{rdir}/.up.{port0}"

    own = tn is None
    ctl = tn or Telnet().connect()
    lst = Telnet().connect()                    # sacrificial: owns `nc -l`
    try:
        ctl.exec(f"mkdir -p {rdir} 2>/dev/null; rm -f {tmp}")
        sent = 0
        for n, off in enumerate(range(0, len(data), CHUNK)):
            piece = data[off:off + CHUNK]
            port = port0 + n
            # `nc -l` occasionally isn't accepting yet when we dial (busybox is slow to
            # get to accept() on a loaded box), so retry the connect on a fresh listener
            # rather than failing the whole transfer.
            sock = None
            for attempt in range(4):
                _r = lst.exec(f"rm -f {tmp}.part; (nc -l -p {port} > {tmp}.part &); sleep 1; netstat -ln | grep -c \":{port} \"")
                time.sleep(0.3)
                try:
                    sock = socket.create_connection((ctl.host, port), timeout=30)
                    break
                except OSError as e:
                    if os.environ.get("DVR_DEBUG"):
                        print(f"    [push] port {port}: listener said {_r!r}, connect: {e}",
                              file=sys.stderr)
                    port += 100                      # a stuck listener keeps the old port
                    time.sleep(1.0)
            if sock is None:
                raise RuntimeError(f"could not reach the device's nc listener for chunk {n}")
            with sock as s:
                s.sendall(piece)
                s.shutdown(socket.SHUT_WR)
                s.settimeout(5)
                try:
                    s.recv(1)
                except OSError:
                    pass
            got = -1
            for _ in range(40):
                time.sleep(0.4)
                out = ctl.exec(f"wc -c < {tmp}.part 2>/dev/null").strip()
                got = int(out) if out.isdigit() else -1
                if got == len(piece):
                    break
            if got != len(piece):
                raise RuntimeError(f"chunk {n}: device got {got} of {len(piece)} bytes")
            ctl.exec(f"cat {tmp}.part >> {tmp}; rm -f {tmp}.part", timeout=60)
            sent += len(piece)
            if progress and len(data) > CHUNK:
                pct = 100 * sent // len(data)
                print(f"\r  upload {sent}/{len(data)} ({pct}%)", end="", file=sys.stderr, flush=True)
        if progress and len(data) > CHUNK:
            print(file=sys.stderr)
        got_md5 = ctl.exec(f"md5sum {tmp}", timeout=180).split()[0]
        if got_md5 != want:
            raise RuntimeError(f"md5 mismatch: local {want} device {got_md5}")
        ctl.exec(f"mv -f {tmp} {remote}; chmod {mode} {remote}", timeout=60)
    finally:
        lst.close()
        if own:
            ctl.close()


def pull(remote: str, local: str) -> int:
    """Fetch a device file over the busybox httpd the backend starts on :8081."""
    import urllib.request
    with Telnet().connect() as t:
        t.exec(f"pidof httpd >/dev/null || httpd -h / -p {FILE_PORT}; echo ok")
    url = f"http://{HOST}:{FILE_PORT}{remote}"
    with urllib.request.urlopen(url, timeout=120) as r, open(local, "wb") as f:
        n = 0
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b); n += len(b)
    return n


# --------------------------------------------------------------- control protocol
def ctl(*cmds: str, timeout: float = 8.0, host: str = HOST, port: int = CTL_PORT,
        strict: bool = False) -> list[str]:
    """Send commands to the DVR control port and return the reply lines.

    Returns [] when the port isn't answering (the stock firmware doesn't have one), so
    callers can treat "no control plane" as a state rather than an exception. Pass
    strict=True if you'd rather see the connection error."""
    out: list[str] = []
    try:
        s = socket.create_connection((host, port), timeout=timeout)
    except OSError:
        if strict:
            raise
        return []
    with s:
        s.settimeout(timeout)
        buf = ""
        for c in cmds:
            s.sendall((c + "\n").encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                d = s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d.decode("latin-1")
            if buf.rstrip().endswith(("END", "OK", "ERR")) and len(cmds) == 1:
                break
        out = [l.strip() for l in buf.splitlines() if l.strip() and l.strip() != "DVR READY"]
    return out


def info() -> dict:
    """Parse INFO into a dict, or {} if the device isn't answering."""
    for line in ctl("INFO"):
        if line.startswith("INFO "):
            d = {}
            for kv in line[5:].split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    d[k] = v
            return d
    return {}


def alive(timeout: float = 2.0) -> bool:
    try:
        with socket.create_connection((HOST, CTL_PORT), timeout=timeout):
            return True
    except OSError:
        return False


def wait_for(what: str = "ctl", timeout: float = 180.0, quiet: bool = False) -> bool:
    """Block until the device answers. what: ctl | telnet | ping."""
    port = {"ctl": CTL_PORT, "telnet": 23, "http": FILE_PORT}.get(what, CTL_PORT)
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection((HOST, port), timeout=2):
                if not quiet:
                    print(f"  {what}:{port} up after {time.time() - t0:.0f}s")
                return True
        except OSError:
            time.sleep(2)
    return False


# ------------------------------------------------------------------------- serial
class Serial:
    """The console on ttyS000. getty auto-logs in as root, so this is a root shell
    that works with no network at all — and the only way into U-Boot."""

    def __init__(self, port: str = SERIAL_PORT, baud: int = SERIAL_BAUD):
        import serial as pyserial          # imported lazily so telnet-only use needs no pyserial
        self.ser = pyserial.Serial(port, baud, timeout=0.2)
        self.log = bytearray()

    def read(self, seconds: float = 2.0) -> str:
        t0 = time.time()
        while time.time() - t0 < seconds:
            d = self.ser.read(8192)
            if d:
                self.log += d
        return bytes(self.log).decode("latin-1", "replace")

    def send(self, text: str, newline: bool = True) -> None:
        self.ser.write(text.encode("latin-1") + (b"\n" if newline else b""))
        self.ser.flush()

    def expect(self, pattern: str, timeout: float = 30.0) -> str | None:
        """Read until a regex matches; returns everything read, or None on timeout."""
        rx = re.compile(pattern)
        t0, acc = time.time(), ""
        while time.time() - t0 < timeout:
            d = self.ser.read(8192)
            if d:
                self.log += d
                acc += d.decode("latin-1", "replace")
                if rx.search(acc):
                    return acc
            else:
                time.sleep(0.05)
        return None

    def interrupt_uboot(self, timeout: float = 90.0) -> bool:
        """Hold CTRL-C down through a power-up to land at the U-Boot prompt.
        bootdelay is 1 s, so this only works across a reboot/power cycle."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.ser.write(b"\x03")
            d = self.ser.read(4096)
            if d:
                self.log += d
                if re.search(rb"(hisilicon|hi3515|=>|# )\s*$", d, re.I):
                    return True
            time.sleep(0.05)
        return False

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self): return self
    def __exit__(self, *a): self.close()


UBOOT_PROMPT = r"hilinux\s*#\s*$"


class UBoot:
    """Drive the bootloader over the serial console — the recovery path of last resort.

    This U-Boot 2011.06 is stripped: there is no `boot` and no `run`, so to start Linux
    you replay `bootcmd` by hand. It also drops UART input during long `cp.b` copies, so
    every command is sent alone and waited on."""

    def __init__(self, ser: "Serial"):
        self.s = ser

    def interrupt(self, timeout: float = 90.0) -> bool:
        """Hammer CTRL-C through a reboot until the prompt appears. bootdelay is 1 s, so
        you must already be hammering when the board comes up — call this and THEN cause
        the reset, or run it across a power cycle."""
        t0, acc = time.time(), b""
        while time.time() - t0 < timeout:
            self.s.ser.write(b"\x03")
            d = self.s.ser.read(4096)
            if d:
                acc += d
                self.s.log += d
                if re.search(UBOOT_PROMPT, acc[-200:].decode("latin-1", "replace")):
                    return True
            time.sleep(0.02)
        return False

    def cmd(self, line: str, timeout: float = 120.0) -> str:
        """Send one command, return everything up to the next prompt."""
        self.s.ser.reset_input_buffer()
        self.s.ser.write(line.encode("latin-1") + b"\n")
        t0, acc = time.time(), ""
        while time.time() - t0 < timeout:
            d = self.s.ser.read(8192)
            if d:
                self.s.log += d
                acc += d.decode("latin-1", "replace")
                if re.search(UBOOT_PROMPT, acc[-120:]):
                    break
            else:
                time.sleep(0.05)
        return acc

    def boot_linux(self, verbose: bool = True) -> None:
        """Replay the stock bootcmd one command at a time (no `run` on this build).

        bootcmd=showlogo; cp.b 0x80300000 0xc1500000 0x500000    rootfs  mtd2 -> RAM
                          cp.b 0x80200000 0xc1a00000 0x100000    (2nd half of mtd1)
                          cp.b 0x80060000 0xc1b00000 0x80000     logo/config from mtd0
                          bootm 0x80100000 0xc1500000            kernel + ramdisk
        """
        for c in ("cp.b 0x80300000 0xc1500000 0x500000",
                  "cp.b 0x80200000 0xc1a00000 0x100000",
                  "cp.b 0x80060000 0xc1b00000 0x80000"):
            out = self.cmd(c, timeout=180)
            if verbose:
                print(f"  {c} -> {out.strip().splitlines()[-1][:70] if out.strip() else 'ok'}")
        self.s.ser.write(b"bootm 0x80100000 0xc1500000\n")
        if verbose:
            print("  bootm issued — Linux is starting")


# ------------------------------------------------------------------- screen capture
def screen(out_png: str, device: str = CAPTURE_DEV, warmup: int = 8) -> str:
    """Grab the DVR's own VGA output through the HDMI capture card.

    The MS2109 needs a few frames before it produces a stable picture, so drop `warmup`
    frames rather than taking the first one (which is usually green or half-torn)."""
    if not os.path.exists(FFMPEG):
        raise RuntimeError(f"ffmpeg not found at {FFMPEG} (see SETUP.md)")
    cmd = [FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
           "-f", "dshow", "-i", f"video={device}",
           "-frames:v", "1", "-vf", f"select=gte(n\\,{warmup})", "-vsync", "0",
           "-update", "1", out_png]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    if not os.path.exists(out_png) or os.path.getsize(out_png) < 1000:
        raise RuntimeError("capture failed: " + (r.stderr or "")[-300:])
    return out_png
