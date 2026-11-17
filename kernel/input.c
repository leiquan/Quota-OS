/*
 * virtio-mmio input (device id 18), legacy (version 1) or modern (version 2).
 * Events are polled. They are not WebCore::PlatformKeyboardEvent values.
 */
#include "quotaos.h"

#define VIRTIO_ID_INPUT 18u
#define VIRTQ_DESC_F_WRITE 2u
#define EV_KEY 1u

struct vdesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct evbuf {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

static uint64_t dev_base;
static uint32_t qsz;
static uint32_t qmask;
static uint8_t *vring;
static uint32_t used_off;
static struct evbuf *events;
static uint16_t last_used;
static uint16_t avail_count;
static int ready;

static void dsb(void)
{
    asm volatile("dsb sy" ::: "memory");
}

static unsigned vring_used_off(unsigned num, unsigned align)
{
    unsigned bytes = 16u * num + 2u * (3u + num);
    return (bytes + align - 1u) & ~(align - 1u);
}

static int setup_legacy(void)
{
    mmio_wr(dev_base + 0x28, 4096); /* GuestPageSize */
    mmio_wr(dev_base + 0x30, 0);    /* QueueSel */
    uint32_t max = mmio_rd(dev_base + 0x34);
    if (max < 2) {
        printf("[input] legacy QueueNumMax %u\n", max);
        return -1;
    }
    qsz = 8;
    if (qsz > max)
        qsz = max;
    qmask = qsz - 1;
    mmio_wr(dev_base + 0x38, qsz);
    mmio_wr(dev_base + 0x3c, 4096); /* QueueAlign */
    uint64_t pfn = (uint64_t)vring / 4096u;
    mmio_wr(dev_base + 0x40, (uint32_t)pfn);
    return 0;
}

static int setup_modern(void)
{
    mmio_wr(dev_base + 0x14, 1);
    uint32_t hi = mmio_rd(dev_base + 0x10);
    mmio_wr(dev_base + 0x24, 1);
    mmio_wr(dev_base + 0x20, hi & 1u); /* VIRTIO_F_VERSION_1 */
    mmio_wr(dev_base + 0x14, 0);
    mmio_wr(dev_base + 0x24, 0);
    mmio_wr(dev_base + 0x20, 0);
    mmio_wr(dev_base + 0x70, 1u | 2u | 8u); /* ACK|DRIVER|FEATURES_OK */
    dsb();
    if (mmio_rd(dev_base + 0x70) & 0x80u) {
        printf("[input] modern feature negotiation failed\n");
        return -1;
    }
    mmio_wr(dev_base + 0x30, 0);
    uint32_t max = mmio_rd(dev_base + 0x34);
    if (max < 2)
        return -1;
    qsz = 8;
    if (qsz > max)
        qsz = max;
    qmask = qsz - 1;
    mmio_wr(dev_base + 0x38, qsz);
    uint64_t desc = (uint64_t)vring;
    uint64_t avail = desc + 16u * qsz;
    uint64_t used = desc + used_off;
    mmio_wr(dev_base + 0x80, (uint32_t)desc);
    mmio_wr(dev_base + 0x84, (uint32_t)(desc >> 32));
    mmio_wr(dev_base + 0x90, (uint32_t)avail);
    mmio_wr(dev_base + 0x94, (uint32_t)(avail >> 32));
    mmio_wr(dev_base + 0xa0, (uint32_t)used);
    mmio_wr(dev_base + 0xa4, (uint32_t)(used >> 32));
    mmio_wr(dev_base + 0x44, 1); /* QueueReady */
    return 0;
}

static void publish_all(void)
{
    struct vdesc *desc = (struct vdesc *)vring;
    uint16_t *avail = (uint16_t *)(vring + 16u * qsz);
    for (uint32_t i = 0; i < qsz; i++) {
        desc[i].addr = (uint64_t)&events[i];
        desc[i].len = sizeof events[i];
        desc[i].flags = VIRTQ_DESC_F_WRITE;
        desc[i].next = 0;
        avail[2 + i] = (uint16_t)i;
    }
    avail[0] = 1; /* NO_INTERRUPT */
    avail_count = (uint16_t)qsz;
    dsb();
    avail[1] = avail_count;
    dsb();
}

static void recycle(uint16_t id)
{
    uint16_t *avail = (uint16_t *)(vring + 16u * qsz);
    avail[2 + (avail_count & qmask)] = id;
    avail_count++;
    dsb();
    avail[1] = avail_count;
    dsb();
    mmio_wr(dev_base + 0x50, 0);
}

int input_init(void)
{
    uint64_t bases[32];
    uint64_t ring_pa, ev_pa;
    int n = dtb_find_bases("virtio,mmio", bases, 32);
    printf("[input] virtio-mmio nodes %d\n", n);
    for (int i = 0; i < n; i++) {
        if (mmio_rd(bases[i]) != 0x74726976u)
            continue;
        uint32_t id = mmio_rd(bases[i] + 8);
        if (id != VIRTIO_ID_INPUT)
            continue;
        dev_base = bases[i];
        break;
    }
    if (!dev_base) {
        printf("[input] no virtio-input (id 18)\n");
        return -1;
    }
    uint32_t version = mmio_rd(dev_base + 4);
    printf("[input] base 0x%lx version %u\n", dev_base, version);
    if (kernel_reserve_alloc(8192, 4096, &ring_pa) != 0 ||
        kernel_reserve_alloc(4096, 4096, &ev_pa) != 0) {
        printf("[input] system reserve has no room\n");
        return -1;
    }
    vring = (uint8_t *)ring_pa;
    events = (struct evbuf *)ev_pa;
    memset(vring, 0, 8192);
    memset(events, 0, 4096);
    used_off = vring_used_off(8, 4096);
    mmio_wr(dev_base + 0x70, 0);
    dsb();
    mmio_wr(dev_base + 0x70, 1);          /* ACKNOWLEDGE */
    mmio_wr(dev_base + 0x70, 1u | 2u);    /* DRIVER */
    int rc = (version == 2) ? setup_modern() : (version == 1) ? setup_legacy() : -1;
    if (rc != 0) {
        printf("[input] queue setup failed (version %u)\n", version);
        return -1;
    }
    if (version == 1)
        used_off = vring_used_off(qsz, 4096);
    publish_all();
    uint32_t st = mmio_rd(dev_base + 0x70);
    st |= 4u; /* DRIVER_OK */
    mmio_wr(dev_base + 0x70, st);
    dsb();
    mmio_wr(dev_base + 0x50, 0);
    /* ID_NAME is 0x01. 0x11 is EV_BITS, which is not the name. */
    mmio_wr(dev_base + 0x100, 0x01u);
    dsb();
    uint32_t sz = mmio_rd(dev_base + 0x100);
    sz = (sz >> 16) & 0xffu;
    char name[56];
    memset(name, 0, sizeof name);
    if (sz > 55)
        sz = 55;
    for (uint32_t i = 0; i < sz; i += 4) {
        uint32_t w = mmio_rd(dev_base + 0x108 + i);
        memcpy(name + i, &w, 4);
    }
    name[sz] = 0;
    printf("[input] name \"%s\" queue %u\n", name[0] ? name : "?", qsz);
    ready = 1;
    last_used = 0;
    return 0;
}

int input_poll(uint16_t *code, uint16_t *value)
{
    if (!ready)
        return 0;
    uint32_t is = mmio_rd(dev_base + 0x60);
    if (is)
        mmio_wr(dev_base + 0x64, is);
    volatile uint16_t *uidx = (volatile uint16_t *)(vring + used_off + 2);
    dsb();
    uint16_t idx = *uidx;
    if (idx == last_used)
        return 0;
    uint32_t slot = last_used & qmask;
    uint32_t *elem = (uint32_t *)(vring + used_off + 4 + slot * 8);
    uint32_t id = elem[0];
    last_used++;
    dsb();
    if (id >= qsz) {
        recycle((uint16_t)id);
        return 0;
    }
    uint16_t type = events[id].type;
    uint16_t c = events[id].code;
    uint32_t v = events[id].value;
    recycle((uint16_t)id);
    if (type != EV_KEY)
        return 0;
    if (code)
        *code = c;
    if (value)
        *value = (uint16_t)v;
    return 1;
}
