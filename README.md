![Quota OS desktop with the Apps directory and three vehicle demo applications](docs/screenshots/apps.png)

# Quota OS

Quota OS is an experimental single-core ARM64 operating system built around resource quotas. Its 1280 × 800 LVGL desktop runs on QEMU. A speedometer uses a fixed Tier 1 CPU slot, a dashcam demo requests a Tier 2 reservation, and music and desktop interaction share the Tier 3 pool. CPU deadlines are measured against the guest clock. The demos use simulated speed, video frames and playback activity.

Quota OS 是一个以资源配额为核心的单核 ARM64 实验操作系统。一级由厂商预留固定时间槽，二级由应用申报、用户选择启用，三级共享剩余资源。桌面提供任务监控、现场测试和持久笔记。当前验收基于 QEMU 虚拟时钟及帧缓冲提交，物理显示和实际 SSD 的最坏时延尚待验证。

## Tasks and application control

System groups scheduler tasks by tier. Each row shows CPU use in the sampled 10 ms period and the applicable reservation, grant or shared pool. Right-click or long-press a demo task to inspect its controls. The kernel protects Tier 1 tasks. Stopping the dashcam or music player releases its reservations, and restarting it goes through admission again.

![System Task showing fixed, reserved and shared CPU budgets](docs/screenshots/task-manager.png)

## System information

Fixed label and value columns keep changing counters aligned. CPU values cover the last completed period. The memory percentage measures reserved capacity. The event counters include Tier 2 budget overruns and Tier 3 pool crossings.

![System Info showing CPU usage, memory reservations and resource events](docs/screenshots/desktop-1280x800.png)

## Live system checks

Open **System → Test** on the vehicle desktop. Select one workload at a time. The two admissible Tier 2 workloads require **Allow launch**. Cancel dismisses the request before it reserves resources.

| Scenario | What the guest checks |
| --- | --- |
| T2 Oversized request | Request 10,000 µs of CPU per 10 ms period. Admission must refuse the launch. |
| T2 Approved launch | Reserve 400 µs per period and 16 KiB. Verify the periodic job runs and meets its deadline. |
| T2 CPU saturation | Run a CPU-bound task with a 400 µs minimum and an 800 µs ceiling per period. Check shortfalls, overruns and measured peak use. |
| T3 Normal task | Launch an EL0 process that yields voluntarily. Verify it receives CPU time. |
| T3 Infinite loop | Launch an EL0 process that never yields. Observe speedometer commits, deadline misses and shared-pool crossings while it runs. |

The four runnable workloads are sampled for at least 100 guest periods and then stopped. In the captured infinite-loop run the guest reported 100 speedometer commits in 101 periods, zero deadline misses and zero pool crossings. The saturated Tier 2 task peaked at 799 µs over 108 periods with zero recorded shortfalls or overruns. These observations cover this QEMU configuration. Fixed budgets under saturation, resource pressure, physical display latency and hardware timing require separate testing. [Serial log](docs/tests/quota-scenarios/ui.log)

![Tier 3 infinite-loop test in the System app](docs/screenshots/system-test.png)

## Files

Apps stays first in the Dock. Open windows and running vehicle apps occupy the other slots. Minimized windows keep their icon and dot. When all other apps close, up to five recent apps from this desktop session appear in most-recently-used order.

Open **Apps → Files** to browse the dedicated note volume. Each file row displays the name, type and byte count. Selecting the row opens the document in Notes. This first version supports one UTF-8 document of up to 495 bytes.

![Files listing the saved document on the device](docs/screenshots/files.png)

## Persistent notes

Choose **Save** in Notes and wait for **Saved** before shutting down. Two alternating disk records protect the previous version during an update. The guest reports success after the block device acknowledges the flush. Starting with the same disk restores the saved file.

![Notes displaying the contents of Note.txt](docs/screenshots/notes.png)

## Run

With QEMU for ARM64 installed, run this command from the repository root.

```sh
bash tools/run-files.sh
```

The launcher uses the prebuilt kernel and creates a dedicated image at `~/.local/share/quota-os/notes.raw` by default. Set `QUOTA_NOTES_DISK` to choose another dedicated image. The status bar displays Shanghai local time.

## Verification

This release passed 100 desktop-stall boots, 100 lifecycle boots, and 10 boots each with and without the dashcam. The tablet-input replay verified all five scenarios and cancellation before approval. With disk writes limited to 128 B/s, the two measured saves took 13.428 and 13.216 seconds of guest time. The speedometer recorded zero deadline misses and Files reopened the saved note after a cold boot. [Cold boot logs](docs/tests/quota-scenarios/cold-boots.tar.gz) · [Approval replay](docs/tests/quota-scenarios/approval.log) · [Save](docs/tests/quota-scenarios/save.log) · [Reopen](docs/tests/quota-scenarios/reopen.log)

In the earlier live-checks regression, 100 lifecycle cold boots and 100 desktop-stall cold boots passed under QEMU TCG with `-icount shift=4,sleep=off`. The two ordinary application modes passed 10 cold boots each. In the separate 128 B/s disk test, two sustained saves took 14.315 s and 13.211 s of guest time. The speedometer committed 1,431 and 1,321 frames respectively, with zero deadline misses. A fresh guest reopened the note through Files and the test compared the bytes. [First boot](docs/tests/optimization/live-checks/save.log) · [Second boot](docs/tests/optimization/live-checks/reopen.log) · [Live checks](docs/tests/optimization/live-checks/ui.log) · [Regression logs](docs/tests/optimization/live-checks/regressions.tar.gz)

The LVGL desktop is the working implementation on `main` and `Desktop-LVGL-120X800`. The `Desktop-Webkit-120X800` branch starts from the same revision for WebKit desktop development. Earlier design notes, build instructions and historical experiments are in the [technical notes](TECHNICAL-NOTES.md). [Apache 2.0 license](LICENSE)
