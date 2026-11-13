/*
 * EL1 异常向量。调度只走 IRQ（当前异常级、使用 SP_ELx）。
 * 同步异常打印后停住，避免静默跑飞。
 *
 * 每个向量槽 128 字节，整张表 2KB 对齐，这是 VBAR_EL1 的硬件要求。
 */
.section .text
.align 11
.global vectors

.macro ventry label
    .align 7
    b \label
.endm

vectors:
    ventry unhandled_sync     /* 0x000 Current EL, SP0, Sync  */
    ventry unhandled_irq      /* 0x080 Current EL, SP0, IRQ   */
    ventry unhandled_sync     /* 0x100 FIQ */
    ventry unhandled_sync     /* 0x180 SError */

    ventry el1_sync           /* 0x200 Current EL, SPx, Sync */
    ventry el1_irq            /* 0x280 Current EL, SPx, IRQ  */
    ventry unhandled_sync     /* 0x300 FIQ */
    ventry unhandled_sync     /* 0x380 SError */

    ventry unhandled_sync     /* 0x400 Lower EL, AArch64 */
    ventry unhandled_irq
    ventry unhandled_sync
    ventry unhandled_sync

    ventry unhandled_sync     /* 0x600 Lower EL, AArch32 */
    ventry unhandled_irq
    ventry unhandled_sync
    ventry unhandled_sync

/*
 * 保存 x0-x30、ELR、SPSR。帧长 272，16 字节对齐。
 * 布局必须和 include/quotaos.h 里的 struct irq_frame 一致。
 */
.macro save_regs
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]
    mrs     x9, ELR_EL1
    mrs     x10, SPSR_EL1
    stp     x9, x10,  [sp, #248]
.endm

.macro restore_regs
    ldp     x9, x10, [sp, #248]
    msr     ELR_EL1, x9
    msr     SPSR_EL1, x10
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    add     sp, sp, #272
.endm

/* 诊断用时间戳。不改变谁被选中、预算怎么扣，只在入口和 eret 前各读一次 CNTPCT。 */
.global irq_entry_pct
.global irq_eret_pct

el1_irq:
    stp     x16, x17, [sp, #-16]!
    mrs     x16, cntpct_el0
    ldr     x17, =irq_entry_pct
    str     x16, [x17]
    ldp     x16, x17, [sp], #16
    save_regs
    mov     x0, sp
    bl      handle_irq          /* 返回值 x0 = 下一个任务的栈（可能已切换） */
    mov     sp, x0
    restore_regs
    stp     x16, x17, [sp, #-16]!
    mrs     x16, cntpct_el0
    ldr     x17, =irq_eret_pct
    str     x16, [x17]
    ldp     x16, x17, [sp], #16
    eret

el1_sync:
    save_regs
    mov     x0, sp
    bl      handle_sync          /* SVC 主动让出时，返回值是下一个任务的栈 */
    mov     sp, x0
    restore_regs
    eret

unhandled_sync:
    save_regs
    mov     x0, sp
    mov     x1, #1
    bl      handle_unhandled
    restore_regs
    eret

unhandled_irq:
    save_regs
    mov     x0, sp
    mov     x1, #2
    bl      handle_unhandled
    restore_regs
    eret

/* 第一次进入任务：x0 指向伪造好的 irq_frame。 */
.global start_first_task
start_first_task:
    mov     sp, x0
    restore_regs
    eret
