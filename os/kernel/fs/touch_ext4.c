#include "quotaos.h"
#include "auth.h"
#include "root_service.h"
#include "touch_ext4.h"

static const char payload[] = "Quota OS Touch UI journaled replacement and cold readback";
static const char keep[] = "touch-keep.txt", scratch[] = "touch-scratch.txt";
struct probe_step {
    enum root_operation op;
    const char *name, *input;
    size_t bytes;
    int result;
};
static const struct probe_step steps[] = {
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

#ifdef QUOTA_TOUCH_SMP
static const struct probe_step continuation[] = {
    {ROOT_SAVE, scratch, payload, sizeof payload - 1, EXT4_OK},
    {ROOT_READ, scratch, payload, sizeof payload - 1, EXT4_OK},
    {ROOT_REMOVE, scratch, NULL, 0, EXT4_OK},
};
#endif
enum { INITIAL_STEPS = sizeof steps / sizeof steps[0], REQUEST_RECORDS = 96 };
static unsigned step, started, running, pending;
#ifdef QUOTA_TOUCH_SMP
extern const char touch_smp_pressure_start[], touch_smp_pressure_end[];
static int64_t pressure_worker;
static uint64_t pressure_period;
static unsigned memory_sweeps;
static volatile uint64_t pressure_words[64 * 1024 / sizeof(uint64_t)];
#endif

static int shell_pending, files_pending;
static enum root_operation shell_op;
static char shell_data[ROOT_SERVICE_BYTES + 1];
static enum root_operation files_op;

int touch_ext4_enabled(void)
{
#ifdef QUOTA_GUI_EXT4
    const char *args = dtb_bootargs();
    return !strcmp(args, TOUCH_EXT4_PRODUCT_BOOTARGS) || !strcmp(args, TOUCH_EXT4_BOOTARGS);
#else
    return 0;
#endif
}
static uint64_t begin, next_request;
static struct sched_system_info before;
static struct app_display_stats first_display;
static unsigned recorded, dropped;
static struct {
    uint64_t submitted, completed;
    enum root_operation op;
    int result;
} io_trace[REQUEST_RECORDS];
static uint64_t scroll_ticks[1024];
static unsigned bus_count, bus_dropped, page_count, page_dropped;
static struct { uint64_t begin, end; unsigned operation, bytes; } bus_trace[4096];
static struct { uint64_t tick; unsigned generation; } page_trace[128];
static unsigned stage_count, stage_dropped;
static struct { uint64_t tick; unsigned stage, generation; } stages[2048];

void touch_ext4_record_io(unsigned op, unsigned bytes, unsigned long begin_tick, unsigned long end_tick)
{
    if (!running) return;
    if (bus_count < sizeof bus_trace / sizeof bus_trace[0])
        bus_trace[bus_count++] = (typeof(*bus_trace)){begin_tick, end_tick, op, bytes};
    else ++bus_dropped;
}

void touch_ext4_record_frame(unsigned generation)
{
    if (!running) return;
    if (page_count < sizeof page_trace / sizeof page_trace[0])
        page_trace[page_count++] = (typeof(*page_trace)){ticks_now(), generation};
    else ++page_dropped;
}

void touch_ext4_record_page_stage(enum touch_page_stage stage, unsigned generation)
{
    if (!running) return;
    if (stage_count < sizeof stages / sizeof stages[0])
        stages[stage_count++] = (typeof(*stages)){ticks_now(), stage, generation};
    else ++stage_dropped;
}

static int starts_with_word(const char *text, const char *word)
{
    while (*word && *text == *word) {
        ++text;
        ++word;
    }
    return !*word && (!*text || *text == ' ');
}

int touch_ext4_shell_command(const char *command, char *reply, size_t capacity)
{
    if (auth_state() != AUTH_OPEN || !touch_ext4_enabled())
        return 0;
    enum root_operation op;
    const char *arg;
    if (starts_with_word(command, "ls")) { op = ROOT_LIST; arg = command + 2; }
    else if (starts_with_word(command, "cat")) { op = ROOT_READ; arg = command + 3; }
    else if (starts_with_word(command, "write")) { op = ROOT_SAVE; arg = command + 5; }
    else if (starts_with_word(command, "rm")) { op = ROOT_REMOVE; arg = command + 2; }
    else return 0;

    while (*arg == ' ') ++arg;
    if (op != ROOT_LIST && *arg == '/') ++arg;
    char file[81];
    size_t length = 0;
    while (arg[length] && arg[length] != ' ' && length < sizeof file - 1)
        ++length;
    memcpy(file, arg, length);
    file[length] = 0;
    const char *value = arg + length;
    while (*value == ' ') ++value;
    size_t bytes = 0;
    while (value[bytes]) ++bytes;

    const char *result;
    if ((op == ROOT_LIST && *arg && strcmp(arg, "/")) ||
        (op != ROOT_LIST && (!length || (op == ROOT_SAVE ? !bytes : bytes != 0))))
        result = op == ROOT_LIST ? "Usage: ls [/]" : op == ROOT_READ ? "Usage: cat FILE" :
                 op == ROOT_SAVE ? "Usage: write FILE TEXT" : "Usage: rm FILE";
    else if (touch_ext4_busy())
        result = "ext4 disk busy. Try again after the current operation.";
    else if (root_service_submit(op, op == ROOT_LIST ? NULL : file,
                                 op == ROOT_SAVE ? value : NULL, bytes))
        result = "Unable to queue ext4 request.";
    else {
        shell_op = op;
        shell_pending = 1;
        result = "ext4 request queued.";
    }
    if (capacity) {
        size_t n = 0;
        while (result[n] && n + 1 < capacity) {
            reply[n] = result[n];
            ++n;
        }
        reply[n] = 0;
    }
    return 1;
}

static const char *take_result(enum root_operation op, int *result)
{
    size_t bytes;
    if (root_service_take(shell_data, ROOT_SERVICE_BYTES, &bytes, result) != 1)
        return NULL;
    printf("[touch-ext4-user] op=%u result=%d bytes=%lu\n", (unsigned)op, *result, (unsigned long)bytes);
    if (*result) return ext4_result_text(*result);
    if (op == ROOT_SAVE) return "ext4 saved";
    if (op == ROOT_REMOVE) return "ext4 removed";
    if (!bytes) return op == ROOT_LIST ? "No files." : "Empty file.";
    for (size_t i = 0; i < bytes; ++i)
        if (!shell_data[i]) shell_data[i] = '?';
    shell_data[bytes] = 0;
    return shell_data;
}

const char *touch_ext4_shell_poll(void)
{
    if (!shell_pending) return NULL;
    int result;
    const char *reply = take_result(shell_op, &result);
    if (reply) shell_pending = 0;
    return reply;
}

int touch_ext4_files_submit(enum root_operation op, const char *name, const char *text, size_t bytes)
{
    if (auth_state() != AUTH_OPEN || !touch_ext4_enabled()) return -1;
    if (touch_ext4_busy()) return 1;
    int rc = root_service_submit(op, name, text, bytes);
    if (!rc) {
        files_pending = 1;
        files_op = op;
    }
    return rc;
}

const char *touch_ext4_files_poll(enum root_operation *op, int *result)
{
    if (!files_pending) return NULL;
    const char *reply = take_result(files_op, result);
    if (reply) {
        *op = files_op;
        files_pending = 0;
    }
    return reply;
}

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
#ifdef QUOTA_TOUCH_SMP
    sched_touch_quota_stop();
#endif
    uint64_t end = ticks_now();
    printf("[touch-ext4] io-end ticks=%lu result=%s cold=%d scroll_delta=%u\n",
           (unsigned long)end, pass ? "PASS" : "FAIL", cold, recorded);
    printf("[touch-ext4] speed_deadlines=%u camera_missed=%u tier2_shortfalls=%lu tier3_over=%lu\n",
           last_display.speed_misses - first_display.speed_misses, last_display.video_missed - first_display.video_missed,
           (unsigned long)(after.tier2_short - before.tier2_short), (unsigned long)(after.tier3_over - before.tier3_over));
    app_frame_trace_dump();
#ifdef QUOTA_TOUCH_SMP
    if (pressure_worker > 0) proc_kill(pressure_worker);
    sched_smp_report();
    sched_touch_quota_dump();
    printf("[touch-ext4] concurrent_memory_sweeps=%u bytes_per_sweep=%u\n",
           memory_sweeps, (unsigned)sizeof pressure_words);
#endif
    for (unsigned i = 0; i < sizeof io_trace / sizeof io_trace[0]; ++i) {
        if (!io_trace[i].completed) break;
        printf("[touch-ext4] request=%u op=%u start=%lu end=%lu result=%d\n",
               i, io_trace[i].op, (unsigned long)io_trace[i].submitted,
               (unsigned long)io_trace[i].completed, io_trace[i].result);
    }
    printf("[touch-ext4] bus-trace count=%u dropped=%u\n", bus_count, bus_dropped);
    for (unsigned i = 0; i < bus_count; ++i)
        printf("[touch-ext4] bus-op=%u bytes=%u start=%lu end=%lu\n",
               bus_trace[i].operation, bus_trace[i].bytes,
               (unsigned long)bus_trace[i].begin, (unsigned long)bus_trace[i].end);
    printf("[touch-ext4] page-trace count=%u dropped=%u\n", page_count, page_dropped);
    for (unsigned i = 0; i < page_count; ++i)
        printf("[touch-ext4] page-frame generation=%u ticks=%lu\n",
               page_trace[i].generation, (unsigned long)page_trace[i].tick);
    printf("[touch-ext4] page-stage-trace count=%u dropped=%u\n", stage_count, stage_dropped);
    for (unsigned i = 0; i < stage_count; ++i)
        printf("[touch-ext4] page-stage=%u generation=%u ticks=%lu\n",
               stages[i].stage, stages[i].generation, (unsigned long)stages[i].tick);
    printf("[touch-ext4] scroll-trace count=%u dropped=%u\n", recorded, dropped);
    for (unsigned i = 0; i < recorded; ++i)
        printf("[touch-ext4] scroll-step ticks=%lu\n", (unsigned long)scroll_ticks[i]);
}

void touch_ext4_scroll_step(void)
{
    if (auth_state() != AUTH_OPEN || strcmp(dtb_bootargs(), TOUCH_EXT4_BOOTARGS))
        return;
    if (shell_pending || files_pending) return;
    if (!started) {
        started = running = 1;
        begin = ticks_now();
        sched_read_system_info(&before);
        app_display_read(&first_display);
        app_frame_trace_start();
#ifdef QUOTA_TOUCH_SMP
        sched_touch_quota_start();
        pressure_worker = proc_spawn_console(touch_smp_pressure_start,
                                               touch_smp_pressure_end - touch_smp_pressure_start);
        printf("[touch-ext4] pressure_worker=%ld workers=%d\n", (long)pressure_worker, smp_online());
#endif
    }
    if (running) {
        if (recorded < sizeof scroll_ticks / sizeof scroll_ticks[0])
            scroll_ticks[recorded++] = ticks_now();
        else ++dropped;
    }
}

int touch_ext4_busy(void)
{
    return running || shell_pending || files_pending;
}

void touch_ext4_poll(void)
{
    if (!running) return;
#ifdef QUOTA_TOUCH_SMP
    uint64_t period = sched_period_now();
    if (period != pressure_period) {
        pressure_period = period;
        for (unsigned i = 0; i < sizeof pressure_words / sizeof pressure_words[0]; ++i)
            pressure_words[i]++;
        ++memory_sweeps;
    }
#endif
    if (step >= REQUEST_RECORDS) { finish(0, 0); return; }
    const struct probe_step *operation;
#ifdef QUOTA_TOUCH_SMP
    const unsigned count = sizeof continuation / sizeof continuation[0];
    operation = step < INITIAL_STEPS ? &steps[step] : &continuation[(step - INITIAL_STEPS) % count];
#else
    operation = &steps[step];
#endif
    if (!pending) {
        uint64_t submitted = ticks_now();
        if (submitted < next_request) return;
        int rc = root_service_submit(operation->op, operation->name,
                                     operation->op == ROOT_SAVE ? operation->input : NULL,
                                     operation->op == ROOT_SAVE ? operation->bytes : 0);
        if (rc < 0) finish(0, 0);
        if (rc) return;
        io_trace[step].submitted = submitted;
        io_trace[step].op = operation->op;
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
    if (result != operation->result ||
        (operation->op == ROOT_READ && result == EXT4_OK &&
         (bytes != operation->bytes || !matches(operation->input, data, bytes)))) {
        printf("[touch-ext4] request-failed step=%u ext4=%d\n", step, result);
        finish(0, 0);
        return;
    }
    ++step;
#ifdef QUOTA_TOUCH_SMP
    if (step >= INITIAL_STEPS && (step - INITIAL_STEPS) % count == 0) {
        if (page_count || ticks_now() - begin >= us_to_ticks(28000000) ||
            step + count > REQUEST_RECORDS) {
            printf("[touch-ext4] io-start ticks=%lu mount=0\n", (unsigned long)begin);
            finish(page_count != 0, 0);
            return;
        }
    }
    /* After lifecycle verification, maintain alternating real writes, reads and flushes
     * until a viewport frame arrives or the bounded observation window expires. */
    next_request = ticks_now() + us_to_ticks(step < INITIAL_STEPS ? 1400000 : 0);
#else
    if (step == INITIAL_STEPS) {
        printf("[touch-ext4] io-start ticks=%lu mount=0\n", (unsigned long)begin);
        finish(1, 0);
    } else {
        next_request = ticks_now() + us_to_ticks(350000);
    }
#endif
}
