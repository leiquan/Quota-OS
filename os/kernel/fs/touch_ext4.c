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
static uint64_t begin;
static struct sched_system_info before;
static struct app_display_stats first_display;
static unsigned recorded, dropped;
static struct {
    uint64_t submitted, completed;
    int result;
} io_trace[sizeof steps / sizeof steps[0]];
static uint64_t scroll_ticks[1024];
static unsigned bus_count, bus_dropped, page_count, page_dropped;
static struct { uint64_t begin, end; unsigned operation, bytes; } bus_trace[512];
static struct { uint64_t tick; unsigned generation; } page_trace[128];

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
    if ((op == ROOT_LIST && *arg) ||
        (op != ROOT_LIST && (!length || (op == ROOT_SAVE ? !bytes : bytes != 0))))
        result = op == ROOT_LIST ? "Usage: ls" : op == ROOT_READ ? "Usage: cat FILE" :
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
    printf("[touch-ext4] bus-trace count=%u dropped=%u\n", bus_count, bus_dropped);
    for (unsigned i = 0; i < bus_count; ++i)
        printf("[touch-ext4] bus-op=%u bytes=%u start=%lu end=%lu\n",
               bus_trace[i].operation, bus_trace[i].bytes,
               (unsigned long)bus_trace[i].begin, (unsigned long)bus_trace[i].end);
    printf("[touch-ext4] page-trace count=%u dropped=%u\n", page_count, page_dropped);
    for (unsigned i = 0; i < page_count; ++i)
        printf("[touch-ext4] page-frame generation=%u ticks=%lu\n",
               page_trace[i].generation, (unsigned long)page_trace[i].tick);
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
    } else if (running) {
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
