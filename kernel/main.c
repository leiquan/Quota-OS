#include "quotaos.h"

void kmain(uint64_t dtb)
{
    asm volatile("msr daifset, #2" ::: "memory");
    uart_init();
    printf("\nQuota OS microkernel\n");
    printf("基于三级资源声明的微内核，保证一级任务永远不卡顿、二级任务运行不卡顿，三级任务则不保证。\n");

    _Static_assert(sizeof(struct irq_frame) == 272, "irq frame must match vectors.S");

    uint64_t ram = dtb_ram_bytes(dtb);
    resource_init(ram);
    gic_init();
    timer_init();
    uint64_t a = now_us();
    for (volatile uint32_t i = 0; i < 200000; i++)
        ;
    uint64_t b = now_us();
    printf("[boot] CNTFRQ %lu Hz, cntpct %lu -> %lu us\n", timer_freq(), a, b);

    sched_init();
    timer_calibrate();
    services_start();
    printf("[boot] tier-1 display-vsync and timer-server are reserved and armed\n");
    sched_start(shell_main);
}
