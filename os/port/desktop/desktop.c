#include "wap_app.h"
#define WAP_TODO_NAME "Todo"
#define WAP_TODO_FILENAME "todo.wap"
#include "shell.h"
#include "guest_page_services.h"
/*
 * LVGL adaptation and the demo screen. Calls the kernel framebuffer and
 * the input queue. Does not program fw_cfg, virtio, or the page tables.
 */
#include "quotaos.h"

#define LV_CONF_INCLUDE_SIMPLE
#include "desktop.h"

#include <stddef.h>

_Static_assert(LV_MEM_SIZE == LVGL_HEAP_BYTES, "LVGL heap must match the admitted size");
_Static_assert(LV_MEM_POOL_EXPAND_SIZE == 0, "LVGL heap must not grow");

static uint8_t draw_buf[LVGL_DRAW_BYTES] __attribute__((aligned(64)));
static uint32_t *fb_px;
static uint32_t fb_w, fb_h, fb_stride;
static lv_obj_t *field;
static lv_obj_t *notification_label;
enum power_action { POWER_NONE, POWER_OFF, POWER_RESET };
static enum power_action requested_power;
static lv_obj_t *menu_list;
static lv_obj_t *detail;
static lv_obj_t *page_home, *page_test;
static lv_obj_t *page_input;
static lv_obj_t *page_cover, *page_apps, *page_files, *page_shell;
static unsigned displayed_frame;
static lv_obj_t *demo_window, *demo_image, *demo_status, *demo_output;
static int64_t demo_handle;
static lv_image_dsc_t demo_frame;
static lv_obj_t *page_zip, *zip_status, *package_entry;
static lv_obj_t *package_icon, *selected_package_view;
static lv_obj_t *shell_input, *shell_output, *source_entry, *source_icon, *source_caption, *note_icon;
static lv_obj_t *shell_history, *shell_prompt, *source_dialog, *source_name, *source_error;
static char shell_transcript[2048];
static lv_obj_t *note_status, *file_entry, *file_empty, *source_title;
static uint64_t save_since;
static struct app_display_stats save_before;
static lv_obj_t *task_body;
static lv_obj_t *tabs[3], *info_panel, *events_panel, *events_text;
static lv_obj_t *info_values[24], *info_crossings;
static lv_obj_t *clock_label;
static uint64_t rtc_base;
enum monitor_page { SYSTEM_TASK, SYSTEM_INFO, SYSTEM_EVENTS };
static enum monitor_page monitor_tab;
enum app_key { APP_SYSTEM, APP_LAUNCHER, APP_NOTES, APP_FILES, APP_SPEED, APP_VIDEO, APP_MUSIC, APP_SHELL, APP_TEST, APP_ZIP, APP_DEMO, APP_COUNT };
static lv_obj_t *dock, *icons[APP_COUNT], *dots[APP_COUNT], *touch_bar;
static lv_indev_t *desktop_keypad;
static lv_group_t *desktop_group;
enum { BAR_HEIGHT = 64, BAR_WIDTH = 280 };
struct desktop_page {
    lv_obj_t *view;
    enum app_key key;
};
static struct desktop_page active_page, back_stack[APP_COUNT];
static unsigned back_count;
enum window_state { CLOSED, OPEN, MINIMIZED };
static enum window_state states[APP_COUNT];
static enum app_key recent[5];
static unsigned recent_count;
static int screen_width;
static void sync_dock(void);
static void dock_state(enum app_key key, enum window_state state)
{
    if (key == APP_COUNT) return;
    states[key] = state;
    if (state == OPEN && key != APP_LAUNCHER) {
        unsigned i = 0;
        while (i < recent_count && recent[i] != key) i++;
        if (i == recent_count && recent_count < 5) recent_count++;
        if (i == 5) i--;
        while (i) { recent[i] = recent[i-1]; i--; }
        recent[0] = key;
    }
    if (dock) sync_dock();
}
static const enum app_key page_keys[] = {APP_SYSTEM, APP_NOTES, APP_COUNT, APP_LAUNCHER, APP_FILES, APP_SHELL, APP_TEST, APP_ZIP};

static lv_obj_t *task_lists[3];
static lv_obj_t *task_items[MAX_TASKS];
static lv_obj_t *empty_tasks[3];
static const char app_monitor[] = "System";
enum { DOCK_STEP = 132, DOCK_ITEM_WIDTH = 120 };

static void sync_dock(void)
{
    enum app_key visible[APP_COUNT] = {APP_LAUNCHER};
    unsigned count = 1;
    unsigned capacity = (screen_width - 12) / DOCK_STEP;
    for (int key = 0; key < APP_COUNT; key++) {
        lv_obj_add_flag(icons[key], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dots[key], LV_OBJ_FLAG_HIDDEN);
    }
    for (unsigned i = 0; i < recent_count && count < capacity; i++)
        if (states[recent[i]] != CLOSED) visible[count++] = recent[i];
    int width = count * DOCK_STEP + 12;
    lv_obj_set_width(dock, width);
    lv_obj_set_x(dock, (screen_width - width) / 2);
    for (unsigned i = 0; i < count; i++) {
        enum app_key key = visible[i];
        int x = 12 + i * DOCK_STEP;
        lv_obj_set_x(icons[key], x);
        lv_obj_remove_flag(icons[key], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(dots[key], x + (DOCK_ITEM_WIDTH - 6) / 2);
        if (states[key] == MINIMIZED) lv_obj_remove_flag(dots[key], LV_OBJ_FLAG_HIDDEN);
    }
}

static void switch_page(struct desktop_page page)
{
    /* Hide the page without discarding its widgets or stopping its app. */
    if (active_page.view) {
        lv_obj_add_flag(active_page.view, LV_OBJ_FLAG_HIDDEN);
        dock_state(active_page.key, active_page.key == APP_LAUNCHER ? CLOSED : MINIMIZED);
    }
    active_page = page;
    lv_obj_t *focus = NULL;
    if (page.view) {
        lv_obj_add_flag(dock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page.view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page.view);
        dock_state(page.key, OPEN);
        if (page.key == APP_NOTES)
            focus = lv_obj_has_flag(source_dialog, LV_OBJ_FLAG_HIDDEN) ? field : source_name;
        else if (page.key == APP_SHELL)
            focus = shell_input;
        else if (page.key == APP_DEMO)
            focus = demo_image;
    } else {
        lv_obj_remove_flag(dock, LV_OBJ_FLAG_HIDDEN);
    }
    lv_indev_set_group(desktop_keypad, focus ? desktop_group : NULL);
    if (focus) lv_group_focus_obj(focus);
}

static void open_page(lv_obj_t *view, enum app_key key)
{
    if (active_page.view == view) return;
    for (unsigned i = 0; i < back_count; i++) {
        if (back_stack[i].view == view) {
            back_count = i;
            switch_page((struct desktop_page){view, key});
            return;
        }
    }
    if (active_page.view && back_count < sizeof back_stack / sizeof back_stack[0])
        back_stack[back_count++] = active_page;
    switch_page((struct desktop_page){view, key});
}

static void show_page(int which)
{
    lv_obj_t *pages[] = {page_home, page_input, page_cover, page_apps,
                         page_files, page_shell, page_test, page_zip};
    open_page(pages[which], page_keys[which]);
}

enum { POINTER_DIAMETER = 30 };
static lv_obj_t *pointer_dot;

static void update_pointer_dot(const struct quota_input *event)
{
    lv_obj_remove_flag(pointer_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(pointer_dot, event->x - POINTER_DIAMETER / 2,
                   event->y - POINTER_DIAMETER / 2);
    if (event->value)
        lv_obj_add_state(pointer_dot, LV_STATE_PRESSED);
    else
        lv_obj_remove_state(pointer_dot, LV_STATE_PRESSED);
}

static struct quota_input keys[32];
static struct quota_input ptrs[32];
static int nkeys, nptrs, key_i, ptr_i;

void lvgl_port_assert(void)
{
    printf("[lvgl] assert\n");
    for (;;)
        asm volatile("wfi");
}

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(now_us() / 1000ull);
}

enum { SAMP_N = 16 };
static uint64_t click_stamp, key_stamp;
static uint64_t click_dt[SAMP_N], key_dt[SAMP_N];
static int n_click, n_key;

static void take_dt(uint64_t *stamp, uint64_t *arr, int *n, const char *kind)
{
    uint64_t d;
    if (!*stamp)
        return;
    d = now_us() - *stamp;
    *stamp = 0;
    if (*n < SAMP_N)
        arr[(*n)++] = d;
    printf("[lvgl] fb commit %s %lu us n=%d (not panel light)\n", kind, (unsigned long)d, *n);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int32_t w = lv_area_get_width(area);
    uint32_t stride_px = fb_stride / 4u;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        uint32_t *dst = fb_px + (uint32_t)y * stride_px + area->x1;
        memcpy(dst, px, (size_t)w * 4u);
        px += w * 4;
    }
    take_dt(&click_stamp, click_dt, &n_click, "click");
    take_dt(&key_stamp, key_dt, &n_key, "key");
    lv_display_flush_ready(disp);
}

static uint32_t map_key(uint16_t code, int shifted)
{
    static const struct {
        uint16_t first, count;
        const char *plain, *shift;
    } rows[] = {
        {2, 12, "1234567890-=", "!@#$%^&*()_+"},
        {16, 12, "qwertyuiop[]", "QWERTYUIOP{}"},
        {30, 12, "asdfghjkl;'`", "ASDFGHJKL:\"~"},
        {43, 11, "\\zxcvbnm,./", "|ZXCVBNM<>?"},
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (code < rows[i].first) continue;
        unsigned offset = code - rows[i].first;
        if (offset < rows[i].count)
            return (unsigned char)(shifted ? rows[i].shift[offset] : rows[i].plain[offset]);
    }
    if (code == 57) return ' ';
    if (code == 28) return LV_KEY_ENTER;
    if (code == 14) return LV_KEY_BACKSPACE;
    return 0;
}

static void compact(struct quota_input *q, int *n, int *i)
{
    int used = *i;
    if (!used)
        return;
    int left = *n - used;
    for (int k = 0; k < left; k++)
        q[k] = q[used + k];
    *n = left;
    *i = 0;
}

static void task_context_at(lv_point_t point);

static void take_inputs(void)
{
    /* Keep events LVGL has not read yet. A refresh can be later than the virtio poll. */
    compact(keys, &nkeys, &key_i);
    compact(ptrs, &nptrs, &ptr_i);
    input_service_poll();
    struct quota_input ev;
    while (input_pop(&ev)) {
        if (ev.kind == QUOTA_INPUT_CONTEXT)
            task_context_at((lv_point_t){ev.x, ev.y});
        else if (ev.kind == QUOTA_INPUT_KEY && nkeys < 32)
            keys[nkeys++] = ev;
        else if (ev.kind == QUOTA_INPUT_POINTER && nptrs < 32)
            ptrs[nptrs++] = ev;
    }
}

enum bar_gesture { BAR_NONE, BAR_TAP, BAR_SWIPE };
enum { BAR_DOUBLE_MS = 320, BAR_TAP_SLOP = 24, BAR_SWIPE_DISTANCE = 64,
       BAR_TAP_MS = 280, BAR_SWIPE_MS = 900 };
static struct {
    enum bar_gesture pending;
    uint32_t released_at, pressed_at;
    int x, y, max_dx, max_dy;
    bool down, captured, second, go_back, go_home;
} bar_input;

static int axis_distance(int a, int b)
{
    return a > b ? a - b : b - a;
}

/* Capture contacts that start on the bar, including motion outside its hit area. */
static bool take_bar_pointer(const struct quota_input *ev)
{
    bool down = ev->value != 0;
    uint32_t now = lvgl_tick_ms();
    if (down && !bar_input.down) {
        int left = ((int)fb_w - BAR_WIDTH) / 2;
        bar_input.captured = ev->y >= (int)fb_h - BAR_HEIGHT &&
                             ev->x >= left && ev->x < left + BAR_WIDTH;
        if (bar_input.captured) {
            bar_input.x = ev->x;
            bar_input.y = ev->y;
            bar_input.pressed_at = now;
            bar_input.max_dx = bar_input.max_dy = 0;
            bar_input.second = bar_input.pending && now - bar_input.released_at <= BAR_DOUBLE_MS;
            lv_obj_add_state(touch_bar, LV_STATE_PRESSED);
        }
    }
    bar_input.down = down;
    if (!bar_input.captured) return false;

    int dx = axis_distance(ev->x, bar_input.x);
    int dy = axis_distance(ev->y, bar_input.y);
    if (dx > bar_input.max_dx) bar_input.max_dx = dx;
    if (dy > bar_input.max_dy) bar_input.max_dy = dy;
    if (!down) {
        int up = bar_input.y - ev->y;
        uint32_t duration = now - bar_input.pressed_at;
        enum bar_gesture gesture = BAR_NONE;
        if (up >= BAR_SWIPE_DISTANCE && up > bar_input.max_dx && duration <= BAR_SWIPE_MS)
            gesture = BAR_SWIPE;
        else if (bar_input.max_dx <= BAR_TAP_SLOP && bar_input.max_dy <= BAR_TAP_SLOP &&
                 duration <= BAR_TAP_MS)
            gesture = BAR_TAP;

        if (bar_input.second && gesture == bar_input.pending) {
            bar_input.go_home = true;
            bar_input.pending = BAR_NONE;
        } else if (bar_input.pending && bar_input.second) {
            bar_input.go_back = true;
            bar_input.pending = BAR_NONE;
        } else if (gesture) {
            if (bar_input.pending) bar_input.go_back = true;
            bar_input.pending = gesture;
            bar_input.released_at = now;
        }
        lv_obj_remove_state(touch_bar, LV_STATE_PRESSED);
        bar_input.second = bar_input.captured = false;
    }
    return true;
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static int x, y, pressed;
    data->continue_reading = false;
    while (ptr_i < nptrs) {
        struct quota_input ev = ptrs[ptr_i++];
        update_pointer_dot(&ev);
        if (take_bar_pointer(&ev)) {
            data->continue_reading = ptr_i < nptrs;
            break;
        }
        x = ev.x;
        y = ev.y;
        pressed = ev.value != 0;
        if (pressed)
            click_stamp = now_us();
        printf("[lvgl] pointer %d,%d down %d\n", x, y, pressed);
        if (ptr_i < nptrs)
            data->continue_reading = true;
        break;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint32_t last;
    static unsigned shift_keys;
    data->key = last;
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    while (key_i < nkeys) {
        struct quota_input ev = keys[key_i++];
        if (ev.code == 42 || ev.code == 54) {
            unsigned mask = ev.code == 42 ? 1u : 2u;
            if (ev.value) shift_keys |= mask;
            else shift_keys &= ~mask;
            continue;
        }
        uint32_t mapped = map_key(ev.code, shift_keys != 0);
        printf("[lvgl] key code %u value %u char %u\n", ev.code, ev.value, mapped);
        if (ev.value)
            key_stamp = now_us();
        if (!mapped)
            continue;
        last = mapped;
        data->key = mapped;
        data->state = ev.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (key_i < nkeys)
            data->continue_reading = true;
        break;
    }
}

static void demo_input_event(lv_event_t *e)
{
    if (!demo_handle || !lv_obj_is_visible(demo_image)) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_BACKSPACE) key = 8;
        else if (key == LV_KEY_ENTER) key = 13;
        proc_page_input(demo_handle, 3, key, 0);
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_area_t bounds;
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(demo_image, &bounds);
    if (code == LV_EVENT_PRESSED) lv_group_focus_obj(demo_image);
    proc_page_input(demo_handle, code == LV_EVENT_PRESSED ? 1 : 2,
                    LV_CLAMP(0, point.x - bounds.x1, GUEST_PREVIEW_WIDTH - 1),
                    LV_CLAMP(0, point.y - bounds.y1, GUEST_PREVIEW_HEIGHT - 1));
}

static void on_click(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(field, "");
    printf("[lvgl] notes cleared\n");
}

static void monitor_visible(lv_obj_t *obj, int visible)
{
    if (visible)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void select_monitor_tab(enum monitor_page tab)
{
    monitor_tab = tab;
    monitor_visible(task_body, tab == SYSTEM_TASK);
    monitor_visible(info_panel, tab == SYSTEM_INFO);
    monitor_visible(events_panel, tab == SYSTEM_EVENTS);
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(i == tab ? 0xe0edff : 0xffffff), 0);
}

static struct {
    int active, selected, protected, disk, video;
    uint64_t period;
    struct app_display_stats apps;
    struct sched_system_info system;
} checks;

static void start_checks(void)
{
    if (checks.active) return;
    checks.selected = checks.active = 1;
    checks.video = vehicle_app_running(TIER2);
    checks.protected = vehicle_app_running(TIER1) && vehicle_app_set_running(TIER1, 0) < 0 &&
                       vehicle_app_running(TIER1);
    checks.disk = note_disk_verify() == 0;
    checks.period = sched_period_now();
    app_display_read(&checks.apps);
    sched_read_system_info(&checks.system);
    lv_label_set_text(detail, "Checks running...");
}

static void poll_checks(void)
{
    if (!checks.active) return;
    uint64_t periods = sched_period_now() - checks.period;
    if (periods < 100 || (checks.disk && !note_disk_verify_result())) return;
    struct app_display_stats a;
    struct sched_system_info sys;
    app_display_read(&a);
    sched_read_system_info(&sys);
    unsigned misses = a.speed_misses - checks.apps.speed_misses;
    unsigned frames = a.frames[0] - checks.apps.frames[0];
    unsigned long shortfalls = sys.tier2_short - checks.system.tier2_short;
    unsigned long overruns = sys.tier2_over - checks.system.tier2_over;
    unsigned long crossings = sys.tier3_over - checks.system.tier3_over;
    const char *protection = !app_desktop_mode() ? "SKIP" : checks.protected ? "PASS" : "FAIL";
    const char *speed = !app_desktop_mode() ? "SKIP" : !misses && frames >= periods - 1 ? "PASS" : "FAIL";
    const char *video = !checks.video || !vehicle_app_running(TIER2) ? "SKIP" :
                        !shortfalls && !overruns ? "PASS" : "FAIL";
    const char *disk = !checks.disk ? "SKIP" : note_disk_verify_result() == 1 ? "PASS" : "FAIL";
    lv_label_set_text_fmt(detail,
        "LIVE RESULTS  /  %lu periods\n\n"
        "%s    Tier 1 stop protection\n%s    Speed: %u commits, %u misses\n"
        "%s    Tier 2: %lu shortfalls, %lu overruns\n%s    Tier 3: %lu pool crossings\n"
        "%s    Note.txt disk readback", (unsigned long)periods, protection, speed, frames, misses,
        video, shortfalls, overruns, crossings ? "FAIL" : "PASS", crossings, disk);
    printf("[checks] protection=%s speed=%s frames=%u misses=%u tier2=%s short=%lu over=%lu pool_crossings=%lu disk=%s\n",
           protection, speed, frames, misses, video, shortfalls, overruns, crossings, disk);
    checks.active = 0;
}

/* Test workloads are single-instance and release their resources after sampling. */
static struct {
    int active, pending;
    uint64_t period;
    struct sched_system_info before;
    struct app_display_stats display;
    struct running_app *reservation, *pressure_memory;
    struct task *task;
    unsigned pressure_kib;
    int64_t process;
} trial;
static lv_obj_t *approve, *cancel_trial, *busy_loop_label;
static const char busy_loop_title[] = "5. T3 Run busy loop";
static unsigned busy_loop_seconds;

static void update_busy_loop_countdown(unsigned seconds)
{
    if (seconds == busy_loop_seconds) return;
    busy_loop_seconds = seconds;
    if (seconds)
        lv_label_set_text_fmt(busy_loop_label, "5. T3 Busy loop  %u s", seconds);
    else
        lv_label_set_text(busy_loop_label, busy_loop_title);
}

static void trial_result(int number, const char *result)
{
    lv_label_set_text(detail, result);
    printf("[quota-test] %d %s\n", number, result);
}

static unsigned trial_periods(int number)
{
    return number == 5 ? 1000 : 100;
}

static void begin_trial(int number, int approved)
{
    if (trial.active) return;
    trial.pending = 0;
    monitor_visible(approve, 0);
    monitor_visible(cancel_trial, 0);
    if (!number) {
        trial_result(0, "Cancelled");
        return;
    }
    if (number == 2 || number == 3) {
        if (!approved) {
            trial.pending = number;
            lv_label_set_text(detail, number == 2 ?
                "Periodic job requests 400 us per 10 ms and 16 KiB." :
                "CPU-bound job requests 400 us guaranteed, up to 800 us per 10 ms, and 16 KiB.");
            monitor_visible(approve, 1);
            monitor_visible(cancel_trial, 1);
            return;
        }
    }
    if (number <= 3) {
        struct app_manifest manifest = {"quota-test", "Test", 16, 16,
            number == 1 ? PERIOD_US : 400, number == 1 ? PERIOD_US : number == 3 ? 800 : 400};
        int reason = resource_check(&manifest);
        if (number == 1) {
            uint32_t cpu_total = resource_app_cpu_capacity_us(), cpu_left;
            uint64_t mem_total = resource_app_mem_capacity_kb(), mem_left;
            resource_app_available(&cpu_left, &mem_left);
            struct running_app *unexpected = resource_commit(&manifest);
            int refused = !unexpected && (reason == ADMIT_DEVICE || reason == ADMIT_OVERSUB);
            char result[300];
            lv_snprintf(result, sizeof result,
                "%s\n\nCPU per 10 ms (us)\n"
                "Requested %u   Available %u   Total %u\n\n"
                "Memory (KiB)\n"
                "Requested %u   Available %lu   Total %lu",
                refused ? "PASS  Launch refused: insufficient CPU quota." :
                          "FAIL  Unexpected admission result.",
                manifest.cpu_min_us, cpu_left, cpu_total, manifest.mem_min_kb,
                (unsigned long)mem_left, (unsigned long)mem_total);
            trial_result(number, result);
            if (unexpected) resource_release(unexpected);
            return;
        }
        trial.reservation = resource_commit(&manifest);
        if (!trial.reservation) { trial_result(number, resource_reason()); return; }
        trial.task = task_try_create("quota-test", TIER2, number == 3 ? KIND_SPIN : KIND_JOB);
        if (!trial.task) {
            resource_release(trial.reservation);
            trial_result(number, "Unable to allocate task.");
            return;
        }
        trial.task->min_t = us_to_ticks(400);
        trial.task->sugg_t = us_to_ticks(number == 3 ? 400 : 0);
        trial.task->deadline_t = us_to_ticks(PERIOD_US);
        trial.task->work_t = us_to_ticks(100);
        trial.reservation->task = trial.task;
        task_start(trial.task);
    } else {
        trial.process = proc_spawn("quota-test", number == 5 ? 17 : 16, 0,0,0,0,0,0,1,1);
        if (trial.process < 0) { trial_result(number, "Unable to launch EL0 process."); return; }
        trial.task = sched_find("quota-test");
        if (number == 5 && note_disk_scrub_start() == 0) {
            uint32_t cpu_available;
            uint64_t memory_available;
            resource_app_available(&cpu_available, &memory_available);
            if (memory_available && memory_available <= UINT32_MAX) {
                unsigned kib = (unsigned)memory_available;
                const struct app_manifest pressure = {"quota-memory", "Capacity pressure", kib, kib, 0, 0};
                trial.pressure_memory = resource_commit(&pressure);
                if (trial.pressure_memory) trial.pressure_kib = kib;
            }
            if (!trial.pressure_memory) {
                unsigned completed_requests;
                note_disk_scrub_stop(&completed_requests);
                proc_kill(trial.process);
                trial_result(number, "FAIL  Unable to reserve remaining application memory.");
                return;
            }
        }
    }
    trial.active = number;
    trial.period = sched_period_now();
    if (number == 5) update_busy_loop_countdown(trial_periods(number) * PERIOD_US / 1000000);
    sched_read_system_info(&trial.before);
    app_display_read(&trial.display);
    if (trial.pressure_memory) app_frame_trace_start();
    char status[64];
    lv_snprintf(status, sizeof status, "Running  /  sampling %u periods", trial_periods(number));
    trial_result(number, status);
}

enum { PRESSURE_WORKING_SET_BYTES = 64 * 1024 };
static volatile uint64_t pressure_words[PRESSURE_WORKING_SET_BYTES / sizeof(uint64_t)];

static void touch_pressure_memory(void)
{
    if (!trial.pressure_memory) return;
    for (unsigned i = 0; i < sizeof pressure_words / sizeof pressure_words[0]; i++)
        pressure_words[i]++;
}

static void poll_trial(void)
{
    int number = trial.active;
    uint64_t elapsed = sched_period_now() - trial.period;
    if (!number) return;
    unsigned duration = trial_periods(number);
    if (elapsed < duration) {
        if (number == 5)
            update_busy_loop_countdown(((duration - (unsigned)elapsed) * PERIOD_US + 999999) / 1000000);
        return;
    }
    if (number == 5) update_busy_loop_countdown(0);
    struct sched_system_info sys;
    struct app_display_stats display;
    sched_read_system_info(&sys);
    app_display_read(&display);
    unsigned frames = display.frames[0] - trial.display.frames[0];
    unsigned misses = display.speed_misses - trial.display.speed_misses;
    unsigned long crossings = sys.tier3_over - trial.before.tier3_over;
    unsigned long over = sys.tier2_over - trial.before.tier2_over;
    unsigned long shortfalls = sys.tier2_short - trial.before.tier2_short;
    uint64_t peak = trial.task ? trial.task->used_max_all : 0;
    int ok = peak && frames + 1 >= elapsed && !misses && !crossings && !shortfalls && !over;
    int combined = trial.pressure_memory != NULL;
    if (combined) {
        unsigned completed_requests;
        unsigned submitted_requests = note_disk_scrub_stop(&completed_requests);
        uint32_t cpu_available;
        uint64_t memory_available;
        resource_app_available(&cpu_available, &memory_available);
        printf("[combined-pressure] periods=%lu cpu_peak_us=%lu memory_reserved_kib=%u memory_available_kib=%lu working_set_kib=72 disk_submitted=%u disk_completed=%u\n",
               (unsigned long)elapsed, (unsigned long)ticks_to_us(peak), trial.pressure_kib,
               (unsigned long)memory_available, submitted_requests, completed_requests);
        ok = ok && completed_requests && submitted_requests <= elapsed + 1 &&
             submitted_requests - completed_requests <= 1;
        resource_release(trial.pressure_memory);
        trial.pressure_memory = NULL;
    }
    if (number < 4) {
        if (number == 2) ok = ok && trial.task->jobs && !trial.task->misses;
        ok = ok && peak <= us_to_ticks(number == 3 ? 800 : 400);
        task_kill(trial.task);
        resource_release(trial.reservation);
    } else {
        ok = ok && proc_alive(trial.process);
        if (proc_kill(trial.process)) ok = 0;
    }
    char camera[64];
    int camera_measured = display.video_running && trial.display.video_running &&
                          display.video_epoch == trial.display.video_epoch;
    unsigned speed_late = display.speed_late_gaps - trial.display.speed_late_gaps;
    unsigned camera_late = display.video_late_gaps - trial.display.video_late_gaps;
    if (camera_measured) {
        unsigned camera_frames = display.frames[1] - trial.display.frames[1];
        unsigned camera_missed = display.video_missed - trial.display.video_missed;
        ok = ok && camera_frames && !camera_missed;
        lv_snprintf(camera, sizeof camera, "Flight Camera %u commits, %u missed",
                    camera_frames, camera_missed);
    } else {
        lv_snprintf(camera, sizeof camera, "Flight Camera not measured");
    }
    const char *timing = speed_late || misses || (camera_measured && camera_late) ||
                         (camera_measured && display.video_missed != trial.display.video_missed)
        ? "FAIL" : camera_measured ? "PASS" : "Camera not measured";
    char result[436];
    lv_snprintf(result, sizeof result,
        "%s  /  %lu periods\nPeak CPU %lu us per period\nSpeed %u commits, %u misses\n%s\nPeak gap: Speed %u us, Dashcam %u us\nTier 2 shortfalls %lu, overruns %lu\nTier 3 pool crossings %lu\nMusic Player %u commits\nStrict frame timing: %s\nLong gaps: GPS Speed %u, Flight Camera %u",
        ok ? "PASS" : "FAIL", (unsigned long)elapsed, (unsigned long)ticks_to_us(peak),
        frames, misses, camera, display.max_gap_us, display.video_max_gap_us, shortfalls, over, crossings,
        display.frames[2] - trial.display.frames[2], timing, speed_late, camera_late);
    trial.active = 0;
    if (combined) app_frame_trace_dump();
    trial_result(number, result);
}

static void shell_refresh(void)
{
    lv_label_set_text(shell_output, shell_transcript);
    lv_obj_update_layout(shell_history);
    int bottom = lv_obj_get_height(shell_output) + 10;
    lv_obj_set_y(shell_prompt, bottom + 4);
    lv_obj_set_y(shell_input, bottom);
    lv_obj_update_layout(shell_history);
    lv_obj_scroll_to_view(shell_input, LV_ANIM_OFF);
}

static void shell_append(const char *line)
{
    unsigned used = 0, added = 0;
    while (shell_transcript[used]) used++;
    while (line[added] && added < sizeof shell_transcript - 2) added++;
    if (used + added + 2 > sizeof shell_transcript) {
        unsigned skip = used + added + 2 - sizeof shell_transcript;
        while (skip < used && shell_transcript[skip++] != '\n') {}
        for (unsigned i = skip; i < used; i++) shell_transcript[i - skip] = shell_transcript[i];
        used -= skip;
    }
    memcpy(shell_transcript + used, line, added);
    shell_transcript[used + added] = '\n';
    shell_transcript[used + added + 1] = 0;
    shell_refresh();
}

static void show_demo_window(void)
{
    open_page(demo_window, APP_DEMO);
}

static void select_package_view(lv_obj_t *view)
{
    if (selected_package_view)
        lv_obj_remove_state(selected_package_view, LV_STATE_CHECKED);
    selected_package_view = view;
    if (view)
        lv_obj_add_state(view, LV_STATE_CHECKED);
}

static void power_poll(void)
{
    if (requested_power == POWER_NONE) return;
    int ready = note_disk_power_ready();
    if (!ready) return;
    enum power_action action = requested_power;
    requested_power = POWER_NONE;
    if (ready < 0) {
        lv_label_set_text(notification_label, "Disk error. Power action canceled.");
        return;
    }
    printf("[power] %s after disk ready\n", action == POWER_RESET ? "reset" : "off");
    register uint64_t function asm("x0") = action == POWER_RESET ? 0x84000009u : 0x84000008u;
    asm volatile("dsb sy; hvc #0" : "+r"(function) :: "memory");
    lv_label_set_text(notification_label, "Power action unavailable.");
}

static void on_group(lv_event_t *e)
{
    int id = (int)(uintptr_t)lv_event_get_user_data(e);
    printf("[menu] button %d\n", id);
    if (id >= 220 && id <= 226) {
        if (id < 225) begin_trial(id - 219, 0);
        else if (trial.pending) begin_trial(id == 225 ? trial.pending : 0, 1);
        return;
    }
    if (id == 215) {
        start_checks();
        return;
    }
    if (id == 9 || id == 209 || id == 103) {
        checks.active = checks.selected = 0;
    }
    if (id == 230) {
        show_page(5);
        if (!shell_transcript[0]) {
            char help[200];
            shell_execute("help", help, sizeof help);
            shell_append("Quota OS Shell  |  /Desktop");
            shell_append(help);
        }
        lv_group_focus_obj(shell_input);
        printf("[shell] opened\n");
        return;
    }
    if (id == 231) {
        const char *command = lv_textarea_get_text(shell_input);
        if (!command[0]) return;
        char output[1024];
        const char *result = output;
        enum shell_action action = shell_execute(command, output, sizeof output);
        if (action == SHELL_CLEAR) {
            shell_transcript[0] = 0;
            shell_refresh();
            lv_textarea_set_text(shell_input, "");
            return;
        }
        if (action == SHELL_REBOOT || action == SHELL_OFF) {
            requested_power = action == SHELL_REBOOT ? POWER_RESET : POWER_OFF;
            result = "Waiting for disk...";
        }
        char entry[256];
        lv_snprintf(entry, sizeof entry, "quota> %s", command);
        shell_append(entry);
        shell_append(result);
        printf("[shell] %s: %s\n", command, result);
        lv_textarea_set_text(shell_input, "");
        return;
    }
    if (id == 233) {
        const char *source = note_disk_source();
        if (source) {
            lv_textarea_set_text(field, source);
            show_page(1);
            lv_group_focus_obj(field);
        }
        return;
    }
    if (id == 241) {
        show_demo_window();
        return;
    }
    if (id == 238) {
        show_page(7);
        return;
    }
    if (id == 239) {
        int ok = wap_export_todo();
        lv_label_set_text(zip_status, ok ? "Export queued. Open Files to see the saved package."
                                        : "Export unavailable. Check the Notes disk.");
        return;
    }
    if (id == 240) {
        lv_obj_t *view = lv_event_get_current_target(e);
        if (selected_package_view != view) {
            select_package_view(view);
            return;
        }
        select_package_view(NULL);
        enum wap_open_result result = wap_open_saved(&demo_handle);
        if (result == WAP_INVALID) {
            lv_label_set_text(zip_status, "Invalid application package.");
            show_page(7);
            return;
        }
        if (result == WAP_RESTORED) {
            show_demo_window();
            printf("[wap] restored page handle=%ld\n", (long)demo_handle);
            return;
        }
        lv_obj_add_flag(demo_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(demo_status, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(demo_output, "");
        displayed_frame = 0;
        demo_frame.data = NULL;
        lv_label_set_text(demo_status, demo_handle > 0 ? "Loading app.html..."
            : result == WAP_LOAD_FAILED ? "Quota OS could not load the page process."
                    : "The page runtime is missing from this system image.");
        show_demo_window();
        printf("[wap] page process handle=%ld\n", (long)demo_handle);
        return;
    }
    if (id == 212) {
        show_page(4);
        printf("[files] directory opened\n");
        return;
    }
    if (id == 213) {
        const char *text = note_disk_text();
        if (text) {
            lv_textarea_set_text(field, text);
            show_page(1);
            printf("[files] opened Note.txt in Notes\n");
        }
        return;
    }
    if (id == 236) {
        if (!lv_textarea_get_text(field)[0]) {
            lv_textarea_set_text(field, "int main(void) {\n"
                "    puts(\"Hello, Quota OS!\");\n"
                "    return 0;\n"
                "}");
            lv_label_set_text(note_status, "Example inserted. Choose Save As...");
        } else {
            lv_label_set_text(note_status, "Clear the editor to insert an example.");
        }
        lv_group_focus_obj(field);
        return;
    }
    if (id == 232) {
        const char *name = note_disk_source_name();
        lv_textarea_set_text(source_name, name ? name : "");
        lv_label_set_text(source_error, "Name the source file");
        monitor_visible(source_dialog, 1);
        lv_obj_move_foreground(source_dialog);
        lv_group_focus_obj(source_name);
        return;
    }
    if (id == 235) {
        monitor_visible(source_dialog, 0);
        lv_group_focus_obj(field);
        return;
    }
    if (id == 214 || id == 234) {
        int rc = id == 234 ? note_disk_save_source(lv_textarea_get_text(field), lv_textarea_get_text(source_name)) :
                             note_disk_save(lv_textarea_get_text(field));
        if (!rc) {
            monitor_visible(source_dialog, 0);
            lv_group_focus_obj(field);
            save_since = now_us();
            app_display_read(&save_before);
            lv_label_set_text(note_status, "Saving...");
        } else {
            if (id == 234)
                lv_label_set_text(source_error, rc == -3 ? "Use a .c name with letters, digits, - or _." :
                    rc == -2 ? "Source limit: 463 bytes." : note_disk_status());
            else
                lv_label_set_text(note_status, rc == -2 ? "Limit: 495 bytes" : note_disk_status());
        }
        return;
    }
    if (id == 103) {
        demo_menu_stop();
        return;
    }
    if (id == 237) {
        show_page(0);
        select_monitor_tab(SYSTEM_EVENTS);
        printf("[desktop] system events opened\n");
        return;
    }
    if (id == 208) {
        show_page(0);
        select_monitor_tab(SYSTEM_INFO);
        printf("[desktop] system info opened\n");
        return;
    }
    if (id == 209) {
        demo_menu_start(9);
        show_page(0);
        select_monitor_tab(SYSTEM_TASK);
        printf("[desktop] task manager opened\n");
        return;
    }
    if (id == 204) {
        show_page(6);
        printf("[desktop] Test opened\n");
        return;
    }
    if (id == 203) {
        show_page(0);
        select_monitor_tab(SYSTEM_TASK);
        printf("[desktop] task manager opened\n");
        return;
    }
    if (id == 210) {
        show_page(3);
        printf("[desktop] Apps opened\n");
        return;
    }
    if (id >= 205 && id <= 207) {
        if (app_desktop_mode()) {
            int tier = id - 204;
            if (!vehicle_app_running(tier) && vehicle_app_set_running(tier, 1)) return;
            open_page(instruments_page(id - 205), APP_SPEED + tier - 1);
            printf("[desktop] app selected %d\n", id - 205 + 1);
        }
        return;
    }
    if (id == 200) {
        show_page(1);
        printf("[lvgl] input page\n");
        return;
    }
    if (id == 201) {
        show_page(2);
        printf("[lvgl] coverage page\n");
        return;
    }
    if (id == 202) {
        show_page(0);
        printf("[lvgl] home\n");
        return;
    }
    demo_menu_start(id);
}

static lv_obj_t *add_row(lv_obj_t *parent, int y, uint32_t bg, uint32_t edge, const char *text, int id)
{
    lv_obj_t *item = lv_button_create(parent);
    int32_t cw = parent == menu_list ? 194 : 380;
    lv_obj_set_pos(item, 0, y);
    lv_obj_set_size(item, cw, 38);
    lv_obj_set_style_bg_color(item, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(edge), 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_shadow_width(item, 0, 0);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(item, lv_color_hex(0x101820), 0);
    lv_obj_add_event_cb(item, on_group, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_t *il = lv_label_create(item);
    lv_label_set_text(il, text);
    lv_obj_set_style_text_color(il, lv_color_hex(0x101820), 0);
    lv_obj_center(il);
    return item;
}

lv_obj_t *desk_label(lv_obj_t *parent, int x, int y, const char *text, uint32_t color)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *files_row(lv_obj_t *parent, const char *name, uintptr_t action,
                           lv_obj_t **title, lv_obj_t **metadata)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_pos(row, 20, 96);
    lv_obj_set_size(row, 458, 66);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0xdbe3f0), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_add_event_cb(row, on_group, LV_EVENT_CLICKED, (void *)action);
    lv_obj_t *file_icon = desk_label(row, 18, 19, LV_SYMBOL_FILE, 0x327b90);
    lv_obj_set_style_text_font(file_icon, &lv_font_montserrat_24, 0);
    *title = desk_label(row, 60, 9, name, 0x243650);
    *metadata = desk_label(row, 60, 36, "", 0x64748b);
    desk_label(row, 428, 23, LV_SYMBOL_RIGHT, 0x75839a);
    return row;
}

static lv_obj_t *desktop_file(lv_obj_t *screen, int x, int y, const char *name,
                              const char *type, int action, lv_obj_t **caption)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 72, 94);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x14213a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, on_group, LV_EVENT_CLICKED, (void *)(uintptr_t)action);

    lv_obj_t *paper = lv_obj_create(button);
    lv_obj_set_size(paper, 42, 50);
    lv_obj_align(paper, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(paper, lv_color_hex(0xf6f9ff), 0);
    lv_obj_set_style_border_color(paper, lv_color_hex(0xc2cde1), 0);
    lv_obj_set_style_border_width(paper, 1, 0);
    lv_obj_set_style_radius(paper, 6, 0);
    lv_obj_set_style_pad_all(paper, 0, 0);
    lv_obj_remove_flag(paper, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lines = desk_label(paper, 12, 5, LV_SYMBOL_LIST, 0x52708f);
    lv_obj_t *extension = desk_label(paper, 0, 0, type, 0x284765);
    lv_obj_align(extension, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_remove_flag(lines, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(extension, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = desk_label(button, 0, 0, name, screen == page_files ? 0x243650 : 0xf5f8ff);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(label, 72);
    lv_obj_set_height(label, 20);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 62);
    if (caption) *caption = label;
    return button;
}

static lv_obj_t *desktop_page_create(lv_obj_t *screen, const char *title)
{
    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_set_pos(page, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_size(page, (int)fb_w, (int)fb_h - STATUS_BAR_HEIGHT - BAR_HEIGHT);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0xf7f9fc), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_set_size(bar, (int)fb_w, 52);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0xdce4ef), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xebf0f7), 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = desk_label(bar, 0, 0, title, 0x263552);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return page;
}

static const struct app_entry {
    const char *name;
    const char *glyph;
    uint32_t color;
    int action;
} app_entries[] = {
    [APP_DEMO] = {WAP_TODO_NAME, LV_SYMBOL_FILE, 0x326e91, 241},
    [APP_ZIP] = {"ZIP", LV_SYMBOL_SAVE, 0x45698b, 238},
    [APP_TEST] = {"Test", LV_SYMBOL_OK, 0x426b8f, 204},
    [APP_SYSTEM] = {app_monitor, LV_SYMBOL_SETTINGS, 0x5368af, 202},
    [APP_LAUNCHER] = {"Apps", LV_SYMBOL_LIST, 0x31798b, 210},
    [APP_SHELL] = {"Shell", LV_SYMBOL_RIGHT, 0x334765, 230},
    [APP_FILES] = {"Files", LV_SYMBOL_DIRECTORY, 0x326e91, 212},
    [APP_NOTES] = {"Notes", LV_SYMBOL_EDIT, 0xae743b, 200},
    [APP_SPEED] = {app_speed, LV_SYMBOL_CHARGE, 0x267757, 205},
    [APP_VIDEO] = {app_video, LV_SYMBOL_VIDEO, 0x476bb4, 206},
    [APP_MUSIC] = {app_music, LV_SYMBOL_AUDIO, 0x81549e, 207},
};

static lv_obj_t *app_icon(lv_obj_t *parent, enum app_key key, int x, int y, int square)
{
    const struct app_entry *app = &app_entries[key];
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, DOCK_ITEM_WIDTH, square + 23);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, on_group, LV_EVENT_CLICKED, (void *)(uintptr_t)app->action);
    lv_obj_t *tile = lv_obj_create(button);
    lv_obj_set_size(tile, square, square);
    lv_obj_align(tile, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(tile, square / 4, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(app->color), 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (key == APP_LAUNCHER) {
        int cell = square / 6, gap = square / 11;
        int inset = (square - 2 * cell - gap) / 2;
        for (int i = 0; i < 4; i++) {
            lv_obj_t *pad = lv_obj_create(tile);
            lv_obj_set_size(pad, cell, cell);
            lv_obj_set_pos(pad, inset + (i % 2) * (cell + gap),
                           inset + (i / 2) * (cell + gap));
            lv_obj_set_style_radius(pad, 2, 0);
            lv_obj_set_style_bg_color(pad, lv_color_hex(i == 3 ? 0xbfe9f0 : 0xffffff), 0);
            lv_obj_set_style_border_width(pad, 0, 0);
            lv_obj_remove_flag(pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    } else {
        lv_obj_t *symbol = lv_label_create(tile);
        lv_label_set_text(symbol, app->glyph);
        lv_obj_set_style_text_font(symbol, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(symbol, lv_color_hex(0xffffff), 0);
        lv_obj_center(symbol);
    }
    lv_obj_t *caption = lv_label_create(button);
    lv_label_set_text(caption, app->name);
    lv_obj_set_style_text_color(caption, lv_color_hex(0x233351), 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, 0);
    return button;
}

static void refresh_clock(void)
{
    static unsigned last = UINT32_MAX;
    /* PL031 supplies UTC. Show the time in Asia/Shanghai. */
    unsigned minute = (mmio_rd(rtc_base) / 60u + 8u * 60u) % (24u * 60u);
    if (minute == last)
        return;
    last = minute;
    lv_label_set_text_fmt(clock_label, "%02u:%02u", minute / 60u, minute % 60u);
}

static void refresh_system_info(void)
{
    if (monitor_tab != SYSTEM_INFO || lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN))
        return;
    struct sched_system_info v;
    sched_read_system_info(&v);
    uint64_t cpu_busy = v.cpu_us[0] + v.cpu_us[1] + v.cpu_us[2] + v.kernel_us;
    uint64_t ram = resource_ram_kb();
    uint64_t reserved = ram - resource_app_mem_capacity_kb() + resource_mem_min_sum_kb();
    unsigned cpu_percent = (unsigned)(cpu_busy >= PERIOD_US ? 100 : cpu_busy * 100 / PERIOD_US);
    unsigned mem_percent = ram ? (unsigned)(reserved * 100 / ram) : 0;
    const uint64_t values[] = {cpu_percent, v.cpu_us[0], v.cpu_us[1], v.cpu_us[2],
        v.tier3_limit_us, v.kernel_us, v.tier1_misses, v.tier2_short,
        mem_percent, ram, system_reserve_kb(), resource_t1_mem_kb(),
        resource_mem_min_sum_kb(), resource_app_mem_capacity_kb(), v.tier2_over, v.tier3_throttled,
        v.peak_busy_us, v.peak_kernel_us, v.max_start_late_us, v.returned_us, v.kernel_warnings, v.kernel_overruns, v.reserve_us, v.reserve_target_us};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
        char number[32];
        lv_snprintf(number, sizeof number, "%lu", (unsigned long)values[i]);
        if (strcmp(number, lv_label_get_text(info_values[i])))
            lv_label_set_text(info_values[i], number);
    }
    char crossings[32];
    lv_snprintf(crossings, sizeof crossings, "%lu", (unsigned long)v.tier3_over);
    if (strcmp(crossings, lv_label_get_text(info_crossings)))
        lv_label_set_text(info_crossings, crossings);
}

static void refresh_system_events(void)
{
    static const char *const names[] = {
        "Tier 1 deadline misses", "Tier 2 quota shortfalls", "Tier 2 quota overruns",
        "Tier 3 pool overruns", "Kernel budget overruns", "GPS Speed frame gap > 10 ms",
        "Flight Camera missed frames", "Flight Camera frame gap > 40 ms"
    };
    enum { EVENT_ROWS = 8 };
    static struct {
        unsigned kind;
        uint64_t period, count;
    } rows[EVENT_ROWS];
    static uint64_t previous[sizeof names / sizeof names[0]];
    static unsigned next, used;
    static int dirty;
    struct sched_system_info info;
    sched_read_system_info(&info);
    struct app_display_stats display;
    app_display_read(&display);
    const uint64_t totals[] = {info.tier1_misses, info.tier2_short, info.tier2_over,
                               info.tier3_over, info.kernel_overruns, display.speed_late_gaps,
                               display.video_missed, display.video_late_gaps};
    for (unsigned i = 0; i < sizeof totals / sizeof totals[0]; i++) {
        if (totals[i] > previous[i]) {
            rows[next].kind = i;
            rows[next].period = info.period;
            rows[next].count = totals[i] - previous[i];
            next = (next + 1) % EVENT_ROWS;
            if (used < EVENT_ROWS) used++;
            dirty = 1;
            lv_label_set_text(notification_label, names[i]);
        }
        previous[i] = totals[i];
    }
    if (!dirty || monitor_tab != SYSTEM_EVENTS ||
        lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN))
        return;

    static char text[640];
    unsigned length = 0;
    for (unsigned i = 0; i < used; i++) {
        unsigned row = (next + EVENT_ROWS - 1 - i) % EVENT_ROWS;
        int written = lv_snprintf(text + length, sizeof text - length,
                                 "Period %lu   %s   +%lu\n",
                                 (unsigned long)rows[row].period, names[rows[row].kind],
                                 (unsigned long)rows[row].count);
        if (written < 0 || (unsigned)written >= sizeof text - length) break;
        length += (unsigned)written;
    }
    lv_label_set_text(events_text, text);
    dirty = 0;
}

static lv_obj_t *dock_minimized_dot(lv_obj_t *dock, int item_x)
{
    lv_obj_t *dot = lv_obj_create(dock);
    lv_obj_set_pos(dot, item_x + (DOCK_ITEM_WIDTH - 6) / 2, 75);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x202e52), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    return dot;
}

static const char *task_display_name(const char *name)
{
    if (!strcmp(name, "dashboard")) return app_speed;
    if (!strcmp(name, "video")) return app_video;
    if (!strcmp(name, "music")) return app_music;
    if (!strcmp(name, "shell")) return "Desktop Shell";
    if (!strcmp(name, "timer-server")) return "Timer Service";
    if (!strcmp(name, "display-vsync")) return "Display Service";
    return name;
}

static lv_obj_t *context_layer, *context_button, *context_label;
static lv_obj_t *stopped_rows[2];
static int context_tier, context_start;
static void refresh_tasks(void);

static void dismiss_context(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(context_layer, LV_OBJ_FLAG_HIDDEN);
}

static void apply_context(lv_event_t *event)
{
    (void)event;
    int result = vehicle_app_set_running(context_tier, context_start);
    printf("[system] tier=%d start=%d result=%d\n", context_tier, context_start, result);
    if (result) {
        lv_label_set_text(context_label, "Unavailable");
        return;
    }
    lv_obj_add_flag(context_layer, LV_OBJ_FLAG_HIDDEN);
    dock_state(APP_SPEED + context_tier - 1, context_start ? OPEN : CLOSED);
    refresh_tasks();
}

static void open_context(int tier, lv_point_t point)
{
    if (!context_layer) {
        context_layer = lv_obj_create(lv_layer_top());
        lv_obj_set_size(context_layer, lv_pct(100), lv_pct(100));
        lv_obj_set_style_pad_all(context_layer, 0, 0);
        lv_obj_set_style_border_width(context_layer, 0, 0);
        lv_obj_set_style_bg_opa(context_layer, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(context_layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(context_layer, dismiss_context, LV_EVENT_CLICKED, NULL);
        context_button = lv_button_create(context_layer);
        lv_obj_set_size(context_button, 176, 48);
        lv_obj_set_style_bg_color(context_button, lv_color_hex(0x344968), 0);
        lv_obj_add_event_cb(context_button, apply_context, LV_EVENT_CLICKED, NULL);
        context_label = lv_label_create(context_button);
        lv_obj_center(context_label);
    }
    context_tier = tier;
    context_start = !vehicle_app_running(tier);
    if (tier == TIER1 || tier == 4) {
        lv_label_set_text(context_label, tier == TIER1 ? "Protected by kernel" : "Hosts desktop UI");
        lv_obj_add_state(context_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(context_button, LV_STATE_DISABLED);
        lv_label_set_text_fmt(context_label, "%s %s", context_start ? "Start" : "Stop",
                              tier == TIER2 ? app_video : app_music);
    }
    lv_obj_set_pos(context_button, point.x < (int)fb_w - 176 ? point.x : (int)fb_w - 176,
                   point.y < (int)fb_h - 48 ? point.y : (int)fb_h - 48);
    lv_obj_remove_flag(context_layer, LV_OBJ_FLAG_HIDDEN);
    printf("[system] context tier=%d start=%d\n", tier, context_start);
}

static int row_tier(lv_obj_t *row)
{
    if (!row) return 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (row != task_items[i]) continue;
        const struct task *t = sched_task_at(i);
        if (!t || t->state == TASK_DEAD) return 0;
        if (t->tier == TIER1) return TIER1;
        if (!strcmp(t->name, "shell")) return 4;
        if (!strcmp(t->name, "video")) return TIER2;
        if (!strcmp(t->name, "music")) return TIER3;
        return 0;
    }
    if (row == stopped_rows[0]) return TIER2;
    if (row == stopped_rows[1]) return TIER3;
    return 0;
}

static void task_context_at(lv_point_t point)
{
    if (context_layer)
        lv_obj_add_flag(context_layer, LV_OBJ_FLAG_HIDDEN);
    int tier = row_tier(lv_indev_search_obj(lv_screen_active(), &point));
    if (tier) open_context(tier, point);
}

static void row_long_press(lv_event_t *event)
{
    int tier = row_tier(lv_event_get_target_obj(event));
    if (tier) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        open_context(tier, point);
    }
}

static void refresh_tasks(void)
{
    if (monitor_tab != SYSTEM_TASK || lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN))
        return;
    for (int tier = TIER1; tier <= TIER3; tier++) {
        int visible = 0;
        lv_obj_t *list = task_lists[tier - TIER1];
        for (int i = 0; i < MAX_TASKS; i++) {
            const struct task *t = sched_task_at(i);
            if (!t || t->tier != tier || t->state == TASK_EMPTY || t->state == TASK_DEAD)
                continue;
            const char *state = t->state == TASK_RUNNING ? "Running" :
                                t->state == TASK_READY ? "Ready" : "Waiting";
            uint64_t flags = irq_save();
            unsigned long cpu = ticks_to_us(t->last_used_t);
            unsigned long grant = ticks_to_us(t->last_grant_t);
            irq_restore(flags);
            unsigned mem_grant = 0;
            unsigned mapped = proc_task_memory_kib((struct task *)t, &mem_grant);
            unsigned long reservation = 0;
            if (tier == TIER1 && t->slot >= 0) {
                int count;
                const struct tier1_decl *decl = resource_tier1(&count);
                int slot = t->slot;
                for (int j = 0; j < count; j++) {
                    if (!decl[j].cpu_us || slot-- != 0)
                        continue;
                    mem_grant = decl[j].mem_kb;
                    reservation = decl[j].cpu_us;
                    break;
                }
            } else if (tier == TIER2) {
                reservation = ticks_to_us(t->min_t);
                for (int j = 0; ; j++) {
                    const struct running_app *app = resource_running(j);
                    if (!app)
                        break;
                    if (app->task == t) {
                        mem_grant = app->mem_granted_kb;
                        break;
                    }
                }
            }
            const char *ram_label = mapped ? "Mapped KiB" : "Reserved KiB";
            char memory[24] = "--";
            if (mapped || mem_grant)
                lv_snprintf(memory, sizeof memory, "%u", mapped ? mapped : mem_grant);
            char keys[96], values[112];
            if (tier == TIER2) {
                lv_snprintf(keys, sizeof keys, "State\nCPU us\nMinimum us\nGrant us\n%s", ram_label);
                lv_snprintf(values, sizeof values, "%s\n%lu\n%lu\n%lu\n%s", state, cpu, reservation, grant, memory);
            } else if (tier == TIER1) {
                lv_snprintf(keys, sizeof keys, "State\nCPU us\nFixed us\n%s", ram_label);
                lv_snprintf(values, sizeof values, "%s\n%lu\n%lu\n%s", state, cpu, reservation, memory);
            } else {
                lv_snprintf(keys, sizeof keys, "State\nCPU us\nCPU pool\n%s", ram_label);
                lv_snprintf(values, sizeof values, "%s\n%lu\nShared\n%s", state, cpu, memory);
            }
            lv_obj_t *item = task_items[i];
            if (!item) {
                task_items[i] = item = lv_obj_create(list);
                lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(item, row_long_press, LV_EVENT_LONG_PRESSED, NULL);
                lv_obj_set_width(item, lv_pct(100));
                lv_obj_set_style_pad_all(item, 8, 0);
                lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(item, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_radius(item, 6, 0);
                lv_obj_set_style_border_width(item, 1, 0);
                lv_obj_set_style_border_color(item, lv_color_hex(0xdbe3f0), 0);
                lv_obj_set_style_text_color(item, lv_color_hex(0x263552), 0);
                lv_obj_t *title = desk_label(item, 0, 0, "", 0x203853);
                lv_obj_set_width(title, lv_pct(100));
                lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
                lv_obj_t *left = desk_label(item, 0, 22, "", 0x475a7a);
                lv_obj_set_width(left, 98);
                lv_obj_t *right = desk_label(item, 98, 22, "", 0x263552);
                lv_obj_set_width(right, 63);
                lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
                lv_label_set_long_mode(right, LV_LABEL_LONG_CLIP);
            } else if (lv_obj_get_parent(item) != list) {
                lv_obj_set_parent(item, list);
            }
            lv_obj_remove_flag(item, LV_OBJ_FLAG_HIDDEN);
            int height = tier == TIER2 ? 130 : 112;
            if (lv_obj_get_height(item) != height) lv_obj_set_height(item, height);
            const char *fields[] = {task_display_name(t->name), keys, values};
            for (unsigned col = 0; col < 3; col++) {
                lv_obj_t *field_label = lv_obj_get_child(item, col);
                if (strcmp(fields[col], lv_label_get_text(field_label)))
                    lv_label_set_text(field_label, fields[col]);
            }
            visible++;
        }
        if (tier > TIER1 && stopped_rows[tier - TIER2]) {
            int stopped = !vehicle_app_running(tier);
            monitor_visible(stopped_rows[tier - TIER2], stopped);
            visible += stopped;
        }
        monitor_visible(empty_tasks[tier - TIER1], !visible);
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!task_items[i])
            continue;
        const struct task *t = sched_task_at(i);
        if (!t || t->state == TASK_EMPTY || t->state == TASK_DEAD)
            lv_obj_add_flag(task_items[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Resolve the first gesture only after giving a matching second gesture a chance. */
static void bar_navigate(void)
{
    if (bar_input.pending && !bar_input.second &&
        lvgl_tick_ms() - bar_input.released_at >= BAR_DOUBLE_MS) {
        bar_input.pending = BAR_NONE;
        bar_input.go_back = true;
    }
    if (bar_input.go_home) {
        bar_input.go_home = bar_input.go_back = false;
        if (context_layer) dismiss_context(NULL);
        if (active_page.view) switch_page((struct desktop_page){0});
        back_count = 0;
        printf("[touch] home\n");
    } else if (bar_input.go_back) {
        bar_input.go_back = false;
        if (context_layer && !lv_obj_has_flag(context_layer, LV_OBJ_FLAG_HIDDEN)) {
            dismiss_context(NULL);
        } else if (active_page.view == page_input && !lv_obj_has_flag(source_dialog, LV_OBJ_FLAG_HIDDEN)) {
            monitor_visible(source_dialog, 0);
            lv_group_focus_obj(field);
        } else if (active_page.view == page_home && monitor_tab != SYSTEM_TASK) {
            select_monitor_tab(SYSTEM_TASK);
        } else if (active_page.view) {
            struct desktop_page previous = back_count ? back_stack[--back_count] : (struct desktop_page){0};
            switch_page(previous);
        }
        printf("[touch] back\n");
    }
}

int lvgl_demo_run(uint32_t *pixels, uint32_t w, uint32_t h, uint32_t stride)
{
    fb_px = pixels;
    fb_w = w;
    fb_h = h;
    fb_stride = stride;
    printf("[lvgl] upstream %d.%d.%d heap %u draw %u\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
           (unsigned)LV_MEM_SIZE, (unsigned)sizeof draw_buf);

    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);

    lv_display_t *disp = lv_display_create((int32_t)w, (int32_t)h);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof draw_buf, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_indev_t *keypad = lv_indev_create();
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, keypad_read);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x182b57), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x6c4d92), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *hill = lv_obj_create(screen);
        lv_obj_set_size(hill, 1480, 640);
        lv_obj_set_pos(hill, -300 + i * 330, 300 + i * 108);
        lv_obj_set_style_radius(hill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(hill, 0, 0);
        lv_obj_set_style_bg_color(hill, lv_color_hex(i == 0 ? 0x345388 : i == 1 ? 0x555eaa : 0x9b76b0), 0);
        lv_obj_remove_flag(hill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *top = lv_obj_create(screen);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, (int)w, STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x152641), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_70, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    notification_label = desk_label(top, 0, 0, "", 0xdce8ff);
    lv_obj_set_width(notification_label, (int)w - 260);
    lv_obj_set_style_text_align(notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(notification_label, LV_LABEL_LONG_DOT);
    lv_obj_center(notification_label);

    lv_obj_t *network_icon = desk_label(top, 0, 0, LV_SYMBOL_WIFI, 0x94a3b8);
    lv_obj_t *volume_icon = desk_label(top, 0, 0, LV_SYMBOL_VOLUME_MAX, 0x94a3b8);
    lv_obj_align(network_icon, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_align(volume_icon, LV_ALIGN_LEFT_MID, 56, 0);
    static const lv_point_precise_t outer_wave[] = {
        {0, 0}, {2, 3}, {3, 7}, {3, 11}, {2, 15}, {0, 18}
    };
    lv_obj_t *volume_wave = lv_line_create(top);
    lv_line_set_points(volume_wave, outer_wave, sizeof outer_wave / sizeof outer_wave[0]);
    lv_obj_set_style_line_width(volume_wave, 1, 0);
    lv_obj_set_style_line_color(volume_wave, lv_color_hex(0x94a3b8), 0);
    lv_obj_align(volume_wave, LV_ALIGN_LEFT_MID, 72, 0);
    if (dtb_find_bases("arm,pl031", &rtc_base, 1) > 0) {
        clock_label = desk_label(top, 0, 0, "", 0xdce8ff);
        lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, -26, 0);
        refresh_clock();
    }

    int apps = app_desktop_mode();
    if (apps) instruments_create(screen, apps, BAR_HEIGHT);
    page_home = desktop_page_create(screen, app_monitor);
    lv_obj_t *about_button = add_row(page_home, 55, 0xffffff, 0xc5d9fa, "About", 201);
    lv_obj_set_pos(about_button, 447, 55);
    lv_obj_set_width(about_button, 136);
    page_test = desktop_page_create(screen, "Test");
    menu_list = lv_obj_create(page_test);
    lv_obj_set_pos(menu_list, 0, 100);
    lv_obj_set_size(menu_list, 220, 360);
    lv_obj_set_style_bg_color(menu_list, lv_color_hex(0xeaf0fa), 0);
    lv_obj_set_style_border_width(menu_list, 0, 0);
    lv_obj_set_style_pad_all(menu_list, 12, 0);
    lv_obj_set_style_radius(menu_list, 0, 0);
    lv_obj_remove_flag(menu_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *load9 = NULL, *watch = NULL;
    if (apps) {
        static const char *const scenarios[] = {
            "1. T2 Request too much", "2. T2 Start a job", "3. T2 Limit CPU use",
            "4. T3 Run normal task"
        };
        for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
            add_row(menu_list, 12 + (int)i * 54, 0xe0edff, 0xc5d9fa, scenarios[i], 220 + (int)i);
        lv_obj_t *busy_loop = add_row(menu_list, 228, 0xe0edff, 0xc5d9fa, busy_loop_title, 224);
        busy_loop_label = lv_obj_get_child(busy_loop, 0);
    } else {
    load9 = add_row(menu_list, 20, 0xe0edff, 0xc5d9fa, "Scheduler stress", 9);
    add_row(menu_list, 74, 0xffffff, 0xdbe3f0, "Stop stress", 103);
    add_row(menu_list, 128, 0xe0edff, 0xc5d9fa, "Run system checks", 215);
    watch = add_row(menu_list, 236, 0xe0edff, 0xc5d9fa, "Run & view tasks", 209);
    }
    detail = desk_label(page_test, 252, 124, "Select a test", 0x263552);
    lv_obj_set_size(detail, 436, 220);
    approve = add_row(menu_list, 290, 0xe0edff, 0xc5d9fa, "Allow launch", 225);
    cancel_trial = add_row(menu_list, 290, 0xffffff, 0xdbe3f0, "Cancel", 226);
    lv_obj_set_parent(approve, page_test);
    lv_obj_set_parent(cancel_trial, page_test);
    lv_obj_set_pos(approve, 252, 365);
    lv_obj_set_pos(cancel_trial, 455, 365);
    monitor_visible(approve, 0);
    monitor_visible(cancel_trial, 0);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    static const char *const tab_names[] = {"Task", "Info", "Events"};
    static const int tab_ids[] = {203, 208, 237};
    for (int i = 0; i < 3; i++) {
        tabs[i] = add_row(page_home, 55, 0xffffff, 0xc5d9fa, tab_names[i], tab_ids[i]);
        lv_obj_set_pos(tabs[i], 12 + i * 145, 55);
        lv_obj_set_width(tabs[i], 136);
    }

    info_panel = lv_obj_create(page_home);
    lv_obj_set_pos(info_panel, 12, 108);
    lv_obj_set_size(info_panel, 694, 336);
    lv_obj_set_style_bg_color(info_panel, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(info_panel, lv_color_hex(0xdbe3f0), 0);
    lv_obj_set_style_border_width(info_panel, 1, 0);
    lv_obj_set_style_pad_all(info_panel, 0, 0);
    lv_obj_remove_flag(info_panel, LV_OBJ_FLAG_SCROLLABLE);
    static const char *const info_names[] = {
        "CPU busy (%)", "Tier 1 CPU (us)", "Tier 2 CPU (us)", "Tier 3 CPU (us)",
        "Tier 3 limit (us)", "Kernel CPU (us)", "Tier 1 misses", "Tier 2 shortfalls",
        "RAM reserved (%)", "Guest RAM (KiB)", "System reserve (KiB)", "Tier 1 reserve (KiB)",
        "App minima (KiB)", "App capacity (KiB)", "Tier 2 overruns", "Tier 3 throttled periods",
        "Busy peak (us)", "Kernel peak (us)", "T1 start late max (us)", "T2 returned (us)", "Kernel warning periods", "Kernel overrun periods", "Kernel reserve (us)", "Reserve target (us)"
    };
    desk_label(info_panel, 16, 10, clock_is_icount() ? "CPU  /  10 ms  /  icount" : "CPU  /  10 ms  /  host", 0x374e90);
    desk_label(info_panel, 356, 10, "Memory and resource events", 0x374e90);
    for (unsigned i = 0; i < sizeof info_names / sizeof info_names[0]; i++) {
        int column = i < 16 ? i / 8 : i < 20 ? (i - 16) / 2 : (i - 20) / 2;
        int row = i < 16 ? i % 8 : i < 20 ? 8 + (i - 16) % 2 : 10 + (i - 20) % 2;
        int x = column ? 356 : 16, y = 36 + row * 23;
        desk_label(info_panel, x, y, info_names[i], 0x475a7a);
        info_values[i] = desk_label(info_panel, x + 186, y, "", 0x263552);
        lv_obj_set_width(info_values[i], 122);
        lv_obj_set_style_text_align(info_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    desk_label(info_panel, 356, 312, "Tier 3 pool crossings", 0x475a7a);
    info_crossings = desk_label(info_panel, 542, 312, "", 0x263552);
    lv_obj_set_width(info_crossings, 122);
    lv_obj_set_style_text_align(info_crossings, LV_TEXT_ALIGN_RIGHT, 0);

    page_input = desktop_page_create(screen, "Notes · Tier 3");
    note_status = desk_label(page_input, 28, 70, "", 0x64748b);
    note_disk_init();
    field = lv_textarea_create(page_input);
    lv_obj_set_style_anim_duration(field, 700, LV_PART_CURSOR);
    lv_obj_set_pos(field, 28, 108);
    lv_obj_set_size(field, (int)w - 56, (int)h - STATUS_BAR_HEIGHT - BAR_HEIGHT - 216);
    lv_textarea_set_one_line(field, false);
    lv_textarea_set_placeholder_text(field, "Start typing...");
    lv_obj_set_style_radius(field, 8, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0xd9e1ee), 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(0x263552), 0);
    int note_actions_y = (int)h - STATUS_BAR_HEIGHT - BAR_HEIGHT - 84;
    lv_obj_t *save = add_row(page_input, note_actions_y, 0xdcebf6, 0xc5d9fa, "Save", 214);
    lv_obj_set_pos(save, (int)w - 140, note_actions_y);
    lv_obj_set_size(save, 112, 56);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x254e88), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(save, 0), lv_color_white(), 0);
    lv_obj_t *save_source = add_row(page_input, note_actions_y, 0xdcebf6, 0xc5d9fa, "Save As...", 232);
    lv_obj_set_pos(save_source, 156, note_actions_y);
    lv_obj_set_size(save_source, 132, 56);
    lv_obj_t *example = add_row(page_input, note_actions_y, 0xffffff, 0xdce4ef, "C Example", 236);
    lv_obj_set_pos(example, 304, note_actions_y);
    lv_obj_set_size(example, 114, 56);
    lv_obj_t *btn = lv_button_create(page_input);
    lv_obj_set_pos(btn, 28, note_actions_y);
    lv_obj_set_size(btn, 112, 56);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xe4eaf3), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x31445c), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Clear");
    lv_obj_center(bl);
    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, field);
    source_dialog = lv_obj_create(page_input);
    lv_obj_set_size(source_dialog, 440, 156);
    lv_obj_center(source_dialog);
    lv_obj_set_style_pad_all(source_dialog, 0, 0);
    lv_obj_remove_flag(source_dialog, LV_OBJ_FLAG_SCROLLABLE);
    source_error = desk_label(source_dialog, 16, 12, "", 0x334765);
    source_name = lv_textarea_create(source_dialog);
    lv_textarea_set_one_line(source_name, true);
    lv_textarea_set_max_length(source_name, 31);
    lv_textarea_set_placeholder_text(source_name, "File name (.c)");
    lv_obj_set_pos(source_name, 16, 42);
    lv_obj_set_size(source_name, 406, 42);
    lv_group_add_obj(group, source_name);
    lv_obj_add_event_cb(source_name, on_group, LV_EVENT_READY, (void *)(uintptr_t)234);
    lv_obj_t *cancel_source = add_row(source_dialog, 100, 0xffffff, 0xdbe3f0, "Cancel", 235);
    lv_obj_set_pos(cancel_source, 198, 100);
    lv_obj_set_width(cancel_source, 104);
    lv_obj_t *confirm_source = add_row(source_dialog, 100, 0xe0edff, 0xc5d9fa, "Save", 234);
    lv_obj_set_pos(confirm_source, 314, 100);
    lv_obj_set_width(confirm_source, 108);
    monitor_visible(source_dialog, 0);
    desktop_keypad = keypad;
    desktop_group = group;

    page_cover = desktop_page_create(screen, "About Quota OS");
    desk_label(page_cover, 28, 70, "Version " QUOTAOS_VERSION "    ARM64", 0x374e90);
    char about_text[160];
    lv_snprintf(about_text, sizeof about_text,
        "%u x %u display\n"
        "Author: leiquan@live.com\n"
        "Display core: Tier 1\n"
        "Desktop interaction: Tier 3",
        (unsigned)w, (unsigned)h);
    lv_obj_t *about = desk_label(page_cover, 28, 111, about_text, 0x64748b);
    lv_obj_set_width(about, 440);
    lv_obj_t *back = add_row(page_cover, 233, 0xe4ecff, 0xc5d9fa, app_monitor, 202);
    lv_obj_set_pos(back, 28, 280);
    lv_obj_set_width(back, 440);

    task_body = lv_obj_create(page_home);
    lv_obj_set_pos(task_body, 12, 108);
    lv_obj_set_size(task_body, 694, 336);
    lv_obj_set_style_bg_opa(task_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(task_body, 0, 0);
    lv_obj_set_style_pad_all(task_body, 0, 0);
    lv_obj_remove_flag(task_body, LV_OBJ_FLAG_SCROLLABLE);
    static const char *const tiers[] = {"Tier 1", "Tier 2", "Tier 3"};
    static const char *const captions[] = {"Vendor reserved", "User selected", "Shared resources"};
    static const uint32_t accent[] = {0x23785c, 0x395cc0, 0x7851a8};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *card = lv_obj_create(task_body);
        lv_obj_set_pos(card, i * 232, 0);
        lv_obj_set_size(card, 216, 322);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xdbe3f0), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xffffff), 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        desk_label(card, 16, 14, tiers[i], accent[i]);
        desk_label(card, 16, 39, captions[i], 0x475a7a);
        desk_label(card, 16, 56, "CPU per 10 ms", 0x64748b);
        lv_obj_t *list = lv_obj_create(card);
        lv_obj_set_pos(list, 10, 78);
        lv_obj_set_size(list, 194, 228);
        lv_obj_set_style_pad_all(list, 5, 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(0xf3f6fb), 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 5, 0);
        if (apps && i) {
            lv_obj_t *row = desk_label(list, 0, 0,
                i == 1 ? "Flight Camera  /  Stopped" : "Music Player  /  Stopped", 0x475a7a);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_style_pad_all(row, 8, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_event_cb(row, row_long_press, LV_EVENT_LONG_PRESSED, NULL);
            stopped_rows[i - 1] = row;
        }
        task_lists[i] = list;
        empty_tasks[i] = desk_label(list, 0, 0, "No active tasks", 0x64748b);
    }
    events_panel = lv_obj_create(page_home);
    lv_obj_set_pos(events_panel, 12, 108);
    lv_obj_set_size(events_panel, 694, 336);
    lv_obj_set_style_bg_color(events_panel, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(events_panel, lv_color_hex(0xdbe3f0), 0);
    lv_obj_set_style_border_width(events_panel, 1, 0);
    lv_obj_set_style_pad_all(events_panel, 0, 0);
    lv_obj_remove_flag(events_panel, LV_OBJ_FLAG_SCROLLABLE);
    desk_label(events_panel, 18, 16, "Recent quota and timing events", 0x263552);
    events_text = desk_label(events_panel, 18, 58, "No events recorded", 0x475a7a);
    lv_obj_set_style_text_line_space(events_text, 10, 0);
    select_monitor_tab(SYSTEM_TASK);

    demo_window = desktop_page_create(screen, WAP_TODO_NAME);
    demo_output = desk_label(demo_window, 28, GUEST_PREVIEW_HEIGHT + 100, "", 0x243650);
    demo_status = desk_label(demo_window, 28, 58, "", 0x536782);
    demo_image = lv_image_create(demo_window);
    lv_obj_add_flag(demo_image, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(group, demo_image);
    lv_obj_add_event_cb(demo_image, demo_input_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(demo_image, demo_input_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(demo_image, demo_input_event, LV_EVENT_KEY, NULL);
    lv_obj_align(demo_image, LV_ALIGN_TOP_MID, 0, 80);
    demo_frame.header.magic = LV_IMAGE_HEADER_MAGIC;
    demo_frame.header.cf = LV_COLOR_FORMAT_XRGB8888;
    demo_frame.header.w = GUEST_PREVIEW_WIDTH;
    demo_frame.header.h = GUEST_PREVIEW_HEIGHT;
    demo_frame.header.stride = GUEST_PREVIEW_WIDTH * 4;
    demo_frame.data_size = GUEST_PREVIEW_HEIGHT * GUEST_PREVIEW_WIDTH * 4;
    page_zip = desktop_page_create(screen, "ZIP");
    desk_label(page_zip, 24, 68, "Export the " WAP_TODO_NAME " application package", 0x536782);
    zip_status = desk_label(page_zip, 24, 160, "", 0x64748b);
    lv_obj_t *zip_title, *zip_meta;
    lv_obj_t *export_row = files_row(page_zip, "Export " WAP_TODO_FILENAME, 239, &zip_title, &zip_meta);
    lv_label_set_text(zip_meta, "Stored ZIP application package");
    page_files = desktop_page_create(screen, "Files");
    desk_label(page_files, 24, 68, "Desktop", 0x536782);
    file_entry = desktop_file(page_files, 20, 94, "Note.txt", "TXT", 213, NULL);
    source_entry = desktop_file(page_files, 116, 94, "", ".c", 233, &source_title);
    package_entry = desktop_file(page_files, 212, 94, WAP_TODO_FILENAME, "WAP", 240, NULL);
    lv_obj_set_style_bg_color(package_entry, lv_color_hex(0xd6e8fb), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(package_entry, LV_OPA_COVER, LV_STATE_CHECKED);
    monitor_visible(package_entry, 0);
    file_empty = desk_label(page_files, 28, 116, "", 0x64748b);
    lv_obj_add_flag(file_entry, LV_OBJ_FLAG_HIDDEN);
    source_icon = desktop_file(screen, 12, 66, "", ".c", 233, &source_caption);
    note_icon = desktop_file(screen, 12, 164, "Note.txt", "TXT", 213, NULL);
    package_icon = desktop_file(screen, 12, 66, WAP_TODO_FILENAME, "WAP", 240, NULL);
    lv_obj_set_style_bg_color(package_icon, lv_color_hex(0x324a68), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(package_icon, LV_OPA_COVER, LV_STATE_CHECKED);
    monitor_visible(package_icon, 0);
    monitor_visible(source_entry, 0);
    monitor_visible(source_icon, 0);
    monitor_visible(note_icon, 0);
    page_shell = desktop_page_create(screen, "Shell");
    shell_history = lv_obj_create(page_shell);
    lv_obj_set_pos(shell_history, 10, 54);
    lv_obj_set_size(shell_history, (int)w - 20, (int)h - STATUS_BAR_HEIGHT - BAR_HEIGHT - 66);
    lv_obj_set_style_bg_color(shell_history, lv_color_hex(0x101216), 0);
    lv_obj_set_style_border_width(shell_history, 0, 0);
    lv_obj_set_style_radius(shell_history, 4, 0);
    lv_obj_set_style_pad_all(shell_history, 12, 0);
    lv_obj_set_scroll_dir(shell_history, LV_DIR_VER);
    shell_output = desk_label(shell_history, 0, 0, "", 0xe4edf9);
    lv_obj_set_width(shell_output, (int)w - 48);
    lv_label_set_long_mode(shell_output, LV_LABEL_LONG_WRAP);
    shell_prompt = desk_label(shell_history, 0, 0, "quota>", 0xe4edf9);
    shell_input = lv_textarea_create(shell_history);
    lv_textarea_set_one_line(shell_input, true);
    lv_textarea_set_max_length(shell_input, 80);
    lv_obj_set_x(shell_input, 58);
    lv_obj_set_size(shell_input, (int)w - 106, 26);
    lv_obj_set_style_pad_all(shell_input, 4, 0);
    lv_obj_set_style_text_color(shell_input, lv_color_hex(0xe4edf9), 0);
    lv_obj_set_style_bg_opa(shell_input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shell_input, 0, 0);
    lv_obj_set_style_outline_width(shell_input, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(shell_input, 0, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(group, shell_input);
    lv_obj_add_event_cb(shell_input, on_group, LV_EVENT_READY, (void *)(uintptr_t)231);
    shell_refresh();
    page_apps = desktop_page_create(screen, "Apps");
    lv_obj_t *test_entry = NULL;
    static const enum app_key catalog[] = {APP_SYSTEM, APP_FILES, APP_NOTES, APP_SPEED, APP_VIDEO, APP_MUSIC, APP_SHELL, APP_TEST, APP_ZIP};
    for (unsigned i = 0; i < sizeof catalog / sizeof catalog[0]; i++) {
        enum app_key key = catalog[i];
        lv_obj_t *entry = app_icon(page_apps, key, 90 + (int)(i % 3) * 300,
                                   112 + (int)(i / 3) * 150, 56);
        if (key == APP_TEST) test_entry = entry;
        if ((key >= APP_SPEED && key <= APP_MUSIC && !apps) || (key == APP_VIDEO && apps != 2)) {
            lv_obj_add_state(entry, LV_STATE_DISABLED);
            lv_obj_set_style_opa(entry, LV_OPA_50, 0);
        }
    }
    screen_width = w;
    dock = lv_obj_create(screen);
    int dock_width = (apps ? 6 : 3) * DOCK_STEP + 12;
    lv_obj_set_pos(dock, ((int)w - dock_width) / 2, (int)h - BAR_HEIGHT - 96);
    lv_obj_set_size(dock, dock_width, 84);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_set_style_radius(dock, 20, 0);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0xdde5f4), 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(0xf1f5fd), 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    for (int key = 0; key < APP_COUNT; key++) {
        icons[key] = app_icon(dock, key, 0, 5, 44);
        dots[key] = dock_minimized_dot(dock, 0);
    }
    for (int tier = 1; apps && tier <= 3; tier++)
        if (vehicle_app_running(tier)) dock_state(APP_SPEED + tier - 1, OPEN);
    dock_state(APP_SYSTEM, OPEN);
    pointer_dot = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(pointer_dot);
    lv_obj_set_size(pointer_dot, POINTER_DIAMETER, POINTER_DIAMETER);
    lv_obj_set_style_radius(pointer_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pointer_dot, lv_color_hex(0x25364b), 0);
    lv_obj_set_style_bg_color(pointer_dot, lv_color_hex(0x216be0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pointer_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pointer_dot, lv_color_hex(0xf6f8ff), 0);
    lv_obj_set_style_border_width(pointer_dot, 2, 0);
    lv_obj_remove_flag(pointer_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pointer_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *navigation = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(navigation);
    lv_obj_set_size(navigation, (int)w, BAR_HEIGHT);
    lv_obj_align(navigation, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(navigation, lv_color_hex(0x16243b), 0);
    lv_obj_set_style_bg_opa(navigation, LV_OPA_COVER, 0);
    lv_obj_remove_flag(navigation, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    touch_bar = lv_obj_create(navigation);
    lv_obj_remove_style_all(touch_bar);
    lv_obj_set_size(touch_bar, 188, 8);
    lv_obj_center(touch_bar);
    lv_obj_set_style_radius(touch_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch_bar, lv_color_hex(0xdde5f4), 0);
    lv_obj_set_style_bg_color(touch_bar, lv_color_hex(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(touch_bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(touch_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    printf("[desktop] ready: fullscreen apps and bottom navigation bar\n");
    printf("[desktop] size %ux%u\n", w, h);
    lv_obj_update_layout(screen);
    {
        lv_area_t box;
        lv_obj_get_coords(test_entry, &box);
        printf("[lvgl] tests box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(tabs[0], &box);
        printf("[lvgl] activity box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        if (watch && load9) {
        lv_obj_get_coords(watch, &box);
        printf("[lvgl] watch box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(load9, &box);
        printf("[lvgl] load9 box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        }
        lv_obj_get_coords(icons[APP_LAUNCHER], &box);
        printf("[lvgl] launcher box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(about_button, &box);
        printf("[lvgl] cover box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(back, &box);
        printf("[lvgl] back box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(btn, &box);
        printf("[lvgl] clear box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
    }
    printf("[menu] serial 6 10 13 14 15 16 18 stay on the coverage page\n");
    printf("[lvgl] ready\n");

    /* The shell stack is 8 KB. These stay off that stack. */
    static char shown[480];
    static char text[480];
    static struct menu_detail d;
    uint64_t panel_at = 0;
    shown[0] = 0;
    /* The UI is a service, not a 45-second demonstration. IRQs remain enabled
     * and the scheduler still charges all rendering/input to Tier 3. */
    for (;;) {
        note_disk_poll();
        touch_pressure_memory();
        power_poll();
        const char *console = proc_console_text();
        if (console) {
            shell_append(console);
            printf("[shell] output: %s", console);
        }
        const char *disk_status = note_disk_status();
        static const char *previous_disk_status;
        if (disk_status != previous_disk_status) {
            previous_disk_status = disk_status;
            lv_label_set_text(note_status, disk_status);
            bool has_note = note_disk_text() != NULL;
            monitor_visible(file_entry, has_note);
            monitor_visible(note_icon, has_note);
            const char *name = note_disk_source_name();
            if (name) {
                lv_obj_set_x(source_entry, 20 + has_note * 96);
                lv_label_set_text(source_title, name);
                lv_label_set_text(source_caption, name);
            }
            monitor_visible(source_entry, name != NULL);
            monitor_visible(source_icon, note_disk_source() != NULL);
            unsigned package_size;
            int has_package = note_disk_package(&package_size) != NULL;
            monitor_visible(package_entry, has_package);
            lv_obj_set_y(package_icon, name ? (has_note ? 262 : 164) : 66);
            monitor_visible(package_icon, has_package);
            lv_obj_set_x(package_entry, 20 + (has_note + (name != NULL)) * 96);
            monitor_visible(file_empty, !has_note && !name && !has_package);
            lv_label_set_text(file_empty, !strcmp(disk_status, "Ready") ? "No saved files" : disk_status);
            if (save_since && strcmp(disk_status, "Saving...")) {
                struct app_display_stats after;
                app_display_read(&after);
                printf("[files] %s elapsed_us=%lu frames=%u misses=%u boot_max_gap_us=%u\n",
                       disk_status, now_us()-save_since, after.frames[0]-save_before.frames[0],
                       after.speed_misses-save_before.speed_misses, after.max_gap_us);
                save_since = 0;
            }
        }
        proc_finish_pending();
        if (demo_handle > 0) {
            char line[129];
            if (proc_backend_output(demo_handle, line, sizeof line))
                lv_label_set_text(demo_output, line);
            unsigned frame_number;
            const uint32_t *surface = proc_page_surface(demo_handle, &frame_number);
            if (surface && displayed_frame != frame_number) {
                displayed_frame = frame_number;
                demo_frame.data = (const uint8_t *)surface;
                lv_image_set_src(demo_image, &demo_frame);
                monitor_visible(demo_status, 0);
                monitor_visible(demo_image, 1);
                lv_obj_invalidate(demo_image);
                printf("[wap] page frame complete handle=%ld generation=%u\n", (long)demo_handle, displayed_frame);
            } else if (!surface && proc_pid(demo_handle) < 0) {
                lv_label_set_text(demo_status, "The page process exited before drawing the window.");
                demo_handle = 0;
            }
        }
        take_inputs();
        lv_timer_handler();
        bar_navigate();
        app_desktop_stall_probe();
        app_desktop_poll();
        uint64_t panel_now = now_us();
        if (panel_at && panel_now - panel_at < 100000)
            continue;
        panel_at = panel_now;
        if (clock_label)
            refresh_clock();
        static uint64_t tasks_at;
        if (panel_now - tasks_at >= 500000) {
            refresh_tasks();
            refresh_system_info();
            refresh_system_events();
            if (apps)
                instruments_refresh();
            tasks_at = panel_now;
        }
        if (apps) { poll_trial(); continue; }
        poll_checks();
        if (checks.selected) continue;
        demo_menu_detail(&d);
        if (d.running) {
            lv_snprintf(text, sizeof text,
                "Workload running\n\n%u / %u periods", d.progress, d.total);
        } else if (d.have_window) {
            char t2[48];
            if (d.t2_ok)
                lv_snprintf(t2, sizeof t2, "%u / %u", d.t2_short, d.job_miss);
            else
                lv_snprintf(t2, sizeof t2, "not sampled");
            lv_snprintf(text, sizeof text,
                "WORKLOAD COMPLETE\n\n"
                "Priority 1 missed deadlines       %u\n"
                "Priority 2 short / missed       %s\n\n"
                "Completed response max       %lu us\n"
                "Completed samples                %u\n"
                "Not started / unfinished       %u / %u\n\n"
                "%s",
                d.t1_miss, t2, d.lat_max_us,
                d.samples, d.not_started, d.unfinished,
                d.t1_miss || (d.t2_ok && (d.t2_short || d.job_miss)) ? "Timing requirements were not met." : "No misses in this observed window.");
        } else {
            lv_snprintf(text, sizeof text,
                "SYSTEM CHECKS\n\nCheck stop protection, speedometer timing, CPU quotas and saved note integrity.\n\nScheduler stress adds competing tasks for 100 periods.");
        }
        if (strcmp(text, shown) != 0 || strcmp(text, lv_label_get_text(detail))) {
            lv_label_set_text(detail, text);
            int n = 0;
            while (text[n] && n < (int)sizeof shown - 1) {
                shown[n] = text[n];
                n++;
            }
            shown[n] = 0;
            if (!d.running && d.have_window)
                printf("[lvgl] detail\n%s\n", text);
        }

    }
    return 0;
}
