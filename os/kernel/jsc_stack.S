/* Run the engine on its dedicated stack. x19 survives calls under AAPCS64. */
.text
.balign 4
.global jsc_call_on_stack
.type jsc_call_on_stack, %function
jsc_call_on_stack:
    adrp x1, jsc_stack
    add x1, x1, :lo12:jsc_stack
    add x1, x1, #256, lsl #12
    stp x19, x30, [sp, #-16]!
    mov x19, sp
    mov sp, x1
    blr x0
    mov sp, x19
    ldp x19, x30, [sp], #16
    ret
.size jsc_call_on_stack, .-jsc_call_on_stack

.bss
.balign 16
.type jsc_stack, %object
jsc_stack:
    .skip 1048576
.size jsc_stack, .-jsc_stack

.section .note.GNU-stack,"",%progbits
