# 四版 QEMU 验收归档 · 2026-11-27

[镜像 SHA-256](SHA256SUMS) · [原始证据 SHA-256](EVIDENCE.sha256)

| 发行版 | 构建所用提交 | 默认来宾配置 | 验收记录 |
| --- | --- | --- | --- |
| CLI | `4703aa787b44c2f8ef6ea76ec7e63c05cb5b334c` | 1 vCPU，4 MiB，`icount shift=4` | [认证、冷启动、配额](cli/driver.log)，[二级 Test](cli/cold.log)，[默认根盘](cli/default.log) |
| GUI | `91878316b44e37917afb3bf290fa8b318b9b16d0` | 2 vCPU，64 MiB，1600 × 900，`icount shift=4` | [综合压力](gui/verified-timing.json)，[原始帧与预算](gui/boot-1.log)，[冷启动](gui/boot-2.log) |
| Web UI | `88dbca86d0484c18dcaeaa09508eba7c6120aaf4` | 2 vCPU，512 MiB，1600 × 900，`icount shift=1` | [Todo 同窗时序](web/verified-timing.json)，[CPU1 记账](web/verified-worker.json)，[冷读回](web/verified-full.json) |
| Touch UI | `86699a5661798944c4536cf676dcf6aee6f5438a` | 2 vCPU，512 MiB，900 × 1600，`icount shift=1` | [默认原生入口](touch/native/driver.log)，[WAP 同盘冷读](touch/cold-shift4/cold.json)，[滚动与 ext4 原始验收](touch/wap-shift4/verdict.json)，[默认时钟复跑](touch/wap-default-clock/verdict.json) |

Touch UI 镜像在归档提交之前构建。要复建上述哈希，请从表里的源代码提交构建。CLI、GUI 和 Touch 本轮使用 Apple Silicon、Homebrew Clang/LLD 23.1.5，QEMU 11.1.3。Web UI 归档保留此前同一正式镜像的原始串口，并重新核对了镜像、运行时及验收脚本结果。[Web UI 构建记录](web/build-command.txt)

## 默认启动命令

每组命令从表中对应的源码检出目录运行。Apple Silicon 上用 Homebrew 工具链时，可先运行：

```sh
export QUOTA_LLVM_BIN="$(brew --prefix llvm)/bin"
export QUOTA_LLD="$(brew --prefix lld)/bin/ld.lld"
```

图形版首次启动会初始化 admin 凭据。图形版根盘首次创建需要 e2fsprogs 的 `mke2fs`。使用原数据盘重启可保留账号和用户文件。

### CLI

```sh
make
bash tools/run-cli.sh
```

### GUI

```sh
bash tools/build-lvgl-guest.sh
bash tools/run-files.sh
```

### Web UI

把已暂存的 ARM64 联合运行时打成独立只读卷，或把已核验的同版运行时卷放在 `build/runtime.raw`。构建时显式选择运行时卷路径，避免已设置的嵌入式构建变量改变镜像类型。

```sh
mkdir -p build
python3 tools/pack-runtime-volume.py STAGING_BUILD_DIR build/runtime.raw
QUOTA_RUNTIME_VOLUME_BOOT=1 bash tools/build-lvgl-guest.sh
bash tools/run-files.sh
```

### Touch UI

```sh
bash tools/build-lvgl-guest.sh
bash tools/run-files.sh
```

启用来宾 Blink/V8/Node.js 运行时后使用独立入口。Touch 运行时卷的 `page-preload.js` 含触屏滚动适配。可从表中同一 Web UI 运行时卷生成，或者用 Touch UI 的暂存工具重新打包。

```sh
mkdir -p build
# ARCHIVE 指向本归档目录。WEB_RUNTIME 指向已核验的 Web UI 运行时卷。
python3 "$ARCHIVE/touch/make-runtime.py" "$WEB_RUNTIME" build/runtime.raw
QUOTA_RUNTIME_VOLUME_BOOT=1 bash tools/build-lvgl-guest.sh
bash tools/run-web-node-volume.sh
```

macOS Cocoa 图形入口是 `bash tools/run-macos-gui.sh`。Touch UI 的 Cocoa WAP 模式用 `QUOTA_RUNTIME_VOLUME=build/runtime.raw bash tools/run-macos-gui.sh`。

## Touch UI 独立复验

| 工况 | 完整预算周期 | GPS Speed 最大间隔 | Flight Camera 最大间隔 | 一级漏期 | 二级欠付 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Todo 滚动与 ext4，默认 shift=1 | 1,410 | 10 ms | 40 ms | 0 | 0 |
| Todo 滚动与 ext4，shift=4 | 3,097 | 10 ms | 40 ms | 0 | 0 |

本次移入 Web UI 的 `15503ba`（可抢占的磁盘读取）、`9464ccf`（分段映射搜索）和 `e11b935`（空闲帧队列及较小验证批次）。默认原生镜像及带 WAP 的镜像分别记录哈希。冷读回、shift=4 联合压力及默认 shift=1 联合压力使用同一带 WAP 的镜像和同一只读运行时卷。每个场景使用独立的 Notes 与 ext4 根盘。冷启动用普通 WAP 入口，首启在真实 Todo 页面保存，完全退出 QEMU，重新用同一 Notes 盘启动，勾选原任务并校验二次落盘的精确数据及校验和。

ext4 压力用 `apps-touch-ext4` 测试入口。真实拖动触发 ext4 创建、替换、读取及删除，并向根盘施加 25 IOPS 节流。检查器对照来宾原始 tick、CPU1 记账、实际 VirtIO 读写与来宾 viewport 提交，同时用压力窗内截图验证列表确实移动。阈值为 62.5 MHz 下的 625,000 和 2,500,000 tick，追踪报丢失即不判通过。数字代表来宾帧缓冲提交。归档里的单次窗口不外推到实体触屏、真实显示延迟或最新镜像的 100 轮覆盖。

默认时钟复跑的 CPU1 工作落在压力开始后的第一个不完整周期，逐周期预算记录从下一个完整周期开始。验收把单独的 CPU1 租约及三级总额账本与同一帧窗口交叉核对，确认实际用量为 46,880 / 62,500 tick。[首次检查器判定](touch/wap-default-clock/initial-verifier.json)保留了只查完整周期时的缺证记录。

从 Touch UI 源码根目录运行下面的命令可重做与归档相同的场景。检查器在工作目录旁创建私有测试盘。

```sh
export PATH="/opt/homebrew/opt/e2fsprogs/sbin:$PATH"
mkdir -p build/release-evidence/touch
cp "$ARCHIVE/touch/"{check,verify-pressure}.py build/release-evidence/touch/
cp build/runtime.raw build/release-evidence/touch/runtime.raw
QA_ICOUNT=4 python3 build/release-evidence/touch/check.py cold cold-review
QA_ICOUNT=4 python3 build/release-evidence/touch/check.py pressure pressure-review
python3 build/release-evidence/touch/verify-pressure.py build/release-evidence/touch/pressure-review
python3 build/release-evidence/touch/check.py pressure pressure-default-review
python3 build/release-evidence/touch/verify-pressure.py build/release-evidence/touch/pressure-default-review
```

[第一次输入脚本重试记录](touch/driver-retry/)保留了自动按键触发长按菜单的串口。重试缩短了按键保持时间并等待默认应用包完成持久化。另保留了两次冷启动后点击位置偏离复选框的记录。其中默认时钟的补测完成了首次保存及重启进入 Todo，复选框操作超时。带精确勾选写回的冷启动验收使用 shift=4。
