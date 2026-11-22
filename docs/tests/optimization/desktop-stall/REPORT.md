# 桌面卡顿隔离验收

最小显示核心与速度表使用一级固定槽。行车记录仪使用二级启动准入，演示声明 600 µs CPU/周期和 16 KiB 内存。三级桌面与音乐共享剩余 CPU 配额。三级桌面在场景中持续忙等，期间不处理输入、LVGL 刷新或音乐画面队列。由独立的二级任务观测 100 个调度周期。一级在写完帧缓冲并执行存储屏障后记一次提交。

- QEMU TCG、Cortex-A72、virt/GICv2、128 MiB、ramfb、`-icount shift=4,sleep=off`。
- 100/100 次冷启动通过。合计 10,000 次速度表提交、0 次一级截止期错过、2,500 次记录仪提交。三级 CPU 池超额 0。每轮速度表最大提交间隔 10,013 µs，测试门槛小于 11,000 µs，目标周期 10,000 µs。
- 两种应用模式各 10 次冷启动，共 20/20 通过。带指针和键盘的单独运行验证了 Notes、About 和系统信息页面。
- 结果衡量虚拟时钟下的帧缓冲提交。实体出屏耗时和硬件显示平面未测试。三级桌面仍在 EL1，测试覆盖可抢占忙循环。二级记录仪提交通过，系统信息仍能观测到二级预算越界事件，严格上限还需单独验证。
- 内存百分比使用已预留配额占 guest RAM 的比例。CPU 百分比使用最近完整调度周期的非空闲记账时间。

复现命令：`python3 tools/app-demo-check.py prebuilt/quotaos.bin /tmp/desktop-stall --desktop-stall --rounds 100`。

镜像 SHA-256：
cae0724c0b0fd8e91798a490b4021dd51886f2915cc9583bbf0ef2249a518e3b  prebuilt/quotaos.bin
QEMU emulator version 11.1.3
