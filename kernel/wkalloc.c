/*
 * Kernel side of WTF::OSAllocator (reserve / commit, guard pages,
 * executable pages). WebKit is not linked. These calls only record
 * what this CPU can and cannot do.
 *
 * Executable pages and guard pages need an MMU. It is off. Those
 * requests are refused. Ordinary bytes come from the 16 MB system
 * reserve, under the page pool, so they are not Tier-1 colored pages
 * and they are not lent to an application admission slot.
 */
#include "quotaos.h"

int wk_reserve(uint64_t bytes, int executable, unsigned guard_pages, uint64_t *pa)
{
    if (executable) {
        printf("[wk] refuse executable %lu bytes (MMU off)\n", bytes);
        return -1;
    }
    if (guard_pages) {
        printf("[wk] refuse %u guard pages (MMU off)\n", guard_pages);
        return -1;
    }
    if (kernel_reserve_alloc(bytes, 4096, pa) != 0) {
        printf("[wk] system reserve has no room for %lu bytes\n", bytes);
        return -1;
    }
    printf("[wk] reserved %lu bytes at 0x%lx in system reserve\n", bytes, *pa);
    return 0;
}
