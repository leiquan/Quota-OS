#!/usr/bin/env python3
"""Open Todo twice in separate 256 MiB VMs and verify UI-driven persistence."""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=Path)
parser.add_argument("runtime", type=Path)
parser.add_argument("seed", type=Path, help="clean Todo disk from examples/todo/build.py")
parser.add_argument("output", type=Path, help="new evidence directory")
parser.add_argument("--shift", type=int, choices=(1, 4), default=1)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
disk = out / "todo.raw"
shutil.copyfile(args.seed, disk)
text = "todo" + secrets.token_hex(3)
hashes = {}
for name, path in (("kernel", args.kernel), ("runtime", args.runtime), ("seed", args.seed)):
    with path.open("rb") as file:
        hashes[name] = hashlib.file_digest(file, "sha256").hexdigest()
(out / "inputs.json").write_text(json.dumps(dict(sha256=hashes, text=text, shift=args.shift), indent=2))

def until(action, timeout, name):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = action()
        if result:
            return result
        time.sleep(.25)
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
    (out / f"command-{number}.json").write_text(json.dumps(cmd))
    vm = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
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
                  for axis, value, span in (("x", x, 1279), ("y", y, 799))]
        qmp("input-send-event", {"events": events})
        for down in (True, False):
            time.sleep(.16)
            qmp("input-send-event", {"events": [{"type":"btn","data":{"button":"left","down":down}}]})
        time.sleep(.16)
    def screenshot(label):
        dest = out / f"{number}-{label}.ppm"
        qmp("screendump", {"filename": str(dest)})
        signature = b"P6\n1280 800\n255\n"
        data = dest.read_bytes()
        if not data.startswith(signature):
            raise RuntimeError("Unexpected framebuffer size or format")
        return data[len(signature):]
    def add_position():
        pixels = screenshot("ready")
        at = pixels.find(bytes.fromhex("177768") * 14)
        if at >= 0 and at % 3 == 0:
            return at//3 % 1280, at//3 //1280
        if "process stopped" in serial.read_text(errors="replace"):
            raise RuntimeError("Guest process exited before Todo became usable")
    try:
        json.loads(vm.stdout.readline())
        qmp("qmp_capabilities")
        until(lambda: serial.exists() and "[files] mounted" in serial.read_text(errors="replace"), 60, "desktop")
        devices = qmp("query-blockstats")
        reads = next(d["stats"]["rd_bytes"] for d in devices if d["device"] == "runtime")
        if reads:
            raise AssertionError(f"Runtime volume read {reads} bytes before launch")
        click(116, 150)
        click(45, 138)
        time.sleep(.55)
        click(45, 138)
        x, y = until(add_position, 900, "Todo Add button")
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
        return dict(boot=number, pid=vm.pid, prelaunch_runtime_read_bytes=reads, generation=generation)
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
