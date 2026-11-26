"""Build Todo and create a fresh, independent Quota OS demonstration disk."""
import argparse
import io
import re
import struct
import subprocess
import zipfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path, help="new directory for todo.wap and todo.raw")
parser.add_argument("--terser", default="terser", help="path to the Terser CLI")
args = parser.parse_args()
source = Path(__file__).resolve().parent

def minify(script):
    return subprocess.run([args.terser, "--compress", "--mangle", "--toplevel"],
                          input=script, capture_output=True, text=True, check=True).stdout.strip()

html = (source / "app.html").read_text()
html = re.sub(r"<script>(.*?)</script>", lambda m: "<script>" + minify(m[1]) + "</script>",
              html, flags=re.S)
html = re.sub(r">\s+<", "><", html).strip()
html = re.sub(r"<style>(.*?)</style>", lambda m: "<style>" + m[1].replace("}\n", "}").rstrip() + "\n</style>", html, flags=re.S)
package = io.BytesIO()
with zipfile.ZipFile(package, "w", zipfile.ZIP_STORED) as archive:
    for name, data in [("app.json", (source / "app.json").read_text().strip()),
                       ("app.html", html), ("app.js", minify((source / "app.js").read_text()))]:
        archive.writestr(zipfile.ZipInfo(name), data.encode())
payload = package.getvalue()
if len(payload) > 4080:
    raise ValueError(f"{len(payload)} bytes exceed the guest package slot")
generation = 1
checksum = ((2166136261 ^ generation) * 16777619) & 0xffffffff
for byte in payload:
    checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
record = struct.pack("<4I", 0x51415031, generation, len(payload), checksum) + payload
disk = bytearray(65536)
offset = 18 * 512 if len(payload) > 2032 else 8 * 512
disk[offset:offset + len(record)] = record
args.output.mkdir(parents=True, exist_ok=False)
(args.output / "todo.wap").write_bytes(payload)
(args.output / "todo.raw").write_bytes(disk)
print(f"Created todo.wap ({len(payload)} bytes) and todo.raw")
