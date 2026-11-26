#include "quotaos.h"

#ifdef QUOTA_RUNTIME_VOLUME_BOOT
enum {
    PAGE_BYTES = 4096, SECTOR_BYTES = 512, QUEUE_SIZE = 4,
    CACHE_PAGES = 16, BATCH_PAGES = 4, INDEX_ENTRY_BYTES = 128, MAX_FILES = 1024,
};
struct volume_header { uint64_t magic; uint32_t version, count; uint64_t size; };
static uint8_t *cache[CACHE_PAGES];
static int cache_frame[CACHE_PAGES];
static unsigned allocated;
static struct { uint64_t offset; unsigned references; int valid; } cached[CACHE_PAGES];
static unsigned destinations[BATCH_PAGES], batch_pages, replace_from;
static uint64_t volume_size, payload_start;
static unsigned file_count;
struct volume_entry { char name[112]; uint64_t offset, size; };
_Static_assert(sizeof(struct volume_entry) == INDEX_ENTRY_BYTES, "volume entry size");
struct descriptor { uint64_t address; uint32_t size; uint16_t flags, next; };
static struct descriptor descriptors[QUEUE_SIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, index, ring[QUEUE_SIZE]; } available;
static volatile struct {
    uint16_t flags, index;
    struct { uint32_t id, size; } ring[QUEUE_SIZE];
} used;
static struct { uint32_t type, reserved; uint64_t sector; } request;
static volatile uint8_t status;
static uint8_t page[BATCH_PAGES * PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
static uint64_t device, capacity, last_period, started;
static uint16_t completed;
enum phase { OFFLINE, IDLE, QUEUED, READING, FAILED };
static enum phase state;
static int probing;
static struct running_app *runtime_budget;

static const struct app_manifest runtime_storage = {
    .name = "WAP runtime storage",
    .role = "Tier 3 shared readonly pages and VirtIO staging",
    .mem_min_kb = (CACHE_PAGES + BATCH_PAGES) * PAGE_BYTES / 1024,
    .mem_sugg_kb = (CACHE_PAGES + BATCH_PAGES) * PAGE_BYTES / 1024,
};

static void set_address(unsigned reg, uintptr_t address)
{
    mmio_wr(device + reg, (uint32_t)address);
    mmio_wr(device + reg + 4, address >> 32);
}

static void release_cache(void)
{
    while (allocated) {
        unsigned slot = --allocated;
        proc_runtime_cache_free(cache_frame[slot]);
        cache[slot] = NULL;
        cached[slot].valid = 0;
    }
    resource_release(runtime_budget);
    runtime_budget = NULL;
}

/* Called after the WAP consumer has stopped. In-flight DMA and borrowed
 * pages retain their charge until the storage service can safely detach. */
int runtime_volume_trim(void)
{
    if (!sched_current() || sched_current()->tier != TIER3 ||
        state != IDLE || !runtime_budget)
        return 0;
    for (unsigned i = 0; i < allocated; ++i)
        if (cached[i].references)
            return 0;
    mmio_wr(device + 0x70, 0);
    release_cache();
    memset(&available, 0, sizeof available);
    completed = 0;
    replace_from = 0;
    request.sector = 0;
    state = OFFLINE;
    printf("[runtime-volume] released %u cache pages after consumer exit\n", CACHE_PAGES);
    return 1;
}

void runtime_volume_init(void)
{
    if (state != OFFLINE)
        return;
    uint64_t nodes[32];
    int count = dtb_find_bases("virtio,mmio", nodes, 32);
    for (int i = 0; i < count; ++i) {
        uint64_t base = nodes[i];
        if (mmio_rd(base + 4) != 2 || mmio_rd(base + 8) != 2)
            continue;
        mmio_wr(base + 0x14, 0);
        if (!(mmio_rd(base + 0x10) & (1u << 5)))
            continue;
        device = base;
        break;
    }
    if (!device)
        return;

    runtime_budget = resource_commit(&runtime_storage);
    if (!runtime_budget) {
        printf("[runtime-volume] Tier 3 memory admission refused\n");
        return;
    }
    while (allocated < CACHE_PAGES) {
        int frame = proc_runtime_cache_alloc(&cache[allocated]);
        if (frame < 0) {
            release_cache();
            printf("[runtime-volume] Tier 3 frame allocation refused\n");
            return;
        }
        cache_frame[allocated++] = frame;
    }

    mmio_wr(device + 0x70, 0);
    mmio_wr(device + 0x70, 3);
    mmio_wr(device + 0x14, 1);
    if (!(mmio_rd(device + 0x10) & 1))
        goto fail;
    mmio_wr(device + 0x24, 0);
    mmio_wr(device + 0x20, 1u << 5);
    mmio_wr(device + 0x24, 1);
    mmio_wr(device + 0x20, 1);
    mmio_wr(device + 0x70, 11);
    if (!(mmio_rd(device + 0x70) & 8))
        goto fail;

    mmio_wr(device + 0x30, 0);
    if (mmio_rd(device + 0x34) < QUEUE_SIZE)
        goto fail;
    mmio_wr(device + 0x38, QUEUE_SIZE);
    set_address(0x80, (uintptr_t)descriptors);
    set_address(0x90, (uintptr_t)&available);
    set_address(0xa0, (uintptr_t)&used);
    /* The device is reset and cannot write queue state until DRIVER_OK. */
    used.index = 0;
    available.flags = 1;
    mmio_wr(device + 0x44, 1);
    mmio_wr(device + 0x70, 15);
    capacity = ((uint64_t)mmio_rd(device + 0x104) << 32) | mmio_rd(device + 0x100);
    if (capacity < PAGE_BYTES / SECTOR_BYTES)
        goto fail;
    last_period = (uint64_t)-1;
    probing = 1;
    batch_pages = 1;
    state = QUEUED;
    return;

fail:
    mmio_wr(device + 0x70, 128);
    state = FAILED;
    release_cache();
}

/* Called by the Tier-3 storage service. A pinned page cannot be recycled
 * until its last caller drops it. Pending requests are retried by the service. */
int runtime_volume_acquire(uint64_t offset, const uint8_t **data)
{
    if (!sched_current() || sched_current()->tier != TIER3 || state == OFFLINE ||
        state == FAILED || (offset & (PAGE_BYTES - 1)))
        return -1;
    if (probing)
        return 0;
    if (offset >= volume_size)
        return -1;
    for (unsigned i = 0; i < CACHE_PAGES; ++i) {
        if (!cached[i].valid || cached[i].offset != offset)
            continue;
        if (cached[i].references == (unsigned)-1)
            return -1;
        ++cached[i].references;
        *data = cache[i];
        return 1;
    }
    if (state != IDLE)
        return 0;
    batch_pages = 0;
    unsigned selected = 0;
    for (unsigned n = 0; n < BATCH_PAGES && n < (volume_size - offset) / PAGE_BYTES; ++n) {
        uint64_t next = offset + n * PAGE_BYTES;
        int present = 0;
        for (unsigned i = 0; i < CACHE_PAGES; ++i)
            if (cached[i].valid && cached[i].offset == next)
                present = 1;
        if (present)
            break;
        unsigned i = 0;
        while (i < CACHE_PAGES) {
            unsigned slot = (replace_from + i) % CACHE_PAGES;
            if (!cached[slot].references && !(selected & (1u << slot))) {
                cached[slot].valid = 0;
                cached[slot].offset = next;
                destinations[batch_pages++] = slot;
                selected |= 1u << slot;
                replace_from = (slot + 1) % CACHE_PAGES;
                break;
            }
            ++i;
        }
        if (i == CACHE_PAGES)
            break;
    }
    if (!batch_pages)
        return -1;
    request.sector = offset / SECTOR_BYTES;
    state = QUEUED;
    return 0;
}

void runtime_volume_release(const uint8_t *data)
{
    for (unsigned i = 0; i < CACHE_PAGES; ++i) {
        if (data && data == cache[i] && cached[i].references) {
            --cached[i].references;
            return;
        }
    }
}

/* A bounded read touches at most two cache pages. Retry the whole read
 * after a miss. The destination is only published after both pages arrive. */
static int read_volume(uint64_t offset, void *destination, unsigned length)
{
    if (state == OFFLINE || state == FAILED || !length || length > PAGE_BYTES)
        return -1;
    if (probing)
        return 0;
    if (offset >= volume_size || length > volume_size - offset)
        return -1;

    unsigned within = offset & (PAGE_BYTES - 1);
    uint64_t aligned = offset - within;
    const uint8_t *first, *second = NULL;
    int result = runtime_volume_acquire(aligned, &first);
    if (result != 1)
        return result;
    unsigned head = PAGE_BYTES - within;
    if (head < length) {
        result = runtime_volume_acquire(aligned + PAGE_BYTES, &second);
        if (result == 1) {
            memcpy(destination, first + within, head);
            memcpy((uint8_t *)destination + head, second, length - head);
            runtime_volume_release(second);
        }
    } else {
        memcpy(destination, first + within, length);
    }
    runtime_volume_release(first);
    return result;
}

int runtime_volume_read_file(const struct runtime_volume_file *file, uint64_t position,
                             void *destination, unsigned length)
{
    if (position > file->size || length > file->size - position ||
        file->offset > volume_size || file->size > volume_size - file->offset)
        return -1;
    return read_volume(file->offset + position, destination, length);
}

int runtime_volume_fill_segment_page(const struct runtime_volume_file *file,
                                     uint64_t position, unsigned in_page,
                                     unsigned bytes, uint8_t *page)
{
    if (in_page >= PAGE_BYTES || bytes > PAGE_BYTES - in_page ||
        position > file->size || bytes > file->size - position)
        return -1;
    if (bytes) {
        int result = runtime_volume_read_file(file, position, page + in_page, bytes);
        if (result != 1)
            return result;
    }
    memset(page, 0, in_page);
    memset(page + in_page + bytes, 0, PAGE_BYTES - in_page - bytes);
    return 1;
}

static int read_index_entry(unsigned index, struct volume_entry *entry)
{
    if (state == OFFLINE || state == FAILED || index >= file_count)
        return -1;
    uint64_t position = sizeof(struct volume_header) + (uint64_t)index * INDEX_ENTRY_BYTES;
    int result = read_volume(position, entry, sizeof *entry);
    if (result != 1)
        return result;

    unsigned n = 0;
    while (n < sizeof entry->name && entry->name[n])
        ++n;
    if (!n || n == sizeof entry->name ||
        (entry->offset & (PAGE_BYTES - 1)) || entry->offset < payload_start ||
        entry->offset >= volume_size || !entry->size ||
        entry->size > volume_size - entry->offset)
        return -1;
    return 1;
}

/* Binary search the sorted index without retaining index pages across calls. */
int runtime_volume_find(const char *name, struct runtime_volume_file *file)
{
    if (state == OFFLINE || state == FAILED)
        return -1;
    if (probing)
        return 0;
    unsigned low = 0, high = file_count;
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        struct volume_entry entry;
        int result = read_index_entry(mid, &entry);
        if (result != 1)
            return result;
        int order = strcmp(name, entry.name);
        if (order < 0) {
            high = mid;
        } else if (order > 0) {
            low = mid + 1;
        } else {
            *file = (struct runtime_volume_file){entry.offset, entry.size};
            return 1;
        }
    }
    return -1;
}

#ifdef QUOTA_NODE_WEB
_Static_assert(RUNTIME_VOLUME_MAX_FILES == MAX_FILES, "volume file limit");
_Static_assert(RUNTIME_VOLUME_NAME_BYTES == sizeof ((struct volume_entry *)0)->name,
               "volume name capacity");

int runtime_volume_io_pending(void)
{
    return state == QUEUED || state == READING;
}

unsigned runtime_volume_index_count(void)
{
    return state == OFFLINE || state == FAILED || probing ? 0 : file_count;
}

int runtime_volume_index_entry(unsigned index, char name[RUNTIME_VOLUME_NAME_BYTES],
                               struct runtime_volume_file *file)
{
    struct volume_entry entry;
    int result = read_index_entry(index, &entry);
    if (result == 1) {
        memcpy(name, entry.name, sizeof entry.name);
        *file = (struct runtime_volume_file){entry.offset, entry.size};
    }
    return result;
}
#endif

void runtime_volume_poll(void)
{
    if (sched_current()->tier != TIER3)
        return;
    if (state == READING) {
        if (used.index == completed) {
            if (now_us() - started > 30000000)
                state = FAILED;
            return;
        }
        asm volatile("dmb oshld" ::: "memory");
        unsigned slot = completed++ % QUEUE_SIZE;
        if (status || used.ring[slot].id || used.ring[slot].size != batch_pages * PAGE_BYTES + 1) {
            state = FAILED;
            return;
        }
        mmio_wr(device + 0x64, 1);
#ifdef QUOTA_WEB_COLD_TRACE
        sched_cold_disk(2, request.sector * SECTOR_BYTES);
#endif
        if (probing) {
            struct volume_header header;
            memcpy(&header, page, sizeof header);
            if (header.magic != 0x31304c4f56545251ull || header.version != 1 ||
                !header.count || header.count > MAX_FILES ||
                header.size < sizeof header + (uint64_t)header.count * INDEX_ENTRY_BYTES ||
                (header.size & (PAGE_BYTES - 1)) || header.size / SECTOR_BYTES > capacity) {
                state = FAILED;
                return;
            }
            volume_size = header.size;
            file_count = header.count;
            payload_start = (sizeof header + (uint64_t)file_count * INDEX_ENTRY_BYTES
                             + PAGE_BYTES - 1) & ~(uint64_t)(PAGE_BYTES - 1);
            probing = 0;
            printf("[runtime-volume] index ready: %u files, %lu bytes, %u cache pages, Tier 3 charge=%u KiB\n",
                   header.count, volume_size, CACHE_PAGES, runtime_budget->mem_granted_kb);
        } else {
            for (unsigned i = 0; i < batch_pages; ++i) {
                unsigned slot = destinations[i];
                memcpy(cache[slot], page + i * PAGE_BYTES, PAGE_BYTES);
                cached[slot].valid = 1;
            }
        }
        state = IDLE;
    }
    if (state != QUEUED || last_period == sched_period_now())
        return;
    last_period = sched_period_now();
    status = 255;
    descriptors[0] = (struct descriptor){(uintptr_t)&request, sizeof request, 1, 1};
    descriptors[1] = (struct descriptor){(uintptr_t)page, batch_pages * PAGE_BYTES, 3, 2};
    descriptors[2] = (struct descriptor){(uintptr_t)&status, 1, 2, 0};
    available.ring[available.index % QUEUE_SIZE] = 0;
    asm volatile("dmb oshst" ::: "memory");
    ++available.index;
    asm volatile("dsb sy" ::: "memory");
    started = now_us();
    state = READING;
#ifdef QUOTA_WEB_COLD_TRACE
    sched_cold_disk(1, request.sector * SECTOR_BYTES);
#endif
    mmio_wr(device + 0x50, 0);
}
#endif
