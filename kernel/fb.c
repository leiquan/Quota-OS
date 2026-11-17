/*
 * QEMU ramfb. The surface sits in the 16 MB system reserve, under the
 * page pool, so it is not lent and it is not a Tier-1 colored page.
 * Publishing it is a device self-test. It is not a WebKit paint.
 */
#include "quotaos.h"

#define FB_W 640u
#define FB_H 480u
#define FB_BPP 4u

extern char __image_start[], __image_end[];

static uint64_t reserve_cursor;
static uint32_t *fb_pixels;
static uint32_t fb_bytes;

static void dsb(void)
{
    asm volatile("dsb sy" ::: "memory");
}

static uint32_t bswap32(uint32_t v)
{
    return __builtin_bswap32(v);
}

static uint64_t bswap64(uint64_t v)
{
    return __builtin_bswap64(v);
}

int kernel_reserve_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)
{
    if (!reserve_cursor) {
        uint64_t base = (uint64_t)__image_start & ~((16ull << 20) - 1);
        reserve_cursor = base + (uint64_t)KERNEL_RESERVE_KB * 1024;
    }
    if (align == 0 || (align & (align - 1)) != 0)
        return -1;
    uint64_t a = (reserve_cursor - bytes) & ~(align - 1);
    if (a < (uint64_t)__image_end)
        return -1;
    reserve_cursor = a;
    *pa = a;
    return 0;
}

struct fw_dma {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

static int fw_dma(uint64_t base, uint32_t control, void *buf, uint32_t len)
{
    struct fw_dma dma;
    dma.control = bswap32(control);
    dma.length = bswap32(len);
    dma.address = bswap64((uint64_t)buf);
    dsb();
    /* High half then low half. The low write starts the transfer. */
    mmio_wr(base + 16, bswap32((uint32_t)((uint64_t)&dma >> 32)));
    mmio_wr(base + 20, bswap32((uint32_t)(uint64_t)&dma));
    for (int i = 0; i < 1000000; i++) {
        dsb();
        uint32_t c = bswap32(dma.control);
        if ((c & ~1u) == 0)
            return (c & 1u) ? -1 : 0;
    }
    return -1;
}

static int ramfb_publish(uint64_t fw, uint64_t pixels, uint32_t w, uint32_t h, uint32_t stride)
{
    char sig[4];
    if (fw_dma(fw, (0u << 16) | 0x08u | 0x02u, sig, 4) != 0)
        return -1;
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        printf("[fb] fw_cfg signature missing\n");
        return -1;
    }
    uint32_t count_be;
    if (fw_dma(fw, (0x19u << 16) | 0x08u | 0x02u, &count_be, 4) != 0)
        return -1;
    uint32_t count = bswap32(count_be);
    int selector = -1;
    for (uint32_t i = 0; i < count && i < 64; i++) {
        uint8_t ent[64];
        if (fw_dma(fw, 0x02u, ent, 64) != 0)
            return -1;
        ent[63] = 0;
        if (strcmp((const char *)(ent + 8), "etc/ramfb") == 0) {
            selector = (ent[4] << 8) | ent[5];
            break;
        }
    }
    if (selector < 0) {
        printf("[fb] etc/ramfb not in fw_cfg (%u files)\n", count);
        return -1;
    }
    uint8_t cfg[28];
    uint64_t addr = bswap64(pixels);
    uint32_t fields[5];
    fields[0] = bswap32(0x34325258u); /* DRM_FORMAT_XRGB8888 */
    fields[1] = 0;
    fields[2] = bswap32(w);
    fields[3] = bswap32(h);
    fields[4] = bswap32(stride);
    memcpy(cfg, &addr, 8);
    memcpy(cfg + 8, fields, 20);
    dsb();
    if (fw_dma(fw, ((uint32_t)selector << 16) | 0x08u | 0x10u, cfg, 28) != 0) {
        printf("[fb] ramfb write failed\n");
        return -1;
    }
    return 0;
}

int fb_device_self_test(uint32_t **pixels_out, uint32_t *w, uint32_t *h, uint32_t *stride)
{
    uint64_t pa, fwreg[4];
    uint32_t bytes = FB_W * FB_H * FB_BPP;
    if (kernel_reserve_alloc(bytes, 4096, &pa) != 0) {
        printf("[fb] system reserve has no room for %u bytes\n", bytes);
        return -1;
    }
    fb_pixels = (uint32_t *)pa;
    fb_bytes = bytes;
    for (uint32_t y = 0; y < FB_H; y++) {
        uint32_t color = 0x000000ffu;
        if (y >= 120)
            color = 0x0000ff00u;
        if (y >= 240)
            color = 0x00ff0000u;
        if (y >= 360)
            color = 0x00ffffffu;
        for (uint32_t x = 0; x < FB_W; x++)
            fb_pixels[y * FB_W + x] = color;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < FB_W * FB_H; i++)
        sum += fb_pixels[i];
    printf("[fb] reserve surface %ux%u at 0x%lx\n", FB_W, FB_H, pa);
    printf("[fb] self-test checksum 0x%x (not a WebKit paint)\n", sum);
    int n = dtb_find_bases("qemu,fw-cfg-mmio", fwreg, 1);
    if (n < 1) {
        printf("[fb] no qemu,fw-cfg-mmio in DTB\n");
        return -1;
    }
    if (ramfb_publish(fwreg[0], pa, FB_W, FB_H, FB_W * FB_BPP) != 0)
        return -1;
    printf("[fb] ramfb published from fw_cfg 0x%lx\n", fwreg[0]);
    if (pixels_out)
        *pixels_out = fb_pixels;
    if (w)
        *w = FB_W;
    if (h)
        *h = FB_H;
    if (stride)
        *stride = FB_W * FB_BPP;
    return 0;
}
