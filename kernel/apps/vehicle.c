#include "quotaos.h"

/* Dedicated framebuffer demo mode. The desktop never draws on these regions. */
static uint32_t *volatile app_surface;
static uint32_t app_stride;
static volatile uint32_t dashboard_frames, video_frames;
static volatile int video_stalled;
static uint32_t music_frames;
static int music_held;
static struct task *dashboard_demo;
static struct running_app *video_demo;
static int64_t music_demo = -1;
static int video_configured;
static int app_test_phase;
enum desktop_probe_state { PROBE_OFF, PROBE_ARMED, PROBE_SPINNING, PROBE_DONE };
static volatile enum desktop_probe_state desktop_probe;
static uint64_t probe_period;
static volatile uint64_t display_commit_tick, display_max_gap;
static volatile uint32_t display_max_gap_us;
static uint32_t probe_frames, probe_misses, probe_video;
static uint64_t probe_pool_exceeded, probe_video_over, probe_video_misses;

void app_desktop_stall_probe(void)
{
    if (desktop_probe != PROBE_ARMED)
        return;
    printf("[desktop-stall] Tier-3 UI entering busy loop\n");
    desktop_probe = PROBE_SPINNING;
    while (desktop_probe == PROBE_SPINNING)
        asm volatile("" ::: "memory");
}

static uint64_t app_test_period;
static uint32_t d0, v0, m0, misses0, dn, vn, mn, dm;

void app_display_read(struct app_display_stats *stats)
{
    stats->frames[0] = dashboard_frames;
    stats->frames[1] = video_frames;
    stats->frames[2] = music_frames;
    stats->speed_misses = dashboard_demo ? dashboard_demo->misses : 0;
    stats->max_gap_us = ticks_to_us(display_max_gap);
    stats->video_paused = video_stalled;
    stats->music_held = music_held;
}

int app_desktop_mode(void)
{
    return app_surface ? (video_configured ? 2 : 1) : 0;
}

static void app_meter(uint32_t x, uint32_t y, uint32_t frame, uint32_t color)
{
    uint32_t *fb = app_surface;
    if (!fb)
        return;
    unsigned filled = 8 + (frame % 29) * 4;
    for (unsigned row = 0; row < APP_METER_H; row++)
        for (unsigned col = 0; col < APP_METER_W; col++)
            fb[(y + row) * app_stride + x + col] = col < filled ? color : 0x182338;
}

void app_display_core_frame(void)
{
    if (!app_surface)
        return;
    static const uint8_t masks[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                    0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    static const uint8_t segments[7][4] = {
        {3,0,12,3}, {14,3,3,10}, {14,15,3,10}, {3,25,12,3},
        {0,15,3,10}, {0,3,3,10}, {3,12,12,3}
    };
    unsigned speed = 45 + ((dashboard_frames + 1) / 10) % 76;
    uint32_t *fb = app_surface;
    for (unsigned y = 0; y < APP_DASH_H; y++)
        for (unsigned x = 0; x < APP_DASH_W; x++)
            fb[(APP_DASH_Y + y) * app_stride + APP_DRAW_X + x] = 0x182338;
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
                    fb[(APP_DASH_Y + r[1] + y) * app_stride +
                       APP_DRAW_X + i * 20 + r[0] + x] = 0x35dfa0;
        }
    }
    /* Complete framebuffer stores before recording a Tier-1 commit. */
    asm volatile("dsb st" ::: "memory");
    uint64_t now = ticks_now();
    if (display_commit_tick && now - display_commit_tick > display_max_gap)
        display_max_gap = now - display_commit_tick;
    display_commit_tick = now;
    dashboard_frames++;
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
        display_max_gap = 0;
        return;
    }
    if (period - probe_period < 100)
        return;

    struct sched_system_info info;
    sched_read_system_info(&info);
    dn = dashboard_frames - probe_frames;
    dm = dashboard_demo->misses - probe_misses;
    vn = video_frames - probe_video;
    display_max_gap_us = ticks_to_us(display_max_gap);
    unsigned exceeds = info.tier3_over - probe_pool_exceeded;
    int ok = period - probe_period == 100 && dn == 100 && dm == 0 &&
             vn == 25 && exceeds == 0 && info.tier2_over == probe_video_over &&
             video_demo->task->misses == probe_video_misses &&
             display_max_gap_us > 0 && display_max_gap_us < 11000;
    desktop_probe = PROBE_DONE;
    printf("[desktop-stall] periods=%lu dashboard_commits=%u misses=%u max_gap_us=%u dashcam_commits=%u pool_exceeded=%u\n",
           period - probe_period, dn, dm, display_max_gap_us, vn, exceeds);
    printf("[desktop-stall] %s\nQA DONE\n", ok ? "PASS" : "FAIL");
}

static void app_demo_video_frame(void)
{
    for (;;) {
        uint64_t period = sched_period_now();
        if (!video_stalled && period % 4 == 0)
            app_meter(APP_DRAW_X, APP_VIDEO_Y, ++video_frames, 0x559dff);
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
    /* Defer only the music viewport to demonstrate a bounded Tier-3 queue. */
    music_held = period % 100 >= 50;
    if (!music_held)
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
            music_demo = proc_spawn("music", 0, 0, 0, (uintptr_t)app_surface, 0,
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
