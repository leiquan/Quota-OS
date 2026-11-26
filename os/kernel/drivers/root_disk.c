#include "quotaos.h"
#include "root_disk.h"
#ifdef QUOTA_GUI_EXT4
#include "touch_ext4.h"
#endif
#include "ext4_jbd2.h"

enum {
    QUEUE = 8, SECTOR = 512, BLOCK = 4096, SERIAL_BYTES = 20,
    READ_ONLY = 1 << 5, FLUSH_FEATURE = 1 << 9,
    READ = 0, WRITE = 1, FLUSH = 4, GET_ID = 8,
};
struct root_desc { uint64_t addr; uint32_t length; uint16_t flags, next; };
static struct root_desc descriptors[QUEUE] __attribute__((aligned(16)));
static struct { uint16_t flags, index, ring[QUEUE]; } available __attribute__((aligned(4)));
static volatile struct {
    uint16_t flags, index;
    struct { uint32_t id, length; } entries[QUEUE];
} used __attribute__((aligned(4)));
static struct { uint32_t type, reserved; uint64_t sector; } request;
static uint8_t sector_data[BLOCK] __attribute__((aligned(64)));
static volatile uint8_t status;
static uint64_t base, capacity, last_period, cached;
static int initialized, failed, submitted, writable;

static void address(unsigned offset, uintptr_t pointer)
{
    mmio_wr(base + offset, (uint32_t)pointer);
    mmio_wr(base + offset + 4, pointer >> 32);
}

static int transfer(unsigned operation, uint64_t sector, unsigned bytes)
{
    if (failed) return -1;
    while (submitted && last_period == sched_period_now()) asm volatile("wfi");
    last_period = sched_period_now();
    submitted = 1;
    request.type = operation;
    request.sector = sector;
    status = 0xff;
    unsigned device_writes = operation == READ || operation == GET_ID;
    descriptors[0] = (struct root_desc){(uintptr_t)&request, sizeof request, 1, bytes ? 1 : 2};
    descriptors[1] = (struct root_desc){(uintptr_t)sector_data, bytes, device_writes ? 3 : 1, 2};
    descriptors[2] = (struct root_desc){(uintptr_t)&status, 1, 2, 0};
    uint16_t index = available.index;
    available.ring[index % QUEUE] = 0;
    asm volatile("dsb sy" ::: "memory");
    available.index = index + 1;
    asm volatile("dsb sy" ::: "memory");
#ifdef QUOTA_GUI_EXT4
    uint64_t trace_begin = ticks_now();
#endif
    mmio_wr(base + 0x50, 0);
    uint64_t deadline = ticks_now() + us_to_ticks(1000000);
    while (used.index == index) {
        if (ticks_now() >= deadline) { failed = 1; return -1; }
        asm volatile("wfi");
    }
    asm volatile("dsb sy" ::: "memory");
    if (used.index != (uint16_t)(index + 1) || used.entries[index % QUEUE].id ||
        used.entries[index % QUEUE].length != (device_writes ? bytes + 1 : 1) || status) {
        failed = 1;
        return -1;
    }
    mmio_wr(base + 0x64, mmio_rd(base + 0x60));
#ifdef QUOTA_GUI_EXT4
    touch_ext4_record_io(operation, bytes, trace_begin, ticks_now());
#endif
    return 0;
}

static int fetch_sector(uint64_t sector)
{
    if (failed || sector >= capacity) return -1;
    if (cached == sector) return 0;
    if (transfer(READ, sector, SECTOR)) return -1;
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

static struct ext4_journal journal;

static int read_block(uint32_t block, void *output)
{
    uint64_t sector = (uint64_t)block * (BLOCK / SECTOR);
    if (sector + BLOCK / SECTOR > capacity) return -1;
    cached = UINT64_MAX;
    if (transfer(READ, sector, BLOCK)) return -1;
    memcpy(output, sector_data, BLOCK);
    return 0;
}

static int write_block(uint32_t block, const void *input)
{
    uint64_t sector = (uint64_t)block * (BLOCK / SECTOR);
    if (!writable || failed || sector + BLOCK / SECTOR > capacity) return -1;
    cached = UINT64_MAX;
    memcpy(sector_data, input, BLOCK);
    return transfer(WRITE, sector, BLOCK);
}

static int flush_disk(void)
{
    return transfer(FLUSH, 0, 0);
}

static enum ext4_cut_point armed_cut;
static const char *const cut_names[] = {NULL, "data", "log", "commit", "checkpoint"};

int root_disk_write_enabled(void)
{
#ifdef QUOTA_GUI_EXT4
    /* The graphical launcher reserves this transport and assigns root explicitly.
     * Keep the existing boot arguments for the desktop and frame probes. */
    return 1;
#else
    const char *args = dtb_bootargs();
    return !strcmp(args, ROOT_RW_BOOTARGS) || !strcmp(args, ROOT_CRASH_BOOTARGS);
#endif
}

int root_disk_arm_cut(const char *name)
{
    if (strcmp(dtb_bootargs(), ROOT_CRASH_BOOTARGS)) return -1;
    for (unsigned i = 1; i < sizeof cut_names / sizeof cut_names[0]; ++i) {
        if (!strcmp(name, cut_names[i])) {
            armed_cut = i;
            return 0;
        }
    }
    return -1;
}

static int crash_cut(enum ext4_cut_point point)
{
    if (point != armed_cut) return 0;
    /* Persist the single home-block write before pausing at the partial checkpoint. */
    if (point == EXT4_CUT_CHECKPOINT && flush_disk()) return -1;
    printf("[ext4-cut] %s\n", cut_names[point]);
    for (;;) asm volatile("wfi");
}

static const struct ext4_block_io root_io = {read_block, write_block, flush_disk, crash_cut};

int root_disk_mount(struct ext4_volume *volume)
{
    if (failed || sched_current()->tier != TIER3) return EXT4_IO;
    if (!initialized) {
        int want_write = root_disk_write_enabled();
        uint64_t nodes[32];
        int count = dtb_find_bases("virtio,mmio", nodes, 32);
        uint64_t device = 0;
        for (int i = 0; i < count; ++i) {
            uint64_t node = nodes[i];
            if (want_write && node != ROOT_RW_MMIO) continue;
            if (mmio_rd(node + 8) != 2 || mmio_rd(node + 4) != 2) continue;
            mmio_wr(node + 0x14, 0);
            uint32_t features = mmio_rd(node + 0x10);
            if (want_write) {
                if ((features & READ_ONLY) || !(features & FLUSH_FEATURE))
                    return EXT4_UNSUPPORTED;
            } else if (!(features & READ_ONLY)) continue;
            if (device) return EXT4_UNSUPPORTED;
            device = node;
        }
        if (!device) return EXT4_NOT_FOUND;
        base = device;
        cached = UINT64_MAX;
        mmio_wr(base + 0x70, 0);
        mmio_wr(base + 0x70, 3);
        mmio_wr(base + 0x24, 0);
        mmio_wr(base + 0x20, want_write ? FLUSH_FEATURE : READ_ONLY);
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
        if (want_write) {
            if (transfer(GET_ID, 0, SERIAL_BYTES)) return EXT4_IO;
            sector_data[SERIAL_BYTES] = 0;
            if (strcmp((const char *)sector_data, "QUOTA-ROOT")) {
                failed = 1;
                return EXT4_UNSUPPORTED;
            }
            writable = 1;
        }
        if (writable) {
            uint64_t blocks = capacity / (BLOCK / SECTOR);
            int rc = ext4_journal_open(&journal, &root_io,
                                       blocks > UINT32_MAX ? UINT32_MAX : blocks);
            if (rc != EXT4_OK && rc != EXT4_NOT_FOUND) {
                failed = 1;
                return rc;
            }
        }
        initialized = 1;
    }
    int rc = ext4_mount_ro(volume, read_root_disk, NULL, capacity * SECTOR);
    if (!rc && writable && !journal.io && !volume->overwrite_supported) {
        memset(volume, 0, sizeof *volume);
        return EXT4_UNSUPPORTED;
    }
    return rc;
}

int root_disk_overwrite(const struct ext4_volume *volume, const char *name,
                        const char *data, size_t bytes)
{
    if (!writable) return EXT4_READ_ONLY;
    if (failed) return EXT4_IO;
    if (volume->read != read_root_disk || sched_current()->tier != TIER3)
        return EXT4_UNSUPPORTED;

    if (journal.io) return ext4_journal_change(&journal, name, data, bytes, 0);

    uint64_t location;
    int rc = ext4_overwrite_location(volume, name, bytes, &location);
    if (rc) return rc;
    uint64_t sector = location / SECTOR;
    cached = UINT64_MAX;
    if (transfer(READ, sector, BLOCK)) return EXT4_IO;
    memcpy(sector_data, data, bytes);
    if (transfer(WRITE, sector, BLOCK) || transfer(FLUSH, 0, 0)) return EXT4_IO;
    return EXT4_OK;
}

int root_disk_remove(const struct ext4_volume *volume, const char *name)
{
    if (!writable) return EXT4_READ_ONLY;
    if (failed) return EXT4_IO;
    if (!journal.io || volume->read != read_root_disk || sched_current()->tier != TIER3)
        return EXT4_UNSUPPORTED;
    return ext4_journal_change(&journal, name, NULL, 0, 1);
}
