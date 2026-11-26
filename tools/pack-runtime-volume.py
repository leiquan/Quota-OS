"""Move staged Web-Node binaries into an indexed, page-aligned volume."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys

build, output = map(Path, sys.argv[1:])
assembly = (build / "wap-libraries.S").read_text()
files = {"node": build / "wap-worker.elf"}
for index, name in re.findall(r'^dso_name_(\d+): .asciz "([^"]+)"$', assembly, re.M):
    embedded = re.search(rf'^dso_{index}: .incbin (.+)$', assembly, re.M)
    if embedded:
        files[name] = build.parent / Path(json.loads(embedded[1]))
    elif f".quad dso_name_{index},wap_interp_start,wap_interp_end" in assembly:
        files[name] = build / "wap-interp.elf"
    else:
        raise ValueError(name)

header = struct.Struct("<8sIIQ")
entry = struct.Struct("<112sQQ")
def aligned(value):
    return (value + 4095) & ~4095

offset = aligned(header.size + entry.size * len(files))
entries = []
for name, path in sorted(files.items()):
    name = name.encode("ascii")
    if not name or len(name) >= 112:
        raise ValueError(name)
    size = path.stat().st_size
    entries.append((name, offset, size, path))
    offset = aligned(offset + size)
with output.open("wb") as volume:
    volume.write(header.pack(b"QRTVOL01", 1, len(entries), offset))
    for name, start, size, _ in entries:
        volume.write(entry.pack(name, start, size))
    for _, start, _, path in entries:
        volume.seek(start)
        with path.open("rb") as source:
            shutil.copyfileobj(source, volume)
    volume.truncate(offset)
with output.open("rb") as volume:
    assert header.unpack(volume.read(header.size)) == (b"QRTVOL01", 1, len(entries), offset)
    records = [entry.unpack(volume.read(entry.size)) for _ in entries]
    for record, (name, start, size, path) in zip(records, entries):
        assert record == (name.ljust(112, b"\0"), start, size)
        volume.seek(start)
        remaining = size
        digest = hashlib.sha256()
        with path.open("rb") as source:
            while remaining:
                data = source.read(min(remaining, 1048576))
                assert data and data == volume.read(len(data)), name
                digest.update(data)
                remaining -= len(data)
        if name in (b"node", b"quota-blink", b"libv8.so"):
            print(name.decode(), size, digest.hexdigest())
print(f"Verified {len(entries)} files, {offset} volume bytes")
