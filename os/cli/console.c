#include "commit.h"
#include "quotaos.h"
#include "root_disk.h"

extern const unsigned char __cli_module[];
extern const unsigned __cli_module_size;
extern const char __cli_source[4096];
/* The process manager uses these queries to classify faults. CLI has no display. */
int fb_reserved_range(uint64_t *start, uint64_t *bytes)
{
    *start = *bytes = 0;
    return -1;
}

int64_t proc_spawn_module(const char *code, unsigned bytes, int tier);

static void run_test(int tier)
{
    unsigned size = __cli_module_size;
    if (!size || size > 4096) {
        printf("Test unavailable (start QEMU with CLI_TEST=1)\n");
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

static struct ext4_volume root_volume;
static unsigned char root_file[4096];

static int print_root_name(void *context, const char *name, unsigned length)
{
    (void)context;
    for (unsigned i = 0; i < length; ++i) printf("%c", name[i]);
    printf("\n");
    return 0;
}

static int ensure_root(void)
{
    int rc = root_volume.read ? EXT4_OK : root_disk_mount(&root_volume);
    if (rc) printf("ext4 mount failed (%d)\n", rc);
    return rc;
}

static void show_commands(void)
{
    printf("System   uname          Show kernel version and architecture\n"
           "         uptime         Show elapsed guest time\n"
           "         mem / free     Show RAM and available application budget\n"
           "         tasks / ps     List active tasks and their tiers\n"
           "         quota / events Show scheduling counters\n"
           "Files    pwd            Show the root directory\n"
           "         ls             List files in /\n"
           "         cat FILE       Read test.c or Note.txt\n"
           "         write TEXT     Save a note to the attached disk\n"
           "         disk           Show disk status and saved note size\n"
           "         rootls         List the read-only ext4 root\n"
           "         rootcat FILE   Read a file from the ext4 root\n"
           "Apps     run test.c     Run the compiled sample in Tier 3\n"
           "         run Test t2    Request a Tier 2 budget for Test\n"
           "Console  echo TEXT      Print text\n"
           "         clear          Clear the terminal screen\n"
           "         help           Show this command guide\n");
}

static void show_welcome(void)
{
    printf("Quota OS CLI %s | ARM64 | %lu KiB RAM\n", QUOTAOS_VERSION, resource_ram_kb());
    printf("Common commands: ls, mem, tasks%s. Type help for all commands.\n",
           __cli_module_size ? ", run test.c" : "");
}

void shell_main(void)
{
    char line[128];
    klog_flush();
    note_disk_init();
    show_welcome();
    for (;;) {
        printf("quota> ");
        readline(line, sizeof line);
        char *command = line;
        while (*command == ' ')
            ++command;
        char *arg = command;
        while (*arg && *arg != ' ')
            ++arg;
        if (*arg)
            *arg++ = 0;
        while (*arg == ' ')
            ++arg;
        if (!*command)
            continue;
        if (!strcmp(command, "echo")) {
            printf("%s\n", arg);
        } else if (!strcmp(command, "cat")) {
            if (!*arg)
                printf("Usage: cat FILE\n");
            else if ((!strcmp(arg, "test.c") || !strcmp(arg, "/test.c")) && __cli_source[0])
                printf("%s", __cli_source);
            else if (strcmp(arg, "Note.txt") || !note_disk_text())
                printf("File not found: %s\n", arg);
            else
                printf("%s\n", note_disk_text());
        } else if (!strcmp(command, "rootls") && !*arg) {
            if (!ensure_root()) {
                int rc = ext4_list_root(&root_volume, print_root_name, NULL);
                if (rc) printf("ext4 directory read failed (%d)\n", rc);
            }
        } else if (!strcmp(command, "rootcat") && *arg) {
            if (!ensure_root()) {
                size_t bytes;
                int rc = ext4_read_root(&root_volume, arg, root_file, sizeof root_file, &bytes);
                if (rc) printf("ext4 file read failed (%d)\n", rc);
                else {
                    for (size_t i = 0; i < bytes; ++i) printf("%c", root_file[i]);
                    printf("\n");
                }
            }
        } else if (!strcmp(command, "write")) {
            if (!*arg)
                printf("Usage: write TEXT\n");
            else if (note_disk_save(arg))
                printf("Cannot save: %s\n", note_disk_status());
            else {
                while (!note_disk_power_ready()) {
                    note_disk_poll();
                    asm volatile("wfi");
                }
                printf("%s\n", note_disk_status());
            }
        } else if (!strcmp(command, "run")) {
            if (!strcmp(arg, "Test") || !strcmp(arg, "test.c"))
                run_test(TIER3);
            else if (!strcmp(arg, "Test t2") || !strcmp(arg, "test.c t2"))
                run_test(TIER2);
            else
                printf("Usage: run Test [t2]\n");
        } else if (*arg)
            printf("Unexpected arguments. Type help for usage.\n");
        else if (!strcmp(command, "help"))
            show_commands();
        else if (!strcmp(command, "clear"))
            printf("\033[2J\033[H");
        else if (!strcmp(command, "uname"))
            printf("Quota OS CLI %s (%s) aarch64\n", QUOTAOS_VERSION, QUOTAOS_COMMIT);
        else if (!strcmp(command, "uptime"))
            printf("Uptime: %lu ms (guest clock)\n", ticks_to_us(ticks_now()) / 1000);
        else if (!strcmp(command, "pwd"))
            printf("/\n");
        else if (!strcmp(command, "disk"))
            printf("%s | Note.txt: %u bytes\n", note_disk_status(), note_disk_bytes());
        else if (!strcmp(command, "mem") || !strcmp(command, "free")) {
            uint64_t ram;
            uint32_t cpu;
            resource_app_available(&cpu, &ram);
            printf("RAM %lu KiB system %u KiB tier1 %u KiB app_available %lu KiB\n",
                   resource_ram_kb(), system_reserve_kb(), resource_t1_mem_kb(), ram);
        } else if (!strcmp(command, "tasks") || !strcmp(command, "ps")) {
            for (int i = 0; i < MAX_TASKS; i++) {
                struct task *t = sched_task_at(i);
                if (t && t->state != TASK_DEAD)
                    printf("%s tier=%d jobs=%u misses=%u\n", t->name, t->tier, t->jobs, t->misses);
            }
        } else if (!strcmp(command, "quota") || !strcmp(command, "events")) {
            struct sched_system_info s;
            sched_read_system_info(&s);
            printf("period=%lu tier1_misses=%lu tier2_short=%lu tier3_over=%lu\n", s.period,
                   s.tier1_misses, s.tier2_short, s.tier3_over);
        } else if (!strcmp(command, "ls")) {
            if (__cli_source[0]) printf("test.c\n");
            if (note_disk_text()) printf("Note.txt\n");
            else if (!__cli_source[0]) printf("No files.\n");
        } else
            printf("Unknown command: %s. Type help.\n", command);
    }
}

void fb_geometry(uint32_t *width, uint32_t *height) { *width = *height = 0; }
