# System status Tier 1 sampler

`QUOTA_SYSTEM_STATUS` opts a graphical image into the Tier 1 sampler.
CLI builds ignore the flag. Build a graphical candidate with
`EXTRA_CFLAGS=-DQUOTA_SYSTEM_STATUS bash tools/build-lvgl-guest.sh`.
Use the equivalent guest builder for the other graphical images.

The slot occupies 50 µs at offset 9,000 µs per 10 ms period. The resource
ledger also deducts the existing 20 µs Tier 1 guard for this slot each
period. The task is released once per 100 periods. The first release
follows 100 complete periods. Existing slots keep their offsets, lengths
and owners. The slot is configured before application admission, and
the ledger continues to charge existing Tier 2 minima. The task's
rolling window and published snapshot occupy one preallocated 4 KiB
Tier 1 page. Any additional color padding is accounted by the existing
memory ledger.

Call `sched_system_status_read(&snapshot)` from the dashboard UI task.
It returns zero before the first sample or in a build without the feature.
Repeat the display update only when `generation` advances. Use
`busy_ticks / window_ticks` for CPU utilization, `reserved_kb / total_kb`
for reserved RAM (including non-application reservations and admitted
minima), `tasks` for live non-idle tasks, and the lifetime
`tier1_misses` and `tier2_short` counters. `release_ticks` and
`sample_ticks` are on the guest counter. `sample_misses` and
`task_peak_ticks` concern only the sampler. Feed the dashboard from this
snapshot after login and put System status above GPS Speed. GUI work
uses its own rendering path and requires separate timing evidence.

## Reproduction

Use a fresh QEMU instance and run the kernel probe:

```sh
qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a35 -m 64M \
  -display none -monitor none -serial stdio -nic none \
  -icount shift=4,sleep=off -kernel build/quotaos-lvgl.bin -append system-status-test
```

The probe admits a 400 µs Tier 2 spinner and starts three Tier 3 spinners.
Check that four releases are 62,500,000 ticks apart on the 62.5 MHz
counter. Check sampler and Tier 1 misses, Tier 2 shortfalls, and Tier 3
overruns. The initial development run observed a 764-tick peak
(~12.2 µs) of charged sampler CPU under this workload. The 50 µs
reservation leaves headroom on this configuration. This measurement is
a QEMU observation, and the reservation does not include UI rendering.

On the same image, run the existing frame regression:

```sh
python3 tools/app-demo-check.py build/quotaos-lvgl.bin build/system-status-gfx \
  --desktop-stall --rounds 1
```

The frame test exercises the existing Tier 3 UI busy loop. The new
instrument's real rendering, Web UI and Touch UI builds, and a
simultaneous CPU, memory and slow-disk workload require their own runs.
