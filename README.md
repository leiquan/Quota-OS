# Quota OS CLI

CLI runs the ARM64 quota kernel and a PL011 serial shell on QEMU virt (one Cortex-A35 core). It links no LVGL renderer and reserves no framebuffer. The fixed Tier 1 services retain the C UI edition's CPU and memory declarations. Their job counters measure scheduler service, not display frames.

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
CLI_RAM=17M CLI_MODULE=build/Test.cap CLI_DISK=notes.raw bash tools/run-cli.sh
```

At the prompt, use `help`, `mem`, `tasks`, `quota` or `events`. Use `write TEXT` to store one note, wait for `Saved`, and use `cat Note.txt` after reboot. The CLI uses the kernel's Notes block service. `ls` lists the saved note.

`run Test` starts the external EL0 sample in Tier 3. `run Test t2` requests a 400 µs minimum per 10 ms period and prints observed grants and usage in counter ticks. The module is staged by the QEMU loader and runs only on request.

## Tested memory configurations

With the QEMU icount virtual clock, the serial prompt responded at 3, 4, 8 and 16 MiB. Test was refused at these sizes. Both modes of Test completed at 17, 18, 20, 24 and 32 MiB. The kernel reserve was 2 MiB and Tier 1 declared 13 MiB. At 17 MiB, 480 KiB remained available for applications after cache-color padding. At 2 MiB QEMU could not place the device tree. The default demo uses 32 MiB.
