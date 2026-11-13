#!/usr/bin/env python3
"""Functional regression for the boot menu (commit A: 2259e51 menus 1-8 and 10).

  run:     python3 tools/menu_regress.py run <kernel.bin> <out.txt> [gic-version]
  compare: python3 tools/menu_regress.py compare <a.txt> <b.txt>

`compare` masks timing numbers (latency, hits, jiffies, recv_avg, ...) and
the per-period sample rows, then diffs everything else byte for byte:
admission results, REFUSED texts, resource tables and the menu flow.
"""
import difflib
import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from qemu_serial import Session  # noqa: E402

P = "select> "
BIG = [
    ("1\n", P, 20), ("4\n", P, 30), ("5\n", P, 10), ("4\n", P, 30), ("5\n", P, 10),
    ("6\n", P, 30),
    ("2\n", "app> ", 10), ("1\n", "allow> ", 10), ("n\n", P, 10),
    ("2\n", "app> ", 10), ("1\n", "allow> ", 10), ("y\n", P, 10),
    ("2\n", "app> ", 10), ("2\n", "allow> ", 10), ("y\n", P, 10),
    ("2\n", "app> ", 10), ("3\n", "allow> ", 10), ("y\n", P, 10),
    ("3\n", "close> ", 10), ("1\n", P, 10),
    ("2\n", "app> ", 10), ("3\n", "allow> ", 10), ("y\n", P, 10),
    ("1\n", P, 10), ("8\n", P, 15), ("7\n", P, 10),
    ("10\n", P, 10),
    ("3\n", "close> ", 10), ("a\n", P, 10),
    ("3\n", P, 10),
    ("10\n", P, 60), ("8\n", P, 10), ("1\n", P, 10),
    ("x\n", P, 10), ("h\n", P, 10),
]
SMALL = [
    ("1\n", P, 20), ("2\n", "app> ", 10), ("1\n", "allow> ", 10), ("y\n", P, 10),
    ("2\n", "app> ", 10), ("4\n", P, 10), ("2\n", "app> ", 10), ("5\n", P, 10), ("1\n", P, 10),
]

TIMING = re.compile(
    r"^\[boot\] (x0=|DTB at|CNTFRQ|timer latency|timer IRQ)|内核调度最长|hits=|jiffies|"
    r"summary:|window misses|jobs=|throttled|recv_avg|max_recv=|^app-\d\d |^TOTAL |^结果:|^截止期:|^T1STAT")
SAMPLE = re.compile(r"^\s+\d+\s+\d+\s+\d+\s+\d+\s+(OK|MISS)$")


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
        if SAMPLE.match(line) or line.startswith("T1JUMP "):
            tag = "<samples>" if SAMPLE.match(line) else "<clock jumps>"
            if out and out[-1] == tag:
                continue
            out.append(tag)
        elif line.startswith("T1HIST "):
            out.append(" ".join(line.split()[:2]) + " <histogram>")
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
