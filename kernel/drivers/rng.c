#include "quotaos.h"

/* One outstanding device-write buffer, shared by startup auxv and getrandom. */
static uint64_t rng_base;
static uint8_t random_bytes[256] __attribute__((aligned(64)));
static struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
} descriptor __attribute__((aligned(16)));
static volatile struct {
    uint16_t flags, idx, ring[1], event;
} available __attribute__((aligned(4)));
static volatile struct {
    uint16_t flags, idx;
    struct { uint32_t id, len; } ring[1];
    uint16_t event;
} used __attribute__((aligned(4)));
static uint16_t seen;
static unsigned remaining;

static void set_address(unsigned offset, uintptr_t address)
{
    mmio_wr(rng_base + offset, (uint32_t)address);
    mmio_wr(rng_base + offset + 4, address >> 32);
}

static void request_bytes(void)
{
    asm volatile("dmb oshst" ::: "memory");
    available.idx++;
    asm volatile("dsb sy" ::: "memory");
    mmio_wr(rng_base + 0x50, 0);
}

void rng_init(void)
{
    uint64_t nodes[32];
    int count = dtb_find_bases("virtio,mmio", nodes, 32);
    for (int i = 0; i < count; ++i) {
        uint64_t addr = nodes[i];
        if (mmio_rd(addr + 8) != 4 || mmio_rd(addr + 4) != 2) continue;
        mmio_wr(addr + 0x70, 0);
        mmio_wr(addr + 0x70, 3);
        mmio_wr(addr + 0x14, 1);
        if (!(mmio_rd(addr + 0x10) & 1)) continue;
        mmio_wr(addr + 0x24, 0);
        mmio_wr(addr + 0x20, 0);
        mmio_wr(addr + 0x24, 1);
        mmio_wr(addr + 0x20, 1);
        mmio_wr(addr + 0x70, 11);
        if (!(mmio_rd(addr + 0x70) & 8)) continue;
        mmio_wr(addr + 0x30, 0);
        if (!mmio_rd(addr + 0x34)) continue;
        mmio_wr(addr + 0x38, 1);
        rng_base = addr;
        descriptor.addr = (uintptr_t)random_bytes;
        descriptor.len = sizeof random_bytes;
        descriptor.flags = 2;
        available.flags = 1;
        set_address(0x80, (uintptr_t)&descriptor);
        set_address(0x90, (uintptr_t)&available);
        set_address(0xa0, (uintptr_t)&used);
        mmio_wr(addr + 0x44, 1);
        mmio_wr(addr + 0x70, 15);
        request_bytes();
        return;
    }
}

int rng_read(void *dst, unsigned bytes)
{
    if (!rng_base) return -19;
    if (!bytes) return 0;
    uint64_t irq = irq_save();
    if (!remaining && used.idx != seen) {
        asm volatile("dmb oshld" ::: "memory");
        seen = used.idx;
        remaining = used.ring[0].len;
        if (used.ring[0].id || !remaining || remaining > sizeof random_bytes) {
            remaining = 0;
            request_bytes();
        }
    }
    if (!remaining) {
        irq_restore(irq);
        return -11;
    }
    unsigned n = bytes < remaining ? bytes : remaining;
    remaining -= n;
    memcpy(dst, random_bytes + remaining, n);
    memset(random_bytes + remaining, 0, n);
    if (!remaining) request_bytes();
    irq_restore(irq);
    return n;
}
