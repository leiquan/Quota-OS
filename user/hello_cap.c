/* Position-independent EL0 command sample. The data page belongs to this app. */
static inline __attribute__((always_inline)) long call(long op, long arg, long length)
{
    register long x0 asm("x0") = arg;
    register long x1 asm("x1") = length;
    register long x8 asm("x8") = op;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
__attribute__((section(".hello_cap"),used,noreturn))
void hello_cap(void)
{
    volatile char *p = (volatile char *)0x2000;
    p[0]='h'; p[1]='e'; p[2]='l'; p[3]='l'; p[4]='o';
    p[5]='.'; p[6]='t'; p[7]='x'; p[8]='t'; p[9]=0;
    long n = call(4,(long)p,0);
    if (n >= 0) call(3,(long)p,n);
    call(1,n < 0,0);
    __builtin_unreachable();
}
