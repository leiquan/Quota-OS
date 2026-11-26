# Quota OS documentation · 文档导航

The three editions share this directory. Each branch's top-level README describes how to build and run that edition.

三个发行版同步维护本目录。构建和启动命令以各分支根目录的 README 为准。

| Area · 内容 | Files · 位置 |
|---|---|
| Official website · 官网 | [English](index.html) · [简体中文](zh.html) · [CSS](site.css) · [JavaScript](site.js) |
| Screenshots · 截图 | [screenshots/](screenshots/) |
| Current desktop frame evidence · 桌面帧间隔证据 | [C UI summary](tests/frame-intervals/base.json) · [Web UI summary](tests/frame-intervals/web.json) · [serial logs](tests/frame-intervals/) |
| Quota and persistence evidence · 配额与持久化证据 | [Quota scenarios](tests/quota-scenarios/) · [Cortex-A35 runs](tests/cortex-a35/) · [C workflow](tests/c-workflow/) |
| Historical notes · 历史技术记录 | [Technical notes](../TECHNICAL-NOTES.md) · [Older acceptance runs](accept-v008/) · [v016 records](v016/) |
| Earlier WebKit experiments · 早期 WebKit 原型 | [Page experiment](tests/webkit-page/) · [Runtime experiment](tests/webkit-stack/) |

## CLI validation · CLI 验收

The released CLI image is 75,240 bytes. The QEMU default is 32 MiB. At 17 MiB, the serial console and both quota modes of the separately loaded EL0 Test passed in a short run. At 16 MiB, the console responds and Test is refused. The 17 MiB result is for QEMU and the bundled Test workload.

CLI 镜像为 75,240 字节，QEMU 默认 32 MiB。17 MiB 下串口和独立 EL0 Test 的二三级模式均通过短程验证。16 MiB 下串口可用，Test 被准入拒绝。17 MiB 结果适用于 QEMU 和本次 Test 负载。

[CLI build and launch · 构建与启动](https://github.com/leiquan/Quota-OS/tree/CLI) · [17 MiB](tests/cli/17M.log) · [16 MiB](tests/cli/16M.log) · [Save](tests/cli/disk-boot-1.log) · [Cold readback](tests/cli/disk-boot-2.log)
