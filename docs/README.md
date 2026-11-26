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

The CLI boot image is 72.25 KiB (73,984 bytes). The QEMU default is 4 MiB. Serial interaction and the standalone C Test passed in both quota modes at 3 and 16 MiB in short runs. Saving a note and reading it back after a cold boot passed at 4 and 16 MiB.

CLI 启动镜像为 72.25 KiB（73,984 字节），QEMU 默认 4 MiB。3 MiB 和 16 MiB 均通过串口交互及独立 C Test 的二三级短程验证。4 MiB 和 16 MiB 下已验证保存及冷启动读回。

[CLI build and launch · 构建与启动](https://github.com/leiquan/Quota-OS/tree/CLI) · [3 MiB](tests/cli-16m/3M.log) · [16 MiB](tests/cli-16m/16M.log) · [4 MiB save](tests/cli-4m/disk-boot-1.log) · [4 MiB cold readback](tests/cli-4m/disk-boot-2.log)

[Earlier 32 MiB profile · 早期 32 MiB 配置记录](tests/cli/)
