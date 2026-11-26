#include "auth.h"
#include "quotaos.h"
#include "shell.h"

struct text { char *next; unsigned left; };
static void append(struct text *t, const char *s)
{
    while (*s && t->left > 1) { *t->next++ = *s++; --t->left; }
    if (t->left) *t->next = 0;
}
static void number(struct text *t, uint64_t x)
{
    char buf[21], *p = buf + sizeof buf;
    *--p = 0;
    do { *--p = '0' + x % 10; } while ((x /= 10));
    append(t, p);
}
static const char *after(const char *s, const char *prefix)
{
    while (*prefix) if (*s++ != *prefix++) return NULL;
    return s;
}
enum shell_action shell_execute(const char *cmd, char *out, unsigned cap)
{
    struct text t = {out,cap};
    append(&t,"");
    const char *name = note_disk_source_name(), *arg;
    if (!strcmp(cmd,"clear")) return SHELL_CLEAR;
    if (!strcmp(cmd,"reboot")) return SHELL_REBOOT;
    if (!strcmp(cmd,"poweroff")) return SHELL_OFF;
    if (!strcmp(cmd,"help")) {
        append(&t,"help pwd ls cat write version ps quota clear reboot poweroff\n");
        append(&t,"cc "); append(&t,name ? name : "hello.c"); append(&t," -o hello.cap\nrun hello.cap");
    } else if (!strcmp(cmd,"pwd")) append(&t,"/Desktop");
    else if (!strcmp(cmd,"version")) append(&t,QUOTAOS_VERSION);
    else if (!strcmp(cmd,"ls")) {
        if (note_disk_text()) append(&t,"Note.txt  ");
        if (name) { append(&t,name); append(&t,"  "); }
        if (note_disk_hello()) append(&t,"hello.txt  ");
        unsigned bytes;
        if (note_disk_package(&bytes)) append(&t,"demo.wap");
        if (cap && !out[0]) append(&t,"Desktop is empty");
    } else if ((arg=after(cmd,"cat "))) {
        const char *s = !strcmp(arg,"hello.txt") ? note_disk_hello() :
            !strcmp(arg,"Note.txt") ? note_disk_text() : name && !strcmp(arg,name) ? note_disk_source() : NULL;
        append(&t,s ? s : "File not found");
    } else if ((arg=after(cmd,"write hello.txt "))) {
        char data[496]; unsigned n=0;
        int quoted = *arg == '"';
        if (quoted) ++arg;
        while (*arg && !(quoted && *arg=='"' && !arg[1]) && n+1<sizeof data) data[n++]=*arg++;
        data[n]=0;
        if ((quoted && (*arg!='"' || arg[1])) || (!quoted && *arg)) append(&t,"Invalid or oversized text");
        else append(&t,note_disk_save_hello(data) ? "Write failed" : "Write queued");
    } else if (!strcmp(cmd,"ps") || (arg=after(cmd,"quota "))) {
        int list=!strcmp(cmd,"ps"), found=0;
        for (int i=0;i<MAX_TASKS;i++) {
            struct task *task=sched_task_at(i);
            if (!task) continue;
            char id[24]; struct text it={id,sizeof id}; number(&it,task->id);
            if (!list && strcmp(arg,id)) continue;
            if (found++) append(&t,"\n");
            append(&t,id); append(&t," "); append(&t,task->name);
            append(&t," tier="); number(&t,task->tier);
            if (!list) {
                append(&t," last_grant_ticks="); number(&t,task->last_grant_t);
                append(&t," last_used_ticks="); number(&t,task->last_used_t);
            }
        }
        if (!found) append(&t,"Task not found");
    } else if ((arg=after(cmd,"cc "))) {
        const char *tail=name ? after(arg,name) : NULL;
        if (!tail || (strcmp(tail," -o hello.cap") && strcmp(tail," -o hello")))
            append(&t,"Usage: cc <saved-source.c> -o hello.cap");
        else if (c_demo_compile(note_disk_source())) append(&t,"Compile error: check the saved C source.");
        else { append(&t,"Compiled "); append(&t,name); append(&t," to "); append(&t,tail+4); }
    } else if (!strcmp(cmd,"run hello.cap") || !strcmp(cmd,"./hello.cap") || !strcmp(cmd,"./hello")) {
        int64_t started;
        if (!strcmp(cmd,"run hello.cap")) {
            extern const char hello_cap_start[], hello_cap_end[];
            started = c_demo_run();
            if (started < 0 && !c_demo_compiled())
                started = proc_spawn_console(hello_cap_start, hello_cap_end-hello_cap_start);
        } else started = c_demo_run();
        append(&t,started<0 ? "Run failed: compile first or wait for a free process slot." : "Running hello.cap (Tier 3)");
    } else append(&t,"Unknown command. Type help.");
    return SHELL_PRINT;
}
int shell_power(int reboot)
{
    int ready=note_disk_power_ready();
    if (ready!=1) return ready;
    register uint64_t fn asm("x0") = reboot ? 0x84000009u : 0x84000008u;
    asm volatile("dsb sy; hvc #0" : "+r"(fn) :: "memory");
    return -1;
}
void shell_serial(void)
{
    note_disk_init();
    auth_serial_login();
    for (;;) {
        while (!note_disk_power_ready()) { note_disk_poll(); asm volatile("wfi"); }
        printf("quota> ");
        char line[544], out[2048];
        readline(line,sizeof line);
        if (!line[0]) continue;
        if (!strcmp(line,"logout")) { auth_logout(); auth_serial_login(); continue; }
        enum shell_action action=shell_execute(line,out,sizeof out);
        if (action==SHELL_CLEAR) printf("\033[2J\033[H");
        else if (action==SHELL_REBOOT || action==SHELL_OFF) {
            shell_power(action==SHELL_REBOOT);
            printf("Power action unavailable\n");
        } else printf("%s\n",out);
    }
}
