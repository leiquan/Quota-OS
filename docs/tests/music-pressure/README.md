# Music Player under Tier 3 pressure · 音乐播放器三级压力实测

One combined CPU, memory and disk pressure run per edition on QEMU, using one Cortex-A35 vCPU and `icount`. UI used 64 MiB and Web UI used 1 GiB. Music Player runs at the rate allowed by the shared Tier 3 pool and bounded display queue.

两版各进行一轮三级 CPU、内存和磁盘组合压力测试。使用 QEMU 单核 Cortex-A35 和 `icount`，UI 为 64 MiB，Web UI 为 1 GiB。Music Player 以三级共享池和有界绘制队列允许的速度提交画面。

| Edition | Idle screenshot | Pressure screenshot | Pressure commits / guest time | Pressure average |
|---|---:|---:|---:|---:|
| UI | 480 FPS | 80 FPS | 2,051 / 10.03 s | 204.5 FPS |
| Web UI | 448 FPS | 197 FPS | 1,997 / 10.04 s | 198.9 FPS |

截图显示的是当时的采样值，压力均值按整段提交数和来宾时间计算。FPS 指来宾帧缓冲提交速率。

Both runs recorded zero GPS Speed misses, zero Flight Camera misses, zero Tier 2 shortfalls and zero Tier 3 pool crossings. The maximum commit intervals were 10 ms for GPS Speed and 40 ms for Flight Camera, with no long gaps in the recorded traces. 两版均通过严格帧时序检查。

| Edition | Code used | Image SHA-256 | Evidence |
|---|---|---|---|
| UI | `83ec4d0` | `d3c6c20d1cb492f453112604e5dcaae68bdc34ec274b07c26ae3371c291a27ff` | [Frame trace and serial log](ui.log) · [Screenshots](../../screenshots/music-pressure-ui.png) |
| Web UI | `16fbdbd` | `4848308fbbc4c8741cd0b69e0d77730afbb359a512006a6c5501992d476d87d0` | [Frame trace and serial log](web-ui.log) · [Screenshots](../../screenshots/music-pressure-web-ui.png) |

Run the corresponding image from the repository root. Use `1G` for Web UI.

```sh
python3 tools/quota-scenarios.py /path/to/image.bin /tmp/quota-music-pressure --memory 64M --combined
```
