#!/usr/bin/env python3
"""Drive the serial menu on 128MB and 64MB guests and write docs/demo-output.txt."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_serial import Session  # noqa: E402

P = "select> "
BIG = [
    ("1\n", P, 20),
    ("4\n", P, 30),                                    # priority, tiering ON
    ("5\n", P, 10), ("4\n", P, 30), ("5\n", P, 10),    # tiering OFF, priority, ON
    ("6\n", P, 30),                                    # containment
    ("2\n", "app> ", 10), ("1\n", "allow> ", 10), ("n\n", P, 10),   # deny Pixel
    ("2\n", "app> ", 10), ("1\n", "allow> ", 10), ("y\n", P, 10),   # Pixel
    ("2\n", "app> ", 10), ("2\n", "allow> ", 10), ("y\n", P, 10),   # Cinema
    ("2\n", "app> ", 10), ("3\n", "allow> ", 10), ("y\n", P, 10),   # Studio refused
    ("3\n", "close> ", 10), ("1\n", P, 10),                          # close Pixel
    ("2\n", "app> ", 10), ("3\n", "allow> ", 10), ("y\n", P, 10),   # Studio ok
    ("1\n", P, 10),
    ("8\n", P, 15),                                    # pressure
    ("7\n", P, 10),                                    # Cluster Sim
    ("3\n", "close> ", 10), ("a\n", P, 10),            # close all
    ("10\n", P, 60),                                   # 32 slices
    ("1\n", P, 10),
]
SMALL = [
    ("1\n", P, 20),
    ("2\n", "app> ", 10), ("1\n", "allow> ", 10), ("y\n", P, 10),   # Pixel fits
    ("2\n", "app> ", 10), ("4\n", P, 10),              # Neural Forge refused, no allow
    ("1\n", P, 10),
]

HEADER = """# Quota OS 演示实录

下面两段都是用 tools/capture_demos.py 驱动 qemu-system-aarch64 实际跑出来的串口记录，不是手写的预期输出。
计时来自客户机的通用定时器，不是脚本填的数字。

命令：

    qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M -nographic -nic none -kernel build/quotaos.bin
    qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 64M -nographic -nic none -kernel build/quotaos.bin

"""


def section(title, text):
    bar = "=" * 80
    return "%s\n%s\n%s\n%s\n" % (bar, title, bar, text)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    kernel = sys.argv[1] if len(sys.argv) > 1 else "build/quotaos.bin"
    out = sys.argv[2] if len(sys.argv) > 2 else "docs/demo-output.txt"
    results = []
    for title, mem, steps in (("场景 A：128MB（普通演示机）", "128M", BIG),
                              ("场景 B：64MB（-m 64M，模拟旧设备）", "64M", SMALL)):
        s = Session(kernel, mem)
        ok = s.run(steps)
        results.append((title, ok, s.close()))
        print(mem, "ok" if ok else "FAILED", flush=True)
    with open(out, "w") as f:
        f.write(HEADER)
        for title, _, text in results:
            f.write(section(title, text))
    print("wrote", out)
    if not all(ok for _, ok, _ in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
