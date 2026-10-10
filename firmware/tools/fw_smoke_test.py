"""Firmware smoke test over USB serial (no app needed).

Uploads a generated bitmap with the chunked file protocol, stores a preset
that uses it, syncs the clock and checks every response.
usage: fw_smoke_test.py PORT
"""
import base64
import json
import struct
import sys
import time
import zlib

import serial


class Dev:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.id = 0
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def cmd(self, **req):
        self.id += 1
        req["id"] = self.id
        self.s.write((json.dumps(req) + "\n").encode())
        end = time.time() + 5
        buf = b""
        while time.time() < end:
            buf += self.s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line.startswith(b"{"):
                    if line:
                        print("  log:", line.decode(errors="replace"))
                    continue
                resp = json.loads(line)
                if resp.get("id") == self.id:
                    return resp
        raise TimeoutError(req["cmd"])


def ldb(w, h, fmt, pixels):
    return b"LDB1" + struct.pack("<HHB3x", w, h, fmt) + pixels


def gradient_a8(w, h):
    return bytes(255 if (x // 4 + y // 4) % 2 == 0 else 60 for y in range(h) for x in range(w))


def upload(dev, path, data, chunk=180):
    r = dev.cmd(cmd="file_begin", path=path, size=len(data), crc=zlib.crc32(data))
    assert r["ok"], r
    for off in range(0, len(data), chunk):
        r = dev.cmd(cmd="file_data", offset=off, data=base64.b64encode(data[off:off + chunk]).decode())
        assert r["ok"], r
    r = dev.cmd(cmd="file_end")
    assert r["ok"], r


def main():
    dev = Dev(sys.argv[1])
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, cond))
        print(("PASS " if cond else "FAIL ") + name + (f"  {detail}" if detail else ""))

    r = dev.cmd(cmd="info")
    check("info", r["ok"] and r["info"]["width"] > 0, r)
    w, h = r["info"]["width"], r["info"]["height"]

    r = dev.cmd(cmd="sync_time", epoch=int(time.time()), tzOffsetMin=540)
    check("sync_time", r["ok"])

    upload(dev, "/assets/test_mask.ldb", ldb(96, 16, 2, gradient_a8(96, 16)))
    files = dev.cmd(cmd="list_files", dir="/assets")["files"]
    check("upload + list", any(f["name"] == "test_mask.ldb" and f["size"] == 12 + 96 * 16 for f in files), files)

    # Wrong CRC must be rejected and must not replace anything.
    data = ldb(4, 4, 2, bytes(16))
    dev.cmd(cmd="file_begin", path="/assets/bad.ldb", size=len(data), crc=1)
    dev.cmd(cmd="file_data", offset=0, data=base64.b64encode(data).decode())
    r = dev.cmd(cmd="file_end")
    check("crc mismatch rejected", not r["ok"] and r["error"] == "crc mismatch", r)

    r = dev.cmd(cmd="file_begin", path="/../etc", size=1, crc=0)
    check("bad path rejected", not r["ok"], r)

    preset = {
        "name": "smoke",
        "transition": "fade",
        "items": [
            {"type": "text", "asset": "test_mask.ldb", "color": "rainbow", "speed": 40, "repeat": 1,
             "bg": {"type": "effect", "effect": "noise"}},
            {"type": "clock", "format": "HMS", "color": "#ffa000", "duration": 3000},
            {"type": "countdown", "seconds": 5, "labelText": "START", "color": "#00ff80"},
            {"type": "effect", "effect": "fire", "duration": 3000},
        ],
    }
    r = dev.cmd(cmd="set_preset", index=1, preset=preset)
    check("set_preset", r["ok"], r)
    r = dev.cmd(cmd="preset", index=1)
    check("select preset", r["ok"], r)
    st = dev.cmd(cmd="status")["status"]
    check("status preset=1", st["preset"] == 1 and not st["demo"] and st["items"] == 4, st)

    r = dev.cmd(cmd="brightness", value=30)
    check("brightness", r["ok"] and dev.cmd(cmd="status")["status"]["brightness"] == 30)
    dev.cmd(cmd="next")
    time.sleep(0.6)
    st = dev.cmd(cmd="status")["status"]
    check("next", st["item"] == 1, st)

    r = dev.cmd(cmd="info")
    check("time synced", abs(r["info"]["time"] - time.time()) < 5, r["info"]["time"])
    check("preset listed", 1 in r["info"]["presets"], r["info"]["presets"])

    failed = [n for n, ok in checks if not ok]
    print(f"\n{len(checks) - len(failed)}/{len(checks)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
