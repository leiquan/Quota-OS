# Vehicle desktop acceptance

Kernel SHA256: `21cd00e1a4e75b59216e01700c901467a1f50d618418556906f86e2e6371f717`.

- GUI pressure with Activity open under icount: 100/100 passed.
- Native-clock diagnostic sample: 9/10 passed.
- Two app modes: 10/10 each. The speedometer made 100 updates with zero deadline misses
  across the 100-period observation. The optional dashcam paused and resumed.
- EL0 isolation, logs, device admission, IPC and bandwidth regression passed in icount.
- The screenshots are QEMU framebuffer captures. Pointer tests opened both tabs
  and selected all three Dock apps.
- The music panel periodically holds its display queue as a deliberate demo.
  Display rates are sampled guest framebuffer writes. The status is instantaneous.
- Media decoding, physical scanout and complete app isolation for the EL1 tasks
  remain outside this test.
