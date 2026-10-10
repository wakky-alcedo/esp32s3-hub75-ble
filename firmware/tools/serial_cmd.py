"""Debug helper: send JSON commands over USB serial and print the log.

usage: serial_cmd.py PORT [--listen SECONDS] [--reset] ['{"cmd":"info"}' ...]
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("commands", nargs="*")
    ap.add_argument("--listen", type=float, default=2.0)
    ap.add_argument("--reset", action="store_true", help="reset the board first")
    a = ap.parse_args()

    s = serial.Serial(a.port, 115200, timeout=0.1)
    if a.reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        time.sleep(0.5)
    for c in a.commands:
        s.write(c.encode() + b"\n")
        time.sleep(0.2)
    end = time.time() + a.listen
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()
    s.close()


if __name__ == "__main__":
    main()
