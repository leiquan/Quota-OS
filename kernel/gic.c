/*
 * QEMU virt 默认 GICv2：
 *   distributor 0x08000000
 *   CPU interface 0x08010000
 * 非安全物理定时器是 PPI，INTID 30。分级调度的所有“真实延迟”都从这次中断开始算。
 */
#include "quotaos.h"

#define GICD 0x08000000ul
#define GICC 0x08010000ul

#define GICD_CTLR      0x000
#define GICD_ISENABLER 0x100
#define GICD_IPRIORITY 0x400

#define GICC_CTLR 0x000
#define GICC_PMR  0x004
#define GICC_BPR  0x008
#define GICC_IAR  0x00c
#define GICC_EOIR 0x010

#define TIMER_IRQ 30

void gic_init(void)
{
    mmio_wr(GICD + GICD_CTLR, 0);
    mmio_wr(GICC + GICC_CTLR, 0);

    /*
     * QEMU virt 默认 secure=off：没有安全世界，中断停在 Group 0，
     * 从 EL1 读 IGROUPR 会恒为 1，写进去并不会真的变成 Group 1。
     * 把它们改成 Group 1 之后 CPU 接口会回 1022，IRQ 线永远不抬起来。
     * 所以这里保持 Group 0，只打开转发、优先级和 30 号 PPI。
     */
    for (int i = 0; i < 16; i++)
        mmio_wr(GICD + GICD_IPRIORITY + (uint64_t)i * 4, 0xa0a0a0a0u);
    mmio_wr(GICD + GICD_ISENABLER, 1u << TIMER_IRQ);

    mmio_wr(GICC + GICC_PMR, 0xff);
    mmio_wr(GICC + GICC_BPR, 0);
    mmio_wr(GICD + GICD_CTLR, 1);
    mmio_wr(GICC + GICC_CTLR, 1);
}

uint32_t gic_ack(void)
{
    return mmio_rd(GICC + GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
    mmio_wr(GICC + GICC_EOIR, iar);
}
