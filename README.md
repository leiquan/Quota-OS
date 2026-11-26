# Quota OS CLI

CLI runs the ARM64 quota kernel and a PL011 serial shell on QEMU virt (one Cortex-A35 core). It links no LVGL renderer and reserves no framebuffer. A Tier 1 timer uses a fixed CPU slot and kernel-resident storage. The shell uses Tier 3.

## Build and boot

Install QEMU and an AArch64 GNU cross toolchain, or set QUOTA_LLVM_BIN to the directory containing clang, llvm-nm and llvm-objcopy and QUOTA_LLD to ld.lld.

```sh
make
bash tools/run-cli.sh
```

To supply the optional Test executable and a persistent note disk:

```sh
# Use a new disk path for the first boot. Reuse the same file for later boots.
test -e notes.raw || dd if=/dev/zero of=notes.raw bs=512 count=16
CLI_TEST=1 CLI_DISK=notes.raw bash tools/run-cli.sh
```

At the prompt, use `help`, `mem`, `tasks`, `quota` or `events`. Use `write TEXT` to store one note, wait for `Saved`, and use `cat Note.txt` after reboot. The CLI uses the kernel's Notes block service. `ls` lists the saved note.

`run Test` starts the external EL0 sample in Tier 3. `run Test t2` requests a 400 µs minimum per 10 ms period and prints observed grants and usage in counter ticks. Edit `cli/test.c` and run `make`. The cross compiler builds ARM64 machine code for QEMU, with no application packaging step. Test executes only when requested.

## Tested memory configurations

The default is 4 MiB. The kernel reserves 2 MiB and reports 2 MiB available for application admission. The EL0 loader currently supports two process slots and one code page per program. The admission capacity alone does not determine how many programs can run concurrently.

Both modes of Test completed on single-core Cortex-A35 QEMU with 3, 4, 8, 16, 17, 18, 20, 24 and 32 MiB. At 2 MiB QEMU could not place the device tree.

[Shared documentation and validation logs · 共享文档与验收日志](docs/README.md)
