#!/usr/bin/env python3
"""Compare native-clock misses with and without cursor traffic or a known VM stop."""
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import sys

kernel, output = (Path(arg).resolve() for arg in sys.argv[1:3])
rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 10
output.mkdir(parents=True, exist_ok=True)
gui = runpy.run_path(str(Path(__file__).with_name("realtime-rounds.py")))["gui"]
os.environ.update(QUOTA_QEMU_ACCEL="hvf", QUOTA_QEMU_CPU="host",
                  QUOTA_QEMU_MACHINE="virt,gic-version=3",
                  QUOTA_GUI_LONGEVITY_SECONDS="0")
summary = {"image_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(), "runs": []}
for i in range(1, rounds + 1):
    for name, traffic, pause in (("cursor", True, 0), ("quiet", False, 0),
                                  ("qmp-stop", False, 35)):
        folder = output / f"{i:02d}-{name}"
        try:
            result = gui(kernel, folder, "host", pointer_traffic=traffic, pause_ms=pause)
            log = (folder / "serial.log").read_text(errors="replace")
            result["logged_timer_lateness_us"] = [
                int(x) for x in re.findall(r"entry-cval (\d+) us", log)
            ]
        except Exception as error:
            result = {"error": repr(error), "passed": False}
        summary["runs"].append(dict(round=i, case=name, **result))
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(i, name, result, flush=True)
