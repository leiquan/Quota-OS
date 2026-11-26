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

Create the scratch Notes disk once. Reuse it for the second boot. At the first prompt, use `ls`, `rootcat /hello.txt`, `run /Test.elf`, `write ext4-note`, and `events`. Shut down QEMU after `Saved`. Run the same launch command again and use `cat Note.txt`, `run /Test.elf`, and `events`. `CLI_TEST=0` disables the existing QEMU Test preload so `Test: EL0 completed` after `run /Test.elf` comes from the disk ELF. With the preload enabled, `run test.c` and `run Test t2` retain their usual paths. Omit `--quiet` to capture the boot memory ledger and use `mem` for application budget. The fixture generator records the installed e2fsprogs version, selected features and image hash. New fixture builds can have different filesystem UUIDs and timestamps.

## Supported profile

- Clean, single-volume ext4 with 4 KiB blocks, 32-byte group descriptors, 128–4096 byte power-of-two inode sizes and in-inode extent leaves. The fixture uses 256-byte inodes and the `filetype,extent` profile.
- Flat directory enumeration and root-level regular files. `rootcat` reads up to 4 KiB. The shared reader also supports bounded reads at file offsets. Missing files and unsupported layouts report distinct errors.
- ELF64 little-endian AArch64 static executables for the Quota OS EL0 console ABI. The loader reads the program headers and read-only load segments directly from the ext4 file. Entry is VA 0 and the load image must fit in the existing 4 KiB EL0 code page. Stack and console services use the existing process manager. ELF with dynamic linking, writable load segments or a different ABI needs its own loader. The Web UI Linux runtime loader and its demand paging are separate from the CLI console ABI.
- The reader rejects recovery-required volumes, unsupported incompatibility and read-only-compatibility feature bits, indexed root directories, multi-level extent trees and unwritten extents. No recursive traversal, ext4 writes, journal replay, or disk boot is provided.
- Read-only VirtIO transport has a single Tier 3 caller and at most one new sector request per scheduler period. UI integration should use a Tier 3 reader so a device wait cannot block a higher-tier frame callback.

CLI smoke results apply to QEMU virt, one Cortex-A35, 4 MiB, and the runner's `-icount shift=4,sleep=off`. GUI integration and frame deadlines require separate evidence.

## Phase 1 writable test volume

Copy the fixture before enabling writes. The generator adds a preallocated
17-byte `scratch.txt`. Notes can be attached alongside the root disk.

```sh
cp build/rw-root.ext4 build/rw-test.ext4 # use a fresh copy for each run
dd if=/dev/zero of=build/rw-notes.raw bs=1024 count=1024
CLI_RAM=4M CLI_TEST=0 CLI_ROOT_RW=1 CLI_ROOT_DISK="$PWD/build/rw-test.ext4" \
  CLI_DISK="$PWD/build/rw-notes.raw" bash tools/run-cli.sh --quiet
```

Generate `build/rw-root.ext4` with
`bash tools/make-cli-root-disk.sh build/rw-root.ext4` if you are building locally.
At the prompt, run `mem`, `rootcat scratch.txt`,
`rootwrite scratch.txt after--ext4-write`, `rootcat scratch.txt`,
`run /Test.elf`, `write rw-notes-marker`, and `events`.
After both saves report success, stop QEMU and use the same launch command
with the same disk paths. Check both `rootcat scratch.txt` and
`cat Note.txt`, then check `events`. Run `e2fsck -fn` on the resulting
root image after QEMU exits.

The writable profile requires a clean, trusted single-group test volume with
only `filetype,extent`. The existing file must have the same nonzero byte
length as the text and occupy one initialized 4 KiB block. Shell line length
also limits the text. Only that block is updated, and `ext4 saved` is printed
after VirtIO FLUSH succeeds. Creation, resizing, deletion and renaming
require a later phase. An I/O error disables subsequent root I/O for the
boot. Data may be partially changed if interrupted or if a write or flush
fails. Crash recovery and atomic replacement require a journal.
Without `CLI_ROOT_RW=1` the runner attaches the root drive read-only.

## Phase 2 journaled test volume

Build the CLI and generate a fresh fixture using e2fsprogs. Keep the
fixture itself immutable and test on copies:

```sh
bash tools/build-cli.sh
CLI_ROOT_JOURNAL=1 bash tools/make-cli-root-disk.sh build/cli-root-journal.ext4
python3 tools/check-cli-ext4-recovery.py build/cli-root-journal.ext4 build/phase2-qa
```

For manual tests, follow the Phase 1 launch command with a copy of the
journaled image and `CLI_ROOT_RW=1`. `rootwrite FILE TEXT` creates a file
or replaces the content of an existing file, and `rootrm FILE` removes
it. A successful replacement writes a fresh data block and flushes it
before publishing the metadata transaction. The shell reports success
after the journal commit, home-block checkpoint, and retirement flushes.

The writable journaled profile is a trusted single-group ext4 image with
4 KiB blocks, 256-byte inodes, `filetype,extent,has_journal`, a one-block
linear root directory, and the contiguous internal inode-8 JBD2 journal
created by the fixture script. The writer handles nonempty single-block
regular files with one link and no external attributes. Text length is
also limited by the CLI input line. Free space must exist for the fresh
data block even during a same-size replacement. The writer uses one
transaction at a time, at most six metadata blocks, and one VirtIO
request in flight. The journal format uses 32-bit destinations, one
descriptor and commit, and no journal checksums or fast commits. A
clean journal can advertise revoke, but pending revoke records and
multi-transaction logs are outside this recovery profile. Clean Linux
mounts are supported for the tested profile. Arbitrary dirty Linux
volumes need Linux recovery. Writable startup refuses incompatible
layouts. With write opt-in disabled, a dirty journal is refused
without replay. The existing no-journal Phase 1 disk still supports its
separate equal-length overwrite operation. Notes keeps its own disk.

The crash harness enables `CLI_ROOT_CRASH_TEST=1`, arms `rootcut` with
`data`, `log`, `commit`, or `checkpoint`, and kills the waiting QEMU
process at the matching serial marker. These points follow the data
flush, the descriptor and journal payload flush, the commit flush, and
a forced flush of just the first home block respectively. Remove has
no data-write point. A fresh 4 MiB guest remounts the same image and
checks complete old or new content as appropriate, free block and inode
counts, and quota events. Fault tests model QEMU loss after explicit
barriers. They do not simulate torn sectors or storage that falsely
acknowledges flushes. The script also checks clean shutdown, Notes,
EL0, Tier 2 and refusal cases, and saves serial and e2fsck logs.

For Linux interoperability, attach a scratch copy of a resulting image
as a block device to a Linux guest. Mount as ext4, read the Quota OS file,
write a different file, then sync and unmount. Start the 4 MiB CLI with
that same copy and read the Linux-written file. Repeat the Linux mount
with a copy of `replace-checkpoint-pre-recovery.ext4` to verify Linux
replays a Quota OS journal and reads the committed file. Inspect the
result with `e2fsck -fn` after the guest exits.
