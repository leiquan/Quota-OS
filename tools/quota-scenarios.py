"""Exercise the five System Test scenarios via QEMU tablet input."""
import argparse
from pathlib import Path
import re
import runpy
import struct
import subprocess
import tempfile
import time

QMP = runpy.run_path(str(Path(__file__).with_name("realtime-rounds.py")))["QMP"]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=Path)
parser.add_argument("out", type=Path)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
def boot():
    log = args.out / "ui.log"
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        sock = tmp + "/qmp"
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a72",
               "-m", "128M", "-display", "none", "-nic", "none", "-global", "virtio-mmio.force-legacy=false",
               "-device", "ramfb", "-device", "virtio-tablet-device", "-device", "virtio-keyboard-device",
               "-icount", "shift=4,sleep=off", "-kernel", str(args.kernel), "-append", "apps-video",
               "-serial", "file:" + str(log), "-qmp", "unix:" + sock + ",server=on,wait=off"]
        proc = subprocess.Popen(cmd)
        try:
            q = QMP(sock)
            def wait(pattern, offset=0):
                end = time.monotonic() + 25
                while time.monotonic() < end:
                    match = re.search(pattern, log.read_text()[offset:])
                    if match:
                        return match
                    time.sleep(.04)
                raise AssertionError(pattern)
            def click(x,y):
                q.call("input-send-event", {"events":[{"type":"abs","data":{"axis":a,"value":v*32767//n}}
                    for a,v,n in [("x",x,1279),("y",y,799)]]})
                time.sleep(.15)
                for down in (True,False):
                    q.call("input-send-event", {"events":[{"type":"btn","data":{"button":"left","down":down}}]})
                    time.sleep(.16)
            wait(r"\[apps\] PASS")
            click(455,198)
            for number in range(1,6):
                offset = len(log.read_text())
                click(195,269+(number-1)*54)
                if number == 2:
                    click(620,508)
                    wait(r"\[quota-test\] 0 Cancelled", offset)
                    assert "[quota-test] 2 Running" not in log.read_text()[offset:]
                    click(195,323)
                if number in (2,3):
                    click(415,508)
                result = wait(r"\[quota-test\] "+str(number)+r" (PASS|FAIL)", offset)
                if result[1] != "PASS": raise AssertionError(f"scenario {number} failed")
            q.call("screendump", {"filename":str((args.out/"test.ppm").resolve())})
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

boot()
