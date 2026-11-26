#!/usr/bin/env python3
"""Exercise Touch UI authentication in an isolated QEMU instance."""
import argparse
from pathlib import Path
import runpy
import secrets
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("image", type=Path)
parser.add_argument("--memory", default="128M")
args = parser.parse_args()
out = Path(tempfile.mkdtemp(prefix="quota-touch-auth-", dir="/tmp"))
print(out, flush=True)
disk = out / "account.raw"
subprocess.run(["python3", "tools/init-admin-disk.py", str(disk)], check=True)
password = "".join(secrets.choice("qwe") for _ in range(12))
qmp = runpy.run_path("tools/realtime-rounds.py")["QMP"]

for boot in (1, 2):
    log = out / f"boot-{boot}.log"
    sock = out / f"boot-{boot}.sock"
    cmd = [
        "qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a35",
        "-m", args.memory, "-display", "none", "-nic", "none",
        "-global", "virtio-mmio.force-legacy=false", "-device", "virtio-rng-device",
        "-device", "ramfb", "-device", "virtio-tablet-device",
        "-device", "virtio-keyboard-device", "-drive", f"if=none,id=notes,format=raw,file={disk}",
        "-device", "virtio-blk-device,drive=notes,serial=QUOTA-NOTES",
        "-icount", "shift=4,sleep=off", "-kernel", str(args.image), "-append", "lvgl",
        "-serial", "file:" + str(log), "-monitor", "none", "-qmp", f"unix:{sock},server=on,wait=off",
    ]
    vm = subprocess.Popen(cmd)
    q = None
    try:
        q = qmp(str(sock))

        def read():
            return log.read_text(errors="replace") if log.exists() else ""

        def wait(marker, offset=0):
            until = time.monotonic() + 12
            while marker not in read()[offset:]:
                if time.monotonic() >= until or vm.poll() is not None:
                    raise AssertionError((marker, read()[offset:][-2000:]))
                time.sleep(.05)

        def click(x, y):
            q.call("input-send-event", {"events": [
                {"type": "abs", "data": {"axis": axis, "value": round(value * 32767 / limit)}}
                for axis, value, limit in [("x", x, 899), ("y", y, 1599)]
            ]})
            time.sleep(.06)
            for pressed in (True, False):
                q.call("input-send-event", {"events": [
                    {"type": "btn", "data": {"button": "left", "down": pressed}}
                ]})
                time.sleep(.07)

        def enter(value, touch=False):
            if touch:
                keys = {"q": 120, "w": 189, "e": 258}
                for key in value:
                    click(keys[key], 1210)
                click(450, 519)
            else:
                for key in [*value, "ret"]:
                    q.call("send-key", {"keys": [{"type": "qcode", "data": key}], "hold-time": 45})
                    time.sleep(.11)

        wait("ramfb published 900x1600")
        wait("[lvgl] ready")
        if boot == 1:
            wait("[session] state=1")
            enter(password, touch=True)
            wait("[session] state=2")
            enter(password)
        wait("[session] state=4")
        q.call("screendump", {"filename": str(out / f"login-{boot}.ppm")})
        enter("wrong")
        time.sleep(.8)
        assert "[session] state=5" not in read()
        enter(password, touch=boot == 2)
        wait("[session] state=5")
        assert "[menu] button" not in read()
        q.call("screendump", {"filename": str(out / f"desktop-{boot}.ppm")})
        # Normal boot has Apps and System in the dock and no foreground app.
        click(517, 1470)
        wait("[menu] button 202")
        click(150, 607)
        click(150, 607)
        wait("[session] state=4", read().find("[session] state=5"))
        enter(password)
        wait("[session] state=5", read().rfind("[session] state=4"))
    finally:
        if q:
            q.close()
        if vm.poll() is None:
            vm.terminate()
        vm.wait(timeout=5)
    assert password.encode() not in log.read_bytes() and password.encode() not in disk.read_bytes()
    print(f"Boot {boot}: wrong password rejected, login and logout verified", flush=True)
