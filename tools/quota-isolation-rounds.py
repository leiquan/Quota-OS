#!/usr/bin/env python3
"""Repeat the EL0 recovery acceptance checks under the instruction clock."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("kernel", type=Path)
parser.add_argument("out", type=Path)
parser.add_argument("--rounds", type=int, default=100)
args = parser.parse_args()
if args.rounds < 1:
    parser.error("--rounds must be positive")
kernel = args.kernel.resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
isolate = runpy.run_path(str(Path(__file__).with_name("realtime-rounds.py")))["isolate"]
os.environ.update(QUOTA_QEMU_ACCEL="tcg", QUOTA_QEMU_CPU="cortex-a72",
                  QUOTA_QEMU_MACHINE="virt,gic-version=2")
summary = {"image_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(), "rounds": []}
for i in range(1, args.rounds + 1):
    try:
        round_dir = out / f"round-{i:03d}"
        result = isolate(kernel, round_dir, "icount")
        log = (round_dir / "serial.log").read_bytes()
        result["clock_ok"] = b"[boot] clock: icount" in log
        result["panicked"] = b"PANIC" in log
        result["passed"] = result["passed"] and result["clock_ok"] and not result["panicked"]
    except Exception as error:
        result = {"passed": False, "error": repr(error)}
    summary["rounds"].append(dict(round=i, **result))
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(i, "PASS" if result["passed"] else "FAIL", flush=True)
raise SystemExit(0 if all(row["passed"] for row in summary["rounds"]) else 1)
