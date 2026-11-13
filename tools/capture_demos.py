#!/usr/bin/env python3
"""Drive the serial menu and save the console transcript."""
import os
import select
import subprocess
import sys
import time

QEMU = [
    "qemu-system-aarch64",
    "-machine", "virt,gic-version=2",
    "-cpu", "cortex-a72",
    "-nographic", "-nic", "none",
    "-kernel", "build/quotaos.bin",
]


def run_case(mem, steps, timeout):
    cmd = QEMU + ["-m", mem]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    buf = b""
    pos = 0

    def pull(deadline):
        nonlocal buf
        while time.time() < deadline:
            r, _, _ = select.select([proc.stdout], [], [], 0.2)
            if not r:
                continue
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                return False
            buf += chunk
        return True

    def wait_prompt(token, limit):
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

    ok = True
    if not wait_prompt("select> ", 15):
        ok = False
    for token, reply, limit in steps:
        if reply is not None:
            proc.stdin.write(reply.encode())
            proc.stdin.flush()
        if token and not wait_prompt(token, limit):
            ok = False
            break
    proc.stdin.write(b"\x01x")
    try:
        proc.stdin.flush()
    except BrokenPipeError:
        pass
    time.sleep(0.3)
    pull(time.time() + 0.5)
    proc.kill()
    proc.wait(timeout=2)
    return ok, buf.decode("utf-8", "replace")


def main():
    big_steps = [
        ("select> ", "1\n", 10),
        ("select> ", "4\n", 20),   # priority ON
        ("select> ", "5\n", 10),   # tiering OFF
        ("select> ", "4\n", 20),   # priority OFF
        ("select> ", "5\n", 10),   # tiering ON
        ("select> ", "6\n", 20),   # containment
        ("app> ", "2\n", 10),
        ("allow> ", "1\n", 10),
        ("select> ", "n\n", 10),   # deny Pixel
        ("app> ", "2\n", 10),
        ("allow> ", "1\n", 10),
        ("select> ", "y\n", 10),   # allow Pixel
        ("app> ", "2\n", 10),
        ("allow> ", "2\n", 10),
        ("select> ", "y\n", 10),   # Cinema
        ("app> ", "2\n", 10),
        ("allow> ", "3\n", 10),
        ("select> ", "y\n", 10),   # Studio, expect refuse
        ("close> ", "3\n", 10),
        ("select> ", "1\n", 10),   # close Pixel
        ("app> ", "2\n", 10),
        ("allow> ", "3\n", 10),
        ("select> ", "y\n", 10),   # Studio succeeds
        ("select> ", "1\n", 10),   # table
        ("select> ", "8\n", 15),   # pressure
        ("select> ", "7\n", 10),   # Cluster Sim
        ("close> ", "3\n", 10),
        ("select> ", "a\n", 10),   # close all
        ("select> ", "9\n", 60),   # min slice
        ("select> ", "10\n", 40),  # dozens of slices
        ("select> ", "1\n", 10),   # final table
        (None, None, 5),
    ]
    small_steps = [
        ("select> ", "1\n", 10),
        ("app> ", "2\n", 10),
        ("allow> ", "1\n", 10),
        ("select> ", "y\n", 10),   # Pixel should fit
        ("select> ", "2\n", 10),
        ("app> ", None, 10),       # wait app prompt then send 4
    ]
    # The last step above is awkward. Do Neural Forge as its own exchange.
    small_steps = [
        ("select> ", "1\n", 10),
        ("app> ", "2\n", 10),
        ("allow> ", "1\n", 10),
        ("select> ", "y\n", 10),
        ("app> ", "2\n", 10),
        ("select> ", "4\n", 10),   # Neural Forge, device refusal, no allow prompt
        ("select> ", "1\n", 10),
        (None, None, 5),
    ]

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    print("running 128M ...", flush=True)
    ok128, text128 = run_case("128M", big_steps, 180)
    open("/tmp/demo128.txt", "w").write(text128)
    print("128M ok", ok128, "bytes", len(text128))
    print("running 64M ...", flush=True)
    ok64, text64 = run_case("64M", small_steps, 30)
    open("/tmp/demo64.txt", "w").write(text64)
    print("64M ok", ok64, "bytes", len(text64))
    if not ok128 or not ok64:
        sys.exit(1)


if __name__ == "__main__":
    main()
