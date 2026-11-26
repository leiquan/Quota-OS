#include "quotaos.h"

struct worker {
    uint64_t stack_top, affinity, storage, table, entry, deadline;
    uint64_t first, last, iterations, slice, compare, trap;
    unsigned online, command;
};
static struct worker *workers[SMP_MAX_CPUS - 1];
static unsigned count;
static uint64_t shared_code;
static uint64_t mair, tcr, ttbr, sctlr;
extern void smp_secondary_entry(void);
extern char smp_vectors[];
struct slice_result {
    uint64_t iterations, irq;
};
extern struct slice_result smp_run_el0(uint64_t entry, uint64_t table, uint64_t deadline, uint64_t *trap);

void smp_secondary_main(struct worker *w)
{
    asm volatile("msr mair_el1,%0; msr tcr_el1,%1; msr ttbr0_el1,%2; "
                 "isb; tlbi vmalle1; dsb ish; isb; msr sctlr_el1,%3; isb" ::"r"(mair),
                 "r"(tcr), "r"(ttbr), "r"(sctlr)
                 : "memory");
    asm volatile("msr vbar_el1,%0; isb" ::"r"(smp_vectors) : "memory");
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    if ((mpidr & 0xff00ffffffull) != w->affinity || !gic_secondary_timer_init())
        for (;;)
            asm volatile("wfe");
    __atomic_store_n(&w->online, 1, __ATOMIC_RELEASE);
    for (;;) {
        while (__atomic_load_n(&w->command, __ATOMIC_ACQUIRE) != 1)
            asm volatile("wfe");
        w->first = ticks_now();
        w->iterations = 0;
        w->compare = w->trap = 0;
        if (w->first < w->deadline) {
            uint64_t end = w->first + w->slice;
            if (end > w->deadline)
                end = w->deadline;
            w->compare = end;
            struct slice_result r = smp_run_el0(w->entry, w->table, end, &w->trap);
            if (r.irq == 27)
                w->iterations = r.iterations;
        }
        w->last = ticks_now();
        __atomic_store_n(&w->command, 2, __ATOMIC_RELEASE);
    }
}

void smp_boot(void)
{
    uint64_t ids[SMP_MAX_CPUS], primary;
    int n = dtb_cpus(ids, SMP_MAX_CPUS);
    if (n < 2)
        return;
    asm volatile("mrs %0,mpidr_el1" : "=r"(primary));
    primary &= 0xff00ffffffull;
    asm volatile("mrs %0,mair_el1; mrs %1,tcr_el1; mrs %2,ttbr0_el1; mrs %3,sctlr_el1"
                 : "=r"(mair), "=r"(tcr), "=r"(ttbr), "=r"(sctlr));
    if (kernel_reserve_alloc(4096, 4096, &shared_code)) return;
    for (int i = 0; i < n && count < SMP_MAX_CPUS - 1; i++) {
        if (ids[i] == primary)
            continue;
        uint64_t pa;
        /* One page of privileged stack and five pages for private EL0 tables and stack. */
        if (kernel_reserve_alloc(6 * 4096, 4096, &pa))
            break;
        memset((void *)(uintptr_t)pa, 0, 6 * 4096);
        struct worker *w = (void *)(uintptr_t)pa;
        w->stack_top = pa + 4096;
        w->storage = w->stack_top;
        w->affinity = ids[i];
        asm volatile("dc cvac,%0; dc cvac,%1; dc cvac,%2; dc cvac,%3; dc cvac,%4; dsb sy" ::"r"(w),
                     "r"(&mair), "r"(&tcr), "r"(&ttbr), "r"(&sctlr)
                     : "memory");
        register uint64_t op asm("x0") = 0xc4000003;
        register uint64_t cpu asm("x1") = ids[i];
        register uint64_t entry asm("x2") = (uintptr_t)smp_secondary_entry;
        register uint64_t context asm("x3") = pa;
        asm volatile("hvc #0" : "+r"(op), "+r"(cpu), "+r"(entry), "+r"(context)::"memory");
        if ((long)op) {
            printf("[smp] CPU_ON affinity=%lu rc=%ld\n", ids[i], (long)op);
            continue;
        }
        uint64_t until = ticks_now() + timer_freq();
        while (!__atomic_load_n(&w->online, __ATOMIC_ACQUIRE) && ticks_now() < until) {
        }
        if (__atomic_load_n(&w->online, __ATOMIC_ACQUIRE))
            workers[count++] = w;
    }
    printf("[smp] workers=%u discovered=%d\n", count, n);
}

int smp_online(void) { return count; }

uint64_t smp_affinity(unsigned index) { return workers[index]->affinity; }

int smp_prepare(uint64_t code)
{
    /* Check every lease before replacing the immutable code shared by the group. */
    for (unsigned i = 0; i < count; i++)
        if (__atomic_load_n(&workers[i]->command, __ATOMIC_ACQUIRE) == 1) return 0;
    memcpy((void *)(uintptr_t)shared_code, (const void *)(uintptr_t)code, 4096);
    for (unsigned offset = 0; offset < 4096; offset += 64)
        asm volatile("dc cvau,%0" :: "r"(shared_code + offset) : "memory");
    asm volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
    for (unsigned i = 0; i < count; i++)
        workers[i]->table = smp_test_space(i, workers[i]->storage, shared_code);
    return count;
}

void smp_submit(unsigned first, unsigned n, uint64_t deadline, uint64_t slice, uint64_t entry)
{
    for (unsigned i = first; i < first + n; i++) {
        struct worker *w = workers[i];
        w->deadline = deadline;
        w->slice = slice;
        w->entry = entry;
        __atomic_store_n(&w->command, 1, __ATOMIC_RELEASE);
    }
    asm volatile("dsb ishst; sev" ::: "memory");
}

uint64_t smp_used(unsigned index)
{
    struct worker *w = workers[index];
    if (__atomic_load_n(&w->command, __ATOMIC_ACQUIRE) != 2)
        return UINT64_MAX;
    return w->last - w->first;
}

int smp_result(unsigned index, uint64_t *start, uint64_t *end, uint64_t *iterations, uint64_t *compare, uint64_t *trap)
{
    struct worker *w = workers[index];
    if (__atomic_load_n(&w->command, __ATOMIC_ACQUIRE) != 2)
        return 0;
    *start = w->first;
    *end = w->last;
    *iterations = w->iterations;
    *compare = w->compare;
    *trap = w->trap;
    __atomic_store_n(&w->command, 0, __ATOMIC_RELEASE);
    return 1;
}
