#!/usr/bin/env python3
"""Recheck the frozen inputs, independent boots and all 100 captured Todo trials."""
import argparse
import hashlib
import json
from pathlib import Path

from check_volume_pressure import verify

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
parser.add_argument("kernel", type=Path)
parser.add_argument("runtime", type=Path)
args = parser.parse_args()
root = args.directory
inputs = json.loads((root / "inputs.json").read_text())
errors = []
for key in ("kernel", "runtime"):
    with getattr(args, key).open("rb") as artifact:
        if hashlib.file_digest(artifact, "sha256").hexdigest() != inputs["sha256"][key]:
            errors.append(f"{key} hash mismatch")
boots = []
for boot in (1, 2):
    checkpoint = root / f"cold-{boot}.json"
    if not checkpoint.exists():
        errors.append(f"boot {boot} missing cold checkpoint")
        continue
    result = json.loads(checkpoint.read_text())
    boots.append(result)
    serial = (root / f"boot-{boot}.log").read_text()
    if result["boot"] != boot or result["ram_bytes"] != 256 << 20 or result["prelaunch_runtime_read_bytes"] != 0:
        errors.append(f"boot {boot} memory or prelaunch runtime read")
    for marker in ("[wap] launch entries tier=3", "[wap] launched pair", "[wap-volume] libv8 executable shared"):
        if marker not in serial:
            errors.append(f"boot {boot} missing {marker}")
if len(boots) == 2 and (boots[0]["pid"] == boots[1]["pid"] or boots[0]["generation"] >= boots[1]["generation"]):
    errors.append("cold boot or saved generation did not change")
passed = captured = 0
for number in range(1, 101):
    directory = root / f"round-{number:03d}"
    evidence = directory / "serial.log"
    if not evidence.exists():
        continue
    captured += 1
    try:
        report = verify(evidence.read_text())
        run = json.loads((directory / "result.json").read_text())
        if run["round"] != number or run["sha256"] != inputs["sha256"] or not report["passed"]:
            errors.append(f"round {number}: {report['first_errors']}")
        else:
            passed += 1
    except (OSError, KeyError, ValueError) as exc:
        errors.append(f"round {number}: {exc}")
if captured != 100:
    errors.append(f"missing {100-captured} round traces")
print(json.dumps(dict(passed=not errors, verified_rounds=passed, captured_rounds=captured,
                      boots=boots, errors=errors), indent=2))
raise SystemExit(bool(errors))
