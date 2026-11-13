# Quota OS

Quota OS is an experimental real-time microkernel for ARM64, currently running on QEMU. Nothing runs without a quota: CPU time and memory are reserved up front and locked in at admission (GPU reservation is planned).

Workloads fall into three tiers:
- **System tier**: vendor components such as drivers and the instrument cluster. Their resources are reserved at the factory, and they must never stall.
- **App tier**: developer apps that declare their quotas and need user approval. If the system can't guarantee a request, the app doesn't start.
- **Default tier**: undeclared tasks that share a best-effort pool. They may slow down, but they can't drag down the tiers above them.

The long-term goal is a single automotive OS that can replace today's QNX + Android dual-system setup.

**Status:** Early prototype running on QEMU (ARM64). Not production-ready or safety-certified. Real-time guarantees are under active testing; tier-1 deadline results will be published with reproducible scripts and logs.

License: [Apache-2.0](LICENSE)

---

## 中文说明

一个用来讲清楚「三级资源声明」的 AArch64 教学微内核。它不是日常能用的操作系统。它能在 QEMU 的 `virt` 机器上启动，用真实的 ARM 通用定时器和 GICv2 中断，把下面这件事演示出来：

**资源在启动前按声明分完。声明了的人有保底，没声明的人用剩下的。谁想多占，就被自己的预算笼子掐住，不能把别人拖卡。**

## 它证明什么

整机只有一份 CPU 时间和一份内存。内核在开机时先把这两份账算死，以后只在账本允许的范围内调度。

| 级别 | 谁来声明 | 什么时候生效 | 跑起来怎样 |
| --- | --- | --- | --- |
| Tier 1 | 厂商，写在映像里 | 开机就划走，不能被抢走 | 截止期最早，先跑 |
| Tier 2 | 应用开发者，写在清单里 | 启动时准入：保底必须放得进剩余容量，并且用户点头 | 每个周期有硬预算，用完就停 |
| Tier 3 | 不声明 | 自动进默认池 | 轮转分享剩下的时间，忙的时候会卡 |

由此得到四条保证，优先级从高到低：

1. 内核自己不被任务拖死。任务关不掉中断，调度路径不靠某个应用让出 CPU。
2. Tier 1 从上电开始就不卡。`display-vsync` 和 `timer-server` 在进菜单之前就已经按周期释放。
3. Tier 2 一旦准入，在自己的声明范围内不卡。
4. Tier 3 可以卡。任何一级的死循环、超时、不返回，都只能吃掉自己声明的那一截，不能拖慢系统和别的已声明应用。

对应的实现：

- Tier 1 的表在 `kernel/resource.c` 的 `tier1_table`，链接进映像，`resource_init` 时一次扣掉。
- Tier 2 的清单在同一个文件的 `catalog`。`resource_check` 先看「这台机器从来就不够」，再看「现在剩余不够，请先关掉谁」。
- 调度在 `kernel/sched.c`。每个任务是一条预算服务器：周期 10ms，预算用完就阻塞到下个周期。这是 deferrable server（硬预留），用来得到和 CBS 一样的隔离：虚拟 CPU = 预算 / 周期。分级打开时按「Tier 1 最早截止期 → Tier 2 保底 → Tier 2 弹性 → Tier 3 轮转」挑选。
- 弹性内存和弹性 CPU 只在有空闲时给出。菜单 8 施加压力后，弹性内存收回保底，弹性 CPU 不再外借。

这次在 QEMU 里实际看到的对照（完整串口在 `docs/demo-output.txt`）：

- 分级打开，8 个 Tier 3 忙等同时跑：`display-vsync` 12 个周期 **0 次**错过，最大开始延迟 122us（截止期 800us）。
- 分级关掉，改成普通 1ms 轮转：同样 12 个周期 **11 次**错过，最大延迟 7976us。
- 一个 Tier 1 死循环驱动被限制在约 438us（上限 400us），一个 Tier 2 死循环被限制在约 1022us（上限 1000us）。守规矩的 Tier 2 `audio-mix` 和两个常驻 Tier 1 **0 次**错过。Tier 3 忙等只拿到大约 7977us 的剩余时间。
- 从启动到演示结束，分级打开期间 `display-vsync` 释放了 1161 次，错过 0 次。关掉分级的那一小段单独记了 11 次，不算进保证。

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

这条命令在 x86_64 的 QEMU 8.2 上同样跑通（本仓库的实录就是这样采的）。Apple Silicon 上 Homebrew 的 `qemu-system-aarch64` 不加速时走 TCG，和这里是同一条客户机路径；`-cpu cortex-a72` 不会用到 Hypervisor.framework。

## 用 UTM 跑

UTM 底层就是 QEMU。请用**模拟（Emulate）**，不要用虚拟化（Virtualize）。虚拟化要求 `-cpu host`，这个内核按 Cortex-A72 和 GICv2 编写。

UTM 是沙盒应用，在 QEMU 参数里手写 `-kernel /路径/quotaos.bin` 会报 `failed to load`，即使路径没错。内核文件必须通过 UTM 自己的文件选择框选中，「新建 Linux 虚拟机 + Boot from kernel image」这条路已在 Apple M 芯片的 Mac 上跑通。

1. 先拿到 `prebuilt/quotaos.bin`（克隆仓库，或单独下载这个文件）。
2. 打开 UTM，点「+」→ Emulate → Linux。
3. 勾选 Boot from kernel image。在 Kernel 一栏点 Browse，选中 `quotaos.bin`。initrd、启动参数都留空。
4. 架构选 ARM64 (aarch64)。内存设 **128 MB**，CPU 1 核。旧设备场景把内存改成 64 MB。
5. 不加磁盘，向导加了就删掉，这个系统不读盘。保存。
6. 编辑这台虚拟机：
   - System 里机器类型确认是 `virt`。
   - QEMU → Arguments 里加上 `-machine virt,gic-version=2 -cpu cortex-a72`。**不要**再写 `-kernel`，UTM 会自己加。和 UTM 生成的 `-machine` 重复的话，只保留带 `gic-version=2` 的那个。
   - 左边栏 Devices 下把 Display 删掉，再 New… → Serial，模式用默认的 Built-in Terminal。
7. 启动。串口窗口里出现和下面「菜单」相同的界面。
8. 如果第一屏是 `timer IRQ did not fire`，说明中断控制器不是 GICv2，回到第 6 步强制 `gic-version=2`。

UTM 里看到的时延只作演示参考，验收数据以命令行固定 QEMU 配置的回归结果为准。

## 菜单

启动后是一个编号菜单。下面按「要看的结论」来按，不必按数字顺序。

### 1. 资源表

输入 `1`。

会打印设备树读到的 RAM、内核保留、Tier 1 静态表、已经准入的 Tier 2、Tier 3 还剩多少 CPU 和内存，以及两个常驻 Tier 1 从上电以来的命中/错过次数。

### 2. 超额：先拒绝，关掉一个再成功

Tier 1 已经占掉每个周期 1500us，留给应用的保底是 8500us。Pixel Racer、Cinema Play、Studio Cut 各要 3000us，第三个放不下。

1. `2`，应用 `1`（Pixel Racer），`allow>` 时输入 `n`。应看到 `CANCELLED: user denied permission`，应用不启动。
2. 再 `2` → `1` → `y`。Pixel Racer 启动，并写明保底和当前给了多少弹性内存。
3. `2` → `2` → `y`。Cinema Play 启动。
4. `2` → `3` → `y`。应看到：

   ```text
   REFUSED: please close Pixel Racer first
   Candidates: Pixel Racer, Cinema Play
   ```

5. `3` → `1`，关掉 Pixel Racer。
6. `2` → `3` → `y`。Studio Cut 启动成功。
7. `1` 再看资源表，确认保底 CPU 是 6000 / 8500 us。

### 3. 优先级：分级开和关

1. 保持分级为 ON（启动默认就是 ON），输入 `4`。12 行里应全是 `OK`，`deadline_misses=0`。
2. `5` 关掉分级，提示变成 plain fair scheduling。
3. 再输入 `4`。同一批 Tier 3 忙等会把 `display-vsync` 挤过 800us 截止期，错过次数接近全部（这次实录是 11/12）。
4. `5` 把分级打开。之后的保证重新生效。关掉的那一段记在 `misses_off`，不记进 `misses`。

### 4. 失范应用隔离

输入 `6`。同时存在：

- `bad-driver`：Tier 1 死循环，但只有 blk-driver 那份 400us 配额。
- `audio-mix`：Tier 2，每个周期只做 400us 活，截止期 4000us。
- `bad-encoder`：Tier 2 死循环，声明 1000us。
- `bg-hog`：Tier 3，不声明，拼命空转。

实录里前三个声明内的任务错过都是 0。死循环任务用完自己的预算就被掐断，不会把 10000us 的周期吃光。Tier 3 只拿到剩下的时间。默认的新策略还会把本周期多用的部分从下个周期扣回，所以长时间平均会贴着申请量，而不是每周期都超十几微秒。

### 5. 声明超过这台机器

128MB 上输入 `7`（Cluster Sim，声明 512MB）。64MB 上用菜单 `2` 选 `4`（Neural Forge，声明 80MB）。两种情况都不应出现 `allow>`，而是直接：

```text
REFUSED: device does not meet minimum requirements
... declares more than the device has.
```

应用不会启动。数字来自设备树里的内存节点，不是写死的 64 或 128。

### 6. 弹性配额

在还有 Pixel / Cinema / Studio 这类带弹性内存的应用时输入 `8`。压力会把「当前弹性」收回成 0 KB，保底不动。压力标志随即清除；已经收回的内存不会自动加回去，CPU 弹性可以再借。

### 7. 欠账扣回和定时器提前（菜单 12、11）

`12` 切换预算策略。默认是新策略：

- 欠账扣回。周期结束时把「用量 − 申请量」滚进欠账（少用就偿还，欠账不为负，也不超过一个申请量）。下一周期的保底是申请量减去欠账。连续 N 个周期的累计用量因此不会按每周期的超出量线性长大，尾差大约只有一个周期。
- 定时器提前。启动时对 400us 间隔采样，用触发延迟的均值（`[boot] timer latency calibration` 里的 mean）把预算定时器提前同样的拍数。这个值每次启动重测，不写死。用量如果比这次武装的量子还多出 40us 以上，多出来的记成时钟跳动，不记进该任务。

`11` 用和菜单 6 相同的失范任务，按当前策略跑 10000 个周期，打印每个周期的 `used_us`。

同一次启动里先新后旧（这次校准均值 17568 ns，申请量 400us，各 10000 周期）：

| | 新策略 | 旧策略 |
| --- | --- | --- |
| 单周期超出均值 | -0.848 us | 17.694 us |
| P99 | 6 us | 23 us |
| P99.9 | 19 us | 34 us |
| 最大 | 40 us | 39 us |
| 100 周期滑窗用量减 100×400us 的最大值 | -13 us | 1927 us |

新策略单周期最大超出 40us。滑窗没有超过申请量。

### 8. 最小切片，以及 32 个虚拟 CPU

先 `3` → `a` 关掉所有 Tier 2，否则批量演示会要求你先关闭它们。

输入 `9`。内核会做三件事，全部用 CNTPCT 测，不写死：

1. 把定时器编程成 20、50、100、200、500、1000us，各采大约 10 次，丢掉第一拍。
2. 两个任务互相 `svc` 让出，用往返时间的一半当作一次切换。
3. 用 6 个会吃满预算的 Tier 2 任务，扫 40、80、120、200、300us。交付值离声明超过约 30% 的，记为 `loose`，不算可用切片。

输入 `10`。在测到的最小可用切片之上（这次用 200us，留有余量）启动 32 个 Tier 2 应用。每个是一条预算服务器，200us / 10000us = 一颗核的 2%。它们都想占满自己的切片。表里是声明值、实际收到的平均值、周期数、没给够的次数。跑完会把这 32 个应用关掉，保底额度还回去。

## 这次 QEMU 上测到的最小切片

机器：QEMU 8.2，`-machine virt,gic-version=2 -cpu cortex-a72`，客户机 `CNTFRQ = 62500000` Hz（一拍 16ns）。

| 项目 | 测到的值 |
| --- | --- |
| 请求 20us | 不稳定（平均约 90us，最大 364us），不能当分辨率 |
| 能稳住的最小请求 | **50us**（实际平均 63us，最大 65us） |
| 上下文切换 | 46 次干净样本，最小 3us，平均 **7us**，最大 28us |
| 公式下界 | 稳定定时器 + 进一次 + 出一次 = 50 + 2×7 = **64us** |
| 实测最小可用切片 | **80us**（40us 实际给了 56us，误差占比太大，记为 loose） |

批量演示因此用 200us。32 个应用实际平均收到 212us，没给够的次数是 0，同时 Tier 1 仍然 0 次错过。

切片不能无限切小，是因为两笔开销省不掉：定时器中断本身有大约十几微秒的提前量误差，每次换任务还要保存/恢复寄存器并重装定时器。切片比这两笔之和还短时，应用声明到的时间大部分耗在进内核上，虚拟 CPU 没有意义。真实机器上这个下界通常更小（硬件定时器和切换都更快），但数量级的算法是一样的：下界 ≈ 定时器分辨率 + 两次切换。

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
- `2` → `4`：Neural Forge 声明 81920 KB，大于整机 65536 KB，也大于扣掉内核和 Tier 1 之后的 35840 KB。输出 `device does not meet minimum requirements`，不询问、不启动。

UTM 里把虚拟机内存改成 64MB，效果相同。

## 和真正的操作系统差在哪里

- 单核。没有多核负载均衡，也没有核间中断。
- 任务都在 EL1，共用一块地址空间。没有 EL0、没有每进程页表，系统调用只是一次 `svc` 换栈。叫它微内核，是因为资源策略集中在内核、驱动和应用是分开的任务；它没有微内核该有的隔离。
- 「内存」是准入账本，不是页分配器。声明 80MB 不会真的去映射 80MB，压力下的收回也只是把账记回来。
- 除了 `display-vsync` 和 `timer-server`，其余 Tier 1「驱动」只有配额，没有设备。
- 预算服务器是全局 10ms 一起补充的硬预留，不是每个任务按自己的截止期单独充能的完整 CBS。隔离效果同类，记账更简单。
- 串口是轮询的 PL011，没有驱动模型，没有 virtio、文件系统、网络。
- 分级关掉的对照实验会故意让 Tier 1 错过截止期。那不是故障，看完记着用菜单 5 打开。
- 启动阶段调度最长 249us，其中超过 200us 的有 2 次。加压之后这个最大值没有再涨。

## 目录

```text
boot/        启动、异常向量、链接脚本
kernel/      串口、设备树、GIC、定时器、资源账本、调度、菜单
include/     公共类型
prebuilt/    编好的 quotaos.bin / quotaos.elf
docs/demo-output.txt
tools/capture_demos.py   自动按菜单跑完并检查提示符
```

重新采集串口：

```bash
python3 tools/capture_demos.py
```

脚本把 128MB 和 64MB 的输出写到 `/tmp/demo128.txt` 和 `/tmp/demo64.txt`。
