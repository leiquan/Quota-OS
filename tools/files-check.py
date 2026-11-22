"""Save through Notes on a rate-limited disk, cold boot and reopen through Files."""
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
disk = args.out / "notes.raw"
with disk.open("xb") as image:
    image.truncate(8192)

def boot(round_no):
    log = args.out / f"boot-{round_no}.log"
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        sock = tmp + "/qmp"
        drive = f"if=none,id=notes,format=raw,file={disk},bps_wr=128"
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a72",
               "-m", "128M", "-display", "none", "-nic", "none", "-global", "virtio-mmio.force-legacy=false",
               "-device", "ramfb", "-device", "virtio-tablet-device", "-device", "virtio-keyboard-device",
               "-drive", drive, "-device", "virtio-blk-device,drive=notes,serial=QUOTA-NOTES",
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
            mount = wait(r"\[files\] mounted bytes=(\d+)")
            box = wait(r"launcher box (\d+),(\d+) (\d+)x(\d+)")
            x,y,w,h = map(int,box.groups())
            click(x+w//2,y+h//2)
            wait(r"Apps opened")
            if round_no == 1:
                assert int(mount[1]) == 0
                click(575,232)
                wait(r"\[lvgl\] input page")
                click(490,390)
                for c in "quota":
                    q.call("human-monitor-command", {"command-line":"sendkey "+c})
                    time.sleep(.14)
            else:
                assert int(mount[1]) == 5
                click(405,232)
                wait(r"\[files\] directory opened")
                time.sleep(.5)
                q.call("screendump", {"filename":str((args.out / "files.ppm").resolve())})
                click(370,258)
                wait(r"\[files\] opened Note.txt in Notes")
            pattern = r"\[files\] Saved elapsed_us=(\d+) frames=(\d+) misses=(\d+) boot_max_gap_us=(\d+)"
            click(713,541)
            wait(pattern)
            offset = len(log.read_text())
            click(713,541)
            result = wait(pattern, offset)
            elapsed, frames, misses, gap = map(int,result.groups())
            assert elapsed >= 100000 and frames >= 10 and misses == 0 and gap < 11000, result.group()
            print(round_no, result.group(), flush=True)
            q.call("screendump", {"filename":str((args.out / f"boot-{round_no}.ppm").resolve())})
            q.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    raw = disk.read_bytes()
    for off in (0,512):
        magic, gen, size, checksum = struct.unpack_from("<4I",raw,off)
        if magic == 0x514e5431 and gen == round_no * 2:
            assert size == 5 and raw[off+16:off+21] == b"quota"
            return
    raise AssertionError("saved generation missing")

for round_no in (1,2):
    boot(round_no)
