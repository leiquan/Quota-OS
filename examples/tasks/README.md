# Task List

A standalone WAP for Quota OS Web UI. Add a task with Enter or Add, check it off, filter to open tasks, or delete a row. Each edit calls the Node backend and displays Saved after the write returns. The count tracks open and total tasks. If the JSON data exceeds the current 496-byte application file, the previous list remains on screen and the app reports the error.

## Build and run

Install Python 3 and the Terser CLI on the host. From the repository root run:

```sh
python3 examples/tasks/build.py build/task-list-demo
```

The new directory contains a stored ZIP named `tasks.wap` and a fresh `tasks.raw` data disk with that package installed. Use an unused output directory. Supply `--terser PATH` if Terser is outside PATH.

Start the Web UI runtime with the task disk in place of its Notes disk. Open Files and double-click `demo.wap` (the current Files label for its single WAP slot). The task list runs in the desktop window. Retain the disk when restarting QEMU to restore the list. The separate disk keeps task data apart from the Notes demo.

The manifest requests Tier 3. Blink renders the controls and the page uses `rpc.get('list')` and `rpc.get('save', tasks)`. The Node backend registers both handlers and persists the list through the guest file service. The system image supplies Blink, Node and V8.
