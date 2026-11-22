# Tier-3 CPU pool acceptance

Image SHA256: `c2ca4dc1edc51b4d0a4f709287dbdc54056f43e68a78870bacefca08855f168e`.

The pool resets every 10 ms. CPU capacity is calculated after system reserve,
Tier-1 slots, guards, and admitted Tier-2 minima. Tier-3 task execution and
attributed IPC are charged to one pool. The picker stops issuing time when
the remaining pool reaches the interrupt and IPC headroom.

- icount GUI: 100/100, 1,989 throttled periods and zero observed pool crossings.
- Native diagnostic: 8/10.
- EL0 recovery: 100/100 suites. Each includes 100 kills and 100 faults.
- The tests do not cover aggregate memory and device limits or complete
  per-app upper limits for Tier 1/2. The Tier-3 EL1 desktop is not fault contained.
  The IPC headroom and delivery latency still require worst-case analysis.
