/*
 * 资源账本：系统预留、Tier 1、Tier 2 的最低值和建议值。
 *
 * 系统预留（内核）：CPU SYS_RESERVE_US / 周期、内存 KERNEL_RESERVE_KB，排在 Tier 1 前面，不外借。
 * Tier 1：厂商在编译映像时写死，每个任务一个固定值，开机一次性划走。
 * Tier 2：应用清单里的最低值和建议值。准入只看最低值：
 *     系统预留 + Tier 1 + ΣTier 2 最低值 <= 100%
 *   放不下就拒绝，并说明关掉哪个应用就能放下；比整机能给应用的还多，就是设备不满足最低要求。
 *   建议值只从空闲里给，有压力时收回。
 * Tier 3：没声明，分剩下的。
 */
#include "quotaos.h"

/* 槽号按这个顺序分：CPU 不为 0 的依次是 0、1、2、3。 */
static const struct tier1_decl tier1_table[] = {
    {"display-vsync", "显示提交，从上电就开始跑", 500, 1000, 2048},
    {"timer-server", "周期时钟，从上电就开始跑", 300, 3500, 1024},
    {"gic-server", "中断路由，只保留槽（空槽）", 300, 6000, 2048},
    {"blk-driver", "存储驱动的槽（失控演示借它）", 400, 8000, 4096},
    {"net-driver", "网络驱动，只占内存", 0, 0, 4096},
};

static const struct app_manifest catalog[] = {
    {"Pixel Racer", "游戏", 8 * 1024, 20 * 1024, 3000, 1000},
    {"Cinema Play", "视频播放", 8 * 1024, 16 * 1024, 3000, 500},
    {"Studio Cut", "剪辑", 8 * 1024, 24 * 1024, 3000, 1000},
    {"Neural Forge", "最低 80MB，小内存机器装不下", 80 * 1024, 96 * 1024, 1000, 0},
    {"Cluster Sim", "最低 512MB，超过这台机器", 512 * 1024, 512 * 1024, 1000, 0},
    {"Render Farm", "最低 CPU 9000us，超过整机能给的", 4 * 1024, 4 * 1024, 9000, 0},
};

static struct running_app apps[MAX_APPS];
static uint64_t ram_kb;
static int pressure;
static uint32_t kernel_reserve = SYS_RESERVE_US;
static uint32_t kernel_target = SYS_RESERVE_US;

uint32_t resource_kernel_reserve_us(void) { return kernel_reserve; }
static char reason[REASON_LEN];

static void radd(const char *s)
{
    int n = 0;
    while (reason[n])
        n++;
    while (*s && n < REASON_LEN - 1)
        reason[n++] = *s++;
    reason[n] = 0;
}

static void radd_u(uint64_t v)
{
    char tmp[24];
    int i = 0;
    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char out[24];
    int n = 0;
    while (i)
        out[n++] = tmp[--i];
    out[n] = 0;
    radd(out);
}

const struct tier1_decl *resource_tier1(int *count)
{
    *count = (int)(sizeof tier1_table / sizeof tier1_table[0]);
    return tier1_table;
}

uint32_t resource_t1_cpu_us(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        s += tier1_table[i].cpu_us;
    return s;
}

/* 每个 Tier 1 槽前有 GUARD_US 的保护时间，这段时间窗口任务不能用，准入时要扣掉 */
uint32_t resource_guard_us(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        if (tier1_table[i].cpu_us)
            s += GUARD_US;
    return s;
}

uint32_t resource_t1_mem_kb(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        s += tier1_table[i].mem_kb;
    return s;
}

uint64_t resource_ram_kb(void)
{
    return ram_kb;
}

uint32_t system_reserve_kb(void)
{
    extern char __image_start[], __image_end[];
    uint64_t base = (uint64_t)__image_start & ~((16ull << 20) - 1);
    /* Image plus the configured display surface. Stay at 16 MB when that already fits. */
    uint64_t need = ((uint64_t)__image_end - base) + (uint64_t)LVGL_FB_BYTES;
    uint32_t kb = (uint32_t)((need + 1023ull) / 1024ull);
    if (kb < KERNEL_RESERVE_KB)
        kb = KERNEL_RESERVE_KB;
    /* 2 MB alignment keeps the colored pool on a block boundary. */
    kb = (kb + 2047u) & ~2047u;
    return kb;
}

uint64_t resource_app_mem_capacity_kb(void)
{
    /* v012 起再扣掉缓存着色补齐（Tier 1 颜色里别人不能用的页） */
    uint64_t fixed = (uint64_t)system_reserve_kb()
        + resource_t1_mem_kb() + color_padding_kb();
    return ram_kb > fixed ? ram_kb - fixed : 0;
}

uint32_t resource_app_cpu_capacity_us(void)
{
    uint32_t fixed = kernel_reserve + resource_t1_cpu_us() + resource_guard_us();
    return PERIOD_US > fixed ? PERIOD_US - fixed : 0;
}

static uint32_t cpu_minima(int begin_period)
{
    uint32_t sum = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        struct running_app *app = &apps[i];
        if (!app->alive) continue;
        sum += app->cpu_min_us;
        if (begin_period && app->task && app->task->tier == TIER2 && app->task->kind == KIND_JOB)
            app->task->refundable_t = us_to_ticks(app->cpu_min_us);
    }
    return sum;
}

uint32_t resource_cpu_min_sum_us(void)
{
    return cpu_minima(0);
}

/* Scheduler calls this with IRQs masked when it establishes the new pool. */
uint32_t resource_period_reservations_us(uint32_t target)
{
    uint32_t reserved = cpu_minima(1);
    kernel_target = target;
    if (kernel_reserve < target) {
        uint32_t capacity = resource_app_cpu_capacity_us();
        uint32_t room = capacity > reserved ? capacity - reserved : 0;
        uint32_t increase = target - kernel_reserve;
        kernel_reserve += increase < room ? increase : room;
    }
    return reserved;
}

uint64_t resource_mem_min_sum_kb(void)
{
    uint64_t s = 0;
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].alive)
            s += apps[i].mem_min_kb;
    return s;
}

static uint64_t sum_granted_kb(void)
{
    uint64_t s = 0;
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].alive)
            s += apps[i].mem_granted_kb;
    return s;
}

void resource_init(uint64_t ram_bytes)
{
    memset(apps, 0, sizeof apps);
    ram_kb = ram_bytes / 1024;
    pressure = 0;
    reason[0] = 0;
    uint32_t reserve_kb = system_reserve_kb();
    printf("[boot] system reserve %u us/period, %u KB;\n       tier-1 %u us/period, %u KB; guard %u us/period\n",
           SYS_RESERVE_US, reserve_kb, resource_t1_cpu_us(), resource_t1_mem_kb(),
           resource_guard_us());
    if (reserve_kb > KERNEL_RESERVE_KB)
        printf("[boot] system reserve grew from %u KB to %u KB to hold the image and framebuffer; tier-1 declarations unchanged\n",
               KERNEL_RESERVE_KB, reserve_kb);
    printf("[boot] EL1 map is the kernel map. EL0 apps use a separate TTBR0. Tier-1 realtime is not claimed here.\n");
}

int resource_pressure(void)
{
    return pressure;
}

/* 有压力：建议值的内存收回到最低值，CPU 建议值停发（调度器看这个标志）。 */
void resource_set_pressure(int on)
{
    pressure = on ? 1 : 0;
    if (!pressure)
        return;
    uint64_t got = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (apps[i].alive && apps[i].mem_granted_kb > apps[i].mem_min_kb) {
            got += apps[i].mem_granted_kb - apps[i].mem_min_kb;
            apps[i].mem_granted_kb = apps[i].mem_min_kb;
        }
    }
    printf("pressure ON: reclaimed %lu KB of suggested memory; minimums untouched.\n", got);
}

const char *resource_reason(void)
{
    return reason;
}

const struct app_manifest *resource_catalog(int *count)
{
    *count = (int)(sizeof catalog / sizeof catalog[0]);
    return catalog;
}

int resource_running_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++)
        n += apps[i].alive;
    return n;
}

struct running_app *resource_running(int index)
{
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        if (n++ == index)
            return &apps[i];
    }
    return 0;
}

void resource_app_available(uint32_t *cpu_us, uint64_t *mem_kb)
{
    uint32_t cpu_cap = resource_app_cpu_capacity_us();
    uint32_t cpu_used = resource_cpu_min_sum_us();
    uint64_t mem_cap = resource_app_mem_capacity_kb();
    uint64_t mem_used = resource_mem_min_sum_kb();
    *cpu_us = kernel_target == kernel_reserve && cpu_cap > cpu_used ? cpu_cap - cpu_used : 0;
    *mem_kb = mem_cap > mem_used ? mem_cap - mem_used : 0;
}

int resource_check(const struct app_manifest *m)
{
    reason[0] = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (apps[i].alive && strcmp(apps[i].name, m->name) == 0) {
            radd("REFUSED: ");
            radd(m->name);
            radd(" is already running\n");
            return ADMIT_DUP;
        }
    }

    uint64_t mem_cap = resource_app_mem_capacity_kb();
    uint32_t cpu_cap = resource_app_cpu_capacity_us();
    int mem_over = m->mem_min_kb > mem_cap;
    int cpu_over = m->cpu_min_us > cpu_cap;
    if (mem_over || cpu_over) {
        radd("REFUSED: device does not meet minimum requirements\n");
        if (mem_over) {
            radd("  minimum memory ");
            radd_u(m->mem_min_kb);
            radd(" KB > ");
            radd_u(mem_cap);
            radd(" KB for apps\n  (RAM ");
            radd_u(ram_kb);
            radd(" KB - system reserve - tier-1 - color padding)\n");
        }
        if (cpu_over) {
            radd("  minimum CPU ");
            radd_u(m->cpu_min_us);
            radd(" us > ");
            radd_u(cpu_cap);
            radd(" us/period for apps\n  (period - system reserve - tier-1 - guard)\n");
        }
        radd("No app can be closed to make room: this device is too small.\n");
        return ADMIT_DEVICE;
    }

    uint64_t mem_left;
    uint32_t cpu_left;
    resource_app_available(&cpu_left, &mem_left);
    if (m->mem_min_kb <= mem_left && m->cpu_min_us <= cpu_left)
        return ADMIT_OK;

    /* 关掉哪一个就能放下：挑最低值最小的那个，少打扰别人。 */
    int best = -1;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        if (m->mem_min_kb > mem_left + apps[i].mem_min_kb ||
            m->cpu_min_us > cpu_left + apps[i].cpu_min_us)
            continue;
        if (best < 0 || apps[i].cpu_min_us < apps[best].cpu_min_us)
            best = i;
    }
    radd("REFUSED: not enough resources for ");
    radd(m->name);
    radd("\n  CPU left ");
    radd_u(cpu_left);
    radd(" us/period, minimum needs ");
    radd_u(m->cpu_min_us);
    radd("\n  memory left ");
    radd_u(mem_left);
    radd(" KB, minimum needs ");
    radd_u(m->mem_min_kb);
    radd("\n");
    if (best >= 0) {
        radd("Please close ");
        radd(apps[best].name);
        radd(" first (frees ");
        radd_u(apps[best].cpu_min_us);
        radd(" us/period, ");
        radd_u(apps[best].mem_min_kb);
        radd(" KB).\n");
    } else {
        radd("Closing any single app is not enough; close several apps first.\n");
    }
    return ADMIT_OVERSUB;
}

struct running_app *resource_commit(const struct app_manifest *m)
{
    if (resource_check(m) != ADMIT_OK)
        return 0;
    int slot = -1;
    for (int i = 0; i < MAX_APPS && slot < 0; i++)
        if (!apps[i].alive)
            slot = i;
    if (slot < 0) {
        reason[0] = 0;
        radd("REFUSED: app table full\n");
        return 0;
    }
    /* 最低值必须落下。别人的建议值内存占着坑，就先收回。 */
    uint64_t cap = resource_app_mem_capacity_kb();
    if (sum_granted_kb() + m->mem_min_kb > cap) {
        for (int i = 0; i < MAX_APPS; i++)
            if (apps[i].alive)
                apps[i].mem_granted_kb = apps[i].mem_min_kb;
    }
    struct running_app *a = &apps[slot];
    memset(a, 0, sizeof *a);
    str_copy(a->name, m->name, (int)sizeof a->name);
    a->mem_min_kb = m->mem_min_kb;
    a->mem_sugg_kb = m->mem_sugg_kb;
    a->cpu_min_us = m->cpu_min_us;
    a->cpu_sugg_us = m->cpu_sugg_us;
    a->mem_granted_kb = m->mem_min_kb;
    uint64_t flags = irq_save();
    if (m->cpu_min_us && (kernel_target > kernel_reserve ||
        (uint64_t)resource_cpu_min_sum_us() + m->cpu_min_us > resource_app_cpu_capacity_us())) {
        irq_restore(flags);
        reason[0] = 0;
        radd("REFUSED: insufficient CPU after kernel reserve update");
        return 0;
    }
    a->alive = 1;
    irq_restore(flags);
    uint64_t granted = sum_granted_kb();
    uint64_t room = cap > granted ? cap - granted : 0;
    uint32_t want = m->mem_sugg_kb > m->mem_min_kb ? m->mem_sugg_kb - m->mem_min_kb : 0;
    if (!pressure)
        a->mem_granted_kb += (uint32_t)(want < room ? want : room);
    return a;
}

int resource_release(struct running_app *a)
{
    if (!a || !a->alive)
        return -1;
    a->alive = 0;
    return 0;
}
