#!/usr/bin/env python3
"""Boot quotaos with -append isolate and save the serial log plus two ramfb frames."""
import os
import socket
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/quotaos.bin"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/isolate-out"
os.makedirs(OUT, exist_ok=True)
LOG = os.path.join(OUT, "serial.log")
QMP = os.path.join(OUT, "qmp.sock")
RAW1 = os.path.join(OUT, "both.raw")
RAW2 = os.path.join(OUT, "after.raw")

def qmp_open():
    for _ in range(80):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(QMP)
            s.settimeout(2)
            return s
        except OSError:
            time.sleep(0.05)
    raise SystemExit("qmp did not open")

def qmp_read(s):
    buf = b""
    while b"\n" not in buf:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    return buf.decode("utf-8", "replace")

def qmp(s, obj):
    import json
    s.sendall((json.dumps(obj) + "\n").encode())
    return qmp_read(s)

def fb_addr(text):
    for line in text.splitlines():
        if "ramfb published" in line and " at 0x" in line:
            return int(line.split(" at 0x", 1)[1].split()[0], 16)
    return None

def save_frame(s, addr, raw_path, ppm_path):
    qmp(s, {"execute": "human-monitor-command",
            "arguments": {"command-line": "pmemsave 0x%x 1228800 %s" % (addr, raw_path)}})
    data = open(raw_path, "rb").read()
    with open(ppm_path, "w") as out:
        out.write("P3\n640 480\n255\n")
        for i in range(0, min(len(data), 640 * 480 * 4), 4):
            b, g, r = data[i], data[i + 1], data[i + 2]
            out.write("%d %d %d\n" % (r, g, b))
    return data

def sample(data, x, y):
    i = (y * 640 + x) * 4
    if i + 3 > len(data):
        return None
    return data[i] | (data[i + 1] << 8) | (data[i + 2] << 16)

def main():
    if os.path.exists(QMP):
        os.unlink(QMP)
    open(LOG, "w").close()
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a72",
        "-m", "128M",
        "-display", "none",
        "-nic", "none",
        "-device", "ramfb",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-kernel", BIN,
        "-append", "isolate",
        "-serial", "file:" + LOG,
        "-qmp", "unix:" + QMP + ",server,nowait",
    ]
    open(os.path.join(OUT, "command.txt"), "w").write(" ".join(cmd) + "\n")
    proc = subprocess.Popen(cmd)
    s = qmp_open()
    qmp_read(s)
    qmp(s, {"execute": "qmp_capabilities"})
    got = {"both": False, "after": False}
    fb = None
    deadline = time.time() + 240
    while time.time() < deadline:
        text = open(LOG, "r", errors="replace").read()
        if fb is None:
            fb = fb_addr(text)
        if fb and "shot both" in text and not got["both"]:
            save_frame(s, fb, RAW1, os.path.join(OUT, "both.ppm"))
            got["both"] = True
            print("captured both", hex(fb))
        if fb and "shot after" in text and not got["after"]:
            save_frame(s, fb, RAW2, os.path.join(OUT, "after.ppm"))
            got["after"] = True
            print("captured after")
        if "[isolate] DONE" in text and got["both"] and got["after"]:
            break
        time.sleep(0.05)
    proc.kill()
    proc.wait(timeout=5)
    text = open(LOG, "r", errors="replace").read()
    print("done", "[isolate] DONE" in text, "frames", got)
    for name, raw in (("both", RAW1), ("after", RAW2)):
        if not os.path.exists(raw):
            print(name, "missing")
            continue
        data = open(raw, "rb").read()
        print(name, "A", hex(sample(data, 56, 112) or 0), "B", hex(sample(data, 408, 112) or 0),
              "bytes", len(data))
    return 0 if "[isolate] DONE" in text and got["both"] and got["after"] else 1

if __name__ == "__main__":
    sys.exit(main())
