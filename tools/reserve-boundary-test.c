#include "quotaos.h"
#include "os/kernel/resource.c"
uint64_t us_to_ticks(uint64_t us) { return us * 64; }
uint64_t color_padding_kb(void) { return 0; }
int main(void) {
    ram_kb = 131072;
    apps[0].alive = 1;
    apps[0].cpu_min_us = 8000;
    struct app_manifest incoming = {"new", "job", 16, 16, 1, 1};
    if (resource_period_reservations_us(600) != 8000) return 1;
    if (resource_kernel_reserve_us() != 420 || resource_app_cpu_capacity_us() != 8000) return 2;
    if (resource_check(&incoming) != ADMIT_OVERSUB || !apps[0].alive || apps[0].cpu_min_us != 8000) return 3;
    resource_release(&apps[0]);
    if (resource_period_reservations_us(600) || resource_kernel_reserve_us() != 600) return 4;
    if (resource_check(&incoming) != ADMIT_OK) return 5;
    printf("PASS 8000 us held; kernel 350 -> 420 of 600; Tier 3 pool 0; new 1 us denied; after release kernel 600, admission open\n");
    return 0;
}
char __image_start[16], __image_end[16];
const char *dtb_bootargs(void) { return ""; }
