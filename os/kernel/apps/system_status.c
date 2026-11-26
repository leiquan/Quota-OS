#include "quotaos.h"
#include "auth.h"
#include "system_status.h"

/* Three columns and five rows per glyph, drawn at 2x with a fixed advance. */
static const uint16_t digits[] = {
    075557, 026227, 071747, 071717, 055711,
    074717, 074757, 071111, 075757, 075717
};

enum { CELLS = STATUS_WIDTH / 8, EMPTY = 10, SLASH = 11 };
static uint8_t previous[STATUS_ROWS][CELLS];
static int visible;

/* Layout first, then update only changed cells in the reserved strip. */
static void number(uint8_t cells[CELLS], unsigned *right, uint64_t n)
{
    do {
        cells[--*right] = n % 10;
        n /= 10;
    } while (n && *right);
}

static void paint_cell(uint32_t *dst, unsigned stride, unsigned digit)
{
    uint16_t bits = digit < 10 ? digits[digit] : digit == SLASH ? 011244 : 0;
    for (unsigned row = 0; row < 5; ++row) {
        uint32_t *line = dst + row * 2 * stride;
        for (unsigned col = 0; col < 4; ++col) {
            uint32_t color = col < 3 && (bits & (1u << (14 - row * 3 - col))) ?
                             0xd8f7ef : 0x182c3e;
            uint64_t pair = (uint64_t)color << 32 | color;
            __builtin_memcpy(line + col * 2, &pair, sizeof pair);
            __builtin_memcpy(line + stride + col * 2, &pair, sizeof pair);
        }
    }
}

void system_status_publish(uint32_t *surface, unsigned stride, unsigned x, unsigned y)
{
    if (!surface || auth_state() != AUTH_OPEN) {
        visible = 0;
        return;
    }
    struct sched_system_info cpu;
    sched_read_system_info(&cpu);
    uint64_t busy = cpu.cpu_us[0] + cpu.cpu_us[1] + cpu.cpu_us[2] + cpu.kernel_us;
    uint32_t spare_cpu;
    uint64_t spare_ram;
    resource_app_available(&spare_cpu, &spare_ram);
    struct system_status_sample sample = {
        .period = cpu.period, .total_kib = resource_ram_kb(),
        .reserved_kib = resource_ram_kb() - spare_ram,
        .t1_misses = cpu.tier1_misses, .t2_short = cpu.tier2_short,
        .busy_percent = (unsigned)(100 * busy / PERIOD_US)
    };
    for (int i = 0; i < MAX_TASKS; ++i) {
        struct task *task = sched_task_at(i);
        if (task && task->state != TASK_DEAD) ++sample.tasks;
    }
    uint64_t values[STATUS_ROWS] = {sample.busy_percent, sample.total_kib, sample.tasks,
                                     sample.t1_misses, sample.t2_short};
    for (unsigned row = 0; row < STATUS_ROWS; ++row) {
        uint32_t *dst = surface + (y + row * STATUS_STEP) * stride + x;
        uint8_t cells[CELLS];
        memset(cells, EMPTY, sizeof cells);
        unsigned right = CELLS;
        number(cells, &right, values[row]);
        if (row == 1) {
            cells[--right] = SLASH;
            number(cells, &right, sample.reserved_kib);
        }
        for (unsigned i = 0; i < CELLS; ++i) {
            if (!visible && cells[i] == EMPTY) {
                previous[row][i] = EMPTY;
                continue;
            }
            if (visible && previous[row][i] == cells[i]) continue;
            paint_cell(dst + i * 8, stride, cells[i]);
            previous[row][i] = cells[i];
        }
    }
    visible = 1;
    asm volatile("dsb st" ::: "memory");
}
