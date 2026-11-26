# Todo

A standalone WAP for Quota OS Web UI. Add a task with Enter or Add, check it off, filter to open tasks, or delete a row. Each edit calls the Node backend and displays Saved after the write returns. The count tracks open and total tasks. If the JSON data exceeds the current 496-byte application file, the previous list remains on screen and the app reports the error.

## Run the built-in example

In Web UI, open Apps → ZIP and select **Export todo.wap**. Open **todo.wap** from Files. The checked-in package is embedded in the OS image. After editing source, rebuild the package with the command below, copy the generated `todo.wap` into this directory, and rebuild the Web UI image.

## Build and run on a dedicated data disk

Install Python 3 and the Terser CLI on the host. From the repository root run:

```sh
python3 examples/todo/build.py build/todo-preview
```

The new directory contains a stored ZIP named `todo.wap` and a fresh `todo.raw` data disk with that package installed. Use an unused output directory. Supply `--terser PATH` if Terser is outside PATH.

Start the Web UI image with the dedicated task disk:

```sh
bash tools/run-qemu-gui.sh -machine virt,gic-version=2 -cpu cortex-a35 -m 1G -nic none \
  -global virtio-mmio.force-legacy=false -device virtio-rng-device -device ramfb \
  -device virtio-tablet-device -device virtio-keyboard-device \
  -drive if=none,id=notes,format=raw,file=build/todo-preview/todo.raw \
  -device virtio-blk-device,drive=notes,serial=QUOTA-NOTES -icount shift=1,sleep=off \
  -kernel build/quotaos-node-web.bin -append apps-video
```

Open Files and double-click `todo.wap`. The task list runs in the desktop window. Retain the disk when restarting QEMU to restore the list. Use a fresh dedicated disk for the first Todo run. Its data format differs from the former note demo. Keep the old disk if it contains a note you want to retain.

The manifest requests Tier 3. Blink renders the controls and the page uses `rpc.get('list')` and `rpc.get('save', tasks)`. The Node backend registers both handlers and persists the list through the guest file service. The system image supplies Blink, Node and V8.
