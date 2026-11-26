#!/usr/bin/env python3
"""Exercise CLI lifecycle and deterministic JBD2 crash points on disk copies."""

import argparse
import os
from pathlib import Path
import select
import secrets
import shutil
import struct
import subprocess
import time
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BASELINE = "before-ext4-write"
UPDATED = "atomic-replacement-with-a-new-length"
CREATED = "new-file-data"
PASSWORD = secrets.token_hex(16)
COUNTERS = b"tier1_misses=0 tier2_short=0 tier3_over=0"


class Guest:
    def __init__(self, disk, log, notes=None, crash=False, preload=False, writable=True):
        self.log = log
        self.temporary = tempfile.TemporaryDirectory() if notes is None else None
        if self.temporary:
            notes = Path(self.temporary.name) / "notes.raw"
        if not notes.exists():
            subprocess.run(["python3", "tools/init-admin-disk.py", str(notes)], cwd=ROOT, check=True)
        environment = dict(
            os.environ, CLI_RAM="4M", CLI_ROOT_RW=str(int(writable)), CLI_ROOT_DISK=str(disk),
            CLI_DISK=str(notes), CLI_TEST=str(int(preload)),
            CLI_ROOT_CRASH_TEST=str(int(crash)),
        )
        self.process = subprocess.Popen(
            ["bash", "tools/run-cli.sh", "--quiet"], cwd=ROOT, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )

    def __enter__(self):
        try:
            login = b"admin | Password: "
            setup = b"admin | Set password: "
            opening = self.read((login, setup))
            assert b"quota> " not in opening
            if setup in opening:
                confirmation = b"admin | Confirm password: "
                self.command(PASSWORD, confirmation, end=confirmation)
                self.command(PASSWORD, login, end=login)
            self.command(PASSWORD, b"4096 KiB RAM")
        except BaseException:
            self.close()
            raise
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        if self.temporary:
            self.temporary.cleanup()

    def read(self, end, expected=None):
        markers = end if isinstance(end, tuple) else (end,)
        deadline = time.monotonic() + 20
        data = b""
        while not any(marker in data for marker in markers):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(("serial timeout", markers))
            if not select.select([self.process.stdout], [], [], remaining)[0]:
                continue
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                raise AssertionError(("guest exited", markers))
            data += chunk
        assert PASSWORD.encode() not in data, "Password echoed"
        self.log.write(data)
        self.log.flush()
        if expected is not None:
            assert expected in data, (expected, data)
        return data

    def command(self, text, expected, end=b"quota> "):
        self.process.stdin.write(text.encode() + b"\r")
        self.process.stdin.flush()
        return self.read(end, expected)

    def cut(self, operation, point):
        self.command("rootcut " + point, b"ext4 crash point armed")
        marker = ("[ext4-cut] " + point + "\r\n").encode()
        data = self.command(operation, marker, end=marker)
        assert b"ext4 saved" not in data and b"ext4 removed" not in data
        self.process.kill()
        self.process.wait(timeout=5)


def superblock(disk):
    with disk.open("rb") as image:
        image.seek(1024)
        block = image.read(1024)
    return (*struct.unpack_from("<II", block, 12), struct.unpack_from("<I", block, 96)[0])


def copy_disk(fixture, output, name):
    result = output / (name + ".ext4")
    shutil.copyfile(fixture, result)
    return result


def check_fs(disk, e2fsck):
    with disk.with_suffix(".fsck.log").open("wb") as log:
        subprocess.run([e2fsck, "-fn", str(disk)], stdout=log,
                       stderr=subprocess.STDOUT, check=True)


def check_lifecycle(fixture, output, e2fsck):
    disk = copy_disk(fixture, output, "lifecycle")
    notes = output / "notes.raw"
    with (output / "lifecycle.log").open("wb") as log:
        with Guest(disk, log, notes) as guest:
            guest.command("cat scratch.txt", b"\r\n" + BASELINE.encode() + b"\r\n")
            guest.command("write scratch.txt " + UPDATED, b"ext4 saved")
            guest.command("write created.txt " + CREATED, b"ext4 saved")
            guest.command("rm scratch.txt", b"ext4 removed")
            guest.command("rm missing.txt", b"not found")
            guest.command("note phase-two-notes", b"\r\nSaved\r\n")
            guest.command("run /Test.elf", b"Test: EL0 completed")
            guest.command("events", COUNTERS)
        with Guest(disk, log, notes, preload=True) as guest:
            guest.command("note", b"\r\nphase-two-notes\r\n")
            guest.command("cat created.txt", b"\r\n" + CREATED.encode() + b"\r\n")
            guest.command("cat scratch.txt", b"not found")
            result = guest.command("run Test t2", b"Test: EL0 completed")
            assert b"minimum=25000" in result
            guest.command("events", COUNTERS)
    check_fs(disk, e2fsck)


def check_cut(fixture, output, e2fsck, kind, operation, point):
    case = kind + "-" + point
    disk = copy_disk(fixture, output, case)
    committed = point in ("commit", "checkpoint")
    with (output / (case + ".log")).open("wb") as log:
        with Guest(disk, log, crash=True) as guest:
            guest.cut(operation, point)
        assert bool(superblock(disk)[2] & 4) == (point != "data")
        shutil.copyfile(disk, output / (case + "-pre-recovery.ext4"))
        with Guest(disk, log) as guest:
            if kind == "create":
                command, expected = "cat created.txt", CREATED if committed else None
            elif kind == "remove":
                command, expected = "cat scratch.txt", None if committed else BASELINE
            else:
                command, expected = "cat scratch.txt", UPDATED if committed else BASELINE
            output_bytes = b"not found" if expected is None else b"\r\n" + expected.encode() + b"\r\n"
            guest.command(command, output_bytes)
            guest.command("events", COUNTERS)
    blocks, inodes, flags = superblock(disk)
    baseline_blocks, baseline_inodes, _ = superblock(fixture)
    freed = (int(kind == "remove") - int(kind == "create")) if committed else 0
    assert (blocks, inodes) == (baseline_blocks + freed, baseline_inodes + freed)
    assert not flags & 4
    check_fs(disk, e2fsck)
    print(case, "PASS")


def check_refusal(fixture, output):
    disk = copy_disk(fixture, output, "unsupported")
    with disk.open("r+b") as image:
        image.seek(1024 + 100)
        image.write(struct.pack("<I", 0x400))  # metadata_csum is outside the writer profile.
    before = disk.read_bytes()
    with (output / "unsupported.log").open("wb") as log:
        with Guest(disk, log) as guest:
            guest.command("write scratch.txt " + UPDATED, b"ext4 mount failed")
            guest.command("events", COUNTERS)
    assert disk.read_bytes() == before
    dirty = output / "replace-commit-pre-recovery.ext4"
    before = dirty.read_bytes()
    with (output / "dirty-read-only.log").open("wb") as log:
        with Guest(dirty, log, writable=False) as guest:
            guest.command("ls", b"ext4 mount failed")
            guest.command("events", COUNTERS)
    assert dirty.read_bytes() == before
    print("mount refusal PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("output", type=Path, help="new directory for disk copies and logs")
    parser.add_argument("--e2fsck", default="e2fsck")
    args = parser.parse_args()
    fixture, output = args.fixture.resolve(), args.output.resolve()
    output.mkdir()
    check_lifecycle(fixture, output, args.e2fsck)
    print("lifecycle PASS")
    for kind, operation in (
        ("replace", "write scratch.txt " + UPDATED),
        ("create", "write created.txt " + CREATED),
        ("remove", "rm scratch.txt"),
    ):
        for point in ("data", "log", "commit", "checkpoint"):
            if kind != "remove" or point != "data":
                check_cut(fixture, output, args.e2fsck, kind, operation, point)
    check_refusal(fixture, output)


if __name__ == "__main__":
    main()
