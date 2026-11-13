# Quota OS（版本 010）

基于三级资源声明的微内核，保证一级任务永远不卡顿、二级任务运行不卡顿，三级任务则不保证。

A microkernel built on three-tier resource declarations: tier-1 tasks never stall, tier-2 tasks run without stalling, and tier-3 tasks get no guarantee.

AArch64，跑在 QEMU `virt` 上，用真实的 ARM 通用定时器和 GIC 中断。还是原型：单核、无 MMU 隔离、没有真实设备驱动，不能用于生产。

License: [Apache-2.0](LICENSE)

## 版本号规则

- 一个版本 = 一次提交，源码和 `prebuilt/` 一起提交。版本号在 `include/quotaos.h` 的 `QUOTAOS_VERSION`，每个版本提交 +1（三位数字）。提交说明以 `v007:` 这样的前缀开头。
- 启动横幅第二行是 `Version: 007 (<7 位提交号>)`。提交号由 `Makefile` 在编译时取 `git rev-parse --short=7 HEAD`。
- **横幅里的提交号 = 这个版本据以编译的提交，也就是版本提交的父提交。** 核对：`git rev-parse --short=7 <版本提交>^` 应等于横幅里的提交号。例：v007 的横幅是 `Version: 007 (82db71a)`。
- 复现某个版本的 `prebuilt/quotaos.bin`：在版本提交上用 `make SRC_COMMIT=<父提交>` 编译。例：在 f9119e9（v007）上 `make SRC_COMMIT=82db71a` 得到 sha256 `4672719363ac3fdde2d425dbd69a12e9e3241486e6d0bb50250e05d5c9557067`，与 v007 的 `prebuilt/quotaos.bin` 相同。直接 `make` 会把版本提交自己的提交号编进横幅，sha256 也就不同（f9119e9 上是 `7eaad174…`）。

## 进度

| 提交 | 内容 | 状态 | 证据 |
| --- | --- | --- | --- |
| A `bcbe7b5` | 清理：删掉探针、诊断菜单和策略开关，行为不变；构建可逐字节复现 | 已验收 | `docs/regress-A/`（菜单回归、行数统计） |
| B `5b82525` | Tier 1 截止期从理想周期起点记账，跳过和未完成都算错过 | icount 30 轮 0 次错过；**普通模式验收未通过，原因排查中** | `docs/tier1-B/`，见下文「Tier 1 截止期回归」 |
| C `252a90d` | 从设备树识别 GIC（中断控制器）v2 / v3 | 已验收 | `docs/gic-C/` |
| `d03cd22` | 启动横幅和 README 换成上面的定位句 | 已推送 | — |
| D（v007） | 严格时间分区（Tier 1 时间槽表）、系统预留、准入含保护时间、十个按意图命名的演示、T1MISS 行带 `J_us=` 和 class a/b | icount 下菜单 5–10 的 Tier 1 错过 0 次、起点抖动 0 tick、槽内 Tier 2/3 0 tick；普通模式照实列出，只作参考 | `docs/accept-D/`、`docs/regress-D/`，见下文「v007 验收」 |
| E1（v009） | 统一声明「层级，周期内占用 B」：CPU、内存、两个模拟设备的占用时间；按资源准入；不能打断的设备申请 Tier 1 被拒并写出缺的条件；超额被拒并写出资源名 | 第 2、4 项 icount 30 轮全部 PASS | `docs/accept-v009/`，见下文「v009（E1）」 |
| E2（v010） | 预算令牌：驱动（被动服务端）替谁干活就扣谁的预算，IPC 内核路径也记在调用方；设备 Tier 1 准入只用出厂声明值，拒绝列出全部缺的条件 | 第 8 项和 E1 各项 icount 30 轮 × GICv2/GICv3 全部 PASS | `docs/accept-v010/`，见下文「v010（E2）」 |
| E3–E4 | 设备闸门、健康监测降级、中断风暴限流、带宽限流和缓存着色（下文设计） | 未实现 | — |

分支 `rework-partitioning`，对比：https://github.com/leiquan/quota-os/compare/main...rework-partitioning

## 三级资源规则

| | 谁声明 | 规则 | 现在的代码 |
| --- | --- | --- | --- |
| 系统预留 | 内核 | 排在 Tier 1 前面的固定 CPU 和内存，不外借 | v007：内存 16 MB，CPU 350 us/周期（icount 实测最坏 15290 tick ≈ 245 us 加余量） |
| Tier 1 | 厂商，写死在映像里 | 每个任务一个固定值，写进按周期触发的时间槽表；用不完的槽空着，不借给任何人，也不许超过 | v007：时间槽表写死在 `kernel/resource.c`，槽尾由定时器掐断，空槽不外借 |
| Tier 2 | 应用清单 | 最低值（保底）每周期给足；建议值（弹性）只来自空闲，有压力时收回；先问用户 y/n 才启动，停止就释放 | 已实现（代码里叫保底/弹性） |
| Tier 3 | 不声明 | 系统决定，只分剩下的，碰不到上面三样 | 已实现（轮转） |

准入按每种资源各算一遍：**系统预留 + 一级 + 二级保底之和 + 保护时间 ≤ 100%**。放不下就拒绝，并说明先关掉哪个应用（`REFUSED: please close … first`）；请求本身超过整机容量，说 `device does not meet minimum requirements`（旧设备场景）。v007 只算 CPU 和内存两种资源，CPU 已含系统预留和每个槽前 10 us 的保护时间（4 个槽共 40 us）；其他资源在 E 里。v010 起每个槽前的保护时间改为 20 us（4 个槽共 80 us），原因见「v010（E2）」。

## 演示菜单

d03cd22 及以前的菜单是：1 资源表、2 启动 Tier 2（先问 y/n）、3 关闭 Tier 2、4 优先级演示、5 分级开关、6 失范隔离、7 声明超过本机、8 内存压力、10 几十个 Tier 2 切片。

v007 起换成十个按意图命名的演示：

| 新 | 演示 | 对应的旧菜单 |
| --- | --- | --- |
| 1 | 资源总览（含系统预留、每个 Tier 1 的预留和实际最多用量） | 1 |
| 2 | 安装应用：允许/拒绝，也可以停止 | 2、3 |
| 3 | 资源不够：拒绝并说先关哪个 | 2 里的拒绝分支、10 |
| 4 | 旧设备：超过整机容量被拒 | 7 |
| 5 | Tier 1 准时：抖动和错过 | 4 |
| 6 | Tier 1 超时在槽尾被掐断 | 6 的一部分 |
| 7 | Tier 1 槽从不外借 | 新增 |
| 8 | Tier 2 最低值有保证，建议值在压力下收回 | 8 |
| 9 | Tier 3 过载不影响上层 | 4 |
| 10 | 任何一级的失控任务只拖慢自己 | 6 |
| — | 分级开关（OFF 对照） | 5，删除：严格分区没有「关掉」这一档 |

## 资源控制设计（规划中，代码尚未实现，见提交 E）
- **统一声明**：所有资源（CPU、内存、设备占用时间、内存带宽、缓存分区）都用「周期 P 内占用 B」声明，三个层级的含义对每种资源都一样。
- **准入**：每种资源各算一遍：一级预留 + 二级保底之和 + 系统预留 + 保护时间 ≤ 容量。任何一种超了，就拒绝启动，并说明超的是哪一项。
- **一级条件**：一级用到的每样资源，要么独占，要么同时满足三条：能切分、能强制停下（从下令到停住的最长时间实测过，并算进保护时间）、最坏耗时有上限并且实测过。不能打断的设备一律不进一级。
- **预算令牌和设备闸门**：驱动替谁干活就扣谁的预算。闸门在放行前检查最坏耗时加切换开销是否放得下；实际耗时由系统自己测，超时就强制复位，并把设备降到三级。
- **共享硬件**：内存带宽按周期限制访问次数，缓存用着色分区，都按最坏情况留余量。
- **不在本设计范围内**：硬件故障靠看门狗和降级显示兜底；实际隔离效果要在真实的 MPAM 板子上验证，QEMU 上只能验证规则逻辑。

（v009 起统一声明和按资源准入已经实现，覆盖 CPU、内存和两个模拟设备，见下文「v009：E1」；v010 实现了预算令牌（驱动替谁干活就扣谁的预算），并把设备 Tier 1 准入改成只用出厂声明值，见「v010（E2）」；本节其余部分仍是设计。）

补充（同为设计，未实现）：

- 新设备一律从 Tier 3 开始。设备描述（出厂写死）四项：单次作业最长耗时、切换开销、从下令中止到停住的最长时间、能否打断。准入只用声明值（v010 已实现）；系统实测只用来核对，实测超出声明算设备故障（健康监测）：记一行日志（设备名、实测、声明、周期号），设备降出 Tier 1，之后的 Tier 1 申请以「实测超出声明」被拒；已在该设备上的 Tier 1 应用同时按各自配置（降级或报警）处理，每个应用记一行；首页显示提示；普通模式下这些日志行标「非 icount，仅供参考」。
- 超时：强制中止/复位，作业丢弃并计数，设备降到 Tier 3，记一行日志（设备名、实测耗时、声明耗时、周期号）。
- 内存带宽按 MemGuard 的做法：每周期访问次数预算，超了就停到下个周期，下个周期额度恢复，不欠账。Tier 1 的物理页按缓存颜色分配。

E 的验收项（测试给出，QEMU icount 模式每项 30 轮；第 2、4 项已在 v009 实现并通过，见「v009（E1）」；第 8 项在 v010 实现并通过，见「v010（E2）」；其余尚未实现，没有结果）：

1. 少报耗时、但能打断的模拟设备：Tier 1 错过 0 次。
2. 不能打断的设备申请 Tier 1：100% 在准入时被拒，并说明缺哪一条。
3. 超时的设备被降到 Tier 3。
4. 超额申请在启动时被拒，并说明是哪种资源。
5. 中断风暴（每秒几万次）：Tier 1 错过 0 次，多出来的中断被限流并计数。
6. 每次实测的中止/复位时间都不超过设备描述里的上限，也不超过保护时间。
7. 降级日志行包含设备名、实测时间、声明时间、周期号。
8. 令牌记账：应用预算减少量等于实测的驱动耗时（误差 1 tick 内），驱动自己的预算不变。
9. 带宽限流不欠账：超额的周期被停住，下个周期额度恢复。
10. 每个 Tier 1 物理页的缓存颜色都正确。

在真实 MPAM 板子上对比「Tier 1 单独跑」和「Tier 3 疯狂访存」的隔离效果：**缺真实板子**，QEMU 上做不了，不算通过或失败。

## v007 验收（内核 sha256 `4672719363ac3fdd…`，编译于 82db71a）

命令（`acceptance.py` 用 `-append qa` 启动，内核依次跑菜单 1、3–10、1，每个计时演示 1000 个周期，打印 `QA DONE` 后结束）：

```bash
make
python3 tools/acceptance.py build/quotaos.bin /tmp/qa.txt icount qa 3     # 加 -icount shift=4,sleep=off
python3 tools/acceptance.py build/quotaos.bin /tmp/qa-n.txt normal qa 3   # 不加 icount
python3 tools/menu_regress.py run build/quotaos.bin /tmp/menus.txt 2
python3 tools/menu_regress.py compare docs/regress-D/menus-v007-gicv2.txt /tmp/menus.txt
```

icount（`docs/accept-D/qa-icount-run{1,2,3}.txt` 三次逐字节相同；`qa-icount-gicv2.txt` 只差 GIC 那一行）：

- 菜单 5、6、9、10：display-vsync 和 timer-server 各 1000 个作业，错过 0，起点偏移恒为 46 tick（抖动 0），class a/b 都是 0。
- 菜单 6：bad-driver 每周期最多 24989 tick（槽长 25000），在槽尾被掐断。
- 菜单 6、7、9：Tier 2/3 在 Tier 1 槽内 0 tick；空槽全部空转。
- 菜单 8：无压力 3000/4000 us，有压力 2000/2000 us，低于最低值的周期 0 个。
- 内核每周期最多 14678 tick（菜单 9），系统预留 21875 tick（350 us），超出 0 次。定时器中断进入比 CVAL 晚 3 tick。

已知：同一映像在 icount 下重复跑，偶尔「timer IRQ entry - CVAL」和「cut by the slot-end timer」两行的最大值是 5 而不是 3 tick，其他行不变；所以「重复运行逐字节相同」不是每次都成立，原因未查明（怀疑 QEMU 在 icount 下中断送达点受宿主机影响）。

普通模式（不加 icount，共用宿主机，负载均值约 4–5，`docs/accept-D/qa-normal-run{1,2,3}.txt`，负载记录在同名 `-host.txt`）：三次的 Tier 1 错过（各菜单合计）分别是 class a 1193 / class b 0、class a 938 / class b 0、class a 4327 / class b 488（第三次跳过周期 2297 个）。这些数字受宿主机时钟跳动影响，只作参考。

## v008：只改输出格式（内核 sha256 `15dcae5c5a66…`，编译于 f9119e9）

- 所有串口输出行不超过 80 列（中日韩字符算 2 列）：启动横幅的定位句在第一个「，」后换行；`[boot] system reserve`、`admission`、`last release`、`started …`、几行「结果」和 `T1MISS` 都拆成两行，续行缩进。数值和字段没有变。
- `tools/menu_regress.py compare` 改成空白归一：按所有空白（含换行）切词、每个中日韩字符单独算一个词，再逐词比较；计时行（含续行）的数字照旧屏蔽。拆行、对齐不算区别，改字或改功能数字算。v007 的期望输出没有重新生成。
- 证据（`docs/accept-v008/summary.txt`）：v007 和 v008 的 icount qa 日志去掉全部空白后、不屏蔽数字，除版本行外完全相同；菜单回归和 qa 日志的空白归一比较只差版本行（`docs/regress-v008/compare-v007-v008.txt`、`docs/accept-v008/compare-qa-icount-v007-v008.txt`）；最宽一行 79 列（`python3 tools/width_check.py <日志>` 列出超过 80 列的行）。
- icount 重复 10 次（`docs/accept-v008/repeat/`）：文件 sha256 有 3 种；去掉两行中断进入时延后 10 份完全相同。这两行的最大值有时是 5 tick 而不是 3 tick（见上文 v007 的「已知」），原因未查明。

## v009：E1 统一声明和按资源准入（内核 sha256 `ede1ecfe159f…`，编译于 d2c58c4）

实现在 `kernel/decl.c`，菜单 11、12 可在 UTM / QEMU 串口里直接看到。

- 资源：CPU（us/周期）、内存（KB）、`sim-dma` 和 `sim-blk` 两个**模拟设备**（内核里的软件模型，不是真实硬件）的占用时间（us/周期）。周期 P = 10000 us。
- 准入，每种资源各算一遍：系统预留 + Tier 1 + Tier 2 最低值之和 + 保护时间 + 本次申请 ≤ 容量。CPU 的保护时间是 4 个槽 × 10 us；设备的保护时间是每条 Tier 1 声明一次实测停住时间。超了就 `REFUSED: <资源名>`，并打印那种资源的算式。Tier 3 不做准入。
- 设备描述（出厂写死）：单次作业最长耗时、切换开销、从下令中止到停住的上限、能否打断、能否切分。系统自己量：在关中断、并确认作业能在下一个 Tier 1 保护区之前做完的窗口里，跑 8 次完整作业、8 次中途中止，取最大值；不用设备自报。
- Tier 1 设备规则：不能打断的设备一律不进 Tier 1（独占也不行），拒绝里写 `missing: forcibly stoppable (… is non-preemptible; measured stop … > bound …)`；能打断的设备要么独占，要么同时满足能切分、实测停住时间 ≤ 上限、实测最长作业 ≤ 声明，缺哪条写哪条。
- `display-vsync` 在 `sim-dma` 上有一条 2000 us 的 Tier 1 声明，只是声明，这一版没有驱动真的去用设备（令牌和闸门在 E2、E3）。

验收（`python3 tools/acceptance.py build/quotaos.bin <out> icount e1`，引导参数 `e1` 依次跑菜单 11、12 和下面两项检查，然后 `QA DONE`）：30 轮日志在 `docs/accept-v009/e1/`，30 份逐字节相同（`SHA256SUMS`）。

- 第 2 项：`sim-blk`（不能打断）申请 Tier 1，B 从 100 到 10000 us 共 100 次：拒绝 100，写出缺的条件 100；对照组（`sim-dma` 的 Tier 1、`sim-blk` 的 Tier 2）都被接纳。`E1 item2 PASS` × 30。
- 第 4 项：7 个用例（只超 CPU、只超内存、只超 `sim-dma`、三样都超、CPU 多 1 us、CPU 正好用满、都放得下），判定 7/7 正确，拒绝里写出的资源名与超的资源完全一致 7/7。`E1 item4 PASS` × 30。
- 同一次启动里 Tier 1 错过 0 次（含设备实测的关中断窗口）。icount 下实测：`sim-dma` 最长作业 396 us、停住 7 us；`sim-blk` 最长作业 1991 us、中止后要 1770 us 才停（不能打断，只能等做完）。

普通模式（不加 icount）的数字只作参考：`docs/regress-v009/menus-v009-gicv2.txt` 那次 `sim-dma` 实测停住 21 us，超过上限 20 us，于是 `cam-app` 的 Tier 1 申请也被拒（`missing: forcibly stoppable`）；`docs/demo-output.txt` 那次是 16 us，被接纳。实测值受宿主机时钟跳动影响，规则按实测值保守处理。

旧功能没有变：v008 和 v009 的 icount qa 日志空白归一比较只差版本行（`docs/accept-v009/compare-qa-icount-v008-v009.txt`）；菜单回归只多出菜单 11、12 和帮助里的两行（`docs/regress-v009/compare-v008-v009.txt`）。

## v010：E2 预算令牌（内核 sha256 `a6f8603f3fc2…`，编译于 4f077b3）

实现在 `kernel/ipc.c`、`kernel/sched.c`，菜单 13 可在 UTM / QEMU 串口里直接看到。做法照 seL4 MCS 的调度上下文借用：

- 两个系统调用：`svc #1` 调用（x0 服务端编号，x1 请求），`svc #2` 回复并等下一个请求。驱动 `sim-dma-drv` 是被动服务端：自己不在任何运行队列里，只在有人调用时借调用方的预算运行。
- 驱动运行的时间记在调用方的预算上（调用方的已用量增加，驱动自己的不变）；调用、回复两次进内核的路径也记在调用方，不记在系统预留。服务端忙时调用方排队，回复后按轮转交给下一个。
- 演示：`cam-app`（Tier 2，最低值 3000 us）和 `bg-caller`（Tier 3）反复请驱动做 50..400 us 的模拟设备工作。

第 8 项（`python3 tools/acceptance.py build/quotaos.bin <out> icount e2 <2|3>`，引导参数 `e2` 跑菜单 13 共 1000 个周期，然后 `QA DONE`）：30 轮 × GICv2、30 轮 × GICv3，日志在 `docs/accept-v010/e2-gicv2/`、`docs/accept-v010/e2-gicv3/`，每组 30 份逐字节相同（`SHA256SUMS`）；两组之间只差 GIC 那一行和驱动自己的墙钟计时一行（只作参考）。

- 调用 15045 次；内核在调用期间量到：驱动运行 189502915 tick + IPC 内核路径 7547640 tick = 197050555 tick，调用方预算正好减少 197050555 tick；每次调用的「减少量 − 实测」最大 0 tick。驱动自己的预算变化 0 tick。`E2 item8 PASS` × 60。
- 同一窗口：Tier 1 错过 0、起点抖动 0 tick；内核每周期最多 11861 tick，系统预留 21875，超出 0 次。
- 说明：「实测」和「扣减」由同一个内核计时得出，这一项验证的是记账没有漏记、错记到驱动或系统预留；驱动自己用墙钟量的时间含被抢占的部分，只作参考。
- 观察到「timer IRQ entry - CVAL」最大 342 tick（v009 qa 里是 3–5 tick）：定时器到点时 CPU 正在 IPC 系统调用路径里（关中断），要等它返回才进中断。所以 Tier 2/3 的预算到点后最多晚这么多才被掐断；Tier 1 槽不受影响（见下一条）。

保护时间从每槽 10 us 改为 20 us：加入 IPC 后，第一次 e2 运行里有系统调用在保护区开始前一刻进内核，把 Tier 1 槽的起点推迟，起点抖动到 183 tick；改成 20 us 后抖动 0（上面 60 轮）。因此 CPU 保护时间 4 × 20 = 80 us，CPU 可准入的容量从 8110 降到 8070 us。

设备 Tier 1 准入改为只用出厂声明值（测试和研究员的反馈）：

- 判断只看声明的单次作业最长耗时、停住上限、能否打断、能否切分，所以每次启动结论一样；实测值照样量出来显示，只用来核对（实测超出声明的健康监测和降级在 E3）。设备保护时间 = Tier 1 声明条数 × 声明的停住上限。
- 拒绝列出全部缺的条件：`sim-blk` 写 `missing: 不能强制叫停` 和 `missing: 不可切分`（后者独占可免）；声明停住上限超过 Tier 1 限值 20 us 是固定拒绝，写 `missing: 停止上限超过 Tier 1 限值`。新增模拟设备 `sim-cam`（声明停住 30 us）演示这一条。
- 30 轮 × GICv2、30 轮 × GICv3（`docs/accept-v010/e1-gicv2/`、`e1-gicv3/`，每组逐字节相同）：`E1 item2 PASS`（100 次拒绝都同时写出两条缺的条件）、`E1 declared-bound PASS`（`sim-cam` 30 次 Tier 1 申请都写出停住上限这一条，Tier 2 对照被接纳）、`E1 item4 PASS`（边界用例改成 CPU 8071 拒绝 / 8070 放得下）；同一次启动 Tier 1 错过 0。

旧功能：v009 和 v010 的 icount qa 日志空白归一比较（`docs/accept-v010/compare-qa-icount-v009-v010-gicv3.txt`，同为 GICv3）只差版本行和保护时间带来的数字：guard 40→80、CPU 容量 8110→8070、剩余 2110→2070、合计 1890→1930。菜单回归（`docs/regress-v010/compare-v009-v010.txt`，普通模式）另外多出菜单 13、帮助一行、菜单 11 的新行、实测数字和普通模式下的 T1MISS 行（全是 class a 时钟跳动，class b 0，只作参考）。最宽一行不超过 80 列（`tools/width_check.py`）。

## 编译

一条命令：

```bash
make
```

需要 `aarch64-linux-gnu-gcc`（Debian/Ubuntu 的包名是 `gcc-aarch64-linux-gnu`）。macOS 上如果工具链叫 `aarch64-elf-gcc`：

```bash
make CROSS=aarch64-elf-
```

产物：

- `build/quotaos.bin`：给 QEMU `-kernel` 用的原始映像。带 Linux arm64 引导头，QEMU 会把设备树指针放进 `x0`。
- `build/quotaos.elf`：同一份内核的 ELF，方便反汇编。
- `prebuilt/` 里有一份已经编好的副本，不装交叉编译器也能跑。

构建可逐字节复现：每个源文件单独编译到 `build/`，`-ffile-prefix-map` 去掉构建路径，链接时 `--build-id=none`，映像里不含时间戳。用同一版 `aarch64-linux-gnu-gcc` 在任何目录下 `make`，`build/` 与 `prebuilt/` 里的 `.bin` 和 `.elf` 的 sha256 应完全一致：

```bash
make clean && make && sha256sum build/quotaos.bin build/quotaos.elf prebuilt/quotaos.bin prebuilt/quotaos.elf
```

## 在 Apple Silicon Mac 上用 Homebrew QEMU 跑

```bash
brew install qemu
qemu-system-aarch64 \
  -machine virt,gic-version=2 \
  -cpu cortex-a72 \
  -m 128M \
  -nographic \
  -nic none \
  -kernel prebuilt/quotaos.bin
```

看到 `select>` 就可以输入菜单编号，回车确认。退出 QEMU：按 `Ctrl-A`，再按 `X`。

这条命令在 x86_64 的 QEMU 上同样跑通（本仓库的实录就是这样采的）。Apple Silicon 上 Homebrew 的 `qemu-system-aarch64` 不加速时走 TCG，和这里是同一条客户机路径；`-cpu cortex-a72` 不会用到 Hypervisor.framework。

## 用 UTM 跑

UTM 底层就是 QEMU。请用**模拟（Emulate）**，不要用虚拟化（Virtualize）。虚拟化要求 `-cpu host`，这个内核按 Cortex-A72 编写。

UTM 是沙盒应用，在 QEMU 参数里手写 `-kernel /路径/quotaos.bin` 会报 `failed to load`，即使路径没错。内核文件必须通过 UTM 自己的文件选择框选中，「新建 Linux 虚拟机 + Boot from kernel image」这条路已在 Apple M 芯片的 Mac 上跑通。

1. 先拿到 `prebuilt/quotaos.bin`（克隆仓库，或单独下载这个文件）。
2. 打开 UTM，点「+」→ Emulate → Linux。
3. 勾选 Boot from kernel image。在 Kernel 一栏点 Browse，选中 `quotaos.bin`。initrd、启动参数都留空。
4. 架构选 ARM64 (aarch64)。内存设 **128 MB**，CPU 1 核。旧设备场景把内存改成 64 MB。
5. 不加磁盘，向导加了就删掉，这个系统不读盘。保存。
6. 编辑这台虚拟机：
   - System 里机器类型确认是 `virt`。
   - **QEMU Machine Properties** 填 `gic-version=2`（新版在 QEMU 页、Arguments 上方；旧版在 System 页，要先打开 Show Advanced Settings）。从 252a90d 起内核也能从设备树认出 GICv3，不填也能启动（命令行 QEMU 10.0 的 `gic-version=2/3/max` 验证过，日志 `docs/gic-C/`；UTM 上还没有实测）。
   - QEMU → Arguments 里可加 `-cpu cortex-a72`。**不要**写 `-kernel`，UTM 会自己加。
   - 左边栏 Devices 下把 Display 删掉，再新建一个 Serial（Built-in Terminal）。较新的 UTM 在没有显示设备时会自动打开串口窗口，已有就不用再加。
7. 启动。串口窗口里出现和下面「菜单」相同的界面。
8. 启动横幅里会有一行 `[boot] GICv3 from DTB: …`（或 `GICv2 …`），说明认出了中断控制器。如果是 `[boot] ERROR: no GICv2 … or GICv3 … interrupt-controller node`，说明设备树里没有这两种 GIC，内核会停在这里，不会 PANIC。

旧版本（2259e51 及以前）只会 GICv2，在 UTM 的 GICv3 上写 GICv2 CPU 接口 0x08010000 会 `PANIC sync ec=0x25 esr=0x96000050 elr=0x400821e0`，当时必须在 QEMU Machine Properties 里填 `gic-version=2`。现在不需要了；填了也照样能跑。

UTM 和不加 icount 的 QEMU 里，客户机时钟跟着宿主机走，宿主机一卡就会出现时钟跳动，看到的时延和错过次数只作参考。

## 模拟旧设备

```bash
qemu-system-aarch64 \
  -machine virt,gic-version=2 \
  -cpu cortex-a72 \
  -m 64M \
  -nographic \
  -nic none \
  -kernel prebuilt/quotaos.bin
```

启动横幅里的 RAM 应是 65536 KB（64MB），设备树地址和 128MB 那次不同。然后：

- `2` → `1` → `y`：Pixel Racer 只要 8MB 和 3000us，仍能启动。
- `4`：Neural Forge 声明 81920 KB，大于扣掉系统预留和 Tier 1 之后的 35840 KB（v007 的 `docs/regress-D/menus-v007-gicv2.txt` 64M 段）。输出 `device does not meet minimum requirements`，不询问、不启动。

UTM 里把虚拟机内存改成 64MB 应得到同样的结果（UTM 上未实测）。

## Tier 1 截止期回归（commit B 5b82525）

记账口径（`kernel/sched.c`）：

- 每个周期任务的释放时刻就是理想周期起点 `epoch + k*10000us`，`epoch` 是第一个任务上 CPU 的时刻。延迟 = 第一次上 CPU 的时刻 − 理想起点。调度器实际放行的时刻另记为 `dispatch_lag`，不拿来抵消迟到。
- 虚拟时钟一次跳过几个整周期，每个周期性 Tier 1 任务每个被跳过的周期都记一次错过（`skipped`），并在 `T1JUMP` 里记下跳之前的周期号、跳过的周期数和时钟间隔。没有豁免。
- 周期结束时作业还没完成，记一次错过（`unfinished`）。
- 分级 OFF 期间释放的作业记进 `misses_off`（对照组），不进保证统计。

回归脚本每轮启动一次，在分级 ON 下依次跑菜单 1、4、6、10、4、6、1，读最后一次菜单 1 的计数：

```bash
git checkout 5b82525 && make
python3 tools/tier1_rounds.py build/quotaos.bin /tmp/t1-icount 30 4   # -icount shift=4,sleep=off
python3 tools/tier1_rounds.py build/quotaos.bin /tmp/t1-normal 30      # 不用 icount
```

固定配置：`qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M -nographic -nic none -kernel build/quotaos.bin`，icount 模式再加 `-icount shift=4,sleep=off`（一条指令 16ns，等于一拍 CNTPCT）。`sleep=off` 是必须的：默认的 `sleep=on` 下，客户机在 `wfi` 里等串口输入时虚拟时钟跟着宿主机实时走，宿主机一忙，定时器中断就晚到几毫秒。

5b82525 的结果（原始日志在 `docs/tier1-B/`，每轮一个 `round-NN.txt`，汇总在 `summary.txt`）：

| 模式（来源） | 任务 | 作业数 | 错过 | 其中跳过周期 / 未完成 | 延迟 p50 | p99 | 最大 | 截止期 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| icount shift=4，30 轮（开发自测） | display-vsync | 212292 | **0** | 0 / 0 | 19us | 26us | 236us | 800us |
| icount shift=4，30 轮（开发自测） | timer-server | 212296 | **0** | 0 / 0 | 112us | 119us | 330us | 1500us |
| 不用 icount，30 轮（开发自测） | display-vsync | 4862 | **65** | 5 / 0 | 54us | 1202us | 7486us | 800us |
| 不用 icount，30 轮（开发自测） | timer-server | 4862 | **52** | 5 / 2 | 149us | 1401us | 7585us | 1500us |
| 不用 icount，30 轮（测试验收） | display-vsync | 4782 | **1631** | 167 / 197 | 53us | 9072us | 10308us | 800us |
| 不用 icount，30 轮（测试验收） | timer-server | 4785 | **1361** | 167 / 57 | 141us | 9422us | 11463us | 1500us |

每行的出处（都是 5b82525 的源码，30 轮）：

- 开发自测：内核 sha256 `08bdaf40…`，日志 `docs/tier1-B/icount-shift4/` 和 `docs/tier1-B/normal/`（`round-NN.txt` + `summary.txt`）。
- 测试验收：测试在同一台共用机器上用 5b82525 源码重新编译后跑的，日志在 `/workspace/quota-os-qa/regress-5b82525/captures/tier1/normal/`（`round-NN.txt` + `summary.txt`，这个目录不在仓库里）。这一组 4792 个周期、跳过 172 个、时钟跳动 14030 次。

开发自测的周期数和跳过：icount 212300 个周期、跳过 0 个、时钟跳动 0 次；不用 icount 4862 个周期、跳过 5 个、时钟跳动 16236 次。

不用 icount 时客户机时钟就是宿主机时钟，QEMU 线程被宿主机调走多久，定时器中断就晚多久（`dispatch_lag_max` 7483us）。宿主机是一台多人共用的机器，两次普通模式跑的时候负载不同：开发自测那次 1 分钟负载均值约 3，15 分钟约 18（`docs/tier1-B/normal/summary.txt` 末尾的 `uptime`）；测试验收那次的负载没有记在它的 `summary.txt` 里。两组错过次数差了二十多倍，不能互相替代，都照实列出。**普通模式验收未通过，原因排查中**：正在给每次错过记 `T1MISS` 行和同一时段的时钟跳动总长 `J`，按测试给的规则区分「时钟跳动造成」和「内核问题」，还没有结论。

## 中断控制器（GICv2 / GICv3，commit C 252a90d）

`kernel/gic.c` 从设备树的 interrupt-controller 节点决定驱动哪种硬件：

- `arm,cortex-a15-gic` 或 `arm,gic-400`：GICv2，distributor + 内存映射的 CPU interface。
- `arm,gic-v3`：GICv3，distributor（亲和路由、Group 1）+ 本核的 redistributor（按 MPIDR 找）+ `ICC_*` 系统寄存器 CPU interface。从 EL2 启动时 `boot/startup.S` 先打开 `ICC_SRE_EL2`。
- 都没有：打印 `[boot] ERROR: …` 并停住，不去碰不存在的寄存器。

检查命令（日志在 `docs/gic-C/`）：

```bash
python3 tools/gic_boot_check.py build/quotaos.bin /tmp/gic
python3 tools/menu_regress.py run build/quotaos.bin /tmp/menus-v3.txt 3
python3 tools/menu_regress.py compare docs/gic-C/menus-gicv2.txt /tmp/menus-v3.txt
```

`gic_boot_check.py` 在 `gic-version=2`、`3`、`max`、默认 `virt`、`virtualization=on`（EL2 启动）和 `-smp 2` 下都启动到 `select>`。菜单回归在 GICv2 和 GICv3 下除了横幅里那行 GIC 之外逐行一致。

## 目录

```text
boot/        启动、异常向量、链接脚本
kernel/      串口、设备树、GIC、定时器、资源账本、调度、菜单、统一声明（decl.c）、预算令牌 IPC（ipc.c）
include/     公共类型
prebuilt/    编好的 quotaos.bin / quotaos.elf
docs/demo-output.txt     串口实录
docs/regress-A/ docs/regress-B/ docs/tier1-B/ docs/gic-C/ docs/accept-D/ docs/regress-D/ docs/accept-v008/ docs/regress-v008/ docs/accept-v009/ docs/regress-v009/ docs/accept-v010/ docs/regress-v010/   各提交的验收日志
tools/qemu_serial.py     驱动 QEMU 串口的公共模块
tools/capture_demos.py   自动按菜单跑完并检查提示符
tools/menu_regress.py    菜单功能回归：跑固定流程，屏蔽计时数字后逐行比较
tools/tier1_rounds.py    Tier 1 截止期回归（B 的菜单流程，只适用于 5b82525 的映像）
tools/acceptance.py      v007 起的验收：用 -append qa / qa-long 启动，跑到 QA DONE
tools/width_check.py     列出超过 80 列的行（中日韩字符算 2 列）
tools/gic_boot_check.py  在几种 GIC 配置下检查能否启动到 select>
```

```bash
make
python3 tools/capture_demos.py                                   # 重写 docs/demo-output.txt
python3 tools/menu_regress.py run build/quotaos.bin /tmp/menus.txt
python3 tools/menu_regress.py compare docs/regress-D/menus-v007-gicv2.txt /tmp/menus.txt
```
