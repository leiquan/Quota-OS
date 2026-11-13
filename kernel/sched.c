/*
 * 调度就是一组预算服务器（deferrable server，CBS 的硬隔离亲戚）。
 *
 * 每个任务有 (budget, period)。period 全局都是 10ms。
 * 一个周期开始时把预算充满；跑的时候按 CNTPCT 扣；扣完就阻塞到下个周期。
 * 这既是“虚拟 CPU”（预算/周期 = 这颗核分给你的比例），也是防失范的笼子：
 * 死循环最多吃掉自己声明的那一截，碰不到别人的保底。
 *
 * 分级打开时的挑选顺序：
 *   1. Tier-1 里预算还在、截止期更早的（所以失控的驱动抢不走 vsync）
 *   2. Tier-2 的保底预算，轮转
 *   3. Tier-2 的弹性预算（压力下跳过）
 *   4. Tier-3 轮转，只拿剩余
 * 分级关闭时所有就绪任务一视同仁，每 1ms 轮转一次，用来对照。
 *
 * 内核自己不进这条队列。定时器中断和这段调度代码始终能跑，
 * 所以“操作系统本身不卡”靠的是：中断不被任务关掉，调度路径很短。
 */
#include "quotaos.h"

static struct task tasks[MAX_TASKS];
static uint8_t stacks[MAX_TASKS][STACK_SIZE] __attribute__((aligned(16)));
static struct task *current_task;
static struct task *idle_task;
static struct task *shell_task;

static int tiering_on = 1;
static int count_guarantees = 1;
static int rr_cursor[4];

static uint64_t epoch;
static uint64_t period_index;
static uint64_t run_start_us;
static uint32_t max_sched_us;
static uint32_t sched_overrun_n;
static uint64_t period_skips;

static int bench_on;
static int bench_rec;
static int bench_skip;
static int bench_target;
static int bench_n;
static uint64_t bench_deadline_us;
static struct task *sample_task;

#define MAX_SAMPLES 16
static struct {
    uint32_t lat;
    uint32_t resp;
    int miss;
} samples[MAX_SAMPLES];
static int sample_n;

static int probe_mode;
static uint32_t probe_target;
static uint32_t probe_deltas[16];
static int probe_count;
static uint64_t probe_last;

static int sw_on;
static uint32_t sw_min, sw_max, sw_n;
static uint64_t sw_sum;
static uint64_t shell_unblock_at;

/*
 * 预算超支诊断。不改变挑选顺序、量子或 charge() 的算法，只在既有路径上读 CNTPCT。
 *
 * 一次“预算切片”从 run_start 记到 charge() 采样。超出量
 *   over = (charge_pct - run_start_pct) - armed_ticks
 * 可以分成三段，并且三段相加等于 over：
 *   记账偏差 skew          = (cval - run_start_pct) - armed_ticks
 *   定时器触发延迟 trig     = entry_pct - cval
 *   入口到记账点 e2charge   = charge_pct - entry_pct
 * 入口到 pick_next、以及 pick 到 eret，发生在 charge() 之后，不进 used_us。
 */
#define DIAG_MAX 1200

struct diag_sample {
    int32_t trig_ns;
    int32_t entry_to_pick_ns;
    int32_t cs_ns;
    int32_t acct_ns;
    int32_t over_ns;
    int32_t skew_ns;
    int32_t entry_to_charge_ns;
    int32_t charged_us;
    int32_t q_us;
};

static struct diag_sample diag_samples[DIAG_MAX];
static int diag_n;
static int diag_pending;
static int diag_stamp_pick;
static int diag_skipped;
static struct task *diag_task;
static uint64_t diag_decision_pct;
static int32_t diag_pending_cs_index;

uint64_t irq_entry_pct;
uint64_t irq_eret_pct;

static uint64_t armed_cval;
static uint64_t armed_ticks;
static uint64_t armed_run_start_pct;
static uint64_t armed_run_start_us;
static uint32_t armed_q_us;
static int armed_budget_slice;
static struct task *armed_task;
static uint64_t latched_cval;
static uint64_t latched_entry;

static uint64_t run_start_pct;

struct task *sched_current(void) { return current_task; }
int sched_tiering(void) { return tiering_on; }
struct task *sched_shell(void) { return shell_task; }
uint64_t sched_period_skips(void) { return period_skips; }
uint32_t sched_max_us(void) { return max_sched_us; }
uint32_t sched_overruns(void) { return sched_overrun_n; }
int bench_active(void) { return bench_on; }
int bench_recording(void) { return bench_rec; }

void sched_set_tiering(int on) { tiering_on = on ? 1 : 0; }
void sched_set_count_guarantees(int on) { count_guarantees = on ? 1 : 0; }
void sched_arm_shell_timeout(uint64_t abs_us) { shell_unblock_at = abs_us; }

struct task *sched_task_at(int index)
{
    if (index < 0 || index >= MAX_TASKS)
        return 0;
    if (tasks[index].state == TASK_EMPTY)
        return 0;
    return &tasks[index];
}

struct task *sched_find(const char *name)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_EMPTY)
            continue;
        if (strcmp(tasks[i].name, name) == 0)
            return &tasks[i];
    }
    return 0;
}

static void name_copy(char *dst, const char *src)
{
    int i = 0;
    for (; i < NAME_LEN - 1 && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void task_exit(void)
{
    if (current_task) {
        current_task->state = TASK_DEAD;
        current_task->keep_running = 0;
    }
    for (;;)
        asm volatile("svc #0" ::: "memory");
}

static void make_frame(struct task *t, void (*entry)(void))
{
    uintptr_t top = ((uintptr_t)t->stack + STACK_SIZE) & ~(uintptr_t)15u;
    struct irq_frame *f = (struct irq_frame *)(top - sizeof(struct irq_frame));
    memset(f, 0, sizeof *f);
    f->elr = (uint64_t)entry;
    f->spsr = 0x345; /* EL1h，只开 IRQ */
    f->r[30] = (uint64_t)task_exit;
    t->sp = (uint64_t)f;
}

struct task *task_create(const char *name, int tier, void (*entry)(void))
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_EMPTY && tasks[i].state != TASK_DEAD)
            continue;
        struct task *t = &tasks[i];
        memset(t, 0, sizeof *t);
        t->id = i;
        t->stack = stacks[i];
        t->tier = tier;
        t->state = TASK_READY;
        name_copy(t->name, name);
        make_frame(t, entry);
        return t;
    }
    printf("PANIC: out of tasks\n");
    for (;;)
        asm volatile("wfi");
}

void task_reset_window(struct task *t)
{
    if (!t)
        return;
    t->w_jobs = 0;
    t->w_miss = 0;
    t->w_max_lat = 0;
    t->w_max_resp = 0;
    t->w_sum_resp = 0;
    t->w_sum_recv = 0;
    t->w_periods = 0;
    t->w_max_recv = 0;
}

static void idle_entry(void)
{
    for (;;)
        asm volatile("wfi");
}

static uint32_t cap_of(struct task *t)
{
    if (!tiering_on || !t || t->budget_us == 0 || t->tier == TIER_IDLE || t->tier == TIER3)
        return 0xffffffffu;
    uint32_t cap = t->budget_us;
    if (t->tier == TIER2 && !resource_pressure())
        cap += t->elastic_us;
    return cap;
}

static void finish_bench(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->bench_owned)
            continue;
        t->state = TASK_DEAD;
        t->keep_running = 0;
        t->periodic = 0;
        t->bench_owned = 0;
        t->job_open = 0;
    }
    if (shell_task) {
        shell_task->wait_bench = 0;
        if (shell_task->state != TASK_DEAD)
            shell_task->state = TASK_READY;
    }
}

static void replenish_one(struct task *t, uint64_t period_start)
{
    if (t->state == TASK_EMPTY)
        return;
    int was_throttled = t->throttled;
    t->throttled = 0;
    t->used_us = 0;
    if (t->state == TASK_DEAD)
        return;
    if (t->wait_bench) {
        t->state = TASK_BLOCKED;
        return;
    }
    if (t->periodic) {
        t->wait_release = 0;
        t->job_open = 1;
        t->started = 0;
        t->release_us = period_start;
        t->seen_release = 1;
        t->state = TASK_READY;
        return;
    }
    if (was_throttled || t->keep_running || t->state == TASK_READY || t->state == TASK_RUNNING)
        t->state = TASK_READY;
}

static void on_boundary(uint64_t period_start)
{
    int do_record = 0;
    if (bench_on && bench_skip) {
        bench_skip = 0;
        bench_rec = 1;
    } else if (bench_on && bench_rec) {
        do_record = 1;
    }

    if (do_record) {
        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t->state == TASK_EMPTY)
                continue;
            int track = (t->bench_owned && t->keep_running) || t->periodic;
            if (!track)
                continue;
            t->w_sum_recv += t->used_us;
            if (t->w_periods < 100000)
                t->w_periods++;
            if (t->used_us > t->w_max_recv)
                t->w_max_recv = t->used_us;
            if (t->bench_owned && t->keep_running && t->budget_us &&
                t->used_us * 100u < t->budget_us * 70u)
                t->w_miss++;
        }
        bench_n++;
    }

    for (int i = 0; i < MAX_TASKS; i++)
        replenish_one(&tasks[i], period_start);

    if (do_record && bench_n >= bench_target) {
        bench_on = 0;
        bench_rec = 0;
        finish_bench();
    }
}

static struct task *pick_tier1(void)
{
    struct task *best = 0;
    uint64_t best_dl = ~0ull;
    uint64_t period_end = epoch + (period_index + 1) * PERIOD_US;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->tier != TIER1 || t->state != TASK_READY)
            continue;
        if (t->used_us >= cap_of(t))
            continue;
        uint64_t dl = period_end;
        if (t->periodic && t->job_open)
            dl = t->release_us + t->rel_deadline_us;
        if (!best || dl < best_dl || (dl == best_dl && t->id < best->id)) {
            best = t;
            best_dl = dl;
        }
    }
    return best;
}

static struct task *pick_tier2(int elastic_pass)
{
    if (elastic_pass && resource_pressure())
        return 0;
    for (int n = 0; n < MAX_TASKS; n++) {
        int i = (rr_cursor[TIER2] + 1 + n) % MAX_TASKS;
        struct task *t = &tasks[i];
        if (t->tier != TIER2 || t->state != TASK_READY || t->budget_us == 0)
            continue;
        if (!elastic_pass) {
            if (t->used_us < t->budget_us) {
                rr_cursor[TIER2] = i;
                return t;
            }
        } else {
            uint32_t cap = t->budget_us + t->elastic_us;
            if (t->used_us >= t->budget_us && t->used_us < cap) {
                rr_cursor[TIER2] = i;
                return t;
            }
        }
    }
    return 0;
}

static struct task *pick_tier3(void)
{
    for (int n = 0; n < MAX_TASKS; n++) {
        int i = (rr_cursor[TIER3] + 1 + n) % MAX_TASKS;
        struct task *t = &tasks[i];
        if (t->tier == TIER3 && t->state == TASK_READY) {
            rr_cursor[TIER3] = i;
            return t;
        }
    }
    return 0;
}

static struct task *pick_fair(void)
{
    for (int n = 0; n < MAX_TASKS; n++) {
        int i = (rr_cursor[0] + 1 + n) % MAX_TASKS;
        struct task *t = &tasks[i];
        if (t->state == TASK_READY && t->tier != TIER_IDLE) {
            rr_cursor[0] = i;
            return t;
        }
    }
    return idle_task;
}

static struct task *pick_next(void)
{
    if (!tiering_on)
        return pick_fair();
    struct task *t = pick_tier1();
    if (t)
        return t;
    t = pick_tier2(0);
    if (t)
        return t;
    t = pick_tier2(1);
    if (t)
        return t;
    t = pick_tier3();
    if (t)
        return t;
    return idle_task;
}

static void arm_for(struct task *next)
{
    uint64_t now = now_us();
    uint64_t boundary = epoch + (period_index + 1) * PERIOD_US;
    uint64_t until = boundary > now ? boundary - now : 200;
    uint64_t q = until;
    int budget_limited = 0;
    uint64_t remain = 0;

    if (!tiering_on) {
        q = OFF_QUANTUM_US;
        if (q > until)
            q = until;
    } else if (!next || next->tier == TIER_IDLE || next->tier == TIER3 || next->budget_us == 0) {
        if (q > 2000)
            q = 2000;
    } else {
        uint32_t cap = cap_of(next);
        remain = 50;
        if (next->used_us < cap)
            remain = (uint64_t)cap - next->used_us;
        if (remain < q) {
            q = remain;
            budget_limited = 1;
        }
    }
    if (q < 20)
        q = 20;
    uint64_t ticks = timer_arm_us(q);
    armed_ticks = ticks;
    armed_cval = timer_read_cval();
    armed_q_us = (uint32_t)q;
    armed_run_start_pct = run_start_pct;
    armed_run_start_us = run_start_us;
    armed_task = next;
    armed_budget_slice = budget_limited && remain >= 20 && q == remain;
}

static uint32_t charge(struct task *t, uint64_t now)
{
    if (!t || t == idle_task)
        return 0;
    if (now < run_start_us)
        return 0;
    uint64_t d = now - run_start_us;
    if (d > 1000000)
        d = 1000000;
    t->used_us += (uint32_t)d;
    return (uint32_t)d;
}

static void maybe_throttle(struct task *t)
{
    if (!tiering_on || !t || t == idle_task)
        return;
    if (t->tier == TIER3 || t->tier == TIER_IDLE || t->budget_us == 0)
        return;
    if (t->state == TASK_DEAD || t->state == TASK_EMPTY)
        return;
    if (t->used_us >= cap_of(t) && t->state != TASK_BLOCKED) {
        t->throttled = 1;
        t->throttle_events++;
        t->state = TASK_BLOCKED;
    }
}

static void diag_finalize(void)
{
    if (!diag_pending)
        return;
    uint64_t eret = irq_eret_pct;
    if (eret < diag_decision_pct)
        return;
    if (diag_pending_cs_index >= 0 && diag_pending_cs_index < diag_n) {
        diag_samples[diag_pending_cs_index].cs_ns =
            (int32_t)timer_ticks_to_ns((int64_t)(eret - diag_decision_pct));
    }
    diag_pending = 0;
}

static void diag_on_slice(struct task *prev, uint32_t charged_us, uint64_t charge_pct)
{
    if (!diag_task || prev != diag_task || !armed_budget_slice)
        return;
    if (armed_task != prev || latched_cval != armed_cval || armed_ticks == 0) {
        diag_skipped++;
        return;
    }
    if (diag_n >= DIAG_MAX)
        return;

    int64_t skew = (int64_t)(armed_cval - armed_run_start_pct) - (int64_t)armed_ticks;
    int64_t trig = (int64_t)(latched_entry - armed_cval);
    int64_t e2c = (int64_t)(charge_pct - latched_entry);
    int64_t over = (int64_t)(charge_pct - armed_run_start_pct) - (int64_t)armed_ticks;

    struct diag_sample *s = &diag_samples[diag_n];
    s->trig_ns = (int32_t)timer_ticks_to_ns(trig);
    s->skew_ns = (int32_t)timer_ticks_to_ns(skew);
    s->entry_to_charge_ns = (int32_t)timer_ticks_to_ns(e2c);
    s->acct_ns = (int32_t)timer_ticks_to_ns(skew + e2c);
    s->over_ns = (int32_t)timer_ticks_to_ns(over);
    s->entry_to_pick_ns = -1;
    s->cs_ns = -1;
    s->charged_us = (int32_t)charged_us;
    s->q_us = (int32_t)armed_q_us;
    diag_pending_cs_index = diag_n;
    diag_pending = 1;
    diag_stamp_pick = 1;
    diag_n++;
}

static uint64_t schedule(uint64_t old_sp)
{
    uint64_t charge_pct = timer_read_pct();
    uint64_t enter = now_us();
    struct task *prev = current_task;
    uint32_t charged_us = 0;

    if (prev) {
        prev->sp = old_sp;
        if (prev->state == TASK_RUNNING)
            prev->state = TASK_READY;
        charged_us = charge(prev, enter);
        maybe_throttle(prev);
        diag_on_slice(prev, charged_us, charge_pct);
    }

    uint64_t now = now_us();
    if (epoch == 0)
        epoch = now;
    if (now >= epoch) {
        uint64_t idx = (now - epoch) / PERIOD_US;
        if (idx != period_index) {
            if (count_guarantees && idx > period_index + 1)
                period_skips += idx - period_index - 1;
            on_boundary(epoch + idx * PERIOD_US);
            period_index = idx;
        }
    }

    if (bench_on && now_us() > bench_deadline_us) {
        bench_on = 0;
        bench_rec = 0;
        finish_bench();
    }
    if (shell_unblock_at && now_us() > shell_unblock_at && shell_task &&
        shell_task->state == TASK_BLOCKED && !shell_task->wait_bench) {
        shell_task->state = TASK_READY;
        shell_unblock_at = 0;
    }

    struct task *next = pick_next();
    if (!next)
        next = idle_task;
    if (diag_stamp_pick && diag_n > 0) {
        uint64_t decision = timer_read_pct();
        diag_decision_pct = decision;
        diag_samples[diag_n - 1].entry_to_pick_ns =
            (int32_t)timer_ticks_to_ns((int64_t)(decision - latched_entry));
        diag_stamp_pick = 0;
    }
    if (next->job_open && !next->started) {
        next->started = 1;
        next->start_us = now_us();
    }
    next->state = TASK_RUNNING;
    current_task = next;
    run_start_us = now_us();
    run_start_pct = timer_read_pct();
    arm_for(next);

    uint64_t cost64 = now_us() - enter;
    uint32_t cost = cost64 > 1000000 ? 1000000 : (uint32_t)cost64;
    if (cost > max_sched_us)
        max_sched_us = cost;
    if (count_guarantees && cost > 200)
        sched_overrun_n++;
    return next->sp;
}

static uint64_t probe_tick(uint64_t sp)
{
    uint64_t now = now_us();
    if (probe_last != 0 && probe_count < 16) {
        uint64_t d = now - probe_last;
        probe_deltas[probe_count++] = d > 1000000 ? 1000000 : (uint32_t)d;
    }
    probe_last = now;
    if (probe_count >= 10) {
        probe_mode = 0;
        return schedule(sp);
    }
    timer_arm_us(probe_target ? probe_target : 1000);
    return sp;
}

volatile uint64_t irq_entries;
volatile uint32_t last_iar;

uint64_t handle_irq(uint64_t frame_sp)
{
    irq_entries++;
    diag_finalize();
    latched_entry = irq_entry_pct;
    latched_cval = timer_read_cval();
    uint32_t iar = gic_ack();
    last_iar = iar;
    uint32_t id = iar & 0x3ffu;
    if (id == 30) {
        /* 先把电平灭掉再 EOI，避免中断粘住。真正的下一个期限由 schedule 重装。 */
        timer_arm_us(100000);
        timer_note_irq();
    }
    if (id < 1020)
        gic_eoi(iar);
    if (id == 1023 || id != 30)
        return frame_sp;
    if (probe_mode == 1)
        return probe_tick(frame_sp);
    return schedule(frame_sp);
}

uint64_t handle_sync(uint64_t frame_sp)
{
    struct irq_frame *f = (struct irq_frame *)frame_sp;
    uint64_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);
    if (ec == 0x15) {
        uint32_t imm = (uint32_t)(esr & 0xffffu);
        if (imm == 1 && current_task && current_task->state == TASK_RUNNING)
            current_task->state = TASK_READY;
        return schedule(frame_sp);
    }
    if (dtb_fault_expected() && (ec == 0x24 || ec == 0x25)) {
        dtb_note_fault();
        f->elr += 4;
        return frame_sp;
    }
    printf("\nPANIC sync ec=0x%x esr=0x%lx elr=0x%lx\n", ec, esr, f->elr);
    for (;;)
        asm volatile("wfi");
}

void handle_unhandled(uint64_t frame_sp, uint64_t kind)
{
    struct irq_frame *f = (struct irq_frame *)frame_sp;
    printf("\nPANIC unhandled vector %lu elr=0x%lx\n", kind, f->elr);
    for (;;)
        asm volatile("wfi");
}

void quotaos_periodic_entry(void)
{
    for (;;) {
        struct task *t = current_task;
        if (t->work_us)
            busy_us(t->work_us);

        t = current_task;
        uint64_t now = now_us();
        uint32_t lat = 0;
        uint32_t resp = 0;
        int miss = 0;
        if (t->seen_release && now >= t->release_us && t->start_us >= t->release_us) {
            lat = (uint32_t)(t->start_us - t->release_us);
            resp = (uint32_t)(now - t->release_us);
            if (t->latency_check)
                miss = lat > t->rel_deadline_us;
            else
                miss = resp > t->rel_deadline_us;
        }
        if (t->seen_release && count_guarantees && tiering_on) {
            if (miss)
                t->misses++;
            else
                t->hits++;
            if (lat > t->max_lat_us)
                t->max_lat_us = lat;
            if (resp > t->max_resp_us)
                t->max_resp_us = resp;
        }
        if (t->seen_release && !tiering_on && miss)
            t->misses_off++;
        if (bench_rec && t->seen_release) {
            t->w_jobs++;
            t->w_sum_resp += resp;
            if (lat > t->w_max_lat)
                t->w_max_lat = lat;
            if (resp > t->w_max_resp)
                t->w_max_resp = resp;
            if (miss)
                t->w_miss++;
            bench_notify_job(t, lat, resp, miss);
        }
        t->job_open = 0;
        t->wait_release = 1;
        t->state = TASK_BLOCKED;
        asm volatile("svc #0" ::: "memory");
    }
}

void quotaos_spinner_entry(void)
{
    for (;;)
        asm volatile("" ::: "memory");
}

void services_start(void)
{
    struct task *t = task_create("display-vsync", TIER1, quotaos_periodic_entry);
    t->budget_us = 500;
    t->work_us = 80;
    t->rel_deadline_us = DEADLINE_VSYNC_US;
    t->periodic = 1;
    t->latency_check = 1;
    t->wait_release = 1;
    t->state = TASK_BLOCKED;

    t = task_create("timer-server", TIER1, quotaos_periodic_entry);
    t->budget_us = 300;
    t->work_us = 40;
    t->rel_deadline_us = DEADLINE_TIMER_US;
    t->periodic = 1;
    t->latency_check = 1;
    t->wait_release = 1;
    t->state = TASK_BLOCKED;
}

void sched_init(void)
{
    memset(tasks, 0, sizeof tasks);
    current_task = 0;
    idle_task = 0;
    shell_task = 0;
    epoch = now_us();
    period_index = 0;
    tiering_on = 1;
    count_guarantees = 1;
}

void sched_start(void (*shell_entry)(void))
{
    idle_task = task_create("idle", TIER_IDLE, idle_entry);
    shell_task = task_create("shell", TIER3, shell_entry);
    shell_task->state = TASK_RUNNING;
    current_task = shell_task;
    run_start_us = now_us();
    run_start_pct = timer_read_pct();
    arm_for(shell_task);
    start_first_task(shell_task->sp);
}

void bench_begin(int periods)
{
    bench_target = periods;
    bench_n = 0;
    bench_rec = 0;
    bench_skip = 1;
    bench_on = 1;
    sample_n = 0;
    bench_deadline_us = now_us() + (uint64_t)(periods + 4) * PERIOD_US + 80000;
}

void bench_watch(struct task *t)
{
    sample_task = t;
}

void bench_notify_job(struct task *t, uint32_t lat, uint32_t resp, int miss)
{
    if (!sample_task || t != sample_task)
        return;
    if (sample_n >= MAX_SAMPLES)
        return;
    samples[sample_n].lat = lat;
    samples[sample_n].resp = resp;
    samples[sample_n].miss = miss;
    sample_n++;
}

int bench_sample_count(void) { return sample_n; }

void bench_sample(int i, uint32_t *lat, uint32_t *resp, int *miss)
{
    if (i < 0 || i >= sample_n) {
        *lat = 0;
        *resp = 0;
        *miss = 0;
        return;
    }
    *lat = samples[i].lat;
    *resp = samples[i].resp;
    *miss = samples[i].miss;
}

void probe_begin(uint32_t target_us)
{
    asm volatile("msr daifset, #2" ::: "memory");
    probe_target = target_us;
    probe_count = 0;
    probe_last = 0;
    probe_mode = 1;
    timer_arm_us(target_us);
    asm volatile("msr daifclr, #2" ::: "memory");
}

int probe_done(void)
{
    return probe_mode == 0 && probe_count >= 8;
}

int probe_samples(uint32_t *min_us, uint32_t *avg_us, uint32_t *max_us)
{
    if (probe_count <= 1)
        return 0;
    /* 丢掉第一拍，它夹着上一次普通调度的间隔。 */
    uint32_t mn = 0xffffffffu, mx = 0, n = 0;
    uint64_t sum = 0;
    for (int i = 1; i < probe_count; i++) {
        uint32_t d = probe_deltas[i];
        if (d < mn)
            mn = d;
        if (d > mx)
            mx = d;
        sum += d;
        n++;
    }
    if (!n)
        return 0;
    *min_us = mn;
    *max_us = mx;
    *avg_us = (uint32_t)(sum / n);
    return (int)n;
}

void switch_measure_begin(void)
{
    sw_on = 1;
    sw_min = 0xffffffffu;
    sw_max = 0;
    sw_n = 0;
    sw_sum = 0;
}

void switch_measure_note(uint32_t us)
{
    if (!sw_on)
        return;
    asm volatile("msr daifset, #2" ::: "memory");
    if (us < sw_min)
        sw_min = us;
    if (us > sw_max)
        sw_max = us;
    sw_sum += us;
    sw_n++;
    asm volatile("msr daifclr, #2" ::: "memory");
}

void switch_measure_end(void)
{
    sw_on = 0;
}

int switch_measure_result(uint32_t *min_us, uint32_t *avg_us, uint32_t *max_us, uint32_t *n)
{
    if (!sw_n)
        return 0;
    *min_us = sw_min;
    *max_us = sw_max;
    *avg_us = (uint32_t)(sw_sum / sw_n);
    *n = sw_n;
    return 1;
}

void diag_watch(struct task *t)
{
    diag_task = t;
    diag_n = 0;
    diag_pending = 0;
    diag_stamp_pick = 0;
    diag_skipped = 0;
    diag_pending_cs_index = -1;
}

static void diag_stat(int32_t (*field)(const struct diag_sample *), int skip_neg,
                      int64_t *sum, int32_t *mx, int *n)
{
    *sum = 0;
    *mx = 0;
    *n = 0;
    int any = 0;
    for (int i = 0; i < diag_n; i++) {
        int32_t v = field(&diag_samples[i]);
        if (skip_neg && v < 0)
            continue;
        if (!any || v > *mx)
            *mx = v;
        *sum += v;
        (*n)++;
        any = 1;
    }
}

static int32_t f_trig(const struct diag_sample *s) { return s->trig_ns; }
static int32_t f_pick(const struct diag_sample *s) { return s->entry_to_pick_ns; }
static int32_t f_cs(const struct diag_sample *s) { return s->cs_ns; }
static int32_t f_acct(const struct diag_sample *s) { return s->acct_ns; }
static int32_t f_over(const struct diag_sample *s) { return s->over_ns; }
static int32_t f_skew(const struct diag_sample *s) { return s->skew_ns; }
static int32_t f_e2c(const struct diag_sample *s) { return s->entry_to_charge_ns; }

static void diag_line(const char *name, int32_t (*field)(const struct diag_sample *), int skip_neg)
{
    int64_t sum;
    int32_t mx;
    int n;
    diag_stat(field, skip_neg, &sum, &mx, &n);
    int64_t mean = n ? sum / n : 0;
    printf("  %-22s  n=%4d  mean=%8ld ns  max=%8d ns\n", name, n, mean, mx);
}

void diag_report(void)
{
    diag_finalize();
    printf("\n=== budget overrun diagnosis ===\n");
    printf("samples=%d skipped=%d\n", diag_n, diag_skipped);
    printf("CNTFRQ=%lu Hz\n", timer_freq());
    printf("over_ns = skew_ns + trig_ns + entry_to_charge_ns\n");
    printf("acct_ns = skew_ns + entry_to_charge_ns = over_ns - trig_ns\n");
    printf("entry_to_pick and context switch are after charge(), not inside used_us\n");
    diag_line("trigger_latency", f_trig, 0);
    diag_line("entry_to_decision", f_pick, 0);
    diag_line("context_switch", f_cs, 1);
    diag_line("accounting_error", f_acct, 0);
    diag_line("overrun_ticks", f_over, 0);
    diag_line("arm_skew", f_skew, 0);
    diag_line("entry_to_charge", f_e2c, 0);

    int64_t us_sum = 0;
    int32_t us_max = 0;
    int us_n = 0;
    int64_t resid = 0;
    for (int i = 0; i < diag_n; i++) {
        int32_t d = diag_samples[i].charged_us - diag_samples[i].q_us;
        us_sum += d;
        if (us_n == 0 || d > us_max)
            us_max = d;
        us_n++;
        int32_t sum3 = diag_samples[i].skew_ns + diag_samples[i].trig_ns +
                       diag_samples[i].entry_to_charge_ns;
        resid += (int64_t)diag_samples[i].over_ns - sum3;
    }
    printf("billed_over_us  n=%d  mean=%ld us  max=%d us  (charged_us - q_us)\n",
           us_n, us_n ? (long)(us_sum / us_n) : 0L, us_max);
    printf("identity_residual_mean=%ld ns (over - skew - trig - entry_to_charge)\n",
           diag_n ? (long)(resid / diag_n) : 0L);
    if (diag_n > 0) {
        printf("q_us first=%d last=%d  charged_us first=%d last=%d\n",
               diag_samples[0].q_us, diag_samples[diag_n - 1].q_us,
               diag_samples[0].charged_us, diag_samples[diag_n - 1].charged_us);
    }
    printf("DIAG_CSV_BEGIN\n");
    printf("i,trig_ns,entry_to_pick_ns,cs_ns,acct_ns,over_ns,skew_ns,entry_to_charge_ns,charged_us,q_us\n");
    for (int i = 0; i < diag_n; i++) {
        struct diag_sample *s = &diag_samples[i];
        printf("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
               i, s->trig_ns, s->entry_to_pick_ns, s->cs_ns, s->acct_ns,
               s->over_ns, s->skew_ns, s->entry_to_charge_ns, s->charged_us, s->q_us);
    }
    printf("DIAG_CSV_END\n");
    printf("DIAG_DONE\n");
    diag_task = 0;
}
