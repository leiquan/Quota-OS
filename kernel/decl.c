/*
 * E1：统一声明「层级，周期 P 内占用 B」，按每种资源各算一遍准入。
 *
 * 资源：CPU（us/周期）、内存（KB）、两个模拟设备的占用时间（us/周期）。
 * 准入（每种资源）：系统预留 + Tier 1 + ΣTier 2 最低值 + 保护时间 + 本次 <= 容量。
 * 哪种超了就拒绝，并在拒绝里写出那种资源的名字和算式。
 *
 * Tier 1 设备规则：不能打断的设备一律不进 Tier 1；能打断的还要
 *   独占，或者同时满足：能切分、能强制停下（实测的下令到停住时间
 *   不超过设备描述的上限）、最坏耗时有上限（实测最长作业不超过声明）。
 * 实测值由系统在关中断、且确认下一个 Tier 1 保护区之前做得完的窗口里量，
 * 不用设备自报。设备是软件模型（模拟设备），不是真实硬件。
 */
#include "quotaos.h"

#define DEV_CHUNK_US 5      /* 模型每做 5 us 检查一次中止命令 */
#define DEV_RESET_US 2      /* 能打断的设备收到中止后复位所需时间 */
#define DEV_TRIALS 8

enum { R_CPU, R_MEM, R_DMA, R_BLK, R_CAM, NRES };
#define NDEV 3
#define T1_STOP_LIMIT_US 20   /* Tier 1 设备的停止上限不得超过它（算进保护时间） */

struct sim_dev {
    const char *name;
    /* 出厂描述（写死） */
    uint32_t max_job_us, switch_us, stop_us;
    int preemptible, divisible;
    /* 模型的真实作业时长（E3 的少报模式会让它大于声明） */
    uint32_t real_job_us;
    /* 系统实测 */
    int measured;
    uint64_t job_max_t, stop_max_t;
};

static struct sim_dev devs[NDEV] = {
    {"sim-dma", 400, 5, 20, 1, 1, 395, 0, 0, 0},
    {"sim-blk", 2000, 10, 20, 0, 0, 1990, 0, 0, 0},
    {"sim-cam", 300, 5, 30, 1, 1, 290, 0, 0, 0},
};

static const char *res_name[NRES] = {"CPU", "memory", "sim-dma", "sim-blk", "sim-cam"};
static const char *res_unit[NRES] = {"us", "KB", "us", "us", "us"};

struct decl {
    char who[NAME_LEN];
    int tier;
    uint64_t b[NRES];
    int live;
};

#define MAX_DECL 16
static struct decl reg[MAX_DECL];

void decl_init(void)
{
    memset(reg, 0, sizeof reg);
    /* 唯一一条设备上的 Tier 1 声明：只是声明，这一版没有真的驱动去用 sim-dma。 */
    str_copy(reg[0].who, "display-vsync", NAME_LEN);
    reg[0].tier = TIER1;
    reg[0].b[R_DMA] = 2000;
    reg[0].live = 1;
}

/* ---------- 模拟设备与实测 ---------- */

/* 在关中断、并且下一个 Tier 1 保护区之前做得完的地方执行一次作业。
 * abort_t 不为 0 时，作业开始 abort_t tick 后发中止命令；返回下令到停住的 tick。 */
static uint64_t dev_exec(struct sim_dev *d, uint64_t abort_t, uint64_t *ran)
{
    uint64_t job = us_to_ticks(d->real_job_us);
    uint64_t need = job + us_to_ticks(d->switch_us + DEV_RESET_US + DEV_CHUNK_US);
    uint64_t t0;
    for (;;) {
        asm volatile("msr daifset, #2" ::: "memory");
        t0 = ticks_now();
        if (sched_next_guard(t0) > t0 + need)
            break;
        asm volatile("msr daifclr, #2" ::: "memory");
        busy_ticks(us_to_ticks(DEV_CHUNK_US));
    }
    busy_ticks(us_to_ticks(d->switch_us));
    uint64_t s = ticks_now();
    uint64_t chunk = us_to_ticks(DEV_CHUNK_US);
    uint64_t abort_at = abort_t ? s + abort_t : ~0ull;
    uint64_t stop = 0, now = s;
    while (now - s < job) {
        uint64_t left = job - (now - s);
        busy_ticks(left < chunk ? left : chunk);
        now = ticks_now();
        if (now >= abort_at && d->preemptible) {
            busy_ticks(us_to_ticks(DEV_RESET_US));
            now = ticks_now();
            stop = now - abort_at;
            break;
        }
    }
    if (abort_t && !stop)
        stop = now > abort_at ? now - abort_at : 0;   /* 不能打断：做完才停 */
    asm volatile("msr daifclr, #2" ::: "memory");
    if (ran)
        *ran = now - s;
    return stop;
}

static void dev_measure(struct sim_dev *d)
{
    uint64_t job = us_to_ticks(d->real_job_us);
    d->job_max_t = 0;
    d->stop_max_t = 0;
    for (int k = 0; k < DEV_TRIALS; k++) {
        uint64_t ran;
        dev_exec(d, 0, &ran);
        if (ran > d->job_max_t)
            d->job_max_t = ran;
        uint64_t st = dev_exec(d, job * (uint64_t)(k + 1) / (DEV_TRIALS + 1), 0);
        if (st > d->stop_max_t)
            d->stop_max_t = st;
    }
    d->measured = 1;
}

static void measure_all(void)
{
    for (int i = 0; i < NDEV; i++)
        if (!devs[i].measured)
            dev_measure(&devs[i]);
}

static uint64_t up_us(uint64_t t)
{
    uint64_t u = ticks_to_us(t);
    return us_to_ticks(u) < t ? u + 1 : u;
}

static void print_devices(void)
{
    printf("  device   preempt divisible max_job switch stop_bound | measured:\n");
    printf("                                                       job_max  stop_max\n");
    for (int i = 0; i < NDEV; i++) {
        struct sim_dev *d = &devs[i];
        printf("  %s  %7s %9s %7u %6u %10u | %7lu %9lu us\n", d->name,
               d->preemptible ? "yes" : "no", d->divisible ? "yes" : "no", d->max_job_us,
               d->switch_us, d->stop_us, up_us(d->job_max_t), up_us(d->stop_max_t));
    }
}

/* ---------- 准入 ---------- */

static char why[REASON_LEN];
static int wn;

static void wadd(const char *s)
{
    while (*s && wn < REASON_LEN - 1)
        why[wn++] = *s++;
    why[wn] = 0;
}

static void wadd_u(uint64_t v)
{
    char t[24];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char o[24];
    int n = 0;
    while (i)
        o[n++] = t[--i];
    o[n] = 0;
    wadd(o);
}

static uint64_t cap_of(int r)
{
    return r == R_MEM ? resource_ram_kb() : PERIOD_US;
}

static uint64_t reserve_of(int r)
{
    return r == R_CPU ? SYS_RESERVE_US : r == R_MEM ? KERNEL_RESERVE_KB : 0;
}

static uint64_t t1_of(int r)
{
    uint64_t s = r == R_CPU ? resource_t1_cpu_us() : r == R_MEM ? resource_t1_mem_kb() : 0;
    for (int i = 0; i < MAX_DECL; i++)
        if (reg[i].live && reg[i].tier == TIER1)
            s += reg[i].b[r];
    return s;
}

static uint64_t t2_of(int r)
{
    uint64_t s = r == R_CPU ? resource_cpu_min_sum_us() : r == R_MEM ? resource_mem_min_sum_kb() : 0;
    for (int i = 0; i < MAX_DECL; i++)
        if (reg[i].live && reg[i].tier == TIER2)
            s += reg[i].b[r];
    return s;
}

/* 保护时间：CPU 每个 Tier 1 槽前 GUARD_US；设备每条 Tier 1 声明留一次声明的停止上限。 */
static uint64_t guard_of(int r, int extra_t1)
{
    if (r == R_CPU)
        return resource_guard_us();
    if (r == R_MEM)
        return 0;
    struct sim_dev *d = &devs[r - R_DMA];
    uint64_t n = extra_t1;
    for (int i = 0; i < MAX_DECL; i++)
        if (reg[i].live && reg[i].tier == TIER1 && reg[i].b[r])
            n++;
    return n * d->stop_us;   /* 只用出厂声明的停止上限：每次启动结果都一样 */
}

/* Tier 1 设备条件，只看出厂声明（实测只用来核对，见菜单 11 的表）。
 * 不满足时把缺的条件全部写进 why，返回非 0。 */
static int t1_device_rule(const char *who, int r)
{
    struct sim_dev *d = &devs[r - R_DMA];
    int other = 0;
    for (int i = 0; i < MAX_DECL; i++)
        if (reg[i].live && reg[i].b[r] && strcmp(reg[i].who, who))
            other = 1;
    int bound_ok = d->stop_us <= T1_STOP_LIMIT_US;
    int refuse = !d->preemptible || !bound_ok || (other && !d->divisible);
    if (!refuse)
        return 0;
    if (!d->preemptible) {
        wadd("  missing: 不能强制叫停 (");
        wadd(d->name);
        wadd(" is non-preemptible)\n");
    }
    if (!bound_ok) {
        wadd("  missing: 停止上限超过 Tier 1 限值 (declared stop ");
        wadd_u(d->stop_us);
        wadd(" us > ");
        wadd_u(T1_STOP_LIMIT_US);
        wadd(" us)\n");
    }
    if (!d->divisible) {
        wadd("  missing: 不可切分 (not divisible)");
        wadd(other ? "\n" : "; exclusive use would waive this one\n");
    }
    return 1;
}

/* 返回超额资源的位图（1<<r）；Tier 1 设备条件不满足时位图里加 1<<8。 */
static int admit(const char *who, int tier, const uint64_t *b)
{
    wn = 0;
    why[0] = 0;
    int bad = 0;
    if (tier == TIER3)
        return 0;   /* 三级只分剩下的，不做准入 */
    for (int r = R_DMA; r < NRES && tier == TIER1; r++)
        if (b[r] && t1_device_rule(who, r))
            bad |= 1 << 8;
    for (int r = 0; r < NRES; r++) {
        if (!b[r])
            continue;
        uint64_t res = reserve_of(r), t1 = t1_of(r), t2 = t2_of(r);
        uint64_t g = guard_of(r, tier == TIER1 && r >= R_DMA);
        uint64_t sum = res + t1 + t2 + g + b[r];
        if (sum <= cap_of(r))
            continue;
        bad |= 1 << r;
        wadd("  ");
        wadd(res_name[r]);
        wadd(" overcommitted: reserve ");
        wadd_u(res);
        wadd(" + tier-1 ");
        wadd_u(t1);
        wadd(" + tier-2 min ");
        wadd_u(t2);
        wadd("\n    + guard ");
        wadd_u(g);
        wadd(" + this ");
        wadd_u(b[r]);
        wadd(" = ");
        wadd_u(sum);
        wadd(" > capacity ");
        wadd_u(cap_of(r));
        wadd(" ");
        wadd(res_unit[r]);
        wadd("\n");
    }
    return bad;
}

static void print_req(const char *who, int tier, const uint64_t *b)
{
    printf("%s asks Tier %d (per %u us):\n ", who, tier, PERIOD_US);
    const char *sep = "";
    for (int r = 0; r < NRES; r++)
        if (b[r]) {
            printf("%s %s %lu %s", sep, res_name[r], b[r], res_unit[r]);
            sep = ",";
        }
    printf("\n");
}

static int try_req(const char *who, int tier, uint64_t cpu, uint64_t mem, uint64_t dma,
                   uint64_t blk, uint64_t cam, int verbose)
{
    uint64_t b[NRES] = {cpu, mem, dma, blk, cam};
    int bad = admit(who, tier, b);
    if (verbose) {
        print_req(who, tier, b);
        if (bad) {
            printf("REFUSED:");
            for (int r = 0; r < NRES; r++)
                if (bad & (1 << r))
                    printf(" %s", res_name[r]);
            if (bad & (1 << 8))
                printf(" (device not allowed in Tier 1)");
            printf("\n%s", why);
        } else {
            printf("OK: admitted (not started; released again)\n");
        }
    }
    return bad;
}

static int why_has(const char *s)
{
    for (int i = 0; why[i]; i++) {
        int j = 0;
        while (s[j] && why[i + j] == s[j])
            j++;
        if (!s[j])
            return 1;
    }
    return 0;
}

/* ---------- 菜单 11 / 12 ---------- */

void decl_demo_device_tier1(void)
{
    printf("\n=== 11 设备申请 Tier 1：不能打断的设备被拒 ===\n");
    printf("Three simulated devices (software models, not real hardware).\n");
    printf("The system measures each one (%d full jobs, %d aborts) with IRQs\n",
           DEV_TRIALS, DEV_TRIALS);
    printf("masked, only where the job ends before the next tier-1 guard.\n");
    printf("Admission uses only the declared (factory) values; the measured\n");
    printf("ones are for verification.\n");
    measure_all();
    print_devices();
    printf("display-vsync already declares sim-dma 2000 us (Tier 1).\n\n");
    try_req("cam-app", TIER1, 0, 0, 1000, 0, 0, 1);
    try_req("rec-app", TIER1, 0, 0, 0, 1000, 0, 1);
    try_req("rec-app", TIER2, 0, 0, 0, 1000, 0, 1);
    try_req("scan-app", TIER1, 0, 0, 0, 0, 500, 1);
}

void decl_demo_overcommit(void)
{
    printf("\n=== 12 超额申请在启动时被拒，并说明是哪种资源 ===\n");
    measure_all();
    try_req("cpu-hog", TIER2, 8200, 1024, 0, 0, 0, 1);
    try_req("mem-hog", TIER2, 1000, 110000, 0, 0, 0, 1);
    try_req("dma-hog", TIER2, 1000, 1024, 8500, 0, 0, 1);
    try_req("all-hog", TIER2, 8200, 110000, 8500, 0, 0, 1);
    try_req("fits", TIER2, 8000, 90000, 7000, 0, 0, 1);
}

/* ---------- E1 验收（qa 引导参数 e1） ---------- */

void decl_qa_e1(int unused)
{
    (void)unused;
    measure_all();
    /* 第 2 项：不能打断的设备申请 Tier 1，100 次都要被拒并写出缺的条件 */
    int n = 0, refused = 0, named = 0;
    for (uint64_t b = 100; b <= 10000; b += 100) {
        int bad = try_req("rec-app", TIER1, 0, 0, 0, b, 0, 0);
        n++;
        if (bad & (1 << 8))
            refused++;
        if (why_has("不能强制叫停") && why_has("不可切分"))
            named++;
    }
    int ctrl_ok = try_req("cam-app", TIER1, 0, 0, 1000, 0, 0, 0) == 0 &&
                  try_req("rec-app", TIER2, 0, 0, 0, 1000, 0, 0) == 0;
    printf("E1 item2 non-preemptible Tier 1: asked %d, refused %d,\n", n, refused);
    printf("  both missing conditions named %d; controls admitted: %s\n", named,
           ctrl_ok ? "yes" : "no");
    printf("E1 item2 %s\n", refused == n && named == n && ctrl_ok ? "PASS" : "FAIL");

    /* 出厂声明的停止上限超过 Tier 1 限值：固定拒绝，与实测无关 */
    int bn = 0, brefused = 0;
    for (uint64_t b = 100; b <= 3000; b += 100) {
        int bad = try_req("scan-app", TIER1, 0, 0, 0, 0, b, 0);
        bn++;
        if ((bad & (1 << 8)) && why_has("停止上限超过 Tier 1 限值"))
            brefused++;
    }
    int bctrl = try_req("scan-app", TIER2, 0, 0, 0, 0, 500, 0) == 0;
    printf("E1 declared-bound sim-cam (declared stop %u us > %u us) Tier 1:\n",
           devs[2].stop_us, T1_STOP_LIMIT_US);
    printf("  asked %d, refused naming the bound %d; Tier 2 control admitted: %s\n", bn,
           brefused, bctrl ? "yes" : "no");
    printf("E1 declared-bound %s\n", brefused == bn && bctrl ? "PASS" : "FAIL");

    /* 第 4 项：超额在启动时被拒，并写出是哪种资源 */
    static const struct {
        uint64_t cpu, mem, dma;
        int want;
    } c[] = {
        {8200, 1024, 0, 1 << R_CPU},
        {1000, 110000, 0, 1 << R_MEM},
        {1000, 1024, 8500, 1 << R_DMA},
        {8200, 110000, 8500, (1 << R_CPU) | (1 << R_MEM) | (1 << R_DMA)},
        {8071, 0, 0, 1 << R_CPU},       /* 350+1500+8071+80 = 10001 */
        {8070, 0, 0, 0},                /* 正好 10000 */
        {8000, 90000, 7000, 0},
    };
    int cases = 0, right = 0, rnamed = 0;
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        int bad = try_req("probe", TIER2, c[i].cpu, c[i].mem, c[i].dma, 0, 0, 0);
        cases++;
        if (bad == c[i].want)
            right++;
        int ok = 1;
        for (int r = 0; r < NRES; r++) {
            char key[32];
            int k = 0;
            for (const char *s = res_name[r]; *s; s++)
                key[k++] = *s;
            const char *t = " overcommitted";
            while (*t)
                key[k++] = *t++;
            key[k] = 0;
            if (((c[i].want >> r) & 1) != why_has(key))
                ok = 0;
        }
        rnamed += ok;
    }
    printf("E1 item4 overcommit: cases %d, verdict right %d,\n", cases, right);
    printf("  resource named exactly %d\n", rnamed);
    printf("E1 item4 %s\n", right == cases && rnamed == cases ? "PASS" : "FAIL");
    uint64_t ma, mb;
    sched_total_misses(&ma, &mb);
    printf("Tier 1 misses since boot (device measurements included): %lu\n", ma + mb);
}
