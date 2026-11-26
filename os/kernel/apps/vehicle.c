#include "quotaos.h"
#include "auth.h"

/* Dedicated framebuffer demo mode. The desktop never draws on these regions. */
static uint32_t *volatile app_surface;
static uint32_t app_stride;
static volatile uint32_t dashboard_frames, video_frames;
static volatile int video_stalled;
static uint64_t video_tick, video_max_gap, video_period;
static uint32_t video_missed, speed_late_gaps, video_late_gaps;
static unsigned video_epoch;
static uint32_t music_frames;
static struct task *dashboard_demo;
static struct running_app *video_demo;
static int64_t music_demo = -1;
static int video_configured;
static int app_test_phase;
enum desktop_probe_state { PROBE_OFF, PROBE_ARMED, PROBE_SPINNING, PROBE_DONE };
static volatile enum desktop_probe_state desktop_probe;
static uint64_t probe_period;
static volatile uint64_t display_commit_tick, display_max_gap;
static uint64_t stall_speed_gap, stall_camera_gap;
static uint32_t probe_frames, probe_misses, probe_video;
static uint64_t probe_pool_exceeded, probe_video_over, probe_video_misses;

static uint64_t app_test_period;
static uint32_t d0, v0, m0, misses0, dn, vn, mn, dm;

static struct sched_system_info stall_info;
static uint64_t stall_periods;
static unsigned stall_exceeds;
static int stall_ok;

void app_desktop_stall_probe(void)
{
    if (desktop_probe != PROBE_ARMED)
        return;
    printf("[desktop-stall] Tier-3 UI entering busy loop\n");
    desktop_probe = PROBE_SPINNING;
    while (desktop_probe == PROBE_SPINNING)
        asm volatile("" ::: "memory");
    struct kattr worst;
    sched_kernel_worst(&worst);
    printf("[kernel-budget] reserve_us=%lu target_us=%lu warnings=%lu overruns=%lu worst_period=%lu guard_us=%lu svc_us=%lu timer_us=%lu\n",
           stall_info.reserve_us, stall_info.reserve_target_us, stall_info.kernel_warnings, stall_info.kernel_overruns, worst.period,
           ticks_to_us(worst.t[KC_GUARD]), ticks_to_us(worst.t[KC_SVC_OTHER]), ticks_to_us(worst.t[KC_TIMER]));
    printf("[peaks] busy_us=%lu kernel_us=%lu start_late_us=%lu returned_us=%lu pool_us=%lu\n",
           stall_info.peak_busy_us, stall_info.peak_kernel_us, stall_info.max_start_late_us,
           stall_info.returned_us, stall_info.tier3_limit_us);
    printf("[desktop-stall] periods=%lu dashboard_commits=%u misses=%u max_gap_us=%lu dashcam_commits=%u pool_exceeded=%u speed_gap_ticks=%lu camera_gap_ticks=%lu tick_hz=%lu\n",
           stall_periods, dn, dm, ticks_to_us(stall_speed_gap), vn, stall_exceeds,
           stall_speed_gap, stall_camera_gap, timer_freq());
    printf("[desktop-stall] %s\nQA DONE\n", stall_ok ? "PASS" : "FAIL");
}

void app_display_read(struct app_display_stats *stats)
{
    uint64_t flags = irq_save();
    stats->sample_tick = ticks_now();
    stats->commit_tick[0] = display_commit_tick;
    stats->commit_tick[1] = video_tick;
    stats->frames[0] = dashboard_frames;
    stats->frames[1] = video_frames;
    stats->frames[2] = music_frames;
    stats->video_epoch = video_epoch;
    stats->video_missed = video_missed;
    stats->video_running = video_demo != NULL;
    stats->speed_late_gaps = speed_late_gaps;
    stats->video_late_gaps = video_late_gaps;
    uint64_t period = sched_period_now();
    if (video_demo && !video_stalled && video_tick && period > video_period)
        stats->video_missed += (uint32_t)(((period - 1) - video_period) / 4);
    stats->speed_misses = dashboard_demo ? dashboard_demo->misses : 0;
    uint64_t speed_gap = display_max_gap, camera_gap = video_max_gap;
    irq_restore(flags);
    stats->max_gap_us = ticks_to_us(speed_gap);
    stats->video_max_gap_us = ticks_to_us(camera_gap);
    stats->video_paused = video_stalled;
}

int app_desktop_mode(void)
{
    return app_surface ? (video_configured ? 2 : 1) : 0;
}

/* The 10 s test produces about 1,250 records. Never log from the drawing path. */
static struct frame_trace {
    uint64_t release, start, copy, commit;
    unsigned stream;
} frame_trace[1536];
static unsigned trace_count, trace_dropped;
static volatile int trace_active;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) || defined(SYSTEM_STATUS_ENABLED)
static uint32_t trace_initial_frames[2];
static uint64_t trace_prev_commit[2], trace_begin_tick;
#endif

void app_frame_trace_start(void)
{
    uint64_t flags = irq_save();
    trace_count = trace_dropped = 0;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) || defined(SYSTEM_STATUS_ENABLED)
    trace_initial_frames[0] = dashboard_frames;
    trace_initial_frames[1] = video_frames;
    trace_prev_commit[0] = display_commit_tick;
    trace_prev_commit[1] = video_tick;
    trace_begin_tick = ticks_now();
#endif
    trace_active = 1;
    irq_restore(flags);
}

static void trace_frame(unsigned stream, uint64_t release, uint64_t start, uint64_t copy, uint64_t commit)
{
    if (!start) return;
    uint64_t flags = irq_save();
    if (trace_active) {
        if (trace_count < sizeof frame_trace / sizeof frame_trace[0])
            frame_trace[trace_count++] = (struct frame_trace){release, start, copy, commit, stream};
        else
            trace_dropped++;
    }
    irq_restore(flags);
}

void app_frame_trace_dump(void)
{
    uint64_t flags = irq_save();
    trace_active = 0;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) || defined(SYSTEM_STATUS_ENABLED)
    uint64_t end = ticks_now();
    uint32_t speed = dashboard_frames - trace_initial_frames[0];
    uint32_t camera = video_frames - trace_initial_frames[1];
#endif
    irq_restore(flags);
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) || defined(SYSTEM_STATUS_ENABLED)
    printf("[frame-window] begin=%lu end=%lu speed=%u camera=%u prev_speed=%lu prev_camera=%lu\n",
           trace_begin_tick, end, speed, camera, trace_prev_commit[0], trace_prev_commit[1]);
#endif
    printf("[frame-trace] hz=%lu count=%u dropped=%u\n", (unsigned long)timer_freq(),
           trace_count, trace_dropped);
    for (unsigned i = 0; i < trace_count; i++) {
        const struct frame_trace *f = &frame_trace[i];
        printf("[frame] stream=%u release=%lu start=%lu copy=%lu commit=%lu\n",
               f->stream, (unsigned long)f->release, (unsigned long)f->start,
               (unsigned long)f->copy, (unsigned long)f->commit);
    }
}

/* The small instrument surfaces are drawn off-screen and copied at a fixed
 * release-relative phase. Commit timestamps are taken after the real stores. */
static uint32_t speed_pixels[APP_DASH_H][APP_DASH_W] __attribute__((aligned(8)));
static uint32_t camera_pixels[APP_METER_H][APP_METER_W] __attribute__((aligned(8)));
_Static_assert(APP_DASH_H <= APP_METER_H && APP_DASH_W <= APP_METER_W,
               "locked surface must fit both instruments");
static uint32_t locked_pixels[APP_METER_H][APP_METER_W] __attribute__((aligned(8)));

static uint64_t publish_instrument(const uint32_t *pixels, unsigned width,
                                   unsigned height, unsigned y, uint64_t target,
                                   uint64_t *copy_start)
{
    /* Keep fixed-phase instrument work alive while the login screen owns scanout. */
    int visible = auth_state() == AUTH_OPEN;
    uint32_t *surface = visible ? app_surface + y * app_stride + APP_DRAW_X
                               : &locked_pixels[0][0];
    unsigned stride = visible ? app_stride : APP_METER_W;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) || defined(SYSTEM_STATUS_ENABLED)
    if (clock_fractional_icount()) {
        /* Eight successive reads span eight instructions. Subtracting
         * 8 * target from their sum gives the first read's displacement
         * in instructions, including its position within the counter tick.
         * Skip that many NOPs before starting the real framebuffer copy. */
        uint64_t a, b, c, d, e, f, g, h;
        asm volatile(
            "1: mrs %[a], cntvct_el0\n"
            "cmp %[a], %[target]\n"
            "b.lo 1b\n"
            "mrs %[a], cntvct_el0\n"
            "mrs %[b], cntvct_el0\n"
            "mrs %[c], cntvct_el0\n"
            "mrs %[d], cntvct_el0\n"
            "mrs %[e], cntvct_el0\n"
            "mrs %[f], cntvct_el0\n"
            "mrs %[g], cntvct_el0\n"
            "mrs %[h], cntvct_el0\n"
            "add %[a], %[a], %[b]\n"
            "add %[c], %[c], %[d]\n"
            "add %[e], %[e], %[f]\n"
            "add %[g], %[g], %[h]\n"
            "add %[a], %[a], %[c]\n"
            "add %[e], %[e], %[g]\n"
            "add %[a], %[a], %[e]\n"
            "sub %[a], %[a], %[target], lsl #3\n"
            "cmp %[a], #8\n"
            "b.hi 3f\n"
            "adr %[b], 2f\n"
            "add %[b], %[b], %[a], lsl #2\n"
            "br %[b]\n"
            "2: .rept 8\n nop\n .endr\n"
            "3:"
            : [a] "=&r"(a), [b] "=&r"(b), [c] "=&r"(c), [d] "=&r"(d),
              [e] "=&r"(e), [f] "=&r"(f), [g] "=&r"(g), [h] "=&r"(h)
            : [target] "r"(target) : "cc", "memory");
    } else
#endif
    if (clock_is_icount()) {
        /* The boot calibration verified one counter tick per instruction.
         * This poll samples every three ticks. Spend the unused part of the
         * last poll on the actual publication path, keeping the copy phase
         * independent of the instruction at which rendering finished. */
        uint64_t late, tail;
        asm volatile(
            "1: mrs %0, cntvct_el0\n"
            "subs %0, %0, %2\n"
            "b.lo 1b\n"
            "cmp %0, #2\n"
            "b.hi 3f\n"
            "adr %1, 2f\n"
            "add %1, %1, %0, lsl #2\n"
            "br %1\n"
            "2: nop\n"
            "nop\n"
            "3:"
            : "=&r"(late), "=&r"(tail) : "r"(target) : "cc", "memory");
    } else {
        while (ticks_now() < target)
            asm volatile("" ::: "memory");
    }
    *copy_start = ticks_now();
    typedef uint64_t pixel_pair __attribute__((may_alias));
    for (unsigned row = 0; row < height; row++) {
        uint32_t *line = surface + row * stride;
        const uint32_t *source = pixels + row * width;
        if ((uintptr_t)line & 7) {
            for (unsigned col = 0; col < width; col++) line[col] = source[col];
        } else {
            pixel_pair *dst = (pixel_pair *)line;
            const pixel_pair *src = (const pixel_pair *)source;
            for (unsigned col = 0; col < width / 2; col++) dst[col] = src[col];
        }
    }
    asm volatile("dsb st" ::: "memory");
    return ticks_now();
}

void app_display_core_frame(uint64_t release)
{
    if (!app_surface)
        return;
    uint64_t trace_start = trace_active ? ticks_now() : 0;
    static const uint8_t masks[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                    0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    static const uint8_t segments[7][4] = {
        {3,0,12,3}, {14,3,3,10}, {14,15,3,10}, {3,25,12,3},
        {0,15,3,10}, {0,3,3,10}, {3,12,12,3}
    };
    unsigned speed = 45 + ((dashboard_frames + 1) / 10) % 76;
    for (unsigned y = 0; y < APP_DASH_H; y++)
        for (unsigned x = 0; x < APP_DASH_W; x++)
            speed_pixels[y][x] = 0x182338;
    for (unsigned i = 0, divisor = 100; i < 3; i++, divisor /= 10) {
        if (!i && speed < 100)
            continue;
        unsigned mask = masks[speed / divisor % 10];
        for (unsigned j = 0; j < 7; j++) {
            if (!(mask & (1u << j)))
                continue;
            const uint8_t *r = segments[j];
            for (unsigned y = 0; y < r[3]; y++)
                for (unsigned x = 0; x < r[2]; x++)
                    speed_pixels[r[1] + y][i * 20 + r[0] + x] = 0x35dfa0;
        }
    }
    /* The linear gauge shares the Tier-1 commit with the numeric readout. */
    unsigned marker = 64 + speed / 2;
    for (unsigned y = 0; y < 8; y++) {
        uint32_t *line = speed_pixels[11 + y];
        for (unsigned x = marker - y / 2; x <= marker + y / 2; x++)
            line[x] = 0x70ebc1;
    }
    for (unsigned x = 64; x < APP_DASH_W; x++)
        speed_pixels[20][x] = 0x82a4ad;
    uint64_t copy_start;
    uint64_t now = publish_instrument(&speed_pixels[0][0], APP_DASH_W, APP_DASH_H,
                                      APP_DASH_Y, release + us_to_ticks(270), &copy_start);
    if (display_commit_tick) {
        uint64_t gap = now - display_commit_tick;
        if (gap > display_max_gap) display_max_gap = gap;
        if (gap > us_to_ticks(PERIOD_US)) speed_late_gaps++;
    }
    display_commit_tick = now;
    dashboard_frames++;
    trace_frame(0, release, trace_start, copy_start, now);
}

static void observe_desktop_stall(uint64_t period)
{
    if (desktop_probe != PROBE_SPINNING)
        return;
    if (!probe_period) {
        struct sched_system_info info;
        sched_read_system_info(&info);
        probe_period = period;
        probe_frames = dashboard_frames;
        probe_misses = dashboard_demo->misses;
        probe_video = video_frames;
        probe_pool_exceeded = info.tier3_over;
        probe_video_over = info.tier2_over;
        probe_video_misses = video_demo->task->misses;
        display_max_gap = video_max_gap = 0;
        return;
    }
    if (period - probe_period < 100)
        return;

    struct sched_system_info info;
    sched_read_system_info(&info);
    dn = dashboard_frames - probe_frames;
    dm = dashboard_demo->misses - probe_misses;
    vn = video_frames - probe_video;
    stall_speed_gap = display_max_gap;
    stall_camera_gap = video_max_gap;
    unsigned exceeds = info.tier3_over - probe_pool_exceeded;
    int ok = period - probe_period == 100 && dn == 100 && dm == 0 &&
             vn == 25 && exceeds == 0 && info.tier2_over == probe_video_over &&
             video_demo->task->misses == probe_video_misses &&
             stall_speed_gap > 0 && stall_speed_gap <= us_to_ticks(10000) &&
             stall_camera_gap > 0 && stall_camera_gap <= us_to_ticks(40000);
    ok = ok && info.reserve_us == info.reserve_target_us && info.reserve_us >= SYS_RESERVE_US &&
         info.reserve_us >= info.peak_kernel_us * 6 / 5 && info.returned_us > 0 && info.returned_us <= 600;
    stall_info = info;
    stall_periods = period - probe_period;
    stall_exceeds = exceeds;
    stall_ok = ok;
    desktop_probe = PROBE_DONE;

}

static void app_demo_video_frame(void)
{
    for (;;) {
        uint64_t period = sched_period_now();
        if (!video_stalled && period % 4 == 0 && (!video_tick || period != video_period)) {
            uint64_t trace_start = trace_active ? ticks_now() : 0;
            uint32_t frame = video_frames + 1;
            int first = !video_tick;
            unsigned phase = frame & 63;
            int bend = (int)(phase < 32 ? phase : 63 - phase) - 16;
            unsigned last = (frame - 1) & 63;
            int old_bend = (int)(last < 32 ? last : 63 - last) - 16;
            for (int y = 0; y < APP_METER_H; y++) {
                uint32_t *row = camera_pixels[y];
                if (first)
                    for (unsigned x = 0; x < APP_METER_W; x++) row[x] = 0x101820;
                int center = 64 + bend * (APP_METER_H - y) / APP_METER_H;
                int old = 64 + old_bend * (APP_METER_H - y) / APP_METER_H;
                int edge = 6 + y;
                if (center != old) {
                    row[old-edge] = row[old-edge+1] = 0x101820;
                    row[old+edge] = row[old+edge+1] = 0x101820;
                    row[old] = row[old+1] = row[old+2] = 0x101820;
                }
                if (first || center != old) {
                    row[center-edge] = row[center-edge+1] = 0x88d8e7;
                    row[center+edge] = row[center+edge+1] = 0x88d8e7;
                }
                unsigned stripe_phase = (unsigned)y + frame;
                if (first || center != old || (stripe_phase & 7) == 0) {
                    uint32_t stripe = (stripe_phase & 8) ? 0xffdf8d : 0x101820;
                    row[center] = row[center+1] = row[center+2] = stripe;
                }
            }
            uint64_t release = sched_period_release(period);
            uint64_t copy_start;
            uint64_t now = publish_instrument(&camera_pixels[0][0], APP_METER_W, APP_METER_H,
                                              APP_VIDEO_Y, release + us_to_ticks(300), &copy_start);
            uint64_t flags = irq_save();
            if (video_tick) {
                uint64_t gap = now - video_tick;
                if (gap > video_max_gap) video_max_gap = gap;
                if (gap > us_to_ticks(4 * PERIOD_US)) video_late_gaps++;
            }
            if (video_tick)
                video_missed += (uint32_t)((period - video_period) / 4 - 1);
            video_period = period;
            video_tick = now;
            video_frames = frame;
            trace_frame(1, release, trace_start, copy_start, now);
            irq_restore(flags);
        }
        observe_desktop_stall(period);
        asm volatile("svc #0" ::: "memory");
    }
}

/* Called by the Tier-3 desktop once per iteration. No waiting in GUI code. */
void app_desktop_poll(void)
{
    if (!app_surface)
        return;
    uint64_t period = sched_period_now();
    /* Present queued pixels in a bounded batch on the Tier-3 desktop. */
    music_frames += (uint32_t)disp_drain(2);
    if (desktop_probe != PROBE_OFF)
        return;
    if (!app_test_phase) {
        app_test_period = period;
        app_test_phase = 1;
    } else if (app_test_phase == 1 && period - app_test_period >= 15) {
        video_stalled = 1;
        app_test_period = period;
        app_test_phase = 2;
    } else if (app_test_phase == 2 && period - app_test_period >= 2) {
        d0 = dashboard_frames;
        v0 = video_frames;
        m0 = proc_submits(music_demo);
        misses0 = dashboard_demo->misses;
        app_test_period = period;
        app_test_phase = 3;
    } else if (app_test_phase == 3 && period - app_test_period >= 100) {
        dn = dashboard_frames - d0;
        vn = video_frames - v0;
        mn = proc_submits(music_demo) - m0;
        dm = dashboard_demo->misses - misses0;
        video_tick = 0;
        video_epoch++;
        video_stalled = 0;
        app_test_period = period;
        app_test_phase = 4;
    } else if (app_test_phase == 4 && period - app_test_period >= 12) {
        int ok = dn == 100 && vn == 0 && dm == 0 && mn > 0 &&
                 (!video_demo || (v0 > 0 && video_frames > v0));
        printf("[apps] during 100 periods: dashboard_frames=%u misses=%u video_frames=%u music_submits=%u\n",
               dn, dm, vn, mn);
        printf("[apps] %s\nQA DONE\n", ok ? "PASS" : "FAIL");
        app_test_phase = 5;
    }
}

int vehicle_app_running(int tier)
{
    if (tier == TIER1)
        return dashboard_demo && dashboard_demo->state != TASK_DEAD;
    if (tier == TIER2)
        return video_demo != 0;
    return tier == TIER3 && music_demo >= 0 && proc_alive(music_demo);
}

int vehicle_app_set_running(int tier, int start)
{
    if (!app_surface || tier < TIER1 || tier > TIER3)
        return -1;
    if (tier == TIER1)
        return start ? 0 : task_kill(dashboard_demo);
    if (vehicle_app_running(tier) == start)
        return 0;
    if (tier == TIER3) {
        if (start) {
            music_demo = proc_spawn("music", EL0_PAINT_MUSIC, 0, 0, (uintptr_t)app_surface, 0,
                                    APP_DRAW_X, APP_MUSIC_Y, APP_MUSIC_SIZE, APP_MUSIC_SIZE);
            return music_demo >= 0 ? 0 : -1;
        }
        if (proc_kill(music_demo))
            return -1;
        music_demo = -1;
        return 0;
    }
    if (!start) {
        if (task_kill(video_demo->task))
            return -1;
        resource_release(video_demo);
        video_demo = 0;
        return 0;
    }
    const struct app_manifest manifest = {"video", "frame demo", 16, 16, 600, 600};
    struct running_app *app = resource_commit(&manifest);
    if (!app)
        return -1;
    struct task *t = task_try_create("video", TIER2, KIND_JOB);
    if (!t) {
        resource_release(app);
        return -1;
    }
    t->min_t = us_to_ticks(manifest.cpu_min_us);
    t->deadline_t = us_to_ticks(PERIOD_US);
    task_set_entry(t, app_demo_video_frame);
    app->task = t;
    video_demo = app;
    video_configured = 1;
    video_tick = 0;
    video_epoch++;
    task_start(t);
    return 0;
}

static int vehicle_lifecycle_check(void)
{
    unsigned cpu = resource_cpu_min_sum_us();
    uint64_t mem = resource_mem_min_sum_kb();
    int pages = proc_pages_free();
    int64_t old_music = music_demo;
    struct task *timer = sched_find("timer-server");
    if (!timer || vehicle_app_set_running(TIER1, 0) >= 0 || task_kill(timer) >= 0 ||
        sched_slot_owner(0) != dashboard_demo || sched_slot_owner(1) != timer)
        return 0;
    if (task_arm_el0(dashboard_demo, 0, 0, 0, 0, 0, 0, 0, 0) >= 0)
        return 0;
    printf("[lifecycle] Tier 1 EL0 launch rejected\n");
    if (vehicle_app_set_running(TIER2, 0) || sched_find("video") ||
        resource_cpu_min_sum_us() + 600 != cpu || resource_mem_min_sum_kb() + 16 != mem)
        return 0;
    if (vehicle_app_set_running(TIER3, 0) || sched_find("music") || proc_alive(old_music) ||
        proc_pages_free() != pages + 3 || resource_mem_min_sum_kb() + 32 != mem ||
        proc_queue_depth() != 0)
        return 0;
    /* Blocked fixtures occupy task slots without competing for CPU. */
    static struct task *fillers[MAX_TASKS];
    int count = 0;
    while (count < MAX_TASKS) {
        struct task *t = task_try_create("slot-test", TIER3, KIND_SPIN);
        if (!t)
            break;
        fillers[count++] = t;
    }
    int rolled_back = vehicle_app_set_running(TIER2, 1) < 0 &&
                      vehicle_app_set_running(TIER3, 1) < 0 &&
                      resource_cpu_min_sum_us() + 600 == cpu &&
                      resource_mem_min_sum_kb() + 32 == mem &&
                      proc_pages_free() == pages + 3;
    while (count)
        task_kill(fillers[--count]);
    if (!rolled_back)
        return 0;
    if (vehicle_app_set_running(TIER2, 1) || vehicle_app_set_running(TIER3, 1) ||
        !vehicle_app_running(TIER2) || !vehicle_app_running(TIER3) ||
        proc_alive(old_music) || proc_pages_free() != pages ||
        resource_cpu_min_sum_us() != cpu || resource_mem_min_sum_kb() != mem)
        return 0;
    printf("[lifecycle] PASS protected=2 released_cpu_us=600 released_ram_kib=32 reclaimed_pages=3 full_table_rollback=2 restarted=2\n");
    return 1;
}

void app_demo_run(int with_video, int stall)
{
    uint64_t pa, bytes;
    uint32_t w, h;
    if (fb_present() || fb_reserved_range(&pa, &bytes)) {
        printf("[apps] FAIL framebuffer\nQA DONE\n");
        return;
    }
    fb_geometry(&w, &h);
    app_stride = w;
    struct task *dashboard = sched_find("dashboard");
    dashboard_demo = dashboard;
    app_surface = (uint32_t *)(uintptr_t)pa;
    video_configured = with_video;
    int music_ok = vehicle_app_set_running(TIER3, 1);
    struct task *music_task = sched_find("music");
    if (with_video)
        vehicle_app_set_running(TIER2, 1);
    struct running_app *video = video_demo;
    struct task *desktop = sched_find("shell");
    if (!desktop || !dashboard || music_ok < 0 || !music_task || (with_video && !video)) {
        printf("[apps] FAIL startup music=%d\nQA DONE\n", music_ok);
        return;
    }
    printf("[apps] dashboard tier=%d slot=%d; music tier=%d EL0; video=%d\n",
           dashboard->tier, dashboard->slot, music_task->tier, video != 0);
    if (dashboard->tier != TIER1 || music_task->tier != TIER3 ||
        desktop->tier != TIER3 ||
        (video && video->task->tier != TIER2)) {
        printf("[apps] FAIL incorrect tier\nQA DONE\n");
        return;
    }
    if (!strcmp(dtb_bootargs(), "apps-lifecycle") && !vehicle_lifecycle_check()) {
        printf("[lifecycle] FAIL\nQA DONE\n");
        return;
    }
    desktop_probe = stall ? PROBE_ARMED : PROBE_OFF;
    lvgl_autostart();
    printf("[apps] FAIL desktop returned\nQA DONE\n");
}
