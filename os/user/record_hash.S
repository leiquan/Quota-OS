/* Pure EL0 computation on a private copy of the pending Notes record. */
.section .rodata.record_hash,"a",%progbits
.balign 4096
.global record_hash_page
record_hash_page:
    mov x1, #4096
    ldp w2, w3, [x1]
    mov w4, #0x0193
    movk w4, #0x0100, lsl #16
    add x5, x1, #16
    cbz w3, 2f
1:  ldrb w6, [x5], #1
    eor w2, w2, w6
    mul w2, w2, w4
    subs w3, w3, #1
    b.ne 1b
2:  str w2, [x1, #8]
    mov w3, #1
    str w3, [x1, #12]
    svc #0
    .balign 4096
