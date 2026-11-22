#!/usr/bin/env python3
"""Check both app-demo boot modes under the acceptance clock."""
import argparse
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=Path)
parser.add_argument("out", type=Path)
parser.add_argument("--rounds", type=int, default=10)
args = parser.parse_args()
if args.rounds < 1:
    parser.error("--rounds must be positive")
args.out.mkdir(parents=True, exist_ok=True)

passed = 0
for round_no in range(1, args.rounds + 1):
    for mode in ("apps", "apps-video"):
        log = args.out / f"{round_no:02d}-{mode}.log"
        cmd = ["qemu-system-aarch64", "-accel", "tcg", "-machine", "virt,gic-version=2",
               "-cpu", "cortex-a72", "-m", "128M", "-display", "none", "-nic", "none",
               "-device", "ramfb", "-icount", "shift=4,sleep=off",
               "-kernel", str(args.kernel), "-append", mode, "-serial", "stdio", "-monitor", "none"]
        with log.open("wb") as stream:
            proc = subprocess.Popen(cmd, stdout=stream, stderr=stream)
            try:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline and proc.poll() is None:
                    if b"QA DONE" in log.read_bytes():
                        break
                    time.sleep(0.05)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                proc.wait(timeout=5)
        raw = log.read_bytes()
        ok = (b"[boot] clock: icount" in raw and b"QA DONE" in raw
              and b"[apps] PASS" in raw and b"PANIC" not in raw)
        passed += ok
        print(round_no, mode, "PASS" if ok else "FAIL", flush=True)
raise SystemExit(0 if passed == args.rounds * 2 else 1)
