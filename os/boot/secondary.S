/* QEMU virt, PSCI via HVC. Enter on a private stack with IRQs masked. */
.text
.global smp_secondary_entry
smp_secondary_entry:
    msr daifset, #15
    msr spsel, #1
    /* PSCI context points at the worker; its first field is the stack top. */
    ldr x1,[x0]
    mov sp,x1
    b smp_secondary_main

/* Only this page remains mapped during the independent EL0 slice.
 * Preserve the EL1 callee-saved registers on the private kernel stack. */
.section .gate,"ax"
.balign 2048
.global smp_vectors
smp_vectors:
.rept 8
    b .
    .balign 128
.endr
    b smp_trap_sync
    .balign 128
    b smp_trap_irq
    .balign 128
    b .
    .balign 128
    b .
    .balign 128
    b .
    .balign 128
    b .
    .balign 128
    b .
    .balign 128
    b .
smp_trap_sync:
    mrs x4,cntvct_el0
    mov x1,#0
    b smp_trap_return
smp_trap_irq:
    mrs x4,cntvct_el0
    mov x1,#1
smp_trap_return:
    mrs x2,tpidr_el1
    msr ttbr0_el1,x2
    isb
    b smp_return_kernel

.global smp_gate_enter
smp_gate_enter:
    msr ttbr0_el1,x1
    isb
    mov x0,#0
    mov x1,#0
    eret

.text
.global smp_run_el0
smp_run_el0:
    stp x19,x20,[sp,#-112]!
    str x3,[sp,#96]
    stp x21,x22,[sp,#16]
    stp x23,x24,[sp,#32]
    stp x25,x26,[sp,#48]
    stp x27,x28,[sp,#64]
    stp x29,x30,[sp,#80]
    mrs x3,ttbr0_el1
    msr tpidr_el1,x3
    msr elr_el1,x0
    mov x3,#0x340
    msr spsr_el1,x3
    mov x3,#8192
    msr sp_el0,x3
    msr cntv_cval_el0,x2
    mov x3,#1
    msr cntv_ctl_el0,x3
    isb
    /* The isolated entry receives no kernel register contents. */
    .irp reg,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
        mov x\reg,xzr
    .endr
    b smp_gate_enter
smp_return_kernel:
    ldr x3,[sp,#96]
    str x4,[x3]
    stp x0,x1,[sp,#-16]!
    cbz x1,1f
    bl gic_ack
    msr cntv_ctl_el0,xzr
    isb
    and x2,x0,#0x3ff
    str x2,[sp,#8]
    bl gic_eoi
1:  msr cntv_ctl_el0,xzr
    isb
    ldp x0,x1,[sp],#16
    ldp x21,x22,[sp,#16]
    ldp x23,x24,[sp,#32]
    ldp x25,x26,[sp,#48]
    ldp x27,x28,[sp,#64]
    ldp x29,x30,[sp,#80]
    ldp x19,x20,[sp],#112
    ret
