/* Quota OS：基于三级资源声明的微内核，保证一级任务永远不卡顿、二级任务运行不卡顿，三级任务则不保证。 */
#pragma once

/* 版本号：每个提交加 1，三位补零。启动横幅和 README 抬头都显示它。 */
#define QUOTAOS_VERSION     "007"

#include <stdint.h>
#include <stddef.h>

/*
 * 资源模型（每个 10ms 周期）：
 *   系统预留  内核自己的 CPU（实测每周期最坏值 + 余量）和内存，不外借。
 *   Tier 1    每个任务一个写死的时间槽，按槽表定时触发，到槽尾准时掐断；
 *             没用完或没人用的槽空着，不借给任何人。
 *   Tier 2    最低值 + 建议值。准入条件：系统预留 + Tier 1 + ΣTier 2 最低值 <= 100%。
 *             最低值每周期都给足；建议值只来自空闲，有压力时收回。
 *   Tier 3    分剩下的，碰不到上面三样。
 */
#define PERIOD_US           10000u
#define T3_QUANTUM_US       1000u
#define GUARD_US            10u      /* 槽前多少微秒停下窗口任务，内核原地等到槽起点 */
#define SYS_RESERVE_US      350u     /* 系统预留 CPU：实测每周期最坏 15290 tick（245 us）+ 余量，见 README */
#define KERNEL_RESERVE_KB   (16u * 1024u)
#define JUMP_US             20u      /* 定时器或槽起点迟到超过这么多，记一次时钟跳动 */

#define MAX_TASKS           48
#define MAX_SLOTS           8
#define STACK_SIZE          8192u
#define MAX_APPS            40
#define NAME_LEN            24
#define REASON_LEN          512

#define TASK_EMPTY          0
#define TASK_READY          1
#define TASK_RUNNING        2
#define TASK_BLOCKED        3
#define TASK_DEAD           4

#define TIER_IDLE           0
#define TIER1               1
#define TIER2               2
#define TIER3               3

#define KIND_SPIN           0   /* 死循环：有多少给多少 */
#define KIND_JOB            1   /* 周期作业：释放后做 work_us，svc 交还 */
#define KIND_SHELL          2
#define KIND_IDLE           3

#define ADMIT_OK            0
#define ADMIT_DEVICE        1
#define ADMIT_OVERSUB       2
#define ADMIT_DUP           3

struct irq_frame {
    uint64_t r[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t pad;
};

/* 一个演示窗口里的统计。时间一律是 CNTVCT 原始 tick（QEMU virt 62.5 MHz，16 ns）。 */
struct task_win {
    uint32_t periods;
    uint32_t jobs;
    uint32_t misses;
    uint32_t short_periods;     /* Tier 2：本周期拿到的少于最低值 */
    uint32_t cuts;              /* 到槽尾或预算用完被掐断的次数 */
    uint64_t used_sum;
    uint64_t used_min;
    uint64_t used_max;
    uint64_t start_min;         /* Tier 1：作业第一条指令读到的时间 - 槽起点 */
    uint64_t start_max;
    uint64_t lat_max;           /* 完成时刻 - 释放时刻 */
};

struct task {
    int id;
    char name[NAME_LEN];
    int tier;
    int kind;
    int state;
    uint64_t sp;
    uint8_t *stack;

    int slot;                   /* Tier 1：槽号，-1 表示没有 */
    uint64_t min_t;             /* Tier 2：每周期最低值（tick） */
    uint64_t sugg_t;            /* Tier 2：建议值里超出最低值的部分 */
    uint64_t work_t;            /* KIND_JOB：每个作业的工作量 */
    uint64_t deadline_t;        /* KIND_JOB：相对释放时刻的截止期 */
    uint64_t used_t;            /* 本周期已用 */

    int job_open;
    int job_started;
    uint64_t release_ts;
    uint64_t start_ts;
    uint64_t job_period;

    int bench_owned;
    int wait_bench;

    uint32_t jobs;              /* 开机以来 */
    uint32_t misses;
    uint64_t used_max_all;      /* 开机以来单周期最多用了多少（Tier 1 的浪费 = 槽长 - 它） */
    struct task_win w;
};

struct tier1_decl {
    const char *name;
    const char *role;
    uint32_t cpu_us;            /* 槽长；0 = 只占内存 */
    uint32_t slot_off_us;       /* 槽在周期里的起点 */
    uint32_t mem_kb;
};

struct app_manifest {
    const char *name;
    const char *role;
    uint32_t mem_min_kb;
    uint32_t mem_sugg_kb;
    uint32_t cpu_min_us;
    uint32_t cpu_sugg_us;
};

struct running_app {
    char name[32];
    uint32_t mem_min_kb;
    uint32_t mem_sugg_kb;
    uint32_t mem_granted_kb;
    uint32_t cpu_min_us;
    uint32_t cpu_sugg_us;
    struct task *task;
    int alive;
};

static inline void mmio_wr(uint64_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}

static inline uint32_t mmio_rd(uint64_t a)
{
    return *(volatile uint32_t *)a;
}

void uart_init(void);
void uart_putc(char c);
void printf(const char *fmt, ...);
void readline(char *buf, int cap);
void put_padded(const char *s, int cols);

void *memset(void *dst, int c, size_t n);
int strcmp(const char *a, const char *b);
void str_copy(char *dst, const char *src, int cap);

uint64_t dtb_ram_bytes(uint64_t hinted);
int dtb_find_compatible(const char *compat, uint64_t *out, int max);
const char *dtb_bootargs(void);

void gic_init(void);
uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);

void timer_init(void);
uint64_t ticks_now(void);
uint64_t timer_freq(void);
uint64_t us_to_ticks(uint64_t us);
uint64_t ticks_to_us(uint64_t t);
uint64_t now_us(void);
void busy_ticks(uint64_t t);

int dtb_fault_expected(void);
void dtb_note_fault(void);

void resource_init(uint64_t ram_bytes);
const struct tier1_decl *resource_tier1(int *count);
uint32_t resource_t1_cpu_us(void);
uint32_t resource_t1_mem_kb(void);
uint32_t resource_guard_us(void);
uint32_t resource_app_cpu_capacity_us(void);
uint64_t resource_app_mem_capacity_kb(void);
uint32_t resource_cpu_min_sum_us(void);
uint64_t resource_mem_min_sum_kb(void);
uint64_t resource_ram_kb(void);
int resource_pressure(void);
void resource_set_pressure(int on);
int resource_check(const struct app_manifest *m);
const char *resource_reason(void);
struct running_app *resource_commit(const struct app_manifest *m);
int resource_release(struct running_app *a);
int resource_running_count(void);
struct running_app *resource_running(int index);
const struct app_manifest *resource_catalog(int *count);

void sched_init(void);
struct task *sched_current(void);
struct task *task_create(const char *name, int tier, int kind);
void task_start(struct task *t);
void task_kill(struct task *t);
void sched_bind_slot(struct task *t, int slot);
struct task *sched_slot_owner(int slot);
struct task *sched_find(const char *name);
struct task *sched_task_at(int index);
void sched_start(void (*shell_entry)(void));
void sched_bench(uint32_t periods);
void sched_print_window(void);
void sched_print_kernel(void);
void sched_print_slots(void);
uint64_t sched_kernel_max_all(void);
uint64_t sched_kernel_max_period(void);
uint64_t sched_irq_lat_max(void);
uint64_t sched_periods(void);
uint64_t sched_foreign_window(void);

uint64_t handle_irq(uint64_t frame_sp);
uint64_t handle_sync(uint64_t frame_sp);
void handle_unhandled(uint64_t frame_sp, uint64_t kind);

void kmain(uint64_t dtb);
void shell_main(void);
void services_start(void);
void job_body(uint64_t start);

void start_first_task(uint64_t frame_sp);
void job_resume(void);
