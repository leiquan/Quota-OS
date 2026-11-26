"""Emit the transitive ARM64 ELF dependencies for the guest runtime file table."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def needed(readelf, elf):
    header = elf.open("rb")
    with header:
        ident = header.read(20)
    if ident[:6] != b"\x7fELF\x02\x01" or ident[18:20] != b"\xb7\x00":
        raise ValueError(f"expected a little-endian AArch64 ELF at {elf}")
    tags = subprocess.check_output([readelf, "-d", elf], text=True)
    return re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", tags)


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("executable", type=Path)
    args.add_argument("output", type=Path)
    args.add_argument("directories", type=Path, nargs="+")
    args.add_argument("--clang", default="clang")
    args.add_argument("--lld", default="lld")
    args.add_argument("--strip-with-objcopy", metavar="OBJCOPY",
                      help="strip non-runtime symbols from staged ELF copies")
    args.add_argument("--strip-flag", default="--strip-unneeded",
                      choices=("--strip-unneeded", "--strip-sections", "--strip-section-headers"))
    args.add_argument("--readelf", default="llvm-readelf")
    args.add_argument("--blink-renderer", type=Path,
                      help="also embed the ARM64 Blink page process and its required libraries")
    assets = args.add_mutually_exclusive_group()
    assets.add_argument("--v8-data", type=Path,
                        help="include the system V8 snapshot and ICU data from this build")
    assets.add_argument("--electron-assets", type=Path,
                      help="include runtime data from the matching Electron distribution")
    args.add_argument("--embedded-interpreter", type=Path,
                      help="reuse the interpreter already embedded by wap-worker.S")
    args.add_argument("--system-font", type=Path,
                      help="TrueType system font exposed to the guest page renderer")
    options = args.parse_args()
    libraries = {}
    pending = [options.executable]
    dependencies = {}
    if options.blink_renderer:
        if not options.v8_data:
            args.error("--blink-renderer requires --v8-data from the joint build")
        libraries["quota-blink"] = options.blink_renderer.resolve()
        pending.append(options.blink_renderer)
    while pending:
        elf = pending.pop().resolve()
        dependencies[elf] = needed(options.readelf, elf)
        for name in dependencies[elf]:
            if not re.fullmatch(r"[A-Za-z0-9._+-]+", name):
                raise ValueError(f"invalid dependency name {name!r}")
            if name in libraries:
                continue
            path = next((directory / name for directory in options.directories
                         if (directory / name).is_file()), None)
            if not path:
                raise FileNotFoundError(f"missing target dependency {name}")
            libraries[name] = path.resolve()
            pending.append(path)

    if options.blink_renderer:
        expected_v8 = (options.v8_data / "libv8.so").resolve(strict=True)
        if libraries.get("libv8.so") != expected_v8:
            raise ValueError("joint build must provide the resolved libv8.so")
        for executable in (options.executable, options.blink_renderer):
            seen = set()
            work = [executable.resolve()]
            while work:
                elf = work.pop()
                if elf in seen:
                    continue
                seen.add(elf)
                work.extend(libraries[name] for name in dependencies[elf])
            if expected_v8 not in seen:
                raise ValueError(f"{executable} has no dependency on the joint libv8.so")

    data_dir = options.v8_data or options.electron_assets
    if data_dir:
        names = ["icudtl.dat", "snapshot_blob.bin"]
        for name in names:
            path = data_dir / name
            if not path.is_file():
                raise FileNotFoundError(path)
            libraries[name] = path.resolve()

    if options.electron_assets or options.blink_renderer:
        if not options.system_font or not options.system_font.is_file():
            args.error("the page renderer requires a readable --system-font")
        libraries["fonts/System-Regular.ttf"] = options.system_font.resolve()
        libraries["fonts.conf"] = Path(__file__).resolve().parents[1] / "os/port/fonts/fonts.conf"

    if options.blink_renderer:
        port = Path(__file__).resolve().parents[1] / "os" / "port"
        core = port / "wap/rpc.js"
        backend = port / "node/rpc-backend.cjs"
        preload = options.output.parent / "quota-node-preload.cjs"
        preload.write_bytes(core.read_bytes() + backend.read_bytes())
        libraries["preload.cjs"] = preload.resolve()
        page_preload = options.output.parent / "quota-page-preload.js"
        pack = options.v8_data / "gen/third_party/blink/public/resources/blink_resources.pak"
        libraries["blink_resources.pak"] = pack.resolve(strict=True)
        # Blink calls this factory with its native writePacket and readPacket functions.
        touch = port / "wap/touch-page.js"
        page_preload.write_bytes(b"(" + touch.read_bytes() + b")(" + core.read_bytes() + b")")
        libraries["page-preload.js"] = page_preload.resolve()

    if options.electron_assets:
        for name in ("v8_context_snapshot.bin",
                     "resources.pak", "chrome_100_percent.pak", "chrome_200_percent.pak",
                     "locales/en-US.pak",):
            path = options.electron_assets / name
            if not path.is_file():
                raise FileNotFoundError(path)
            libraries[name] = path.resolve()

        shell = Path(__file__).resolve().parents[1] / "os/port/electron"
        adapter = options.output.parent / "libquota-entropy.so"
        subprocess.run([options.clang, "--target=aarch64-linux-gnu", "-fPIC", "-shared",
                        "-nostdlib", "-fno-builtin", "-O2", f"-fuse-ld={options.lld}",
                        str(shell / "entropy-fd.c"), "-o", str(adapter)], check=True)
        libraries["libquota-entropy.so"] = adapter.resolve()
        libraries["preload.cjs"] = shell / "preload.cjs"
        for name in ("main.cjs", "package.json"):
            libraries["resources/app/" + name] = shell / name

    assembly = ['.section .rodata.wap_libs,"a",%progbits']
    entries = []
    for i, (name, path) in enumerate(sorted(libraries.items())):
        if options.embedded_interpreter and path.samefile(options.embedded_interpreter):
            assembly.append(f'dso_name_{i}: .asciz "{name}"')
            entries.append(f'.quad dso_name_{i},wap_interp_start,wap_interp_end')
            continue
        if options.strip_with_objcopy and path in dependencies:
            elf_dir = options.output.parent / "runtime-elf"
            elf_dir.mkdir(exist_ok=True)
            staged = (elf_dir / name).resolve()
            subprocess.run([options.strip_with_objcopy, options.strip_flag, path, staged], check=True)
            path = staged
        assembly.extend([f'dso_name_{i}: .asciz "{name}"', '.balign 4096',
                         f'dso_{i}: .incbin {json.dumps(str(path), ensure_ascii=False)}',
                         f'dso_end_{i}:'])
        entries.append(f'.quad dso_name_{i},dso_{i},dso_end_{i}')
    assembly.extend(['.balign 8', '.global wap_libs_start,wap_libs_end',
                     'wap_libs_start:', *entries, 'wap_libs_end:'])
    options.output.write_text("\n".join(assembly) + "\n")


if __name__ == "__main__":
    main()
