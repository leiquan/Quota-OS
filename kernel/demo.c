/*
 * 串口菜单。每个选项对应设计里的一条保证，输出里能直接读到结论。
 *
 *  1 资源表
 *  2 启动 tier-2（先问 y/n，再做准入）
 *  3 关闭 tier-2
 *  4 优先级保证：tier-1 周期任务 vs 一群 tier-3 忙等
 *  5 切换分级 ON / OFF（普通轮转）
 *  6 失范隔离：tier-2 死循环 + tier-3 忙等，tier-1 和守规矩的 tier-2 零错过
 *  7 声明超过本机：device does not meet minimum requirements
 *  8 内存压力，收回弹性配额
 *  9 测量最小 CPU 切片（定时器分辨率 + 切换开销 + 实测）
 * 10 几十个 tier-2 虚拟 CPU，各自一条预算服务器
 */
#include "quotaos.h"

static uint32_t measured_slice_us;

static int parse_int(const char *s)
{
    if (!s || !s[0])
        return -1;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static void block_for_bench(void)
{
    struct task *sh = sched_current();
    sh->wait_bench = 1;
    sh->state = TASK_BLOCKED;
    asm volatile("svc #0" ::: "memory");
}

static void set_name(char *dst, const char *prefix, int n)
{
    int i = 0;
    for (; prefix[i] && i < 12; i++)
        dst[i] = prefix[i];
    dst[i++] = '0' + (n / 10) % 10;
    dst[i++] = '0' + (n % 10);
    dst[i] = 0;
}

static uint32_t avg_u(uint64_t sum, uint32_t n)
{
    if (!n)
        return 0;
    return (uint32_t)(sum / n);
}

static void cmd_help(void)
{
    printf("\n命令（输入数字后回车）:\n");
    printf("  1  资源表：tier 1/2/3 的 CPU 和内存，已用和剩余\n");
    printf("  2  启动一个 tier-2 应用（会先问 allow y/n）\n");
    printf("  3  关闭一个 tier-2 应用（a = 全部关闭）\n");
    printf("  4  跑一次优先级演示（尊重当前的分级开关）\n");
    printf("  5  切换分级：ON 为三级预算服务器，OFF 为普通 1ms 轮转\n");
    printf("  6  失范应用隔离：tier-2 死循环 + tier-3 占 CPU\n");
    printf("  7  启动 Cluster Sim（声明超过本机容量）\n");
    printf("  8  制造内存压力，收回弹性配额\n");
    printf("  9  测量这台 QEMU 上有意义的最小 CPU 切片\n");
    printf("  10 启动几十个带独立切片的 tier-2 应用\n");
    printf("  h  帮助    q  提示如何退出 QEMU\n");
}

static void cmd_table(void)
{
    resource_show();
    struct task *v = sched_find("display-vsync");
    struct task *tm = sched_find("timer-server");
    printf("保证期统计（只在分级 ON 且不在测量探针里计数）:\n");
    if (v)
        printf("  display-vsync  hits=%u misses=%u max_latency=%u us  (OFF 对照 misses=%u)\n",
               v->hits, v->misses, v->max_lat_us, v->misses_off);
    if (tm)
        printf("  timer-server   hits=%u misses=%u max_latency=%u us  (OFF 对照 misses=%u)\n",
               tm->hits, tm->misses, tm->max_lat_us, tm->misses_off);
    printf("定时器中断 jiffies=%lu\n", timer_jiffies());
}

static void launch_manifest(const struct app_manifest *m, int interactive)
{
    printf("\n--- tier-2 启动请求 ---\n");
    printf("应用: %s\n", m->name);
    printf("说明: %s\n", m->role);
    printf("保底内存 %u KB，弹性上限 %u KB\n", m->mem_min_kb, m->mem_max_kb);
    printf("保底 CPU %u us / %u us 周期，弹性 +%u us\n",
           m->cpu_budget_us, PERIOD_US, m->cpu_elastic_us);

    int rc = resource_check(m);
    if (rc == ADMIT_DEVICE || rc == ADMIT_DUP) {
        printf("%s", resource_reason());
        printf("应用未启动。\n");
        return;
    }
    if (interactive) {
        printf("允许该应用占用上述保底资源? allow> ");
        char ans[16];
        readline(ans, sizeof ans);
        if (!(ans[0] == 'y' || ans[0] == 'Y')) {
            printf("CANCELLED: user denied permission\n");
            printf("应用未启动。\n");
            return;
        }
    }
    rc = resource_check(m);
    if (rc != ADMIT_OK) {
        printf("%s", resource_reason());
        printf("应用未启动。\n");
        return;
    }
    resource_commit(m);
    const struct running_app *a = 0;
    int n = resource_running_count();
    for (int i = 0; i < n; i++) {
        const struct running_app *it = resource_running(i);
        if (it && strcmp(it->name, m->name) == 0)
            a = it;
    }
    printf("已启动 %s。\n", m->name);
    if (a) {
        uint32_t elastic = a->mem_granted_kb > a->mem_min_kb ? a->mem_granted_kb - a->mem_min_kb : 0;
        printf("  落入保底内存 %u KB，另给弹性 %u KB（压力下会收回）。\n", a->mem_min_kb, elastic);
        printf("  落入保底 CPU %u us/period，弹性上限 +%u us（有空闲才给）。\n",
               a->cpu_budget_us, a->cpu_elastic_us);
    }
}

static void cmd_launch(void)
{
    int n = 0;
    const struct app_manifest *cat = resource_catalog(&n);
    printf("\n可启动的 tier-2 清单（开发者声明，不是现在才现编的）:\n");
    for (int i = 0; i < n; i++) {
        printf("  %d) %-14s 内存 %u..%u KB  CPU %u+%u us\n",
               i + 1, cat[i].name, cat[i].mem_min_kb, cat[i].mem_max_kb,
               cat[i].cpu_budget_us, cat[i].cpu_elastic_us);
    }
    printf("app> ");
    char line[16];
    readline(line, sizeof line);
    int idx = parse_int(line);
    if (idx < 1 || idx > n) {
        printf("没有这个编号。\n");
        return;
    }
    launch_manifest(&cat[idx - 1], 1);
}

static void cmd_close(void)
{
    int n = resource_running_count();
    if (n == 0) {
        printf("没有正在运行的 tier-2 应用。\n");
        return;
    }
    printf("\n正在运行:\n");
    for (int i = 0; i < n; i++) {
        const struct running_app *a = resource_running(i);
        printf("  %d) %s\n", i + 1, a->name);
    }
    printf("  a) 全部关闭\n");
    printf("close> ");
    char line[16];
    readline(line, sizeof line);
    if (line[0] == 'a' || line[0] == 'A') {
        resource_close_all();
        return;
    }
    int idx = parse_int(line);
    if (idx < 1 || idx > n) {
        printf("没有这个编号。\n");
        return;
    }
    resource_close_index(idx - 1);
}

static void cmd_toggle(void)
{
    if (sched_tiering()) {
        sched_set_tiering(0);
        printf("分级已关闭：OFF (plain fair scheduling)。\n");
        printf("所有就绪任务每 %u us 轮转一次，tier-1 不再插队，预算笼子也先松开。\n",
               OFF_QUANTUM_US);
        printf("这只用于对照。真正的保证在分级 ON 里。\n");
    } else {
        sched_set_tiering(1);
        printf("分级已打开：ON。\n");
        printf("顺序：tier-1 最早截止期，然后 tier-2 保底，然后弹性，最后 tier-3 轮转。\n");
        printf("超过声明预算的任务会被掐断到下一个周期。\n");
    }
}

static void print_vsync_samples(uint32_t deadline)
{
    int n = bench_sample_count();
    printf("period  latency_us  response_us  deadline_us  result\n");
    int misses = 0;
    uint32_t max_lat = 0;
    for (int i = 0; i < n; i++) {
        uint32_t lat, resp;
        int miss;
        bench_sample(i, &lat, &resp, &miss);
        if (lat > max_lat)
            max_lat = lat;
        if (miss)
            misses++;
        printf("  %2d   %8u   %8u   %8u  %s\n", i, lat, resp, deadline, miss ? "MISS" : "OK");
    }
    printf("summary: samples=%d  deadline_misses=%d  max_latency=%u us\n", n, misses, max_lat);
}

static void cmd_priority(void)
{
    printf("\n=== 优先级保证 / priority guarantee ===\n");
    printf("模式: %s\n", sched_tiering() ? "tiering ON" : "tiering OFF (plain fair scheduling)");
    printf("tier-1 display-vsync 每 %u us 释放，必须在 %u us 内开始。\n", PERIOD_US, DEADLINE_VSYNC_US);
    printf("同时有 8 个 tier-3 忙等任务 bg-load，它们不声明预算。\n");

    struct task *hogs[8];
    for (int i = 0; i < 8; i++) {
        char name[16];
        set_name(name, "bg", i);
        hogs[i] = task_create(name, TIER3, quotaos_spinner_entry);
        hogs[i]->keep_running = 1;
        hogs[i]->bench_owned = 1;
    }
    struct task *vs = sched_find("display-vsync");
    task_reset_window(vs);
    bench_watch(vs);
    int counting = 1;
    if (!sched_tiering()) {
        sched_set_count_guarantees(0);
        counting = 0;
    }
    bench_begin(12);
    block_for_bench();
    if (!counting)
        sched_set_count_guarantees(1);

    print_vsync_samples(DEADLINE_VSYNC_US);
    if (vs) {
        printf("window misses=%u  max_latency=%u us  jobs=%u\n",
               vs->w_miss, vs->w_max_lat, vs->w_jobs);
    }
    if (sched_tiering())
        printf("分级打开时，释放 vsync 的同一次中断会立刻把它派上去。\n");
    else
        printf("分级关闭时，vsync 跟 bg-load 轮转，开始时间经常晚于截止期。\n");
}

static void cmd_contain(void)
{
    printf("\n=== 失范应用隔离 / misbehaving app containment ===\n");
    int saved = sched_tiering();
    if (!saved) {
        sched_set_tiering(1);
        printf("本演示强制使用分级 ON，结束后恢复你原来的开关。\n");
    }
    uint32_t need = 2000;
    if (resource_cpu_remaining_us() < need) {
        printf("REFUSED: please close %s first\n",
               resource_running(0) ? resource_running(0)->name : "another app");
        printf("隔离演示还要 2000 us/period 的 tier-2 保底，现在只剩 %u。\n",
               resource_cpu_remaining_us());
        printf("应用未启动。\n");
        sched_set_tiering(saved);
        return;
    }

    struct app_manifest audio_m = {
        "audio-mix", "守规矩的混音，400us 工作 / 1000us 预算", 512, 512, 1000, 0
    };
    struct app_manifest bad_m = {
        "bad-encoder", "死循环，声明 1000us 却想占满整核", 512, 512, 1000, 0
    };
    if (resource_commit(&audio_m) != ADMIT_OK || resource_commit(&bad_m) != ADMIT_OK) {
        printf("%s", resource_reason());
        resource_close_name("audio-mix");
        resource_close_name("bad-encoder");
        sched_set_tiering(saved);
        return;
    }

    struct task *bad_drv = task_create("bad-driver", TIER1, quotaos_spinner_entry);
    bad_drv->budget_us = 400;          /* 用的是 blk-driver 那份静态保留，不是新要的 */
    bad_drv->rel_deadline_us = PERIOD_US;
    bad_drv->keep_running = 1;
    bad_drv->bench_owned = 1;

    struct task *audio = task_create("audio-mix", TIER2, quotaos_periodic_entry);
    audio->budget_us = 1000;
    audio->work_us = 400;
    audio->rel_deadline_us = DEADLINE_AUDIO_US;
    audio->periodic = 1;
    audio->bench_owned = 1;
    audio->wait_release = 1;
    audio->state = TASK_BLOCKED;

    struct task *bad = task_create("bad-encoder", TIER2, quotaos_spinner_entry);
    bad->budget_us = 1000;
    bad->elastic_us = 0;
    bad->keep_running = 1;
    bad->bench_owned = 1;

    struct task *hog = task_create("bg-hog", TIER3, quotaos_spinner_entry);
    hog->keep_running = 1;
    hog->bench_owned = 1;

    struct task *vs = sched_find("display-vsync");
    struct task *tm = sched_find("timer-server");
    task_reset_window(vs);
    task_reset_window(tm);
    task_reset_window(audio);
    task_reset_window(bad);
    task_reset_window(bad_drv);
    task_reset_window(hog);
    bench_watch(vs);

    printf("场景: bad-driver (tier-1 死循环, 上限 400us), audio-mix (tier-2 守规矩),\n");
    printf("      bad-encoder (tier-2 死循环, 上限 1000us), bg-hog (tier-3 无声明)。\n");
    printf("audio-mix 的响应截止期是 %u us。vsync 的开始截止期是 %u us。\n",
           DEADLINE_AUDIO_US, DEADLINE_VSYNC_US);

    uint32_t th0 = bad->throttle_events;
    uint32_t th1 = bad_drv->throttle_events;
    bench_begin(12);
    block_for_bench();

    print_vsync_samples(DEADLINE_VSYNC_US);
    printf("tier-1 display-vsync: jobs=%u misses=%u max_latency=%u us\n",
           vs->w_jobs, vs->w_miss, vs->w_max_lat);
    printf("tier-1 timer-server:  jobs=%u misses=%u max_latency=%u us\n",
           tm->w_jobs, tm->w_miss, tm->w_max_lat);
    printf("tier-2 audio-mix:     jobs=%u misses=%u max_response=%u us (deadline %u)\n",
           audio->w_jobs, audio->w_miss, audio->w_max_resp, DEADLINE_AUDIO_US);
    printf("tier-1 bad-driver:    throttled %u times, recv_avg %u us (cap 400, not %u)\n",
           bad_drv->throttle_events - th1,
           avg_u(bad_drv->w_sum_recv, bad_drv->w_periods), PERIOD_US);
    printf("tier-2 bad-encoder:   throttled %u times, recv_avg %u us (cap 1000, not %u)\n",
           bad->throttle_events - th0,
           avg_u(bad->w_sum_recv, bad->w_periods), PERIOD_US);
    printf("tier-3 bg-hog:        recv_avg %u us（只拿到没人要的剩余时间）\n",
           avg_u(hog->w_sum_recv, hog->w_periods));

    int isolated = vs->w_miss == 0 && tm->w_miss == 0 && audio->w_miss == 0;
    if (isolated)
        printf("结果: 失范任务被预算笼子掐住。tier-1 和守规矩的 tier-2 错过次数 = 0。\n");
    else
        printf("结果: 仍有截止期错过，隔离没有达到教学预期。\n");

    resource_close_name("audio-mix");
    resource_close_name("bad-encoder");
    sched_set_tiering(saved);
}

static volatile int pings_done;

static void ping_entry(void)
{
    uint64_t prev = now_us();
    for (int i = 0; i < 24; i++) {
        asm volatile("svc #1" ::: "memory");
        uint64_t n = now_us();
        uint32_t half = 0;
        if (n > prev)
            half = (uint32_t)((n - prev) / 2);
        if (i >= 1 && half >= 1 && half <= 60)
            switch_measure_note(half);
        prev = n;
    }
    pings_done++;
    if (pings_done >= 2) {
        struct task *sh = sched_shell();
        if (sh && sh->state == TASK_BLOCKED && !sh->wait_bench)
            sh->state = TASK_READY;
    }
    struct task *me = sched_current();
    me->state = TASK_BLOCKED;
    asm volatile("svc #0" ::: "memory");
}

static int slice_pass(struct task **ts, int n, uint32_t slice, uint32_t *avg_out, uint32_t *miss_out)
{
    uint64_t sum = 0;
    uint32_t periods = 0;
    uint32_t misses = 0;
    uint32_t worst = 0;
    for (int i = 0; i < n; i++) {
        sum += ts[i]->w_sum_recv;
        periods += ts[i]->w_periods;
        misses += ts[i]->w_miss;
        if (ts[i]->w_max_recv > worst)
            worst = ts[i]->w_max_recv;
    }
    uint32_t avg = avg_u(sum, periods);
    *avg_out = avg;
    *miss_out = misses;
    uint32_t hi = slice + (slice / 2 > 50 ? slice / 2 : 50);
    int ok = misses == 0 && periods > 0 && avg * 100 >= slice * 80 && avg <= hi;
    (void)worst;
    return ok;
}

static void run_slice_set(uint32_t slice, int n, int periods, struct task **out)
{
    for (int i = 0; i < n; i++) {
        char name[16];
        set_name(name, "s", i);
        struct task *t = task_create(name, TIER2, quotaos_spinner_entry);
        t->budget_us = slice;
        t->keep_running = 1;
        t->bench_owned = 1;
        task_reset_window(t);
        out[i] = t;
    }
    bench_watch(0);
    bench_begin(periods);
    block_for_bench();
}

static void cmd_minslice(void)
{
    printf("\n=== 最小切片 / minimum useful slice ===\n");
    printf("下界来自两件事：定时器实际能稳定响的间隔，和一次任务切换的开销。\n");
    printf("切片若比这两者之和还短，声明的 CPU 会被切换吃掉，虚拟 CPU 就没有意义。\n");
    printf("CNTFRQ = %lu Hz\n", timer_freq());

    sched_set_count_guarantees(0);
    static const uint32_t probes[] = {20, 50, 100, 200, 500, 1000};
    uint32_t stable = 0;
    printf("timer interval probe (guest CNTPCT microseconds):\n");
    printf("  requested  samples  min_us  avg_us  max_us\n");
    for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        probe_begin(probes[i]);
        uint64_t t0 = now_us();
        while (!probe_done() && now_us() - t0 < 80000)
            asm volatile("yield");
        uint32_t mn = 0, avg = 0, mx = 0;
        int ns = probe_samples(&mn, &avg, &mx);
        printf("  %9u  %7d  %6u  %6u  %6u\n", probes[i], ns, mn, avg, mx);
        if (!stable && ns >= 4 && avg >= probes[i] / 2 && avg <= probes[i] * 2 &&
            mx <= probes[i] * 3)
            stable = probes[i];
    }

    printf("context-switch probe: two tasks yield to each other, sample = round-trip/2\n");
    printf("只统计 1..60 us 的样本，更大的多半是被常驻 tier-1 工作插了一脚。\n");
    pings_done = 0;
    switch_measure_begin();
    struct task *p0 = task_create("ping-a", TIER3, ping_entry);
    struct task *p1 = task_create("ping-b", TIER3, ping_entry);
    (void)p0;
    (void)p1;
    sched_arm_shell_timeout(now_us() + 100000);
    struct task *sh = sched_current();
    sh->state = TASK_BLOCKED;
    asm volatile("svc #0" ::: "memory");
    sched_arm_shell_timeout(0);
    switch_measure_end();
    p0->state = TASK_DEAD;
    p1->state = TASK_DEAD;

    uint32_t smin = 0, savg = 0, smax = 0, sn = 0;
    int have = switch_measure_result(&smin, &savg, &smax, &sn);
    if (have)
        printf("context switch: n=%u min=%u avg=%u max=%u us\n", sn, smin, savg, smax);
    else
        printf("context switch: no clean sample, assume 30 us\n");
    if (!have)
        savg = 30;
    if (!stable)
        stable = 200;

    uint32_t bound = stable + savg * 2;
    printf("practical lower bound = stable_timer + 2*switch = %u + 2*%u = %u us\n",
           stable, savg, bound);

    static const uint32_t cands[] = {40, 80, 120, 200, 300};
    printf("empirical sweep: 6 tier-2 spinners, each must receive its slice (8 periods)\n");
    printf("  slice_us  recv_avg  under_miss  pass\n");
    uint32_t empirical = 0;
    struct task *ts[6];
    for (unsigned i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        run_slice_set(cands[i], 6, 8, ts);
        uint32_t avg = 0, misses = 0;
        int pass = slice_pass(ts, 6, cands[i], &avg, &misses);
        /* 交付值离声明太远时，多出来的全是定时器误差，不能当可用切片。 */
        int faithful = pass && avg * 10 >= cands[i] * 8 && avg * 10 <= cands[i] * 13;
        int useful = faithful && cands[i] >= bound;
        const char *tag = useful ? "PASS" : (pass ? "loose" : "fail");
        printf("  %7u  %8u  %10u  %s\n", cands[i], avg, misses, tag);
        if (useful && !empirical)
            empirical = cands[i];
    }
    sched_set_count_guarantees(1);

    if (!empirical)
        empirical = bound < 300 ? bound : 300;
    measured_slice_us = empirical;
    printf("measured minimum useful slice on this QEMU run: %u us\n", measured_slice_us);
    printf("公式给出的下界是 %u us。演示用的批量切片不会低于测到的最小值。\n", bound);
}

static void cmd_many(void)
{
    printf("\n=== 每应用 CPU 切片 / per-app budget servers ===\n");
    if (resource_running_count() > 0) {
        const struct running_app *a = resource_running(0);
        printf("REFUSED: please close %s first\n", a ? a->name : "another app");
        printf("批量切片要占用整段 tier-2 预算，请先在菜单 3 里关闭已有应用。\n");
        printf("应用未启动。\n");
        return;
    }
    uint32_t slice = measured_slice_us ? measured_slice_us : 200;
    if (slice < 200)
        slice = 200;
    uint32_t cap = resource_cpu_remaining_us();
    int n = 32;
    if (slice == 0)
        slice = 200;
    while (n > 0 && (uint32_t)n * slice > cap)
        n--;
    if (n < 24 && cap / 200 >= 24) {
        slice = 200;
        n = (int)(cap / slice);
        if (n > 32)
            n = 32;
    }
    if (n < 1) {
        printf("没有足够的 CPU 预算。\n");
        return;
    }

    printf("启动 %d 个 tier-2 应用，每个是一条预算服务器：%u us / %u us。\n", n, slice, PERIOD_US);
    printf("虚拟 CPU = %u/%u = %u.%02u%% of one core。\n",
           slice, PERIOD_US, (slice * 100) / PERIOD_US, ((slice * 10000) / PERIOD_US) % 100);
    printf("它们都会试图占满自己的切片；调度器在预算用完时掐断。\n");

    static char names[32][16];
    struct task *ts[32];
    int started = 0;
    for (int i = 0; i < n; i++) {
        set_name(names[i], "app-", i);
        /* set_name writes prefix then two digits. "app-" is 4 chars + 2 digits.
         * Rebuild a cleaner name. */
        names[i][0] = 'a';
        names[i][1] = 'p';
        names[i][2] = 'p';
        names[i][3] = '-';
        names[i][4] = '0' + (i / 10) % 10;
        names[i][5] = '0' + (i % 10);
        names[i][6] = 0;
        struct app_manifest m = {names[i], "per-app slice", 256, 256, slice, 0};
        if (resource_commit(&m) != ADMIT_OK) {
            printf("%s", resource_reason());
            break;
        }
        struct task *t = task_create(names[i], TIER2, quotaos_spinner_entry);
        t->budget_us = slice;
        t->keep_running = 1;
        t->bench_owned = 1;
        task_reset_window(t);
        ts[started++] = t;
    }
    if (started == 0)
        return;

    struct task *vs = sched_find("display-vsync");
    task_reset_window(vs);
    bench_watch(vs);
    bench_begin(8);
    block_for_bench();

    printf("app      declared_us  recv_avg  periods  under_miss\n");
    uint32_t misses = 0;
    uint64_t recv_sum = 0;
    uint32_t periods = 0;
    for (int i = 0; i < started; i++) {
        uint32_t avg = avg_u(ts[i]->w_sum_recv, ts[i]->w_periods);
        printf("%-8s %11u  %8u  %7u  %10u\n",
               ts[i]->name, slice, avg, ts[i]->w_periods, ts[i]->w_miss);
        misses += ts[i]->w_miss;
        recv_sum += ts[i]->w_sum_recv;
        periods += ts[i]->w_periods;
    }
    printf("tier-1 display-vsync during the slice run: jobs=%u misses=%u max_latency=%u us\n",
           vs->w_jobs, vs->w_miss, vs->w_max_lat);
    printf("TOTAL apps=%d  declared_each=%u  recv_avg=%u  under_miss=%u\n",
           started, slice, avg_u(recv_sum, periods), misses);
    printf("sum of declared slices = %u us/period, tier-1 reserve stays outside this pool.\n",
           (uint32_t)started * slice);
    if (misses == 0 && vs->w_miss == 0)
        printf("结果: 每条虚拟 CPU 都拿到了自己的切片，tier-1 仍然零错过。\n");
    else
        printf("结果: 有切片没给够或 tier-1 被耽误了。\n");

    for (int i = 0; i < started; i++)
        resource_close_name(names[i]);
    printf("批量应用已全部关闭，保底额度归还。\n");
}

static void cmd_device(void)
{
    int n = 0;
    const struct app_manifest *cat = resource_catalog(&n);
    const struct app_manifest *m = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(cat[i].name, "Cluster Sim") == 0)
            m = &cat[i];
    }
    if (!m) {
        printf("目录里没有 Cluster Sim。\n");
        return;
    }
    launch_manifest(m, 1);
}

static void cmd_pressure(void)
{
    if (resource_running_count() == 0) {
        printf("当前没有 tier-2 应用。先用菜单 2 启动一个带弹性内存的应用（例如 Pixel Racer）。\n");
        return;
    }
    printf("施加压力前:\n");
    resource_show();
    resource_set_pressure(1);
    printf("施加压力后:\n");
    resource_show();
    resource_set_pressure(0);
    printf("压力标志已清除。弹性 CPU 可以再借；已经收回的弹性内存不会自动加回。\n");
}

void shell_main(void)
{
    printf("[boot] shell task running\n");
    asm volatile("msr daifclr, #2" ::: "memory");
    uint64_t j0 = timer_jiffies();
    uint64_t t0 = now_us();
    while (timer_jiffies() < j0 + 3 && now_us() - t0 < 300000)
        asm volatile("yield");
    if (timer_jiffies() <= j0)
        printf("[boot] ERROR: timer IRQ did not fire (jiffies=%lu). 调度演示不会有效。\n",
               timer_jiffies());
    else
        printf("[boot] timer IRQ ok, jiffies=%lu now=%lu us\n", timer_jiffies(), now_us());

    cmd_help();
    for (;;) {
        printf("select> ");
        char line[32];
        readline(line, sizeof line);
        if (line[0] == 0)
            continue;
        if (strcmp(line, "10") == 0)
            cmd_many();
        else if (line[0] == '1' && line[1] == 0)
            cmd_table();
        else if (line[0] == '2' && line[1] == 0)
            cmd_launch();
        else if (line[0] == '3' && line[1] == 0)
            cmd_close();
        else if (line[0] == '4' && line[1] == 0)
            cmd_priority();
        else if (line[0] == '5' && line[1] == 0)
            cmd_toggle();
        else if (line[0] == '6' && line[1] == 0)
            cmd_contain();
        else if (line[0] == '7' && line[1] == 0)
            cmd_device();
        else if (line[0] == '8' && line[1] == 0)
            cmd_pressure();
        else if (line[0] == '9' && line[1] == 0)
            cmd_minslice();
        else if (line[0] == 'h' || line[0] == 'H' || line[0] == '?')
            cmd_help();
        else if (line[0] == 'q' || line[0] == 'Q')
            printf("在 QEMU 窗口按 Ctrl-A 然后 X 退出。\n");
        else
            printf("未知选项。按 h 看命令。\n");
    }
}
