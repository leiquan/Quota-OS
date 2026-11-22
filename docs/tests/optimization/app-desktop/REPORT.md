Both boot modes passed 10/10 under QEMU TCG icount shift=4,sleep=off.
Image SHA256: `0af5cc07104305b41ac836e9de94eefa5c8d68efa506d708db79930f242335a0`.

LVGL was ready before the observation. Dashboard wrote 100 updates with zero
dashboard misses over the observed 100 periods. Optional video wrote zero
updates during the stall and resumed afterwards. Music continued submitting.
The visible Cocoa preview used virtio input and produced the README capture.

Media output is simulated. This run does not certify physical screen refresh,
full address-space isolation for Tier 1/2, or wall-clock hard realtime.
