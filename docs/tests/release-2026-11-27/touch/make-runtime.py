"""Derive the Touch runtime volume from the verified Web UI volume."""
import hashlib
from pathlib import Path
import struct
import sys

source_path, output_path = map(Path, sys.argv[1:])
web_hash = "8ea60812cd649d6bc4ba72526e21624b3b3de17815d762b13e5c2689950e8cea"
touch_hash = "94f82a26b26699ef02e3dfc4982328ff9b0fa02c07f8d9f0a6f7f605c447324a"
header = struct.Struct("<8sIIQ")
entry = struct.Struct("<112sQQ")
preload_name = b"page-preload.js".ljust(112, b"\0")
scripts = Path("os/port/wap")
preload = (b"(" + (scripts / "touch-page.js").read_bytes()
           + b")(" + (scripts / "rpc.js").read_bytes() + b")")


def aligned(offset):
    return (offset + 4095) & ~4095


with source_path.open("rb") as source:
    if hashlib.file_digest(source, "sha256").hexdigest() != web_hash:
        raise ValueError("Unexpected Web UI runtime hash")
    source.seek(0)
    magic, version, count, size = header.unpack(source.read(header.size))
    if (magic, version, size) != (b"QRTVOL01", 1, source_path.stat().st_size):
        raise ValueError("Unexpected runtime volume format")
    entries = [entry.unpack(source.read(entry.size)) for _ in range(count)]
    if sum(name == preload_name for name, _, _ in entries) != 1:
        raise ValueError("Expected exactly one page preload")
    next_offset = aligned(header.size + entry.size * count)
    plan = []
    for name, before, length in entries:
        if name == preload_name:
            length = len(preload)
        plan.append((name, before, next_offset, length))
        next_offset = aligned(next_offset + length)
    with output_path.open("xb") as output:
        output.write(header.pack(magic, version, count, next_offset))
        for name, _, offset, length in plan:
            output.write(entry.pack(name, offset, length))
        for name, before, offset, length in plan:
            output.seek(offset)
            if name == preload_name:
                output.write(preload)
                continue
            source.seek(before)
            while length:
                data = source.read(min(length, 1024 * 1024))
                if not data:
                    raise ValueError("Unexpected end of runtime volume")
                output.write(data)
                length -= len(data)
        output.truncate(next_offset)
with output_path.open("rb") as result:
    digest = hashlib.file_digest(result, "sha256").hexdigest()
if digest != touch_hash:
    raise ValueError(f"Unexpected Touch UI runtime hash {digest}")
print(f"{digest}  {output_path}")
