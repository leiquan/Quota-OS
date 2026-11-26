#include "quotaos.h"

/* Initial freestanding C subset: int main(void) { puts("text"); return 0; }.
 * The generated binary uses the EL0 console and exit calls. */
static union {
    uint32_t instructions[128];
    char bytes[512];
} program;
static unsigned program_size;

static int take(const char **source, const char *token)
{
    const char *p = *source;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    while (*token)
        if (*p++ != *token++) return 0;
    *source = p;
    return 1;
}

int c_demo_compile(const char *s)
{
    program_size = 0;
    if (!s || !take(&s,"int") || !take(&s,"main") || !take(&s,"(") ||
        !take(&s,"void") || !take(&s,")") || !take(&s,"{") ||
        !take(&s,"puts") || !take(&s,"(") || !take(&s,"\""))
        return -1;
    char *literal = program.bytes + 28;
    unsigned n = 0;
    while (*s && *s != '"') {
        if (n >= 126 || *s < ' ' || *s > '~' || *s == '\\') return -1;
        literal[n++] = *s++;
    }
    if (*s != '"' || !take(&s,"\"") || !take(&s,")") || !take(&s,";") ||
        !take(&s,"return") || !take(&s,"0") || !take(&s,";") || !take(&s,"}") ||
        !take(&s,"") || *s)
        return -1;
    literal[n++] = '\n';
    program.instructions[0] = 0xd2800380; /* mov x0, #28 */
    program.instructions[1] = 0xd2800001 | (n << 5);
    program.instructions[2] = 0xd2800068; /* mov x8, #3: console */
    program.instructions[3] = 0xd4000001;
    program.instructions[4] = 0xd2800000; /* exit status 0 */
    program.instructions[5] = 0xd2800028; /* mov x8, #1: exit */
    program.instructions[6] = 0xd4000001;
    program_size = 28 + n;
    return 0;
}

int64_t c_demo_run(void)
{
    if (!program_size) return -1;
    return proc_spawn_console(program.bytes, program_size);
}

int c_demo_compiled(void) { return program_size != 0; }
