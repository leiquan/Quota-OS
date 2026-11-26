# Todo

A standalone WAP for Quota OS. Add a task with Enter or Add, check it off, filter to open tasks, or delete a row. Each edit calls the Node backend and displays Saved after the write returns. The count tracks open and total tasks. If the JSON data exceeds the current 496-byte application file, the previous list remains on screen and the app reports the error.

## Run the built-in example

In Touch UI, sign in and tap **todo.wap** on the desktop. Use a Touch UI image that includes the Web runtime to run the app. The native image shows the same file and explains the runtime requirement when opened. Long-press the file and choose **Delete todo.wap** to remove it from the desktop and Files. An explicit export from ZIP restores the example while retaining the saved tasks. Existing packages and task data on the Notes disk are kept.

The checked-in package is embedded in the image. After editing source, rebuild the package with the command below, copy the generated `todo.wap` into this directory, and rebuild the image.

## Build and run on a dedicated data disk

Install Python 3 and the Terser CLI on the host. From the repository root run:

```sh
python3 examples/todo/build.py build/todo-preview
```

The new directory contains a stored ZIP named `todo.wap` and a fresh `todo.raw` data disk with that package installed. Use an unused output directory. Supply `--terser PATH` if Terser is outside PATH.

Use the standard Touch UI Web runtime entry, which defaults to two virtual CPUs and 512 MiB. Prepare the read-only runtime volume at `build/runtime.raw` and run these commands from the repository root.

```sh
QUOTA_RUNTIME_VOLUME_BOOT=1 bash tools/build-lvgl-guest.sh
python3 tools/init-admin-disk.py build/todo-preview/account.raw --from-legacy build/todo-preview/todo.raw
QUOTA_NOTES_DISK=build/todo-preview/account.raw bash tools/run-web-node-volume.sh
```

The launcher creates an independent `touch-ui-root.ext4` beside the account disk. Set `QUOTA_ROOT_DISK=none` to use the Notes compatibility mode.

Sign in and tap **todo.wap**. Reuse the same account disk to keep the task list across restarts.

The manifest requests Tier 3. Blink renders the controls and the page uses `rpc.get('list')` and `rpc.get('save', tasks)`. The Node backend registers both handlers and persists the list through the guest file service. The system image supplies Blink, Node and V8.
