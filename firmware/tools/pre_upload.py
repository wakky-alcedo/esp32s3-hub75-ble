"""PlatformIO pre-upload hook.

esptool's USB reset is a CPU-only reset that leaves the HUB75 GDMA running,
which corrupts RAM and hangs the ROM downloader. Instead, ask the running
firmware to stop the DMA and reboot into download mode itself, then let
esptool connect without resetting.
"""
import json
import time

Import("env")  # noqa: F821  (provided by PlatformIO)


def enter_bootloader(source, target, env):
    import serial

    port = env.subst("$UPLOAD_PORT")
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.3
        s.dtr = False
        s.rts = False
        s.open()
        s.write(json.dumps({"cmd": "bootloader"}).encode() + b"\n")
        reply = s.read(200)
        s.close()
        print("pre_upload: firmware answered" if reply else "pre_upload: no answer (already in download mode?)")
    except Exception as e:
        print(f"pre_upload: could not send bootloader command: {e}")
    time.sleep(2.0)


def patch_flags(source, target, env):
    # The platform builder fills UPLOADERFLAGS after pre: scripts run, so
    # rewrite them right before the upload. watchdog_reset afterwards resets
    # the whole chip (a plain RTS reset would stay in download mode).
    flags = list(env.get("UPLOADERFLAGS", []))
    flags = ["no_reset" if f == "default_reset" else f for f in flags]
    flags = ["watchdog_reset" if f == "hard_reset" else f for f in flags]
    env.Replace(UPLOADERFLAGS=flags)


env.AddPreAction("upload", patch_flags)
env.AddPreAction("upload", enter_bootloader)
