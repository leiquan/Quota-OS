#include "quotaos.h"
#include "root_disk.h"

enum { QUEUE = 8, SECTOR = 512, READ_ONLY = 1 << 5 };
struct root_desc { uint64_t addr; uint32_t length; uint16_t flags, next; };
static struct root_desc descriptors[QUEUE] __attribute__((aligned(16)));
static struct { uint16_t flags, index, ring[QUEUE]; } available __attribute__((aligned(4)));
static volatile struct {
    uint16_t flags, index;
    struct { uint32_t id, length; } entries[QUEUE];
} used __attribute__((aligned(4)));
static struct { uint32_t type, reserved; uint64_t sector; } request;
static uint8_t sector_data[SECTOR] __attribute__((aligned(64)));
static volatile uint8_t status;
static uint64_t base, capacity, last_period, cached;
static int initialized, failed, submitted;

static void address(unsigned offset, uintptr_t pointer)
{
    mmio_wr(base + offset, (uint32_t)pointer);
    mmio_wr(base + offset + 4, pointer >> 32);
}

static int fetch_sector(uint64_t sector)
{
    if (failed || sector >= capacity) return -1;
    if (cached == sector) return 0;
    while (submitted && last_period == sched_period_now()) asm volatile("wfi");
    last_period = sched_period_now();
    submitted = 1;
    request.sector = sector;
    status = 0xff;
    descriptors[0] = (struct root_desc){(uintptr_t)&request, sizeof request, 1, 1};
    descriptors[1] = (struct root_desc){(uintptr_t)sector_data, SECTOR, 3, 2};
    descriptors[2] = (struct root_desc){(uintptr_t)&status, 1, 2, 0};
    uint16_t index = available.index;
    available.ring[index % QUEUE] = 0;
    asm volatile("dsb sy" ::: "memory");
    available.index = index + 1;
    asm volatile("dsb sy" ::: "memory");
    mmio_wr(base + 0x50, 0);
    uint64_t deadline = ticks_now() + us_to_ticks(1000000);
    while (used.index == index) {
        if (ticks_now() >= deadline) { failed = 1; return -1; }
        asm volatile("wfi");
    }
    asm volatile("dsb sy" ::: "memory");
    if (used.index != (uint16_t)(index + 1) || used.entries[index % QUEUE].id ||
        used.entries[index % QUEUE].length != SECTOR + 1 || status) {
        failed = 1;
        return -1;
    }
    mmio_wr(base + 0x64, mmio_rd(base + 0x60));
    cached = sector;
    return 0;
}

static int read_root_disk(void *context, uint64_t offset, void *output, size_t bytes)
{
    (void)context;
    uint8_t *dst = output;
    if (sched_current()->tier != TIER3) return -1;
    if (offset > capacity * SECTOR || bytes > capacity * SECTOR - offset) return -1;
    while (bytes) {
        if (fetch_sector(offset / SECTOR)) return -1;
        unsigned start = offset % SECTOR;
        size_t length = SECTOR - start;
        if (length > bytes) length = bytes;
        memcpy(dst, sector_data + start, length);
        dst += length;
        offset += length;
        bytes -= length;
    }
    return 0;
}

int root_disk_mount(struct ext4_volume *volume)
{
    if (!initialized) {
        uint64_t nodes[32];
        int count = dtb_find_bases("virtio,mmio", nodes, 32);
        uint64_t device = 0;
        for (int i = 0; i < count; ++i) {
            uint64_t node = nodes[i];
            if (mmio_rd(node + 8) != 2 || mmio_rd(node + 4) != 2) continue;
            mmio_wr(node + 0x14, 0);
            if (!(mmio_rd(node + 0x10) & READ_ONLY)) continue;
            if (device) return EXT4_UNSUPPORTED;
            device = node;
        }
        if (!device) return EXT4_NOT_FOUND;
        base = device;
        cached = UINT64_MAX;
        mmio_wr(base + 0x70, 0);
        mmio_wr(base + 0x70, 3);
        mmio_wr(base + 0x24, 0);
        mmio_wr(base + 0x20, READ_ONLY);
        mmio_wr(base + 0x14, 1);
        if (!(mmio_rd(base + 0x10) & 1)) return EXT4_UNSUPPORTED;
        mmio_wr(base + 0x24, 1);
        mmio_wr(base + 0x20, 1);
        mmio_wr(base + 0x70, 11);
        if (!(mmio_rd(base + 0x70) & 8)) return EXT4_UNSUPPORTED;
        mmio_wr(base + 0x30, 0);
        if (mmio_rd(base + 0x34) < QUEUE) return EXT4_UNSUPPORTED;
        mmio_wr(base + 0x38, QUEUE);
        address(0x80, (uintptr_t)descriptors);
        address(0x90, (uintptr_t)&available);
        address(0xa0, (uintptr_t)&used);
        available.flags = 1;
        mmio_wr(base + 0x44, 1);
        mmio_wr(base + 0x70, 15);
        capacity = (uint64_t)mmio_rd(base + 0x100) | (uint64_t)mmio_rd(base + 0x104) << 32;
        if (!capacity || capacity > UINT64_MAX / SECTOR) return EXT4_CORRUPT;
        initialized = 1;
    }
    return ext4_mount_ro(volume, read_root_disk, NULL, capacity * SECTOR);
}
