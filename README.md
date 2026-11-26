# Quota OS CLI

CLI runs the ARM64 quota kernel and a PL011 serial shell on QEMU virt (one Cortex-A35 core). It links no LVGL renderer and reserves no framebuffer. A Tier 1 timer uses a fixed CPU slot and kernel-resident storage. The shell uses Tier 3.

## Quota OS editions · 三个发行版

CLI, C UI and Web UI share the ARM64 quota kernel. CLI provides the serial console and supports a separately launched EL0 Test application. C UI adds the native LVGL desktop. Web UI extends that desktop with Tier 3 WAP applications powered by Blink, V8 and Node.js.

三个版本共用 ARM64 配额内核。CLI 提供串口命令行，Test 作为独立 EL0 应用按需运行。C UI 增加 LVGL 原生桌面，Web UI 在桌面上增加三级 WAP 运行时。

| Edition · 发行版 | Default QEMU RAM · 默认内存 | ARM64 boot image · 启动镜像 |
|---|---|---|
| [CLI](https://github.com/leiquan/Quota-OS/tree/CLI) | 4 MiB | 72.3 KiB |
| [C UI](https://github.com/leiquan/Quota-OS/tree/C-UI) | 64 MiB | 685.4 KiB |
| [Web UI](https://github.com/leiquan/Quota-OS/tree/Web-UI) | 1 GiB | 357.5 MiB |

The two desktop figures are verified QEMU launch configurations. CLI passed serial interaction and the standalone EL0 Test in both quota modes at 3 MiB in a short QEMU run. Graphical builds include their respective UI and runtime components.

图形版内存值是已验证的 QEMU 启动配置。CLI 在 3 MiB 下通过串口交互和独立 EL0 Test 的二三级短程验证。Web UI 镜像包含共享运行时、动态库、字体和启动数据，示例 `demo.wap` 为 **1,705 字节**。

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
