#ifndef QUOTA_SYSTEM_STATUS_H
#define QUOTA_SYSTEM_STATUS_H
#include <stdint.h>

/* Numeric strip of each of the five native rows. The edition supplies origin. */
enum { STATUS_WIDTH = 192, STATUS_HEIGHT = 10, STATUS_STEP = 18, STATUS_ROWS = 5 };
struct system_status_sample {
    uint64_t period, reserved_kib, total_kib, t1_misses, t2_short;
    unsigned busy_percent, tasks;
};
void system_status_publish(uint32_t *surface, unsigned stride, unsigned x, unsigned y);
#endif
