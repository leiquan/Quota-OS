**Official website / 官网** · [English](https://www.quotaos.org/) · [简体中文](https://www.quotaos.org/zh.html)

# Quota OS CLI

CLI runs the ARM64 quota kernel and a PL011 serial shell on QEMU virt (one Cortex-A35 core). It links no LVGL renderer and reserves no framebuffer. A Tier 1 timer uses a fixed CPU slot and kernel-resident storage. The shell uses Tier 3.

[Music Player pressure results / 音乐播放器掉帧实测](docs/tests/music-pressure/) · [官网实验截图](https://www.quotaos.org/zh.html#music-pressure)

## Quota OS editions · 四个发行版

CLI, GUI, Web UI and Touch UI share the ARM64 quota kernel. CLI provides the serial console and supports a separately launched EL0 Test application. GUI adds the native LVGL desktop. Web UI extends that desktop with Tier 3 WAP applications powered by Blink, V8 and Node.js. Touch UI provides a 900 × 1600 portrait desktop with full-screen applications and a bottom gesture bar.

四个版本共用 ARM64 配额内核。CLI 提供串口命令行，Test 作为独立 EL0 应用按需运行。GUI 增加 LVGL 原生桌面，Web UI 在桌面上增加三级 WAP 运行时。Touch UI 提供 900 × 1600 竖屏、全屏应用和底部手势栏。

| Edition · 发行版 | Default QEMU RAM · 默认内存 | ARM64 boot image · 启动镜像 |
|---|---|---|
| [CLI](https://github.com/leiquan/Quota-OS/tree/CLI) | 4 MiB | 111.5 KiB |
| [GUI](https://github.com/leiquan/Quota-OS/tree/GUI) | 64 MiB | 764.8 KiB |
| [Web UI](https://github.com/leiquan/Quota-OS/tree/Web-UI) | 512 MiB | 874.2 KiB + runtime volume · 另附运行时卷 |
| [Touch UI](https://github.com/leiquan/Quota-OS/tree/Touch-UI#run-touch-ui) | 512 MiB | 825.0 KiB (LVGL) |

Graphical launchers default to two vCPUs. CLI uses one vCPU. The graphical memory figures describe their listed QEMU launch configurations. CLI passed serial interaction and the standalone EL0 Test in both quota modes at 3 MiB in a short QEMU run. Graphical builds include their respective GUI and runtime components.

三个图形版默认配置两个虚拟 CPU。GUI 默认分配 64 MiB，Web UI 和 Touch UI 默认分配 512 MiB 虚拟机内存。CLI 在 3 MiB 下通过串口交互和独立 EL0 Test 的二三级短程验证。Web UI 从独立只读卷加载运行时、动态库和字体，示例 `todo.wap` 为 **2,021 字节**。

[Web UI app structure and validation · Web UI 应用结构与验收](docs/README.md#web-ui-development--web-ui-开发) · [Todo / 任务清单示例](https://github.com/leiquan/Quota-OS/tree/Web-UI/examples/todo)

## Build and boot

Install QEMU and an AArch64 GNU cross toolchain, or set QUOTA_LLVM_BIN to the directory containing clang, llvm-nm and llvm-objcopy and QUOTA_LLD to ld.lld.

```sh
make
bash tools/run-cli.sh
```

On a new installation, set and confirm the `admin` password, then sign in.
Later boots wait at the password prompt. Run `logout` to lock the shell.

The default launcher attaches a separate read-only ext4 example disk.
After signing in, `ls` and `ls /` list the mounted root, including `todo.wap` on the example disk. Use `cat /FILE` to read a text file on that disk. The package is the same example
distributed by Web UI and Touch UI. CLI provides the file without a
Web runtime. To use a different root disk, set `CLI_ROOT_DISK` explicitly.
Set it to an empty value to start without a root disk.

The default administrator and Notes disk is `build/cli/admin.raw`.
Pass `CLI_DISK` for a separate installation disk. To copy an older
Notes disk into a new installation, run
`python3 tools/init-admin-disk.py admin.raw --from-legacy notes.raw`
and boot with `CLI_DISK=admin.raw`. The initializer takes a new output path.

At the prompt, use `help`, `mem`, `tasks`, `quota` or `events`. Use `note TEXT` to store one note, wait for `Saved`, and use `note` after reboot. The CLI uses the kernel's Notes block service. With no ext4 disk attached, use `cat test.c` to read the built-in Test source. Use `run test.c` to execute the compiled sample. Set `CLI_TEST=0` at boot to omit the example.

`run Test` starts the external EL0 sample in Tier 3. `run Test t2` requests a 400 µs minimum per 10 ms period and prints observed grants and usage in counter ticks. Edit `os/cli/test.c` and run `make`. The cross compiler builds ARM64 machine code for QEMU, with no application packaging step. Test executes only when requested.

Kernel boot diagnostics are enabled by default. Run `bash tools/run-cli.sh --quiet` for the concise welcome screen on the same image. Boot errors remain visible.

## Tested memory configurations

The default is 4 MiB. The kernel reserves 2 MiB and reports 2 MiB available for application admission. The EL0 loader currently supports two process slots and one code page per program. The admission capacity alone does not determine how many programs can run concurrently.

Both modes of Test completed on single-core Cortex-A35 QEMU with 3, 4, 8, 16, 17, 18, 20, 24 and 32 MiB. At 2 MiB QEMU could not place the device tree.

[Shared documentation and validation logs · 共享文档与验收日志](docs/README.md)
