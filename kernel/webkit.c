/*
 * -append webkit. Autostart of the native port hook.
 * The WebKit engine is not linked. Nothing here parses HTML, CSS, or JS.
 * Tier-1 slots keep running; this shell task stays Tier 3.
 * MMU is off. Do not read any line here as hardware isolation.
 */
#include "quotaos.h"
#include "webkit_host.h"

__attribute__((weak)) int webkit_engine_linked(void)
{
    return 0;
}

__attribute__((weak)) int webkit_engine_main(struct webkit_host *host)
{
    (void)host;
    return -1;
}

static void gap(const char *s)
{
    printf("[webkit] unfinished: %s\n", s);
}

void webkit_autostart(void)
{
    printf("[webkit] autostart on the Tier-3 shell task\n");
    printf("[webkit] app mem capacity %lu KB\n", resource_app_mem_capacity_kb());
    printf("[webkit] inspected 4d967f4ca7bd tag WebKit-7625.2.8.18.3\n");
    printf("[webkit] that tree's WPE project version is 2.53.3\n");
    int linked = webkit_engine_linked();
    printf("[webkit] engine linked: %s\n", linked ? "yes" : "no");
    if (!linked)
        printf("[webkit] HTML/CSS/JS is not running\n");
    gap("no QuotaOS port in WebKit ALL_PORTS");
    gap("WebCore is forced off without JavaScriptCore");
    gap("OSAllocatorPOSIX uses mmap, mprotect, guard pages");
    gap("MMU is off, so that allocator has no VM");
    gap("WPE needs ICU, libxml2, sqlite, harfbuzz, png, jpeg, webp");
    gap("WPE also needs glib, libsoup, epoxy, and a UI process");
    gap("virtio EV_KEY is not a PlatformKeyboardEvent");
    gap("a contiguous app heap would use Tier-1 colors");

    uint32_t *pixels = 0;
    uint32_t w = 0, h = 0, stride = 0;
    int fb = fb_device_self_test(&pixels, &w, &h, &stride);
    int keys = input_init();
    if (linked && fb == 0) {
        struct webkit_host host;
        host.pixels = pixels;
        host.width = w;
        host.height = h;
        host.stride_bytes = stride;
        host.now_us = now_us;
        host.poll_key = input_poll;
        host.heap = 0;
        host.heap_bytes = 0;
        printf("[webkit] calling engine (heap 0 bytes)\n");
        int rc = webkit_engine_main(&host);
        printf("[webkit] engine returned %d\n", rc);
    }
    if (keys != 0) {
        printf("[webkit] keyboard absent, no input events\n");
        printf("[webkit] done\n");
        return;
    }
    printf("[webkit] polling keyboard 2s\n");
    uint64_t t0 = now_us();
    uint32_t n = 0;
    int saw_down = 0;
    uint16_t down_code = 0;
    while (now_us() - t0 < 2000000ull) {
        uint16_t code, value;
        if (!input_poll(&code, &value))
            continue;
        n++;
        printf("[webkit] EV_KEY code %u value %u (not WebKit)\n", code, value);
        if (value == 1 && !saw_down) {
            saw_down = 1;
            down_code = code;
        }
    }
    printf("[webkit] key events %u\n", n);
    if (saw_down)
        printf("[webkit] first press code %u\n", down_code);
    else
        printf("[webkit] no key press in 2s\n");
    printf("[webkit] done\n");
}
