/* ARM 通用定时器，EL1 物理定时器 CNTP_*。频率从 CNTFRQ_EL0 读，不写死。 */
#include "quotaos.h"

static uint64_t cntfrq;
static volatile uint64_t jiffies;

static inline uint64_t rd_cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t rd_cntpct(void)
{
    uint64_t v;
    asm volatile("isb");
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

void timer_init(void)
{
    cntfrq = rd_cntfrq();
    if (cntfrq == 0)
        cntfrq = 62500000;
    uint64_t ctl = 1; /* ENABLE, IMASK=0 */
    asm volatile("msr cntp_ctl_el0, %0" :: "r"(ctl));
    timer_arm_us(2000);
}

void timer_arm_us(uint64_t us)
{
    uint64_t ticks = (us * cntfrq) / 1000000ull;
    if (ticks < 1)
        ticks = 1;
    if (ticks > 0x7fffffff)
        ticks = 0x7fffffff;
    asm volatile("msr cntp_tval_el0, %0" :: "r"(ticks));
    asm volatile("isb");
}

uint64_t now_us(void)
{
    uint64_t pct = rd_cntpct();
    uint64_t f = cntfrq;
    return (pct / f) * 1000000ull + ((pct % f) * 1000000ull) / f;
}

uint64_t timer_freq(void)
{
    return cntfrq;
}

uint64_t timer_jiffies(void)
{
    return jiffies;
}

void timer_note_irq(void)
{
    jiffies++;
}

void busy_us(uint32_t us)
{
    uint64_t end = now_us() + us;
    while (now_us() < end)
        asm volatile("" ::: "memory");
}
