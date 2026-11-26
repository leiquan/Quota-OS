#!/usr/bin/env python3
"""Open Todo twice in separate 256 MiB VMs and verify UI-driven persistence."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import time

from check_volume_pressure import verify

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=Path)
parser.add_argument("runtime", type=Path)
parser.add_argument("seed", type=Path, help="clean Todo disk from examples/todo/build.py")
parser.add_argument("output", type=Path, help="new evidence directory")
parser.add_argument("--shift", type=int, choices=(1, 4), default=1)
parser.add_argument("--pressure-rounds", type=int, default=0,
                    help="run 1 to 100 pressure trials after cold readback (QA runtime required)")
parser.add_argument("--system-launch", action="store_true",
                    help="exercise the normal authenticated Web UI launcher at its default RAM")
parser.add_argument("--ram-mib", type=int, choices=(256,512), default=256,
                    help="expected guest RAM, verified using QMP")
args = parser.parse_args()
if not 0 <= args.pressure_rounds <= 100:
    parser.error("--pressure-rounds must be 0 to 100")
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
disk = out / "todo.raw"
root = Path(__file__).resolve().parents[1]
width, height = (1600, 900) if args.system_launch else (1280, 800)
password = secrets.token_hex(6)
if args.system_launch:
    subprocess.run(["python3", str(root / "tools/init-admin-disk.py"), str(disk),
                    "--from-legacy", str(args.seed)], check=True)
else:
    shutil.copyfile(args.seed, disk)
text = "todo" + secrets.token_hex(3)
hashes = {}
for name, path in (("kernel", args.kernel), ("runtime", args.runtime), ("seed", args.seed)):
    with path.open("rb") as file:
        hashes[name] = hashlib.file_digest(file, "sha256").hexdigest()
(out / "inputs.json").write_text(json.dumps(dict(sha256=hashes, text=text, shift=args.shift), indent=2))

def until(action, timeout, name, interval=.25):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = action()
        if result:
            return result
        time.sleep(interval)
    raise TimeoutError(name)

def record(expected):
    raw = disk.read_bytes()
    records = []
    for sector in (6, 7):
        start = sector * 512
        magic, generation, size, crc = struct.unpack_from("<4I", raw, start)
        if magic != 0x514e4431 or size > 496:
            continue
        payload = raw[start+16:start+16+size]
        checksum = ((2166136261 ^ generation) * 16777619) & 0xffffffff
        for byte in payload:
            checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
        if checksum == crc:
            records.append((generation, payload))
    if records:
        generation, payload = max(records)
        if payload == json.dumps(expected, separators=(",", ":")).encode():
            return generation
    return 0

def boot(number):
    serial = out / f"boot-{number}.log"
    cmd = ["qemu-system-aarch64", "-machine", "virt,gic-version=2", "-cpu", "cortex-a35",
           "-m", "256M", "-display", "none", "-nic", "none", "-global", "virtio-mmio.force-legacy=false",
           "-device", "virtio-rng-device", "-device", "ramfb", "-device", "virtio-tablet-device",
           "-device", "virtio-keyboard-device", "-drive", f"if=none,id=notes,format=raw,file={disk}",
           "-device", "virtio-blk-device,drive=notes,serial=QUOTA-NOTES", "-drive",
           f"if=none,id=runtime,format=raw,readonly=on,file={args.runtime.resolve()}",
           "-device", "virtio-blk-device,drive=runtime,serial=QUOTA-RUNTIME", "-icount",
           f"shift={args.shift},sleep=off", "-kernel", str(args.kernel.resolve()), "-append", "apps-video",
           "-serial", f"file:{serial}", "-qmp", "stdio"]
    env = os.environ.copy()
    if args.system_launch:
        env.pop("QUOTA_RAM", None)
        env.update(QUOTA_KERNEL=str(args.kernel.resolve()), QUOTA_RUNTIME_VOLUME=str(args.runtime.resolve()),
                   QUOTA_NOTES_DISK=str(disk), QUOTA_SERIAL=f"file:{serial}")
        cmd = ["/bin/bash", str(root / "tools/run-files.sh"), "-display", "none", "-qmp", "stdio"]
    (out / f"command-{number}.json").write_text(json.dumps(cmd))
    vm = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
    def qmp(name, parameters=None):
        msg = dict(execute=name, id=1)
        if parameters is not None:
            msg["arguments"] = parameters
        vm.stdin.write(json.dumps(msg) + "\n")
        vm.stdin.flush()
        for line in vm.stdout:
            reply = json.loads(line)
            if reply.get("id") == 1:
                if "error" in reply:
                    raise RuntimeError(reply["error"])
                return reply["return"]
        raise RuntimeError("QEMU exited")
    def click(x, y):
        events = [{"type": "abs", "data": {"axis": axis, "value": round(value*32767/span)}}
                  for axis, value, span in (("x", x, width-1), ("y", y, height-1))]
        qmp("input-send-event", {"events": events})
        for down in (True, False):
            time.sleep(.16)
            qmp("input-send-event", {"events": [{"type":"btn","data":{"button":"left","down":down}}]})
        time.sleep(.16)
    def screenshot(label):
        dest = out / f"{number}-{label}.ppm"
        qmp("screendump", {"filename": str(dest)})
        signature = f"P6\n{width} {height}\n255\n".encode()
        data = dest.read_bytes()
        if not data.startswith(signature):
            raise RuntimeError("Unexpected framebuffer size or format")
        return data[len(signature):]
    def add_position():
        pixels = screenshot("ready")
        at = pixels.find(bytes.fromhex("177768") * 14)
        if at >= 0 and at % 3 == 0:
            return at//3 % width, at//3 //width
        if "process stopped" in serial.read_text(errors="replace"):
            raise RuntimeError("Guest process exited before Todo became usable")
    def pressure_rounds(x, y):
        # A separate saved row arms the real major-GC observer after cold readback.
        click(x-50, y+8)
        for char in 'gcstress':
            qmp("send-key", {"keys": [{"type":"qcode","data":char}], "hold-time":60})
            time.sleep(.8)
        click(x+10, y+8)
        until(lambda: "[v8-major]" in serial.read_text(errors="replace"), 180, "major GC")
        if args.system_launch:
            box = re.search(r"launcher box (\d+),(\d+) (\d+)x(\d+)", serial.read_text())
            x0, y0, w, h = map(int, box.groups())
            click(x0+w//2, y0+h//2)
        else:
            click(376, 742)
        until(lambda: "[desktop] Apps opened" in serial.read_text(errors="replace"), 60, "Apps")
        click(404, 457)
        until(lambda: "[desktop] Test opened" in serial.read_text(errors="replace"), 60, "Test")
        for index in range(1, args.pressure_rounds+1):
            folder = out / f"round-{index:03d}"
            folder.mkdir()
            with serial.open("rb") as log:
                offset = log.seek(0, 2)
                def tail():
                    log.seek(offset)
                    return log.read().decode(errors="replace")
                try:
                    click(194, 484)
                    until(lambda: "[quota-test] 5 " in tail(), 90, "registered pressure input")
                    until(lambda: "Long gaps: GPS Speed" in tail(), 240, "completed trial")
                    # Node posts PerformanceObserver entries after the collector returns.
                    after = len(tail())
                    until(lambda: "[v8-major]" in tail()[after:], 180, "post-window GC record")
                    report = verify(tail())
                finally:
                    (folder / "serial.log").write_text(tail())
            report.update(round=index, sha256=hashes, ram_mib=args.ram_mib, icount_shift=args.shift)
            (folder / "result.json").write_text(json.dumps(report, indent=2))
            print(f"round={index} passed={report['passed']}", flush=True)
            if not report['passed']:
                raise RuntimeError(f"Pressure verification failed in {folder}")
    try:
        json.loads(vm.stdout.readline())
        qmp("qmp_capabilities")
        ram = qmp("query-memory-size-summary")["base-memory"]
        if ram != args.ram_mib << 20:
            raise AssertionError(f"Expected {args.ram_mib} MiB, got {ram} bytes")
        if args.system_launch:
            def state(value):
                until(lambda: serial.exists() and f"[session] state={value} " in serial.read_text(), 120, "session " + str(value))
            def enter_password():
                for char in (*password, "ret"):
                    qmp("send-key", {"keys":[{"type":"qcode","data":char}], "hold-time":45})
                    time.sleep(.22)
            if number == 1:
                state(1); enter_password(); state(2); enter_password()
            state(4)
            screenshot("locked")
            enter_password()
            state(5)
        until(lambda: serial.exists() and "[files] mounted" in serial.read_text(errors="replace"), 60, "desktop")
        devices = qmp("query-blockstats")
        reads = next(d["stats"]["rd_bytes"] for d in devices if d["device"] == "runtime")
        if reads:
            raise AssertionError(f"Runtime volume read {reads} bytes before launch")
        if not args.system_launch:
            click(116, 150)
        click(45, 138)
        time.sleep(.55)
        click(45, 138)
        x, y = until(add_position, 1800, "Todo Add button", interval=2)
        if number == 1:
            click(x-50, y+8)
            for ch in text:
                qmp("send-key", {"keys":[{"type":"qcode","data":ch}], "hold-time":60})
                time.sleep(.8)
            click(x+10, y+8)
        else:
            # Toggling this restored row requires the page and backend to have
            # reloaded the saved title. The disk assertion checks its exact value.
            click(193, y+40)
        generation = until(lambda: record([[text, number == 2]]), 90, "committed Todo record")
        screenshot("committed")
        result = dict(boot=number, pid=vm.pid, ram_bytes=ram, prelaunch_runtime_read_bytes=reads, generation=generation)
        (out / f"cold-{number}.json").write_text(json.dumps(result, indent=2))
        if number == 2 and args.pressure_rounds:
            pressure_rounds(x, y)
        return result
    finally:
        vm.terminate()
        vm.wait(timeout=10)

results = []
for number in (1, 2):
    results.append(boot(number))
    (out / "result.json").write_text(json.dumps(results, indent=2))
    print(results[-1], flush=True)
if results[1]["generation"] <= results[0]["generation"]:
    raise AssertionError("Cold edit did not advance the persistent generation")
