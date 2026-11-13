#!/usr/bin/env python3
"""Functional regression for the boot menu (commit D: the ten intent-named demos).

  run:     python3 tools/menu_regress.py run <kernel.bin> <out.txt> [gic-version]
  compare: python3 tools/menu_regress.py compare <a.txt> <b.txt>

`compare` masks only the numbers on timing lines (ticks, latencies, received
CPU, kernel overhead, ...) and collapses T1MISS rows, then diffs everything
else byte for byte: admission results, REFUSED texts, tables and menu flow.
"""
import difflib
import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qemu_serial import Session  # noqa: E402

P = "select> "
A = "app> "
Y = "allow> "
BIG = [
    ("1\n", P, 20),
    ("2\n", A, 10), ("1\n", Y, 10), ("n\n", P, 10),     # deny Pixel Racer
    ("2\n", A, 10), ("1\n", Y, 10), ("y\n", P, 10),     # Pixel Racer
    ("2\n", A, 10), ("2\n", Y, 10), ("y\n", P, 10),     # Cinema Play
    ("2\n", A, 10), ("3\n", P, 10),                     # Studio Cut refused
    ("2\n", A, 10), ("s1\n", P, 10),                    # stop Pixel Racer
    ("2\n", A, 10), ("3\n", Y, 10), ("y\n", P, 10),     # Studio Cut fits now
    ("1\n", P, 10),
    ("2\n", A, 10), ("s1\n", P, 10), ("2\n", A, 10), ("s1\n", P, 10),
    ("3\n", Y, 10), ("y\n", P, 20),
    ("4\n", P, 10),
    ("5\n", P, 60), ("6\n", P, 60), ("7\n", P, 60), ("8\n", P, 120),
    ("9\n", P, 60), ("10\n", P, 60),
    ("1\n", P, 10), ("x\n", P, 10), ("h\n", P, 10),
]
SMALL = [
    ("1\n", P, 20), ("4\n", P, 10),
    ("2\n", A, 10), ("4\n", P, 10),                     # Neural Forge: device too small
    ("2\n", A, 10), ("1\n", Y, 10), ("y\n", P, 10),     # Pixel Racer fits
    ("1\n", P, 10),
]

TIMING = re.compile(
    r"^\[boot\] (x0=|DTB at|kernel image|slot table)|^  (display-vsync|timer-server|gic-server|"
    r"blk-driver) |last release|measured since boot|^misses: class|^kernel per period|"
    r"^timer IRQ entry|^kernel entry|^slot guard|^wait loop|^clock jumps|^Tier 2/3 ticks|"
    r"of which not covered|^\s+\d+ \S+\s+\d+\s+\d+\s+\d+|^  (app-A|app-B|game|greedy-app) |"
    r"^bad-|cut by the slot-end|audio-mix: jobs|^tier-3 per loop|^结果:|over \d+ periods")


def run(kernel, out, gic="2"):
    parts = []
    ok_all = True
    for mem, steps in (("128M", BIG), ("64M", SMALL)):
        s = Session(kernel, mem, machine="virt,gic-version=" + gic)
        ok = s.run(steps)
        parts.append("##### %s ok=%s\n%s" % (mem, ok, s.close()))
        ok_all &= ok
        print(mem, "ok" if ok else "FAILED")
    open(out, "w").write("\n".join(parts))
    return 0 if ok_all else 1


def normalize(text):
    out = []
    for line in text.splitlines():
        if line.startswith("T1MISS "):
            if out and out[-1] == "<T1MISS rows>":
                continue
            out.append("<T1MISS rows>")
        elif TIMING.search(line):
            out.append(re.sub(r"\s+", " ", re.sub(r"\d+", "#", line)))
        else:
            out.append(line)
    return out


def compare(a, b):
    na = normalize(open(a).read())
    nb = normalize(open(b).read())
    diff = list(difflib.unified_diff(na, nb, a, b, lineterm="", n=1))
    print("\n".join(diff) if diff else "IDENTICAL after masking timing numbers")
    return 1 if diff else 0


if __name__ == "__main__":
    if len(sys.argv) in (4, 5) and sys.argv[1] == "run":
        sys.exit(run(*sys.argv[2:]))
    if len(sys.argv) == 4 and sys.argv[1] == "compare":
        sys.exit(compare(sys.argv[2], sys.argv[3]))
    sys.exit(__doc__)
