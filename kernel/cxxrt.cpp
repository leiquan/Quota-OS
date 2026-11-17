/* Freestanding C++ runtime for Quota OS. Not libstdc++. Not WebKit. */
#include "quotaos.h"

static uint8_t cxx_heap[65536] __attribute__((aligned(16)));
static uint32_t cxx_used;

void *operator new(unsigned long n)
{
    uint32_t a = (cxx_used + 15u) & ~15u;
    if (n > sizeof cxx_heap || a > sizeof cxx_heap - n) {
        printf("[cxx] heap exhausted\n");
        for (;;)
            asm volatile("wfi");
    }
    cxx_used = a + (uint32_t)n;
    return cxx_heap + a;
}

void *operator new[](unsigned long n)
{
    return operator new(n);
}

void operator delete(void *) noexcept
{
}

void operator delete(void *, unsigned long) noexcept
{
}

void operator delete[](void *) noexcept
{
}

void operator delete[](void *, unsigned long) noexcept
{
}

extern "C" void __cxa_pure_virtual(void)
{
    printf("[cxx] pure virtual call\n");
    for (;;)
        asm volatile("wfi");
}
