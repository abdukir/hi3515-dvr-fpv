#!/usr/bin/env python3
"""
Minimal TFTP server for dumping / flashing the Hi3515 DVR.

- Supports WRQ (device -> PC uploads, i.e. dumping flash to us)
- Supports RRQ (PC -> device downloads, i.e. serving files to reflash later)
- octet mode + blksize/timeout/tsize option negotiation (OACK)
- Files are read from / written to the directory given on the command line
  (default: ./dump).  Uploaded files that already exist get a .1/.2 suffix so
  you never silently clobber a previous dump.

Usage:
    py -3 tftp_server.py            # serves ./dump on 0.0.0.0:69
    py -3 tftp_server.py mydir 6969 # custom dir + port

Run from an *elevated* shell if you use the default port 69 (privileged).
Make sure Windows Firewall allows inbound UDP on the chosen port.
"""
import os
import socket
import struct
import sys
import threading

OPCODE = {"RRQ": 1, "WRQ": 2, "DATA": 3, "ACK": 4, "ERROR": 5, "OACK": 6}


def log(msg):
    print(msg, flush=True)


def parse_request(data):
    # opcode(2) filename\0 mode\0 [opt\0 val\0]...
    opcode = struct.unpack("!H", data[:2])[0]
    parts = data[2:].split(b"\x00")
    filename = parts[0].decode("latin-1")
    mode = parts[1].decode("latin-1").lower() if len(parts) > 1 else "octet"
    opts = {}
    rest = parts[2:]
    for i in range(0, len(rest) - 1, 2):
        k = rest[i].decode("latin-1").lower()
        v = rest[i + 1].decode("latin-1")
        if k:
            opts[k] = v
    return opcode, filename, mode, opts


def send_error(sock, addr, code, msg):
    pkt = struct.pack("!HH", OPCODE["ERROR"], code) + msg.encode("latin-1") + b"\x00"
    sock.sendto(pkt, addr)


def uniquify(path):
    if not os.path.exists(path):
        return path
    base, ext = os.path.splitext(path)
    n = 1
    while os.path.exists(f"{base}.{n}{ext}"):
        n += 1
    return f"{base}.{n}{ext}"


def handle_wrq(root, filename, opts, addr):
    """Device is uploading a file to us (a flash dump)."""
    safe = os.path.basename(filename) or "upload.bin"
    dst = uniquify(os.path.join(root, safe))
    blksize = int(opts.get("blksize", 512))
    timeout = float(opts.get("timeout", 3))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(max(timeout, 3))
    log(f"[WRQ] {addr} -> {dst}  (blksize={blksize})")

    # Acknowledge negotiated options, else plain ACK block 0.
    ack_opts = {}
    if "blksize" in opts:
        ack_opts["blksize"] = str(blksize)
    if "timeout" in opts:
        ack_opts["timeout"] = str(int(timeout))
    if "tsize" in opts:
        ack_opts["tsize"] = opts["tsize"]

    if ack_opts:
        pkt = struct.pack("!H", OPCODE["OACK"])
        for k, v in ack_opts.items():
            pkt += k.encode() + b"\x00" + v.encode() + b"\x00"
        sock.sendto(pkt, addr)
    else:
        sock.sendto(struct.pack("!HH", OPCODE["ACK"], 0), addr)

    total = 0
    expected = 1
    with open(dst, "wb") as f:
        while True:
            try:
                data, raddr = sock.recvfrom(blksize + 4)
            except socket.timeout:
                log(f"[WRQ] timeout after {total} bytes -> {dst}")
                break
            op, block = struct.unpack("!HH", data[:4])
            if op != OPCODE["DATA"]:
                continue
            if block == expected:
                payload = data[4:]
                f.write(payload)
                total += len(payload)
                sock.sendto(struct.pack("!HH", OPCODE["ACK"], block & 0xFFFF), raddr)
                expected = (expected + 1) & 0xFFFF
                if len(payload) < blksize:
                    log(f"[WRQ] DONE  {total} bytes -> {dst}")
                    break
            else:
                # re-ack previous
                sock.sendto(struct.pack("!HH", OPCODE["ACK"], (expected - 1) & 0xFFFF), raddr)
    sock.close()


def handle_rrq(root, filename, opts, addr):
    """Device is downloading a file from us (to reflash / load)."""
    safe = os.path.basename(filename)
    src = os.path.join(root, safe)
    if not os.path.isfile(src):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        send_error(s, addr, 1, "File not found")
        s.close()
        log(f"[RRQ] {addr} asked for {safe} -> NOT FOUND")
        return

    blksize = int(opts.get("blksize", 512))
    timeout = float(opts.get("timeout", 3))
    fsize = os.path.getsize(src)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(max(timeout, 3))
    log(f"[RRQ] {addr} <- {src}  ({fsize} bytes, blksize={blksize})")

    ack_opts = {}
    if "blksize" in opts:
        ack_opts["blksize"] = str(blksize)
    if "tsize" in opts:
        ack_opts["tsize"] = str(fsize)
    if "timeout" in opts:
        ack_opts["timeout"] = str(int(timeout))

    if ack_opts:
        pkt = struct.pack("!H", OPCODE["OACK"])
        for k, v in ack_opts.items():
            pkt += k.encode() + b"\x00" + v.encode() + b"\x00"
        sock.sendto(pkt, addr)
        # wait for ACK 0
        try:
            data, addr = sock.recvfrom(516)
        except socket.timeout:
            log("[RRQ] no ACK for OACK, aborting")
            sock.close()
            return

    block = 1
    with open(src, "rb") as f:
        while True:
            chunk = f.read(blksize)
            pkt = struct.pack("!HH", OPCODE["DATA"], block & 0xFFFF) + chunk
            for _ in range(5):
                sock.sendto(pkt, addr)
                try:
                    data, addr = sock.recvfrom(516)
                    op, ackblk = struct.unpack("!HH", data[:4])
                    if op == OPCODE["ACK"] and ackblk == (block & 0xFFFF):
                        break
                except socket.timeout:
                    continue
            block = (block + 1) & 0xFFFF
            if len(chunk) < blksize:
                log(f"[RRQ] DONE  sent {src}")
                break
    sock.close()


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "dump"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 69
    os.makedirs(root, exist_ok=True)
    root = os.path.abspath(root)

    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    log(f"TFTP server listening on 0.0.0.0:{port}")
    log(f"Serving / storing files in: {root}")
    log("Waiting for the DVR to connect...  (Ctrl-C to stop)\n")

    while True:
        data, addr = srv.recvfrom(1024)
        opcode, filename, mode, opts = parse_request(data)
        if opcode == OPCODE["WRQ"]:
            threading.Thread(target=handle_wrq, args=(root, filename, opts, addr), daemon=True).start()
        elif opcode == OPCODE["RRQ"]:
            threading.Thread(target=handle_rrq, args=(root, filename, opts, addr), daemon=True).start()
        else:
            log(f"[?] unexpected opcode {opcode} from {addr}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped.")
