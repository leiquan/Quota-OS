# Quota OS documentation · 文档导航

The three editions share this directory. Each branch's top-level README describes how to build and run that edition.

三个发行版同步维护本目录。构建和启动命令以各分支根目录的 README 为准。

| Area · 内容 | Files · 位置 |
|---|---|
| Kernel walkthrough · 内核入门 | [从启动到调度](kernel-guide.md) |
| Official website · 官网 | [English](index.html) · [简体中文](zh.html) · [CSS](site.css) · [JavaScript](site.js) |
| Screenshots · 截图 | [screenshots/](screenshots/) |
| Current desktop frame evidence · 桌面帧间隔证据 | [UI summary](tests/frame-intervals/base.json) · [Web UI summary](tests/frame-intervals/web.json) · [serial logs](tests/frame-intervals/) |
| Quota and persistence evidence · 配额与持久化证据 | [Quota scenarios](tests/quota-scenarios/) · [Cortex-A35 runs](tests/cortex-a35/) · [C workflow](tests/c-workflow/) |
| Historical notes · 历史技术记录 | [Technical notes](../TECHNICAL-NOTES.md) · [Older acceptance runs](accept-v008/) · [v016 records](v016/) |
| Earlier WebKit experiments · 早期 WebKit 原型 | [Page experiment](tests/webkit-page/) · [Runtime experiment](tests/webkit-stack/) · [Archived HTML sample](archive/webkit/hello.html) |

## Edition guides · 各版指南

| Edition · 发行版 | Environment · 环境 | Default QEMU RAM · 默认内存 | Evidence · 验收 |
|---|---|---|---|
| [CLI](https://github.com/leiquan/Quota-OS/tree/CLI) | Serial console · 串口 | 4 MiB | [Standalone Test and cold readback](tests/cli-4m/) |
| [UI](https://github.com/leiquan/Quota-OS/tree/UI) | Native LVGL desktop · 原生桌面 | 64 MiB | [Frame timing](tests/frame-intervals/base.json) |
| [Web UI](https://github.com/leiquan/Quota-OS/tree/Web-UI) | Native desktop and WAP · 桌面与 WAP | 1 GiB | [Frame timing](tests/frame-intervals/web.json) · [WAP cold readback](screenshots/web-node-cold.png) |

## Web UI development · Web UI 开发

Web UI adds a Tier 3 WAP runtime to the native desktop. The package root contains `app.json` (the Tier 3 manifest), `app.html` (HTML, CSS and browser JavaScript) and optional `app.js` (Node.js background logic). Blink renders the page and the demo uses `rpc.get('save', tasks)` and `rpc.get('list')` to communicate with the Node.js process.

Web UI 在原生桌面上增加三级 WAP 运行时。包根目录的 `app.json` 声明三级，`app.html` 提供页面、样式和浏览器端 JS，可选的 `app.js` 提供 Node.js 后台逻辑。Blink 渲染界面，Todo 通过本地消息接口保存和读回任务。

The 1 GiB QEMU profile has separate evidence for WAP cold readback and guest framebuffer timing. / 1 GiB QEMU 配置下，WAP 冷启动读回和虚拟机帧缓冲提交分别有验收记录。

[Build and boot · 构建与启动](https://github.com/leiquan/Quota-OS/tree/Web-UI#启动本地-web-ui-构建) · [Demo source · 示例源码](https://github.com/leiquan/Quota-OS/tree/Web-UI/examples/todo) · [Cold readback · 冷启动读回](screenshots/web-node-cold.png) · [Frame timing · 帧间隔](tests/frame-intervals/web.json)

### Todo · 任务清单

The standalone Tier 3 WAP supports adding, completing, filtering and deleting tasks. The Node.js backend saves edits. Build with Python 3 and Terser by running `python3 examples/todo/build.py build/todo-preview` from Web-UI, using a new output directory. Boot with the generated `todo.raw`, then open the current `todo.wap` entry in Files. Reuse the same disk on restart.

独立的三级任务清单支持添加、完成、筛选和删除。Node.js 后台保存修改。按示例说明生成并挂载专用的 `todo.raw`，在 Files 中打开 `todo.wap` 的入口。重启时复用同一块盘可恢复列表。

[Source and launch guide · 源码与启动指南](https://github.com/leiquan/Quota-OS/tree/Web-UI/examples/todo) · [Screenshot · 实拍](screenshots/web-ui-todo.png)

## CLI validation · CLI 验收

The CLI boot image is 72.25 KiB (73,984 bytes). The QEMU default is 4 MiB. Serial interaction and the standalone C Test passed in both quota modes at 3 and 16 MiB in short runs. Saving a note and reading it back after a cold boot passed at 4 and 16 MiB.

CLI 启动镜像为 72.25 KiB（73,984 字节），QEMU 默认 4 MiB。3 MiB 和 16 MiB 均通过串口交互及独立 C Test 的二三级短程验证。4 MiB 和 16 MiB 下已验证保存及冷启动读回。

[CLI build and launch · 构建与启动](https://github.com/leiquan/Quota-OS/tree/CLI) · [Current macOS QEMU serial transcript · 串口实录](tests/cli-layout/4M.log) · [3 MiB](tests/cli-16m/3M.log) · [16 MiB](tests/cli-16m/16M.log) · [4 MiB save](tests/cli-4m/disk-boot-1.log) · [4 MiB cold readback](tests/cli-4m/disk-boot-2.log)

[Earlier 32 MiB profile · 早期 32 MiB 配置记录](tests/cli/)

## Repository directories · 仓库目录

| Directory · 目录 | Contents · 内容 |
|---|---|
| `boot/`, `kernel/`, `include/` | Startup, kernel, drivers and shared interfaces · 启动、内核、驱动及公共接口 |
| `port/`, `third_party/` | Quota OS integrations and vendored upstream dependencies · 系统适配与随仓库维护的上游依赖 |
| `user/`, `cli/`, `examples/` | User-mode programs, CLI samples and Web UI examples, as present in each edition · 按发行版提供的用户态程序、CLI 样例与 Web UI 示例 |
| `tools/` | Build, launch, regression and historical reproducer scripts · 构建、启动、回归与历史问题复现脚本 |
| `prebuilt/` | Published desktop preview and versioned experimental images. Consult `SHA256SUMS` alongside the artifact · 桌面预览与历史实验镜像，下载后查验同目录的校验清单 |
| `docs/` | Website and evidence. Per-run logs remain under their original paths · 官网与验收证据，逐轮日志保留原路径 |
