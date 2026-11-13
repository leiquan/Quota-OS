/*
 * 三级资源声明。
 *
 * Tier 1：厂商在编译映像时写死，boot 时一次性划走，运行中不能被抢走。
 * Tier 2：应用清单里的保底额度。能装下才准入；弹性部分有空闲才给，压力下收回。
 * Tier 3：没声明的人，用剩下的，按时间片轮转。
 *
 * 准入只看“保底能不能放进剩余容量”。这是后面调度器敢做硬保证的前提：
 * 已经答应出去的 CPU 预算之和，永远不超过一个周期。
 */
#include "quotaos.h"

static const struct tier1_decl tier1_table[] = {
    {"timer-server", "周期时钟，从上电就开始跑", 300, 1024},
    {"gic-server", "中断路由，只保留配额", 300, 2048},
    {"blk-driver", "存储驱动配额（失控驱动演示用它做上限）", 400, 4096},
    {"net-driver", "网络驱动，只占内存", 0, 4096},
    {"display-vsync", "显示提交，从上电就开始跑", 500, 2048},
};

static const struct app_manifest catalog[] = {
    {"Pixel Racer", "游戏，保底 3ms/10ms", 8 * 1024, 20 * 1024, 3000, 1000},
    {"Cinema Play", "视频播放", 8 * 1024, 16 * 1024, 3000, 500},
    {"Studio Cut", "剪辑，用来把 CPU 预算撑满", 8 * 1024, 24 * 1024, 3000, 1000},
    {"Neural Forge", "声明 80MB，小内存机器装不下", 80 * 1024, 96 * 1024, 1000, 0},
    {"Cluster Sim", "声明 512MB，超过这台机器", 512 * 1024, 512 * 1024, 1000, 0},
};

static struct running_app apps[MAX_APPS];
static uint64_t ram_kb;
static int pressure;
static char reason[REASON_LEN];

static void rclear(void)
{
    reason[0] = 0;
}

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
    if (v == 0)
        tmp[i++] = '0';
    while (v) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char out[24];
    int n = 0;
    while (i)
        out[n++] = tmp[--i];
    out[n] = 0;
    radd(out);
}

static uint32_t tier1_cpu(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        s += tier1_table[i].cpu_us;
    return s;
}

static uint32_t tier1_mem(void)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++)
        s += tier1_table[i].mem_kb;
    return s;
}

static uint64_t sum_min_kb(void)
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

static uint32_t sum_cpu(void)
{
    uint32_t s = 0;
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].alive)
            s += apps[i].cpu_budget_us;
    return s;
}

static int find_alive_name(const char *name)
{
    for (int i = 0; i < MAX_APPS; i++)
        if (apps[i].alive && strcmp(apps[i].name, name) == 0)
            return i;
    return -1;
}

void resource_init(uint64_t ram_bytes)
{
    memset(apps, 0, sizeof apps);
    ram_kb = ram_bytes / 1024;
    pressure = 0;
    rclear();
    printf("[boot] tier-1 reserved %u KB and %u us/period\n", tier1_mem(), tier1_cpu());
}

uint64_t resource_ram_kb(void)
{
    return ram_kb;
}

uint64_t resource_app_mem_capacity_kb(void)
{
    uint64_t fixed = (uint64_t)KERNEL_RESERVE_KB + tier1_mem();
    if (ram_kb <= fixed)
        return 0;
    return ram_kb - fixed;
}

uint32_t resource_app_cpu_capacity_us(void)
{
    uint32_t t1 = tier1_cpu();
    if (t1 >= PERIOD_US)
        return 0;
    return PERIOD_US - t1;
}

uint32_t resource_cpu_remaining_us(void)
{
    uint32_t cap = resource_app_cpu_capacity_us();
    uint32_t used = sum_cpu();
    return used >= cap ? 0 : cap - used;
}

uint64_t resource_mem_remaining_kb(void)
{
    uint64_t cap = resource_app_mem_capacity_kb();
    uint64_t used = sum_min_kb();
    return used >= cap ? 0 : cap - used;
}

int resource_pressure(void)
{
    return pressure;
}

static uint64_t reclaim_elastic(void)
{
    uint64_t got = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        if (apps[i].mem_granted_kb > apps[i].mem_min_kb) {
            got += apps[i].mem_granted_kb - apps[i].mem_min_kb;
            apps[i].mem_granted_kb = apps[i].mem_min_kb;
        }
    }
    return got;
}

void resource_set_pressure(int on)
{
    pressure = on ? 1 : 0;
    if (pressure) {
        uint64_t kb = reclaim_elastic();
        printf("内存压力：收回弹性配额 %lu KB，保底不动。\n", kb);
        printf("CPU 弹性同样停止出借，调度器只给 tier-2 的保底预算。\n");
    }
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
        if (apps[i].alive)
            n++;
    return n;
}

const struct running_app *resource_running(int index)
{
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        if (n == index)
            return &apps[i];
        n++;
    }
    return 0;
}

static void name_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; i < cap - 1 && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

int resource_check(const struct app_manifest *m)
{
    rclear();
    if (!m || !m->name) {
        radd("REFUSED: missing manifest\n");
        return ADMIT_DEVICE;
    }
    if (find_alive_name(m->name) >= 0) {
        radd("REFUSED: ");
        radd(m->name);
        radd(" is already running\n");
        return ADMIT_DUP;
    }

    uint64_t mem_cap = resource_app_mem_capacity_kb();
    uint32_t cpu_cap = resource_app_cpu_capacity_us();
    int mem_over = m->mem_min_kb > mem_cap || m->mem_min_kb > ram_kb;
    int cpu_over = m->cpu_budget_us > cpu_cap;

    if (mem_over || cpu_over) {
        radd("REFUSED: device does not meet minimum requirements\n");
        radd(m->name);
        radd(" declares more than the device has.\n");
        if (mem_over) {
            radd("  declared minimum memory: ");
            radd_u(m->mem_min_kb);
            radd(" KB\n  detected RAM: ");
            radd_u(ram_kb);
            radd(" KB\n  available to applications after kernel reserve and tier-1: ");
            radd_u(mem_cap);
            radd(" KB\n");
        }
        if (cpu_over) {
            radd("  declared minimum CPU: ");
            radd_u(m->cpu_budget_us);
            radd(" us/period\n  available to applications after tier-1 reserve: ");
            radd_u(cpu_cap);
            radd(" us/period\n");
        }
        radd("The app cannot run on this device at all.\n");
        return ADMIT_DEVICE;
    }

    uint64_t mem_left = resource_mem_remaining_kb();
    uint32_t cpu_left = resource_cpu_remaining_us();
    if (m->mem_min_kb > mem_left || m->cpu_budget_us > cpu_left) {
        char names[8][32];
        int nc = 0;
        for (int i = 0; i < MAX_APPS && nc < 8; i++) {
            if (!apps[i].alive)
                continue;
            uint64_t mem2 = mem_left + apps[i].mem_min_kb;
            uint32_t cpu2 = cpu_left + apps[i].cpu_budget_us;
            if (m->mem_min_kb <= mem2 && m->cpu_budget_us <= cpu2) {
                name_copy(names[nc], apps[i].name, 32);
                nc++;
            }
        }
        if (nc == 0) {
            for (int i = 0; i < MAX_APPS && nc < 8; i++) {
                if (!apps[i].alive)
                    continue;
                name_copy(names[nc], apps[i].name, 32);
                nc++;
            }
        }
        radd("REFUSED: please close ");
        radd(nc ? names[0] : "another app");
        radd(" first\nCandidates:");
        if (nc == 0)
            radd(" (none)\n");
        for (int i = 0; i < nc; i++) {
            radd(i ? ", " : " ");
            radd(names[i]);
        }
        radd("\nCPU remaining ");
        radd_u(cpu_left);
        radd(" us/period, need ");
        radd_u(m->cpu_budget_us);
        radd(" us/period.\nMemory remaining ");
        radd_u(mem_left);
        radd(" KB, need ");
        radd_u(m->mem_min_kb);
        radd(" KB.\n");
        return ADMIT_OVERSUB;
    }
    return ADMIT_OK;
}

int resource_commit(const struct app_manifest *m)
{
    int rc = resource_check(m);
    if (rc != ADMIT_OK)
        return rc;
    int slot = -1;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        rclear();
        radd("REFUSED: app table full\n");
        return ADMIT_OVERSUB;
    }

    /* 保底必须落下。弹性如果占着坑，先收回再分配。 */
    while (sum_granted_kb() + m->mem_min_kb > resource_app_mem_capacity_kb()) {
        if (reclaim_elastic() == 0)
            break;
    }

    struct running_app *a = &apps[slot];
    memset(a, 0, sizeof *a);
    name_copy(a->name, m->name, (int)sizeof a->name);
    a->mem_min_kb = m->mem_min_kb;
    a->mem_max_kb = m->mem_max_kb;
    a->cpu_budget_us = m->cpu_budget_us;
    a->cpu_elastic_us = m->cpu_elastic_us;
    a->mem_granted_kb = m->mem_min_kb;
    a->alive = 1;

    uint64_t room = 0;
    uint64_t cap = resource_app_mem_capacity_kb();
    uint64_t granted = sum_granted_kb();
    if (cap > granted)
        room = cap - granted;
    uint32_t want = m->mem_max_kb > m->mem_min_kb ? m->mem_max_kb - m->mem_min_kb : 0;
    if (!pressure && want > 0) {
        uint32_t give = want;
        if ((uint64_t)give > room)
            give = (uint32_t)room;
        a->mem_granted_kb += give;
    }
    return ADMIT_OK;
}

static void kill_slot(int slot)
{
    apps[slot].alive = 0;
}

int resource_close_index(int index)
{
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        if (n == index) {
            printf("已关闭 %s，释放保底内存 %u KB、CPU %u us/period。\n",
                   apps[i].name, apps[i].mem_min_kb, apps[i].cpu_budget_us);
            kill_slot(i);
            return 0;
        }
        n++;
    }
    return -1;
}

int resource_close_name(const char *name)
{
    int i = find_alive_name(name);
    if (i < 0)
        return -1;
    kill_slot(i);
    return 0;
}

int resource_close_all(void)
{
    int n = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        kill_slot(i);
        n++;
    }
    if (n)
        printf("已关闭全部 tier-2 应用（%d 个）。\n", n);
    return n;
}

void resource_show(void)
{
    uint32_t t1cpu = tier1_cpu();
    uint32_t t1mem = tier1_mem();
    uint64_t mem_cap = resource_app_mem_capacity_kb();
    uint32_t cpu_cap = resource_app_cpu_capacity_us();

    printf("\n========== 资源表 / resource table ==========\n");
    printf("检测到的 RAM: %lu KB (%lu MB)，来自设备树/探测\n", ram_kb, ram_kb / 1024);
    printf("内核保留:     %u KB（映像、栈、内核记账，不参与应用分配）\n", KERNEL_RESERVE_KB);
    printf("CPU 周期:     %u us    分级调度: %s\n", PERIOD_US, sched_tiering() ? "ON" : "OFF");
    printf("内核调度最长一次: %u us   超过 200us 的次数: %u   跳过的周期: %lu   时钟跳动: %u 次/最长 %u us\n",
           sched_max_us(), sched_overruns(), sched_period_skips(),
           sched_clock_stalls(), sched_clock_stall_max_us());

    printf("\n[Tier 1] 厂商声明，启动时静态保留，不能被抢走\n");
    for (unsigned i = 0; i < sizeof tier1_table / sizeof tier1_table[0]; i++) {
        const struct tier1_decl *d = &tier1_table[i];
        struct task *t = sched_find(d->name);
        printf("  %-16s CPU %4u us  MEM %5u KB  %s\n", d->name, d->cpu_us, d->mem_kb, d->role);
        if (t && t->state != TASK_EMPTY && t->state != TASK_DEAD) {
            printf("    运行中  hits=%u  misses_on=%u  misses_off=%u  max_latency=%u us\n",
                   t->hits, t->misses, t->misses_off, t->max_lat_us);
        }
    }
    printf("  小计             CPU %4u us  MEM %5u KB\n", t1cpu, t1mem);

    printf("\n[Tier 2] 已准入的应用（保底不可被 tier-3 侵占，弹性可收回）\n");
    int any = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!apps[i].alive)
            continue;
        any = 1;
        struct running_app *a = &apps[i];
        printf("  %-16s CPU %4u+%u us  MEM %u KB (elastic now %u KB)\n",
               a->name, a->cpu_budget_us, a->cpu_elastic_us, a->mem_min_kb,
               a->mem_granted_kb > a->mem_min_kb ? a->mem_granted_kb - a->mem_min_kb : 0);
    }
    if (!any)
        printf("  (无)\n");
    printf("  已承诺保底 CPU %u / %u us    已承诺保底内存 %lu / %lu KB\n",
           sum_cpu(), cpu_cap, sum_min_kb(), mem_cap);

    printf("\n[Tier 3] 默认池，没声明就走这里，轮转分享剩余时间\n");
    printf("  剩余可承诺 CPU: %u us/period\n", resource_cpu_remaining_us());
    printf("  剩余可承诺内存: %lu KB\n", resource_mem_remaining_kb());
    printf("  当前任务:");
    int shown = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = sched_task_at(i);
        if (!t || t->tier != TIER3)
            continue;
        if (t->state == TASK_DEAD || t->state == TASK_EMPTY)
            continue;
        printf(" %s", t->name);
        shown = 1;
    }
    if (!shown)
        printf(" (无)");
    printf("\n");
    printf("压力标志: %s（置位时弹性 CPU/内存都不再外借）\n", pressure ? "ON" : "off");
    printf("==========================================\n");
}
