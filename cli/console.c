#include "quotaos.h"

extern const unsigned char __cli_module[];
/* The process manager uses these queries to classify faults. CLI has no display. */
int fb_reserved_range(uint64_t *start, uint64_t *bytes)
{
    *start = *bytes = 0;
    return -1;
}

int64_t proc_spawn_module(const char *code, unsigned bytes, int tier);

static void run_test(int tier)
{
    unsigned size;
    memcpy(&size, __cli_module + 8, sizeof size);
    if (__cli_module[4] != 'Q' || __cli_module[5] != 'C' || __cli_module[6] != 'A' ||
        __cli_module[7] != 'P' || size < 12 || size > 4096) {
        printf("Test.cap unavailable\n");
        return;
    }
    int64_t h = proc_spawn_module((const char *)__cli_module, size, tier);
    if (h < 0) {
        printf("Test admission refused\n");
        return;
    }
    uint64_t last = sched_period_now();
    while (proc_alive(h)) {
        asm volatile("wfi");
        uint64_t now = sched_period_now();
        if (tier == TIER2 && now != last) {
            last = now;
            for (int i = 0; i < MAX_TASKS; ++i) {
                struct task *t = sched_task_at(i);
                if (t && t->tier == TIER2 && t->state != TASK_DEAD)
                    printf("[quota] period=%lu grant=%lu used=%lu minimum=%lu\n", now,
                           t->last_grant_t, t->last_used_t, t->min_t);
            }
        }
    }
    const char *result = proc_console_text();
    printf("%s\n", result ? result : "Test exited without output");
}

void shell_main(void)
{
    char line[128];
    klog_flush();
    note_disk_init();
    for (;;) {
        printf("quota> ");
        readline(line, sizeof line);
        if (!strcmp(line, "help"))
            printf("help mem tasks quota events ls cat Note.txt write TEXT run Test [t2]\n");
        else if (!strcmp(line, "run Test"))
            run_test(TIER3);
        else if (!strcmp(line, "run Test t2"))
            run_test(TIER2);
        else if (!strcmp(line, "mem")) {
            uint64_t ram;
            uint32_t cpu;
            resource_app_available(&cpu, &ram);
            printf("RAM %lu KiB system %u KiB tier1 %u KiB app_available %lu KiB\n",
                   resource_ram_kb(), system_reserve_kb(), resource_t1_mem_kb(), ram);
        } else if (!strcmp(line, "tasks")) {
            for (int i = 0; i < MAX_TASKS; i++) {
                struct task *t = sched_task_at(i);
                if (t && t->state != TASK_DEAD)
                    printf("%s tier=%d jobs=%u misses=%u\n", t->name, t->tier, t->jobs, t->misses);
            }
        } else if (!strcmp(line, "quota") || !strcmp(line, "events")) {
            struct sched_system_info s;
            sched_read_system_info(&s);
            printf("period=%lu tier1_misses=%lu tier2_short=%lu tier3_over=%lu\n", s.period,
                   s.tier1_misses, s.tier2_short, s.tier3_over);
        } else if (!strcmp(line, "ls"))
            printf("%s\n", note_disk_text() ? "Note.txt" : "");
        else if (!strcmp(line, "cat Note.txt"))
            printf("%s\n", note_disk_text() ? note_disk_text() : "File not found");
        else if (line[0] == 'w' && line[1] == 'r' && line[2] == 'i' && line[3] == 't' &&
                 line[4] == 'e' && line[5] == ' ') {
            if (note_disk_save(line + 6))
                printf("Disk unavailable\n");
            else {
                while (!note_disk_power_ready()) {
                    note_disk_poll();
                    asm volatile("wfi");
                }
                printf("%s\n", note_disk_status());
            }
        } else if (*line)
            printf("Unknown command. Type help.\n");
    }
}

void fb_geometry(uint32_t *width, uint32_t *height) { *width = *height = 0; }
