# CLI read-only ext4 root disk

The CLI still boots the kernel via QEMU's `-kernel`. Attach a raw ext4 volume as a second logical filesystem if you also use Notes. The runner exposes the root drive as read-only, and the guest submits only VirtIO block reads to that drive. The existing writable Notes device keeps its fixed-slot format and its `QUOTA-NOTES` device identity.

## Build and run

From the CLI checkout, with the existing guest toolchain, QEMU and e2fsprogs on PATH:

```sh
bash tools/build-cli.sh
bash tools/make-cli-root-disk.sh build/cli-root.ext4
dd if=/dev/zero of=build/cli-ext4.notes bs=1024 count=1024
CLI_TEST=0 CLI_ROOT_DISK="$PWD/build/cli-root.ext4" \
  CLI_DISK="$PWD/build/cli-ext4.notes" bash tools/run-cli.sh --quiet
```

Create the scratch Notes disk once. Reuse it for the second boot. At the first prompt, use `rootls`, `rootcat /hello.txt`, `run /Test.elf`, `write ext4-note`, and `events`. Shut down QEMU after `Saved`. Run the same launch command again and use `cat Note.txt`, `run /Test.elf`, and `events`. `CLI_TEST=0` disables the existing QEMU Test preload so `Test: EL0 completed` after `run /Test.elf` comes from the disk ELF. With the preload enabled, `run test.c` and `run Test t2` retain their usual paths. Omit `--quiet` to capture the boot memory ledger and use `mem` for application budget. The fixture generator records the installed e2fsprogs version, selected features and image hash. New fixture builds can have different filesystem UUIDs and timestamps.

## Supported profile

- Clean, single-volume ext4 with 4 KiB blocks, 32-byte group descriptors, 128–4096 byte power-of-two inode sizes and in-inode extent leaves. The fixture uses 256-byte inodes and the `filetype,extent` profile.
- Flat directory enumeration and root-level regular files. `rootcat` reads up to 4 KiB. The shared reader also supports bounded reads at file offsets. Missing files and unsupported layouts report distinct errors.
- ELF64 little-endian AArch64 static executables for the Quota OS EL0 console ABI. The loader reads the program headers and read-only load segments directly from the ext4 file. Entry is VA 0 and the load image must fit in the existing 4 KiB EL0 code page. Stack and console services use the existing process manager. ELF with dynamic linking, writable load segments or a different ABI needs its own loader. The Web UI Linux runtime loader and its demand paging are separate from the CLI console ABI.
- The reader rejects recovery-required volumes, unsupported incompatibility and read-only-compatibility feature bits, indexed root directories, multi-level extent trees and unwritten extents. No recursive traversal, ext4 writes, journal replay, or disk boot is provided.
- Read-only VirtIO transport has a single Tier 3 caller and at most one new sector request per scheduler period. UI integration should use a Tier 3 reader so a device wait cannot block a higher-tier frame callback.

CLI smoke results apply to QEMU virt, one Cortex-A35, 4 MiB, and the runner's `-icount shift=4,sleep=off`. GUI integration and frame deadlines require separate evidence.
