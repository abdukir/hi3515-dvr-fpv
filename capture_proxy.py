#!/usr/bin/env python3
"""
Logging TCP proxy to capture the DVR's 8670 control + media protocol.

Point your ActiveX/CMS client at THIS PC (192.168.1.74) instead of the DVR.
The proxy forwards every connection to the real DVR and logs both directions
(hex + ascii) to capture/ so we can decode the TLV wire format.

It listens on several ports and forwards each to the same port on the DVR:
  8670  -> control (UDServerPort)
  and a few likely media ports; add more with --ports once we see the real one.

Usage:
    py -3 capture_proxy.py                 # DVR=192.168.1.108, ports 8670,8000-8010
    py -3 capture_proxy.py 192.168.1.108 8670,5000,6000
"""
import os
import socket
import sys
import threading
import time

DVR = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.108"
if len(sys.argv) > 2:
    PORTS = [int(p) for p in sys.argv[2].split(",")]
else:
    PORTS = [8670] + list(range(8000, 8011)) + [5000, 6000, 34567]

CAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "capture")
os.makedirs(CAP, exist_ok=True)
_lock = threading.Lock()
_conn = 0


def hexdump(b):
    lines = []
    for i in range(0, len(b), 16):
        chunk = b[i:i + 16]
        hexs = " ".join(f"{x:02x}" for x in chunk)
        asci = "".join(chr(x) if 32 <= x < 127 else "." for x in chunk)
        lines.append(f"  {i:06x}  {hexs:<48}  {asci}")
    return "\n".join(lines)


def pump(src, dst, logf, tag, cid):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            with _lock:
                with open(logf, "a", encoding="utf-8") as f:
                    f.write(f"\n--- {tag} conn#{cid} len={len(data)} ---\n")
                    f.write(hexdump(data) + "\n")
            dst.sendall(data)
    except Exception:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except Exception:
            pass


def handle(client, addr, port):
    global _conn
    with _lock:
        _conn += 1
        cid = _conn
    logf = os.path.join(CAP, f"port{port}_conn{cid}.log")
    print(f"[proxy] conn#{cid} {addr} -> DVR:{port}  logging to {os.path.basename(logf)}", flush=True)
    try:
        upstream = socket.create_connection((DVR, port), timeout=5)
    except Exception as e:
        print(f"[proxy] conn#{cid} upstream connect {DVR}:{port} failed: {e}", flush=True)
        client.close()
        return
    t1 = threading.Thread(target=pump, args=(client, upstream, logf, "C->S", cid), daemon=True)
    t2 = threading.Thread(target=pump, args=(upstream, client, logf, "S->C", cid), daemon=True)
    t1.start(); t2.start()
    t1.join(); t2.join()
    client.close(); upstream.close()
    print(f"[proxy] conn#{cid} closed", flush=True)


def listen_on(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("0.0.0.0", port))
    except Exception as e:
        print(f"[proxy] cannot bind {port}: {e}", flush=True)
        return
    s.listen(8)
    print(f"[proxy] listening on 0.0.0.0:{port} -> {DVR}:{port}", flush=True)
    while True:
        c, a = s.accept()
        threading.Thread(target=handle, args=(c, a, port), daemon=True).start()


def main():
    print(f"[proxy] DVR={DVR}  ports={PORTS}")
    print(f"[proxy] point your client at THIS PC. Captures -> {CAP}\n")
    for p in PORTS:
        threading.Thread(target=listen_on, args=(p,), daemon=True).start()
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped.")
