# Task List

A standalone WAP for Quota OS Web UI. Add a task with Enter or Add, check it off, filter to open tasks, or delete a row. Each edit calls the Node backend and displays Saved after the write returns. The count tracks open and total tasks. If the JSON data exceeds the current 496-byte application file, the previous list remains on screen and the app reports the error.

## Build and run

Install Python 3 and the Terser CLI on the host. From the repository root run:

```sh
python3 examples/tasks/build.py build/task-list-demo
```

The new directory contains a stored ZIP named `tasks.wap` and a fresh `tasks.raw` data disk with that package installed. Use an unused output directory. Supply `--terser PATH` if Terser is outside PATH.

Start the Web UI image with the dedicated task disk:

```sh
qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a35 -m 1G -nic none \
  -global virtio-mmio.force-legacy=false -device virtio-rng-device -device ramfb \
  -device virtio-tablet-device -device virtio-keyboard-device \
  -drive if=none,id=notes,format=raw,file=build/task-list-demo/tasks.raw \
  -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES -icount shift=1,sleep=off \
  -kernel build/quotaos-node-web.bin -append apps-video
```

Open Files and double-click `demo.wap` (the current Files label for its single WAP slot). The task list runs in the desktop window. Retain the disk when restarting QEMU to restore the list. The separate disk keeps task data apart from the Notes demo.

The manifest requests Tier 3. Blink renders the controls and the page uses `rpc.get('list')` and `rpc.get('save', tasks)`. The Node backend registers both handlers and persists the list through the guest file service. The system image supplies Blink, Node and V8.
