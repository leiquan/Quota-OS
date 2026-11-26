/* The authenticated overlap probe drives the same EL0 loop on CPU0 in both
 * configurations. CPU1 runs a timer-bounded copy charged to the Tier 3 pool. */
.section .rodata.touch_smp_pressure,"a",%progbits
.balign 4
.global touch_smp_pressure_start, touch_smp_pressure_end
touch_smp_pressure_start:
    adr x0, .Lworker
    mov x8, #6
    svc #0
.Lworker:
    mov x0, #0
1:  add x0, x0, #1
    b 1b
touch_smp_pressure_end:
