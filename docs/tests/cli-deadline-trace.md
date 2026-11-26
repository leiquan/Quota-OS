# CLI deadline and interference probe

Build `bash tools/build-cli.sh` with the normal guest toolchain. Use
`CLI_RAM=4M CLI_TEST=1 CLI_ROOT_DISK= bash tools/run-cli.sh --quiet`. Sign in as admin and enter `timing`
and `events`. The probe runs the existing Tier 1 timer service and admits
the EL0 Test with its existing Tier 2 CPU minimum. Use a fresh QEMU instance
for each case so the boot-cumulative timing peaks and event counts reset.

- Disk case: attach an ext4 fixture and a scratch Notes disk using the
  commands in [cli-ext4.md](cli-ext4.md). Keep `CLI_TEST=1` and enter
  `timing disk` before any root read, then `events`. Mount and file reads
  occur during the trace. The disk case expects a fixture containing `hello.txt`. Notes counters include startup reads.
- Fault case: add `CLI_TIMING_TEST=1`, then enter `timing block` and
  `events`. The Tier 3 shell masks interrupts once for about 22 ms
  on this disposable boot. An observed missed Tier 1 commit and a
  runnable Tier 2 budget shortfall make this a valid negative test.
- Buffer test: use `CLI_TIMING_TEST=1` and enter `timing overflow`.
  Expect `count=128`, nonzero `dropped`, and `complete=0`.

Each `[job]` contains the scheduled release, first actual execution tick,
completion SVC entry tick, and absolute deadline. Zero completion means a
job was closed without committing. The headless timer service has no draw
or framebuffer stages. Each `[period]` contains Tier 2 grant, usage,
committed minimum and the existing runnable-at-period-end shortfall flag.
The result separately reports missing commits, late completions, adjacent
commit gaps strictly greater than the expected period, and shortfalls.
Only complete captures support window-level conclusions.

The timing peaks are observed guest ticks since boot. Scheduler residence
and covered process IRQ-mask spans use existing counters. VirtIO samples
cover notification through guest observation of completion or timeout,
including scheduling and host I/O latency. They are not CPU occupancy or
nonpreemptible duration. Root and Notes have one request in flight each,
at most one new submission per scheduler period per driver, with 512-byte
data requests. VirtIO input consumes at most eight completions per device
per service call from a single producer-index snapshot.

The admission calculation continues to deduct the existing 20 us guard
per Tier 1 slot. The work-count limits above bound the number of operations,
not platform execution time. A complete time bound requires accounting for
IRQ masking and MMIO or other unbounded waits per path. Measurements here
do not establish such a bound and must not be used as one in admission.

Graphical consumers should extend the existing vehicle frame records with
separate scheduler start, draw start, first actual framebuffer store, and
post-store commit, using the same guest clock and an explicit drop count.
Validate both frame streams in one window with Tier 3 CPU, actual memory
traffic and slow disk active. Compare raw ticks to 10 ms for GPS Speed and
40 ms for Flight Camera, and report no-commit, deadline, gap and quota
failures individually. This CLI probe does not verify graphical deadlines.
