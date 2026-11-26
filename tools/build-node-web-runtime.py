"""Build the ARM64 Node Web runtime in a prepared Chromium 142 source tree."""
import argparse
import json
import os
import shutil
from pathlib import Path
import subprocess


def link(source, target):
    if target.exists() or target.is_symlink():
        if target.resolve() != source.resolve():
            raise ValueError(f"existing {target} does not refer to {source}")
    else:
        target.symlink_to(source.resolve())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("chromium", type=Path)
    parser.add_argument("node", type=Path, help="patched Node 22.21.1 source")
    parser.add_argument("output", type=Path)
    parser.add_argument("--sysroot", required=True, type=Path)
    parser.add_argument("--mac-sdk", type=Path)
    parser.add_argument("--gn", default="gn")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    root = args.chromium.resolve(strict=True)
    output = args.output.resolve()
    repo = Path(__file__).resolve().parents[1]
    integration = root / "third_party/blink/quota_runtime"
    (integration / "include").mkdir(parents=True, exist_ok=True)
    node_root = args.node.resolve(strict=True)
    link(node_root, root / "node")
    entry = node_root / "src/quota_node_main.cc"
    source = repo / "port/node/main.cc"
    if not entry.is_file() or entry.read_bytes() != source.read_bytes():
        link(source, entry)
    link(repo / "port/blink", integration / "window")
    link(repo / "include/guest_page_services.h", integration / "include/guest_page_services.h")
    node_scripts = ["node.gni", "unofficial.gni", "src/inspector/unofficial.gni"]
    node_scripts += [f"deps/{name}/unofficial.gni" for name in (
        "ada", "llhttp", "ncrypto", "nghttp2", "simdjson", "nbytes", "simdutf", "brotli",
        "sqlite", "ngtcp2", "uv", "uvwasi", "cares", "zstd", "googletest", "openssl", "histogram")]
    dotfile = root / ".quota-node-web.gn"
    dotfile.write_text((root / ".gn").read_text() + "\nexec_script_allowlist += " +
                       json.dumps(["//node/" + name for name in node_scripts]) + "\n")
    settings = dict(target_os="linux", target_cpu="arm64", is_debug=False,
                    is_component_build=True, symbol_level=0, use_remoteexec=False,
                    use_clang_modules=False, clang_use_chrome_plugins=False,
                    target_sysroot=str(args.sysroot.resolve(strict=True)),
                    v8_enable_javascript_promise_hooks=True, use_glib=False, use_gio=False,
                    use_gtk=False, ozone_auto_platforms=False, ozone_platform_headless=True,
                    node_v8_path="//v8", node_simdutf_path="//node/deps/simdutf",
                    node_enable_inspector=False)
    if args.mac_sdk:
        settings["mac_sdk_path"] = str(args.mac_sdk.resolve(strict=True))
    target = "third_party/blink/quota_runtime/window:runtime"
    subprocess.run([args.gn, "gen", str(output), "--root=.", f"--dotfile={dotfile.name}",
                    "--root-target=//" + target,
                    "--args=" + "\n".join(f"{k} = {json.dumps(v)}" for k, v in settings.items())],
                   cwd=root, check=True)
    if not args.generate_only:
        jobs = ["-j", str(args.jobs)] if args.jobs else []
        # Node's config.gypi generator also invokes gn by name.
        gn_bin = Path(shutil.which(args.gn) or args.gn).resolve(strict=True)
        env = os.environ.copy()
        env.update(QUOTA_GN_DOTFILE=str(dotfile), QUOTA_GN_TARGET="//" + target)
        env["PATH"] = str(gn_bin.parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([args.ninja, "-C", str(output), *jobs, target], check=True, env=env)


if __name__ == "__main__":
    main()
