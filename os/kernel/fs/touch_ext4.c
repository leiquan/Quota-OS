#include "quotaos.h"
#include "auth.h"
#include "root_disk.h"
#include "touch_ext4.h"

static volatile int running;
static unsigned scroll_steps, logged_steps, dropped_steps;
static uint64_t scroll_ticks[1024];

static int read_expected(const struct ext4_volume *volume, const char *name, const char *expected, size_t expected_bytes)
{
    char data[96];
    size_t bytes;
    int rc = ext4_read_root(volume, name, data, sizeof data, &bytes);
    if (rc != EXT4_OK || bytes != expected_bytes) return 0;
    for (size_t i = 0; i < bytes; ++i) if (data[i] != expected[i]) return 0;
    return 1;
}

static void ext4_lifecycle(void)
{
    struct ext4_volume volume;
    const char *keep = "touch-keep.txt";
    const char *scratch = "touch-scratch.txt";
    static const char payload[] = "Quota OS Touch UI journaled replacement and cold readback";
    uint64_t begin = ticks_now();
    int rc = root_disk_mount(&volume);
    printf("[touch-ext4] io-start ticks=%lu mount=%d\n", (unsigned long)begin, rc);
    unsigned steps = scroll_steps;
    struct ext4_file file;
    int existing = !rc ? ext4_open_root(&volume, keep, &file) : rc;
    int cold = existing == EXT4_OK;
    int pass = !rc && (cold ? read_expected(&volume, keep, payload, sizeof payload - 1) :
        existing == EXT4_NOT_FOUND &&
        root_disk_overwrite(&volume, scratch, "created", 7) == EXT4_OK &&
        read_expected(&volume, scratch, "created", 7) &&
        root_disk_overwrite(&volume, scratch, payload, sizeof payload - 1) == EXT4_OK &&
        read_expected(&volume, scratch, payload, sizeof payload - 1) &&
        root_disk_remove(&volume, scratch) == EXT4_OK &&
        ext4_open_root(&volume, scratch, &file) == EXT4_NOT_FOUND &&
        root_disk_overwrite(&volume, keep, payload, sizeof payload - 1) == EXT4_OK &&
        read_expected(&volume, keep, payload, sizeof payload - 1));
    uint64_t finish = ticks_now();
    printf("[touch-ext4] io-end ticks=%lu result=%s cold=%d scroll_delta=%u\n",
           (unsigned long)finish, pass ? "PASS" : "FAIL", cold, scroll_steps - steps);
    running = 0;
    app_frame_trace_dump();
    printf("[touch-ext4] scroll-trace count=%u dropped=%u\n", logged_steps, dropped_steps);
    for (unsigned i = 0; i < logged_steps; ++i)
        printf("[touch-ext4] scroll-step ticks=%lu\n", (unsigned long)scroll_ticks[i]);
    task_kill(sched_current());
    for (;;) asm volatile("wfi");
}

void touch_ext4_scroll_step(void)
{
    if (auth_state() != AUTH_OPEN || strcmp(dtb_bootargs(), TOUCH_EXT4_BOOTARGS))
        return;
    ++scroll_steps;
    if (running) {
        if (logged_steps < sizeof scroll_ticks / sizeof scroll_ticks[0])
            scroll_ticks[logged_steps++] = ticks_now();
        else ++dropped_steps;
        return;
    }
    if (scroll_steps != 1) return;
    struct task *task = task_try_create("touch-ext4", TIER3, KIND_SPIN);
    if (!task) return;
    running = 1;
    app_frame_trace_start();
    task_set_entry(task, ext4_lifecycle);
    task_start(task);
}
