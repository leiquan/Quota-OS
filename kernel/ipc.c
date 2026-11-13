/*
 * E2：预算令牌随 IPC 走。驱动是被动服务端，自己不被调度；应用调用它时，
 * 内核把应用的预算借给驱动用，驱动干活的时间记在应用头上（seL4 MCS 的做法），
 * 驱动自己的预算不动。
 */
#include "quotaos.h"

static uint64_t sys_call(uint64_t server, uint64_t req)
{
    register uint64_t x0 asm("x0") = server;
    register uint64_t x1 asm("x1") = req;
    asm volatile("svc #1" : "+r"(x0) : "r"(x1) : "memory");
    return x0;
}

static uint64_t sys_reply_wait(uint64_t reply)
{
    register uint64_t x0 asm("x0") = reply;
    asm volatile("svc #2" : "+r"(x0) : : "memory");
    return x0;
}

/* 驱动：收到的请求是要占用模拟设备多少 us；回复它自己量的耗时（只作参考）。 */
/* 第一次请求由内核放进 x0，从这里开始；以后每次从 svc #2 返回时拿到下一个请求。 */
static void driver_entry(uint64_t req)
{
    for (;;) {
        uint64_t t0 = ticks_now();
        busy_ticks(us_to_ticks(req));
        uint64_t r = ticks_now() - t0;
        req = sys_reply_wait(r);
    }
}

static int drv_id;

/* 应用：不停地调驱动，请求 50..400 us（固定的伪随机序列）。 */
static void app_entry(void)
{
    uint32_t x = 12345;
    for (;;) {
        x = x * 1103515245u + 12345u;
        sys_call((uint64_t)drv_id, 50 + (x >> 16) % 351);
    }
}

void e2_demo(uint32_t periods, int qa_line)
{
    printf("\n=== 13 预算令牌：驱动替谁干活就扣谁的预算 ===\n");
    struct task *drv = task_create("sim-dma-drv", TIER_SERVER, KIND_SERVER);
    task_set_entry(drv, (void (*)(void))driver_entry);
    drv->min_t = us_to_ticks(500);      /* 驱动自己的预算（这里不用） */
    drv->bench_owned = 1;
    drv_id = drv->id;
    struct task *app = task_create("cam-app", TIER2, KIND_SPIN);
    task_set_entry(app, app_entry);
    app->min_t = us_to_ticks(3000);
    app->bench_owned = 1;
    struct task *bg = task_create("bg-caller", TIER3, KIND_SPIN);
    task_set_entry(bg, app_entry);
    bg->bench_owned = 1;
    task_start(app);
    task_start(bg);
    printf("cam-app (tier 2, min 3000 us) and bg-caller (tier 3) call\n");
    printf("sim-dma-drv (passive server) for 50..400 us of device work each.\n");
    struct ipc_stats s;
    sched_ipc_stats(&s, 1);
    uint64_t drv_used0 = drv->used_t;
    sched_bench(periods);
    sched_ipc_stats(&s, 0);
    uint64_t drv_change = drv->used_t - drv_used0;
    printf("calls %lu; measured by the kernel during the calls:\n", s.calls);
    printf("  driver running %lu ticks + IPC kernel path %lu ticks\n", s.run_sum,
           s.meas_sum - s.run_sum);
    printf("  = %lu ticks; caller budgets dropped by %lu ticks\n", s.meas_sum,
           s.drop_sum);
    printf("  longest call %lu ticks; max |drop - measured| per call: %lu ticks\n",
           s.meas_max, s.diff_max);
    printf("  IPC kernel path charged to callers in total (not to the system\n");
    printf("  reserve): %lu ticks\n", s.kern_sum);
    printf("  driver's own budget used during the window: %lu ticks\n", drv_change);
    printf("  driver's own wall-clock timing incl. preemption (reference only):\n");
    printf("  total %lu ticks\n", s.self_sum);
    sched_print_window();
    sched_print_kernel();
    if (qa_line)
        printf("E2 item8 %s\n",
               s.calls > 0 && s.diff_max <= 1 && drv_change == 0 ? "PASS" : "FAIL");
}
