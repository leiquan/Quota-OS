# Quota OS 内核入门

从一次启动、一次时钟中断和一次任务切换开始读。建议先运行 CLI，在串口执行 `help`、`tasks`、`quota` 和 `events`，观察内核看到的任务和配额。

## 第一站：上电到命令行

1. [os/boot/startup.S](../os/boot/startup.S) 从 ARM64 入口开始，准备栈和异常向量，然后调用 `kmain`。
2. [os/kernel/main.c](../os/kernel/main.c) 初始化串口、内存保护、进程表，再从设备树读取 RAM，初始化资源账本、GIC 与时钟。
3. `kmain` 调用 `sched_init`。设备标定完成后启动服务，最后由 `sched_start(shell_main)` 进入调度。
4. [os/kernel/shell.c](../os/kernel/shell.c) 是当前图形版构建的内核 Shell 入口。CLI 的构建和命令以 [CLI 分支 README](https://github.com/leiquan/Quota-OS/tree/CLI) 为准。

## 第二站：一次中断怎样触发任务切换

1. [os/boot/vectors.S](../os/boot/vectors.S) 的 `el1_irq` 或 `el0_irq` 保存寄存器。先对照 [irq_frame 的定义](../os/include/quotaos.h)，理解为什么结构体布局必须和汇编一致。
2. [os/kernel/sched.c](../os/kernel/sched.c) 的 `handle_irq` 读取 GIC 中断号。调度时钟进入 `schedule`。先看它如何保存上一任务并记账，再看它怎样处理周期边界及调用 `dispatch`。
3. `dispatch` 在一级固定槽、已准入的二级任务和三级共享池之间选择可运行任务。跟踪 `pick_t2` 和 `pick_t3`，再回头看配额用完后为什么会换任务。
4. `schedule` 返回选中任务保存现场的栈地址。汇编恢复寄存器，通过 `eret` 回到执行现场。

## 第三站：资源与隔离

| 先读 | 要回答的问题 |
|---|---|
| [os/kernel/resource.c](../os/kernel/resource.c) | 可用于准入的 CPU 和内存预算从哪里来？何时拒绝申请？ |
| [os/kernel/decl.c](../os/kernel/decl.c) | 系统服务如何登记资源需求并启动？ |
| [os/kernel/mmu.c](../os/kernel/mmu.c)、[os/kernel/proc.c](../os/kernel/proc.c) | EL0 任务怎样拥有独立地址空间？切换任务时怎样切换页表？ |
| [os/kernel/ipc.c](../os/kernel/ipc.c) | 服务请求怎样传递和返回？ |
| [os/kernel/drivers/](../os/kernel/drivers/) | 串口、中断控制器、时钟和磁盘在哪一层接入？ |

读到时间变量时先确认单位。调度热路径多数使用计数器 tick，用户声明或报告中还会看到微秒，以及按周期计算的预算。一级槽、二级最低值和三级共享池要结合具体周期和测试条件阅读。

## 从一个可复现的练习开始

按 CLI README 启动默认 4 MiB 配置。先运行 `run Test`，再运行 `run Test t2`，比较同一份 C 示例在三级共享与二级准入两种情况下的输出。先找清楚“申请值、授予值、实际用量”三者在哪里生成，再读 [测试记录索引](README.md)。有限轮次和 QEMU 的虚拟时钟只为对应场景提供证据。

[历史设计与验收记录](../TECHNICAL-NOTES.md)

## 从输入设备到 Touch UI

Touch UI 的 QEMU 虚拟平板通过 VirtIO-MMIO 提交绝对坐标。内核驱动把事件放入输入队列，三级桌面从队列读取输入并交给 LVGL。可以在 900 × 1600 窗口里移动指针到四角，观察串口坐标，再用底部 Bar 验证应用切换和状态恢复。[启动与操作](https://github.com/leiquan/Quota-OS/tree/Touch-UI#run-touch-ui) · [代码路径与验证范围](https://github.com/leiquan/Quota-OS/blob/Touch-UI/TECHNICAL-NOTES.md#touch-ui)
