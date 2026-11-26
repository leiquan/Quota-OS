.section .text
.global _start
_start:
    b test_main
    .ascii "QCAP"
    .word test_end - _start
test_main:
    mov x3, #16
    lsl x3, x3, #16
1:  subs x3, x3, #1
    b.ne 1b
    adr x0, message
    mov x1, #(test_end-message)
    mov x8, #3
    svc #0
    mov x8, #1
    svc #0
message:
    .ascii "Test: EL0 completed"
test_end:
