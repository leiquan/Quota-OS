#!/usr/bin/env python3
"""Run paired native-clock workload windows with Task Manager closed and open."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import sys

kernel, output = (Path(arg).resolve() for arg in sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)
gui = runpy.run_path(str(Path(__file__).with_name("realtime-rounds.py")))["gui"]
os.environ.update(QUOTA_QEMU_ACCEL="hvf", QUOTA_QEMU_CPU="host",
                  QUOTA_QEMU_MACHINE="virt,gic-version=3",
                  QUOTA_GUI_LONGEVITY_SECONDS="0")
summary = {"image_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(), "runs": []}
for index in range(1, 11):
    for opened in (False, True) if index % 2 else (True, False):
        case = "open" if opened else "closed"
        try:
            result = gui(kernel, output / f"{index:02d}-{case}", "host", task_manager=opened)
        except Exception as error:
            result = {"error": repr(error), "passed": False}
        summary["runs"].append(dict(round=index, case=case, **result))
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(index, case, result, flush=True)
