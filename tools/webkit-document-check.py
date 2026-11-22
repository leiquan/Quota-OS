"""Cold-boot the document loader with distinct files and both size boundaries."""
import re
import subprocess
import tempfile
import time
from pathlib import Path

launcher = Path(__file__).resolve().with_name("run-webkit.sh")
with tempfile.TemporaryDirectory() as tmp:
    page = Path(tmp) / "test, page.html"
    log = Path(tmp) / "guest.log"
    for payload in (b"<p>Hello</p>", "<h1>配额</h1>".encode(), b"x" * 16384, b"x" * 16385, b""):
        page.write_bytes(payload)
        with log.open("wb") as stream:
            proc = subprocess.Popen([launcher, "--page-bytes", page, "-display", "none"],
                                    stdout=stream, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    result = log.read_text(errors="replace")
                    if "[webkit] done" in result or proc.poll() is not None:
                        break
                    time.sleep(0.05)
                assert "[webkit] done" in result, result[-1200:]
                if 0 < len(payload) <= 16384:
                    checksum = 2166136261
                    for byte in payload:
                        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
                    m = re.search(r"\[document\] loaded bytes=(\d+) fnv1a=(?:0x)?([0-9a-f]+)", result)
                    assert m and (int(m[1]), int(m[2], 16)) == (len(payload), checksum), result
                else:
                    assert "[document] rejected" in result and "[document] loaded" not in result
                print(f"{len(payload):5d} bytes PASS", flush=True)
            finally:
                proc.terminate()
                proc.wait(timeout=5)
