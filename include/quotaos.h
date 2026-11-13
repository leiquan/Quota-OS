/* Quota OS：用“启动前就把资源声明完”来保证不卡顿的教学微内核。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define PERIOD_US           10000u
#define OFF_QUANTUM_US      1000u
#define DEADLINE_VSYNC_US   800u
#define DEADLINE_TIMER_US   1500u
#define DEADLINE_AUDIO_US   4000u
#define KERNEL_RESERVE_KB   (16u * 1024u)

#define MAX_TASKS           48
#define STACK_SIZE          8192u
#define MAX_APPS            40
#define NAME_LEN            24
#define REASON_LEN          320

#define TASK_EMPTY          0
#define TASK_READY          1
#define TASK_RUNNING        2
#define TASK_BLOCKED        3
#define TASK_DEAD           4

#define TIER_IDLE           0
#define TIER1               1
#define TIER2               2
#define TIER3               3

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

struct task {
    int id;
    char name[NAME_LEN];
    int tier;
    int state;
    uint64_t sp;
    uint8_t *stack;

    /* 预算服务器：每 PERIOD_US 保证 budget_us，用完即停。弹性部分可被收回。 */
    uint32_t budget_us;
    uint32_t elastic_us;
    uint32_t used_us;
    uint32_t rel_deadline_us;
    uint32_t work_us;

    int periodic;       /* 每个周期释放一次 */
    int keep_running;   /* 死循环/占满型：用完预算被掐断，下个周期再跑 */
    int latency_check;  /* 1 = 用“释放到开始”判断是否错过截止期 */
    int job_open;
    int started;
    int wait_release;
    int wait_bench;
    int throttled;
    int bench_owned;    /* 演示结束时回收，不动常驻 tier-1 */
    int seen_release;

    uint64_t release_us;
    uint64_t start_us;

    uint32_t hits;
    uint32_t misses;
    uint32_t misses_off;
    uint32_t max_lat_us;
    uint32_t max_resp_us;
    uint32_t throttle_events;

    uint32_t w_jobs;
    uint32_t w_miss;
    uint32_t w_max_lat;
    uint32_t w_max_resp;
    uint64_t w_sum_resp;
    uint64_t w_sum_recv;
    uint32_t w_periods;
    uint32_t w_max_recv;
};

struct tier1_decl {
    const char *name;
    const char *role;
    uint32_t cpu_us;
    uint32_t mem_kb;
};

struct app_manifest {
    const char *name;
    const char *role;
    uint32_t mem_min_kb;
    uint32_t mem_max_kb;
    uint32_t cpu_budget_us;
    uint32_t cpu_elastic_us;
};

struct running_app {
    char name[32];
    uint32_t mem_min_kb;
    uint32_t mem_max_kb;
    uint32_t mem_granted_kb;
    uint32_t cpu_budget_us;
    uint32_t cpu_elastic_us;
    int alive;
};

void uart_init(void);
void uart_putc(char c);
int uart_getc_nonblock(void);
char uart_getc(void);
void printf(const char *fmt, ...);
void readline(char *buf, int cap);

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);

uint64_t dtb_ram_bytes(uint64_t hinted);

void gic_init(void);
void gic_debug(void);
uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);

void timer_init(void);
uint64_t timer_arm_us(uint64_t us);
uint64_t timer_read_pct(void);
uint64_t timer_read_cval(void);
int64_t timer_ticks_to_ns(int64_t ticks);
void timer_note_irq(void);
uint64_t now_us(void);
uint64_t timer_freq(void);
uint64_t timer_jiffies(void);

int dtb_fault_expected(void);
void dtb_note_fault(void);

void busy_us(uint32_t us);

void resource_init(uint64_t ram_bytes);
void resource_show(void);
uint64_t resource_ram_kb(void);
uint64_t resource_app_mem_capacity_kb(void);
uint32_t resource_app_cpu_capacity_us(void);
uint32_t resource_cpu_remaining_us(void);
uint64_t resource_mem_remaining_kb(void);
int resource_pressure(void);
void resource_set_pressure(int on);
int resource_check(const struct app_manifest *m);
const char *resource_reason(void);
int resource_commit(const struct app_manifest *m);
int resource_close_index(int index);
int resource_close_name(const char *name);
int resource_close_all(void);
int resource_running_count(void);
const struct running_app *resource_running(int index);
const struct app_manifest *resource_catalog(int *count);

void sched_init(void);
struct task *sched_current(void);
struct task *task_create(const char *name, int tier, void (*entry)(void));
void task_reset_window(struct task *t);
void sched_set_tiering(int on);
int sched_tiering(void);
void sched_set_count_guarantees(int on);
void sched_arm_shell_timeout(uint64_t abs_us);
struct task *sched_shell(void);
struct task *sched_find(const char *name);
struct task *sched_task_at(int index);
uint64_t sched_period_skips(void);
uint32_t sched_max_us(void);
uint32_t sched_overruns(void);
uint32_t sched_clock_stalls(void);
uint32_t sched_clock_stall_max_us(void);
void sched_start(void (*shell_entry)(void));

uint64_t handle_irq(uint64_t frame_sp);
uint64_t handle_sync(uint64_t frame_sp);
void handle_unhandled(uint64_t frame_sp, uint64_t kind);

void kmain(uint64_t dtb);
void shell_main(void);

/* 定时器分辨率探测。调度器在探测期间不切换任务。 */
void probe_begin(uint32_t target_us);
int probe_done(void);
int probe_samples(uint32_t *min_us, uint32_t *avg_us, uint32_t *max_us);

/* 上下文切换测量：SVC #1 让出但保持 READY。 */
void switch_measure_begin(void);
void switch_measure_note(uint32_t us);
void switch_measure_end(void);
int switch_measure_result(uint32_t *min_us, uint32_t *avg_us, uint32_t *max_us, uint32_t *n);

void diag_watch(struct task *t);
void diag_report(void);

void bench_begin(int periods);
int bench_recording(void);
void bench_watch(struct task *t);
void bench_notify_job(struct task *t, uint32_t lat, uint32_t resp, int miss);
int bench_active(void);
int bench_sample_count(void);
void bench_sample(int i, uint32_t *lat, uint32_t *resp, int *miss);

void services_start(void);
void quotaos_periodic_entry(void);
void quotaos_spinner_entry(void);

void start_first_task(uint64_t frame_sp);
