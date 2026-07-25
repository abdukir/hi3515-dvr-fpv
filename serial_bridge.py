#!/usr/bin/env python3
"""
Persistent serial bridge for the Hi3515 DVR on COM21 @ 115200 8N1.

- Continuously reads the serial port and appends everything to serial.log
- Watches cmds.txt (append-only) and sends any new lines to the device.

Command file protocol (one directive per line, appended to cmds.txt):
    <text>          -> send text + "\n"          (a normal shell/U-Boot command)
    RAW:<text>      -> send text with NO newline
    HEX:03          -> send raw bytes from hex   (e.g. 03 = Ctrl-C, 18 = Ctrl-X)
    ENTER           -> send a bare "\n"
    #<anything>     -> comment, ignored

We deliberately do NOT toggle DTR/RTS so we never reset the board.
"""
import binascii
import os
import sys
import threading
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM21"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "serial.log")
CMDS = os.path.join(HERE, "cmds.txt")


def open_port():
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.bytesize = serial.EIGHTBITS
    ser.parity = serial.PARITY_NONE
    ser.stopbits = serial.STOPBITS_ONE
    ser.timeout = 0.05
    ser.write_timeout = 3
    # Do not drive DTR/RTS -> avoid resetting the target.
    ser.dsrdtr = False
    ser.rtscts = False
    ser.xonxoff = False
    ser.open()
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception:
        pass
    return ser


def reader(ser, stop):
    with open(LOG, "ab", buffering=0) as f:
        while not stop.is_set():
            try:
                data = ser.read(4096)
            except Exception as e:
                f.write(f"\n[bridge: read error {e}]\n".encode())
                time.sleep(0.5)
                continue
            if data:
                f.write(data)


def process_directive(ser, line):
    raw = line.rstrip("\r\n")
    if raw == "" or raw.startswith("#"):
        return
    if raw == "ENTER":
        ser.write(b"\n")
    elif raw.startswith("HEX:"):
        payload = raw[4:].strip().replace(" ", "")
        try:
            ser.write(binascii.unhexlify(payload))
        except Exception as e:
            print(f"[bridge] bad hex: {e}", flush=True)
    elif raw.startswith("RAW:"):
        ser.write(raw[4:].encode("latin-1"))
    else:
        ser.write(raw.encode("latin-1") + b"\n")
    ser.flush()


def main():
    # Fresh command queue each run so stale commands aren't replayed.
    open(CMDS, "w").close()
    with open(LOG, "ab", buffering=0) as f:
        f.write(f"\n===== bridge start {PORT}@{BAUD} =====\n".encode())

    ser = open_port()
    stop = threading.Event()
    t = threading.Thread(target=reader, args=(ser, stop), daemon=True)
    t.start()
    print(f"[bridge] {PORT}@{BAUD} open. log={LOG} cmds={CMDS}", flush=True)

    offset = 0
    try:
        while True:
            try:
                with open(CMDS, "r", encoding="latin-1") as cf:
                    cf.seek(offset)
                    new = cf.read()
                    offset = cf.tell()
            except FileNotFoundError:
                new = ""
            if new:
                for line in new.splitlines():
                    process_directive(ser, line)
                    time.sleep(0.05)
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        time.sleep(0.2)
        ser.close()
        print("[bridge] closed", flush=True)


if __name__ == "__main__":
    main()
