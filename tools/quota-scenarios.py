"""Exercise the five Test app scenarios via QEMU tablet input."""
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
parser.add_argument("--combined", action="store_true", help="Attach a throttled disk for Test 5")
parser.add_argument("--memory", default="64M", help="QEMU guest RAM (1G for Web Node)")
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
def report_frame_trace(log):
    text = log.read_text(errors="replace")
    header = re.search(r"\[frame-trace\] hz=(\d+) count=(\d+) dropped=(\d+)", text)
    assert header, "missing frame trace"
    hz, count, dropped = map(int, header.groups())
    frames = [tuple(map(int, row)) for row in re.findall(
        r"\[frame\] stream=(\d+) release=(\d+) start=(\d+) copy=(\d+) commit=(\d+)", text)]
    assert hz and count == len(frames) and not dropped, header.group()
    for stream, period_us in ((0, 10000), (1, 40000)):
        rows = [(release, start, copy, commit) for kind, release, start, copy, commit in frames if kind == stream]
        assert len(rows) > 1 and all(release <= start <= copy <= commit for release, start, copy, commit in rows)
        long_gaps = []
        for prev, curr in zip(rows, rows[1:]):
            assert curr[0] > prev[0], (prev, curr)
            release_delta = curr[0] - prev[0]
            wait_delta = (curr[1] - curr[0]) - (prev[1] - prev[0])
            draw_delta = (curr[2] - curr[1]) - (prev[2] - prev[1])
            copy_delta = (curr[3] - curr[2]) - (prev[3] - prev[2])
            gap = curr[3] - prev[3]
            assert release_delta + wait_delta + draw_delta + copy_delta == gap
            if gap * 1000000 > period_us * hz:
                long_gaps.append((gap, release_delta, wait_delta, draw_delta, copy_delta))
        if long_gaps:
            gap, release, wait, draw, copy = (value * 1e6 / hz for value in max(long_gaps))
            print(f"frame trace {stream}: {len(rows)} frames, {len(long_gaps)} long gaps, "
                  f"worst {gap:.3f} us = release {release:.3f} + wait {wait:+.3f} + draw/phase {draw:+.3f} + copy {copy:+.3f}", flush=True)
            raise AssertionError(f"stream {stream} exceeded {period_us} us")
        else:
            print(f"frame trace {stream}: {len(rows)} frames, no long gaps", flush=True)


def boot():
    log = args.out / "ui.log"
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        sock = tmp + "/qmp"
        cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a35",
               "-m", args.memory, "-display", "none", "-nic", "none", "-global", "virtio-mmio.force-legacy=false",
               "-device", "ramfb", "-device", "virtio-tablet-device", "-device", "virtio-keyboard-device",
               "-icount", "shift=4,sleep=off", "-kernel", str(args.kernel), "-append", "apps-video",
               "-serial", "file:" + str(log), "-qmp", "unix:" + sock + ",server=on,wait=off"]
        if args.combined:
            disk = Path(tmp) / "notes.raw"
            with disk.open("xb") as image:
                image.truncate(8192)
            cmd += ["-drive", f"if=none,id=notes,format=raw,file={disk},bps_rd=4096",
                    "-device", "virtio-blk-device,drive=notes,serial=QUOTA-NOTES"]
        proc = subprocess.Popen(cmd)
        try:
            q = QMP(sock)
            def wait(pattern, offset=0, timeout=25):
                end = time.monotonic() + timeout
                while time.monotonic() < end:
                    match = re.search(pattern, log.read_text(errors="replace")[offset:])
                    if match:
                        return match
                    time.sleep(.04)
                raise AssertionError(pattern)
            def click(x,y):
                q.call("input-send-event", {"events":[{"type":"abs","data":{"axis":a,"value":v*32767//n}}
                    for a,v,n in [("x",x,1599),("y",y,899)]]})
                time.sleep(.15)
                for down in (True,False):
                    q.call("input-send-event", {"events":[{"type":"btn","data":{"button":"left","down":down}}]})
                    time.sleep(.16)
            wait(r"\[apps\] PASS")
            def click_logged(name):
                box = wait(r"\[lvgl\] "+name+r" box (\d+),(\d+) (\d+)x(\d+)")
                x, y, w, h = map(int, box.groups())
                click(x + w // 2, y + h // 2)
            click(455,198)
            wait(r"\[desktop\] system events opened")
            time.sleep(.3)
            q.call("screendump", {"filename": str((args.out / "events.ppm").resolve())})
            click_logged("launcher")
            wait(r"\[desktop\] Apps opened")
            click_logged("tests")
            wait(r"\[desktop\] Test opened")
            for number in ((5,) if args.combined else range(1, 6)):
                offset = len(log.read_text())
                click(195,269+(number-1)*54)
                if number == 2:
                    click(620,508)
                    wait(r"\[quota-test\] 0 Cancelled", offset)
                    assert "[quota-test] 2 Running" not in log.read_text(errors="replace")[offset:]
                    click(195,323)
                if number in (2,3):
                    click(415,508)
                if number == 5:
                    wait(r"\[quota-test\] 5 Running", offset)
                    q.call("screendump", {"filename": str((args.out / "countdown.ppm").resolve())})
                pattern = rf"\[quota-test\] {number} (PASS|FAIL)"
                if number > 1:
                    pattern += r"  /  (\d+) periods"
                result = wait(pattern, offset, timeout=120 if number == 5 else 25)
                if result[1] != "PASS": raise AssertionError(f"scenario {number} failed")
                if number == 1:
                    cpu = wait(r"CPU per 10 ms \(us\)\nRequested (\d+) +Available (\d+) +Total (\d+)", offset)
                    requested, available, total = map(int, cpu.groups())
                    assert requested > total >= available, cpu.group()
                    time.sleep(.25)
                    q.call("screendump", {"filename": str((args.out / "admission.ppm").resolve())})
                if number == 5 and args.combined:
                    combined = wait(r"\[combined-pressure\] periods=(\d+) cpu_peak_us=(\d+) memory_reserved_kib=(\d+) memory_available_kib=(\d+) working_set_kib=(\d+) disk_submitted=(\d+) disk_completed=(\d+)", offset)
                    periods, cpu, reserved, available, working_set, submitted, completed = map(int, combined.groups())
                    assert cpu > 0 and reserved > 0 and available == 0 and working_set > 8, combined.group()
                    assert 0 < completed <= submitted <= periods + 1 and submitted - completed <= 1, combined.group()
                if number > 1:
                    assert int(result[2]) >= (1000 if number == 5 else 100), result.group()
                    camera = wait(r"Flight Camera (\d+) commits, (\d+) missed", offset)
                    assert int(camera[1]) > 0 and int(camera[2]) == 0, camera.group()
                    gaps = wait(r"Peak gap: Speed (\d+) us, Dashcam (\d+) us", offset)
                    speed_us, camera_us = map(int, gaps.groups())
                    assert 0 < speed_us < 11000 and 0 < camera_us < 41000, gaps.group()

            if args.combined:
                report_frame_trace(log)
                wait(r"Strict frame timing: PASS", offset)
            q.call("screendump", {"filename":str((args.out/"test.ppm").resolve())})
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

boot()
