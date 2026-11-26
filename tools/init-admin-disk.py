"""Create a new installation disk, optionally copying a legacy Notes volume."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--from-legacy", type=Path)
args = parser.parse_args()
data = bytearray(args.from_legacy.read_bytes() if args.from_legacy else bytes(65536))
offset = 63 * 512
if args.from_legacy and len(data) >= 8192 and len(data) % 512 == 0 and len(data) < 65536:
    data.extend(bytes(65536 - len(data)))
if len(data) < offset + 512 or any(data[offset:offset + 512]):
    parser.error("credential sector unavailable or already initialized")
record = b"QAINIT01" + os.urandom(16) + bytes(456)
data[offset:offset + 512] = record + hashlib.blake2b(record, digest_size=32).digest()
with tempfile.NamedTemporaryFile(dir=args.output.parent) as temp:
    temp.write(data)
    temp.flush()
    os.fsync(temp.fileno())
    os.link(temp.name, args.output)
