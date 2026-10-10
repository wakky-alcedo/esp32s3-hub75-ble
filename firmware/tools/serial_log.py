"""Log USB serial to stdout without resetting the board (DTR/RTS held low).

usage: serial_log.py PORT [SECONDS] [JSON_COMMAND_TO_SEND_FIRST]
"""
import sys
import time

import serial

s = serial.Serial()
s.port = sys.argv[1]
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
end = time.time() + float(sys.argv[2]) if len(sys.argv) > 2 else float("inf")
first = sys.argv[3] if len(sys.argv) > 3 else None
while time.time() < end:
    try:
        if not s.is_open:
            s.open()
            if first:
                s.write(first.encode() + b"\n")
                first = None
        data = s.read(4096)
        if data:
            sys.stdout.write(time.strftime("%H:%M:%S ") + data.decode(errors="replace").replace("\n", "\n" + time.strftime("%H:%M:%S ")))
            sys.stdout.flush()
    except serial.SerialException:
        # The port disappears while the board reboots; wait and reopen.
        try:
            s.close()
        except Exception:
            pass
        time.sleep(0.5)
