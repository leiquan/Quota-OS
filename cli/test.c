/* Standalone EL0 program. The console service accepts a slice of the code page. */
void _start(void)
{
    static const char message[] = "Test: EL0 completed";
    for (volatile unsigned count = 0; count < 262144; ++count) {
    }

    register unsigned long op __asm__("x8") = 3;
    register const char *text __asm__("x0") = message;
    register unsigned long length __asm__("x1") = sizeof message - 1;
    __asm__ volatile("svc #0" : "+r"(text) : "r"(length), "r"(op) : "memory");
    op = 1;
    __asm__ volatile("svc #0" : : "r"(op) : "memory");
    __builtin_unreachable();
}
