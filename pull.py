#!/usr/bin/env python3
"""Connect OUT to the DVR's nc listener and save the stream.

Usage: py -3 pull.py <host> <port> <outfile>
"""
import hashlib
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
outfile = sys.argv[3]

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(15)
t0 = time.time()
s.connect((host, port))
total = 0
h = hashlib.md5()
with open(outfile, "wb") as f:
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            print("[pull] socket timeout (assuming end)")
            break
        if not chunk:
            break
        f.write(chunk)
        h.update(chunk)
        total += len(chunk)
s.close()
dt = time.time() - t0
print(f"[pull] {outfile}: {total} bytes in {dt:.1f}s  md5={h.hexdigest()}")
