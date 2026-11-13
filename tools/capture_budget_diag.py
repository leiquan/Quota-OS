#!/usr/bin/env python3
"""Reproduce the tier-1 budget overrun diagnosis and save the console log."""
import os
import select
import subprocess
import sys
import time

KERNEL = sys.argv[1] if len(sys.argv) > 1 else "prebuilt/quotaos.bin"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/budget-diag-full.txt"


def main():
    cmd = [
        "qemu-system-aarch64",
        "-machine", "virt,gic-version=2",
        "-cpu", "cortex-a72",
        "-m", "128M",
        "-nographic",
        "-kernel", KERNEL,
    ]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    buf = b""
    pos = 0

    def pull(deadline):
        nonlocal buf
        while time.time() < deadline:
            ready, _, _ = select.select([proc.stdout], [], [], 0.3)
            if not ready:
                continue
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                return False
            buf += chunk
        return True

    def wait(token, limit):
        nonlocal pos
        deadline = time.time() + limit
        token_b = token.encode()
        while time.time() < deadline:
            idx = buf.find(token_b, pos)
            if idx >= 0:
                pos = idx + len(token_b)
                return True
            if proc.poll() is not None:
                return False
            pull(time.time() + 0.3)
        return False

    if not wait("select> ", 20):
        sys.exit("boot prompt missing")
    proc.stdin.write(b"11\n")
    proc.stdin.flush()
    if not wait("DIAG_DONE\n", 240):
        open(OUT, "wb").write(buf)
        sys.exit("diagnosis did not finish; partial log in " + OUT)
    open(OUT, "wb").write(buf.replace(b"\r\n", b"\n").replace(b"\r", b"\n"))
    proc.stdin.write(b"\x01x")
    try:
        proc.stdin.flush()
    except BrokenPipeError:
        pass
    time.sleep(0.2)
    proc.kill()
    print("wrote", OUT, "bytes", os.path.getsize(OUT))


if __name__ == "__main__":
    main()
