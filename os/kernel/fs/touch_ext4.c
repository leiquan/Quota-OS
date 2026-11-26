#include "quotaos.h"
#include "auth.h"
#include "root_service.h"
#include "touch_ext4.h"

static const char payload[] = "Quota OS Touch UI journaled replacement and cold readback";
static const char keep[] = "touch-keep.txt", scratch[] = "touch-scratch.txt";
static const struct {
    enum root_operation op;
    const char *name, *input;
    size_t bytes;
    int result;
} steps[] = {
    {ROOT_READ, keep, NULL, 0, EXT4_NOT_FOUND},
    {ROOT_SAVE, scratch, "created", 7, EXT4_OK},
    {ROOT_READ, scratch, "created", 7, EXT4_OK},
    {ROOT_SAVE, scratch, payload, sizeof payload - 1, EXT4_OK},
    {ROOT_READ, scratch, payload, sizeof payload - 1, EXT4_OK},
    {ROOT_REMOVE, scratch, NULL, 0, EXT4_OK},
    {ROOT_READ, scratch, NULL, 0, EXT4_NOT_FOUND},
    {ROOT_SAVE, keep, payload, sizeof payload - 1, EXT4_OK},
    {ROOT_READ, keep, payload, sizeof payload - 1, EXT4_OK},
};

static unsigned step, started, running, pending;
static uint64_t begin;
static struct sched_system_info before;
static struct app_display_stats first_display;
static unsigned recorded, dropped;
static struct {
    uint64_t submitted, completed;
    int result;
} io_trace[sizeof steps / sizeof steps[0]];
static uint64_t scroll_ticks[1024];

static int matches(const char *expected, const char *actual, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        if (expected[i] != actual[i]) return 0;
    return 1;
}

static void finish(int pass, int cold)
{
    struct sched_system_info after;
    struct app_display_stats last_display;
    sched_read_system_info(&after);
    app_display_read(&last_display);
    running = pending = 0;
    uint64_t end = ticks_now();
    printf("[touch-ext4] io-end ticks=%lu result=%s cold=%d scroll_delta=%u\n",
           (unsigned long)end, pass ? "PASS" : "FAIL", cold, recorded);
    printf("[touch-ext4] speed_deadlines=%u camera_missed=%u tier2_shortfalls=%lu tier3_over=%lu\n",
           last_display.speed_misses - first_display.speed_misses, last_display.video_missed - first_display.video_missed,
           (unsigned long)(after.tier2_short - before.tier2_short), (unsigned long)(after.tier3_over - before.tier3_over));
    app_frame_trace_dump();
    for (unsigned i = 0; i < sizeof io_trace / sizeof io_trace[0]; ++i) {
        if (!io_trace[i].completed) break;
        printf("[touch-ext4] request=%u op=%u start=%lu end=%lu result=%d\n",
               i, steps[i].op, (unsigned long)io_trace[i].submitted,
               (unsigned long)io_trace[i].completed, io_trace[i].result);
    }
    printf("[touch-ext4] scroll-trace count=%u dropped=%u\n", recorded, dropped);
    for (unsigned i = 0; i < recorded; ++i)
        printf("[touch-ext4] scroll-step ticks=%lu\n", (unsigned long)scroll_ticks[i]);
}

void touch_ext4_scroll_step(void)
{
    if (auth_state() != AUTH_OPEN || strcmp(dtb_bootargs(), TOUCH_EXT4_BOOTARGS))
        return;
    if (!started) {
        started = running = 1;
        begin = ticks_now();
        sched_read_system_info(&before);
        app_display_read(&first_display);
        app_frame_trace_start();
    } else if (running) {
        if (recorded < sizeof scroll_ticks / sizeof scroll_ticks[0])
            scroll_ticks[recorded++] = ticks_now();
        else ++dropped;
    }
}

int touch_ext4_busy(void)
{
    return running != 0;
}

void touch_ext4_poll(void)
{
    if (!running) return;
    if (!pending) {
        uint64_t submitted = ticks_now();
        int rc = root_service_submit(steps[step].op, steps[step].name,
                                     steps[step].op == ROOT_SAVE ? steps[step].input : NULL,
                                     steps[step].op == ROOT_SAVE ? steps[step].bytes : 0);
        if (rc < 0) finish(0, 0);
        if (rc) return;
        io_trace[step].submitted = submitted;
        pending = 1;
    }
    static char data[ROOT_SERVICE_BYTES];
    size_t bytes;
    int result;
    int got = root_service_take(data, sizeof data, &bytes, &result);
    if (got < 0) { finish(0, 0); return; }
    if (!got) return;
    io_trace[step].completed = ticks_now();
    io_trace[step].result = result;
    pending = 0;
    if (step == 0 && result == EXT4_OK) {
        printf("[touch-ext4] io-start ticks=%lu mount=0\n", (unsigned long)begin);
        finish(bytes == sizeof payload - 1 && matches(payload, data, bytes), 1);
        return;
    }
    if (result != steps[step].result ||
        (steps[step].op == ROOT_READ && result == EXT4_OK &&
         (bytes != steps[step].bytes || !matches(steps[step].input, data, bytes)))) {
        printf("[touch-ext4] request-failed step=%u ext4=%d\n", step, result);
        finish(0, 0);
        return;
    }
    if (++step == sizeof steps / sizeof steps[0]) {
        printf("[touch-ext4] io-start ticks=%lu mount=0\n", (unsigned long)begin);
        finish(1, 0);
    }
}
