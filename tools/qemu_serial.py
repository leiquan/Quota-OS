#!/usr/bin/env python3
"""Drive Quota OS over the QEMU serial console.  Shared by the capture scripts."""
import os
import select
import subprocess
import time

BASE = [
    "qemu-system-aarch64",
    "-machine", "virt,gic-version=2",
    "-cpu", "cortex-a72",
    "-nographic", "-nic", "none",
]


class Session:
    def __init__(self, kernel, mem="128M", extra=None, machine=None):
        cmd = list(BASE)
        if machine:
            cmd[cmd.index("virt,gic-version=2")] = machine
        cmd += ["-m", mem, "-kernel", kernel] + list(extra or [])
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.buf = b""
        self.pos = 0

    def _pull(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            r, _, _ = select.select([self.proc.stdout], [], [], min(0.1, max(0, deadline - time.monotonic())))
            if not r:
                continue
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                return False
            self.buf += chunk
        return True

    def wait(self, token, limit):
        token_b = token.encode()
        deadline = time.monotonic() + limit
        drained = False
        while True:
            idx = self.buf.find(token_b, self.pos)
            if idx >= 0:
                self.pos = idx + len(token_b)
                return True
            remaining = deadline - time.monotonic()
            if drained or remaining <= 0:
                return False
            drained = not self._pull(min(remaining, 0.2))

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def run(self, steps):
        """steps: list of (reply, token, limit). Send reply, then wait for token."""
        ok = self.wait("select> ", 30)
        for reply, token, limit in steps:
            if not ok:
                break
            if reply is not None:
                self.send(reply)
            if token:
                ok = self.wait(token, limit)
        return ok

    def close(self):
        try:
            self.send("\x01x")
        except (BrokenPipeError, OSError):
            pass
        self._pull(0.3)
        self.proc.kill()
        self.proc.wait(timeout=5)
        return self.text()

    def text(self):
        return self.buf.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
