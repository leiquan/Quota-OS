/*
 * 启动：EL2 降到 EL1（QEMU virt 默认开虚拟化，复位在 EL2），
 * 清 BSS，装异常向量，把设备树指针交给 C。
 *
 * 这是微内核的“上电入口”，还没有任何任务。三级资源表在 kmain 里建立。
 */
.section .text.boot
.global _start

_start:
    /* Linux arm64 引导头。QEMU 看到 magic 才会按 Linux 协议把 DTB 放进 x0。
     * 用 ELF 启动时入口仍是这里，第一条指令直接跳过头部。 */
    b       past_header
    .word   0                 /* code1 */
#ifdef QUOTA_WAP_FULL_RUNTIME
    .quad   0x80000           /* linked offset from the start of RAM */
    .quad   __boot_image_size /* reserve BSS when QEMU places the DTB */
#else
    .quad   0                 /* text_offset */
    .quad   0                 /* image_size */
#endif
    .quad   2                 /* flags: 内核可放在任意偏移 */
    .quad   0
    .quad   0
    .quad   0
    .word   0x644d5241        /* "ARM\x64" */
    .word   0
past_header:
    /* x0 = DTB（Linux boot 协议）。先存到 x19，后面降特权会用到 x0。 */
    mov     x19, x0

    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    el1_entry

    /* ---------- 从 EL2 落到 EL1 ---------- */
    /* 让 EL1 能直接访问物理计时器和计数器，否则调度时钟会陷入 EL2。 */
    mrs     x1, CNTHCTL_EL2
    orr     x1, x1, #3
    msr     CNTHCTL_EL2, x1
    msr     CNTVOFF_EL2, xzr

    /* 不把 SIMD / 系统寄存器访问陷入 EL2。 */
    mov     x1, #0x33ff
    msr     CPTR_EL2, x1
    msr     HSTR_EL2, xzr

    /* 有 GICv3 系统寄存器接口时，允许 EL1 使用 ICC_*（ICC_SRE_EL2.Enable|SRE）。 */
    mrs     x1, ID_AA64PFR0_EL1
    ubfx    x1, x1, #24, #4
    cbz     x1, 9f
    mrs     x1, S3_4_C12_C9_5        /* ICC_SRE_EL2 */
    mov     x2, #0x9
    orr     x1, x1, x2
    msr     S3_4_C12_C9_5, x1
    isb
9:
    /* EL1 使用 AArch64。 */
    mov     x1, #(1 << 31)
    msr     HCR_EL2, x1

    /* SCTLR_EL1：MMU 关闭，只保留架构要求必须为 1 的位。 */
    mov     x1, #0x0800
    movk    x1, #0x30d0, lsl #16
    msr     SCTLR_EL1, x1

    /* SPSR：EL1h，屏蔽 DAIF。等内核把 GIC 和时钟配好再开中断。 */
    mov     x1, #0x3c5
    msr     SPSR_EL2, x1
    adr     x1, el1_entry
    msr     ELR_EL2, x1
    eret

el1_entry:
    /* Allow FP/SIMD at EL1. The CLoop image uses it. Handlers do not. */
    mrs     x1, cpacr_el1
    orr     x1, x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb
    msr     DAIFSet, #0xf
    ldr     x1, =_boot_stack_top
    mov     sp, x1

    ldr     x1, =gate_vectors
    msr     VBAR_EL1, x1
    isb

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:
    cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    mov     x0, x19
    bl      kmain

3:
    wfi
    b       3b

.section .bss
.align 4
_boot_stack:
    .skip 16384
.global _boot_stack_top
_boot_stack_top:

#ifdef QUOTA_WAP_FULL_RUNTIME
.text
.global wap_fp_save
wap_fp_save:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    add x0,x0,#512
    mrs x1,fpcr
    mrs x2,fpsr
    stp x1,x2,[x0]
    ret
.global wap_fp_restore
wap_fp_restore:
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    add x0,x0,#512
    ldp x1,x2,[x0]
    msr fpcr,x1
    msr fpsr,x2
    ret
#endif
