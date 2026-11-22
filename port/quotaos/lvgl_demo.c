/*
 * LVGL adaptation and the demo screen. Calls the kernel framebuffer and
 * the input queue. Does not program fw_cfg, virtio, or the page tables.
 */
#include "quotaos.h"

#define LV_CONF_INCLUDE_SIMPLE
#include "lvgl.h"

#include <stddef.h>

_Static_assert(LV_MEM_SIZE == LVGL_HEAP_BYTES, "LVGL heap must match the admitted size");
_Static_assert(LV_MEM_POOL_EXPAND_SIZE == 0, "LVGL heap must not grow");

static uint8_t draw_buf[LVGL_DRAW_BYTES] __attribute__((aligned(64)));
static uint32_t *fb_px;
static uint32_t fb_w, fb_h, fb_stride;
static lv_obj_t *field;
static lv_obj_t *menu_list;
static lv_obj_t *detail;
static lv_obj_t *page_home;
static lv_obj_t *page_input;
static lv_obj_t *page_cover, *page_apps, *page_files, *page_shell;
static lv_obj_t *shell_input, *shell_output, *source_entry, *source_icon, *source_caption, *note_icon;
static lv_obj_t *shell_history, *shell_prompt, *source_dialog, *source_name, *source_error;
static char shell_transcript[2048];
static lv_obj_t *note_status, *file_entry, *file_empty, *file_metadata, *source_title, *source_metadata;
static uint64_t save_since;
static struct app_display_stats save_before;
static lv_obj_t *task_body;
static lv_obj_t *tabs[3], *info_panel;
static lv_obj_t *info_values[24], *info_crossings;
static lv_obj_t *app_cards[3];
static lv_obj_t *app_rate[3], *app_status[3];
static lv_obj_t *clock_label;
static uint64_t rtc_base;
enum monitor_page { SYSTEM_TASK, SYSTEM_INFO, SYSTEM_TESTS };
static enum monitor_page monitor_tab;
enum app_key { APP_SYSTEM, APP_LAUNCHER, APP_NOTES, APP_FILES, APP_SPEED, APP_VIDEO, APP_MUSIC, APP_SHELL, APP_COUNT };
static lv_obj_t *dock, *icons[APP_COUNT], *dots[APP_COUNT];
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
static const enum app_key page_keys[] = {APP_SYSTEM, APP_NOTES, APP_COUNT, APP_LAUNCHER, APP_FILES, APP_SHELL};

static lv_obj_t *task_lists[3];
static lv_obj_t *task_items[MAX_TASKS];
static lv_obj_t *empty_tasks[3];
static const char app_speed[] = "Speed";
static const char app_video[] = "Dashcam";
static const char app_music[] = "Music Player";
static const char app_monitor[] = "System";
enum { DOCK_STEP = 132, DOCK_ITEM_WIDTH = 120 };

static void sync_dock(void)
{
    enum app_key visible[APP_COUNT] = {APP_LAUNCHER};
    unsigned count = 1;
    for (int key = 0; key < APP_COUNT; key++) {
        lv_obj_add_flag(icons[key], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dots[key], LV_OBJ_FLAG_HIDDEN);
        if (key != APP_LAUNCHER && states[key] != CLOSED) visible[count++] = key;
    }
    if (count == 1)
        for (unsigned i = 0; i < recent_count; i++) visible[count++] = recent[i];
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

static void show_page(int which)
{
    lv_obj_t *win = which == 0 ? page_home : which == 1 ? page_input : which == 2 ? page_cover : which == 3 ? page_apps : which == 4 ? page_files : page_shell;
    lv_obj_remove_flag(win, LV_OBJ_FLAG_HIDDEN);
    dock_state(page_keys[which], OPEN);
    lv_obj_move_foreground(win);
}

#define CUR_W 16
#define CUR_H 16
static uint8_t cursor_px[CUR_W * CUR_H * 4];
static lv_image_dsc_t cursor_dsc;

static void cursor_px_set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t *p = cursor_px + ((y * CUR_W) + x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

/* Tip at (0,0). Black arrow, one-pixel white outline. 1024 bytes, fixed. */
static void build_cursor(void)
{
    static const char *core[CUR_H] = {
        "#...............",
        "##..............",
        "#.#.............",
        "#..#............",
        "#...#...........",
        "#....#..........",
        "#.....#.........",
        "#......#........",
        "#.......#.......",
        "#........#......",
        "#....#####......",
        "#.#.#...........",
        "##..#...........",
        "#...#...........",
        "....#...........",
        "....#...........",
    };
    int black[CUR_H][CUR_W];
    for (int y = 0; y < CUR_H; y++) {
        for (int x = 0; x < CUR_W; x++)
            black[y][x] = core[y][x] == '#';
    }
    for (int y = 0; y < CUR_H; y++) {
        for (int x = 0; x < CUR_W; x++) {
            if (black[y][x]) {
                cursor_px_set(x, y, 0, 0, 0, 255);
                continue;
            }
            int edge = 0;
            for (int dy = -1; dy <= 1 && !edge; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && xx >= 0 && yy < CUR_H && xx < CUR_W && black[yy][xx])
                        edge = 1;
                }
            }
            if (edge)
                cursor_px_set(x, y, 255, 255, 255, 255);
            else
                cursor_px_set(x, y, 0, 0, 0, 0);
        }
    }
    cursor_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cursor_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    cursor_dsc.header.flags = 0;
    cursor_dsc.header.w = CUR_W;
    cursor_dsc.header.h = CUR_H;
    cursor_dsc.header.stride = CUR_W * 4;
    cursor_dsc.data_size = sizeof cursor_px;
    cursor_dsc.data = cursor_px;
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
    int mode = app_desktop_mode();
    for (int32_t y = area->y1; y <= area->y2; y++) {
        uint32_t *dst = fb_px + (uint32_t)y * stride_px;
        int end = area->x2 + 1;
        int reserved = 0;
        if (mode) {
            if (y >= APP_DASH_Y && y < APP_DASH_Y + APP_DASH_H)
                reserved = APP_DASH_W;
            else if (mode == 2 && y >= APP_VIDEO_Y && y < APP_VIDEO_Y + APP_METER_H)
                reserved = APP_METER_W;
            else if (y >= APP_MUSIC_Y && y < APP_MUSIC_Y + APP_MUSIC_SIZE)
                reserved = APP_MUSIC_SIZE;
        }
        if (reserved && area->x1 < APP_DRAW_X + reserved && end > APP_DRAW_X) {
            if (area->x1 < APP_DRAW_X)
                memcpy(dst + area->x1, px, (size_t)(APP_DRAW_X - area->x1) * 4u);
            int right = APP_DRAW_X + reserved;
            if (right < end)
                memcpy(dst + right, px + (right - area->x1) * 4u,
                       (size_t)(end - right) * 4u);
        } else {
            memcpy(dst + area->x1, px, (size_t)w * 4u);
        }
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

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static int x, y, pressed;
    data->continue_reading = false;
    while (ptr_i < nptrs) {
        struct quota_input ev = ptrs[ptr_i++];
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

static void trial_buttons_visible(int visible);
static void select_monitor_tab(enum monitor_page tab)
{
    monitor_tab = tab;
    trial_buttons_visible(tab == SYSTEM_TESTS);
    monitor_visible(task_body, tab == SYSTEM_TASK);
    monitor_visible(menu_list, tab == SYSTEM_TESTS);
    monitor_visible(detail, tab == SYSTEM_TESTS);
    monitor_visible(info_panel, tab == SYSTEM_INFO);
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
    struct running_app *reservation;
    struct task *task;
    int64_t process;
} trial;
static lv_obj_t *approve, *cancel_trial;

static void trial_buttons_visible(int visible)
{
    if (approve) { monitor_visible(approve, visible && trial.pending); monitor_visible(cancel_trial, visible && trial.pending); }
}

static void trial_result(int number, const char *result)
{
    lv_label_set_text(detail, result);
    printf("[quota-test] %d %s\n", number, result);
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
            struct running_app *unexpected = resource_commit(&manifest);
            trial_result(number, !unexpected && (reason == ADMIT_DEVICE || reason == ADMIT_OVERSUB)
                ? "PASS  Launch refused: insufficient CPU quota." : "FAIL  Unexpected admission result.");
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
        trial.process = proc_spawn("quota-test", number == 5 ? 15 : 16, 0,0,0,0,0,0,1,1);
        if (trial.process < 0) { trial_result(number, "Unable to launch EL0 process."); return; }
        trial.task = sched_find("quota-test");
    }
    trial.active = number;
    trial.period = sched_period_now();
    sched_read_system_info(&trial.before);
    app_display_read(&trial.display);
    trial_result(number, "Running  /  sampling 100 periods");
}

static void poll_trial(void)
{
    int number = trial.active;
    uint64_t elapsed = sched_period_now() - trial.period;
    if (!number || elapsed < 100) return;
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
    if (number < 4) {
        if (number == 2) ok = ok && trial.task->jobs && !trial.task->misses;
        ok = ok && peak <= us_to_ticks(number == 3 ? 800 : 400);
        task_kill(trial.task);
        resource_release(trial.reservation);
    } else {
        ok = ok && proc_alive(trial.process);
        if (proc_kill(trial.process)) ok = 0;
    }
    char result[300];
    lv_snprintf(result, sizeof result,
        "%s  /  %lu periods\nPeak CPU %lu us per period\nSpeed %u commits, %u misses\nTier 2 shortfalls %lu, overruns %lu\nTier 3 pool crossings %lu",
        ok ? "PASS" : "FAIL", (unsigned long)elapsed, (unsigned long)ticks_to_us(peak),
        frames, misses, shortfalls, over, crossings);
    trial.active = 0;
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

static void shell_help(char *out, unsigned capacity)
{
    const char *name = note_disk_source_name();
    if (name)
        lv_snprintf(out, capacity, "help / pwd / ls / clear\n"
                    "cc %s -o hello   Compile your source\n"
                    "./hello   Run in Tier 3", name);
    else
        lv_snprintf(out, capacity, "help / pwd / ls / clear\n"
                    "In Notes, choose C Example, then Save As... to save a .c file.\n"
                    "Return here to compile and run it.");
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
        monitor_visible(page_apps, 0);
        dock_state(APP_LAUNCHER, CLOSED);
        show_page(5);
        if (!shell_transcript[0]) {
            char help[200];
            shell_help(help, sizeof help);
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
        char output[200], compile[96];
        const char *name = note_disk_source_name();
        const char *result = "Unknown command. Type help.";
        lv_snprintf(compile, sizeof compile, "cc %s -o hello", name ? name : "hello.c");
        if (!strcmp(command, "help")) {
            shell_help(output, sizeof output);
            result = output;
        }
        else if (!strcmp(command, "pwd"))
            result = "/Desktop";
        else if (!strcmp(command, "ls")) {
            lv_snprintf(output, sizeof output, "%s%s%s",
                        note_disk_text() ? "Note.txt" : "",
                        note_disk_text() && name ? "  " : "",
                        name ? name : "");
            result = output[0] ? output : "Desktop is empty";
        }
        else if (!strcmp(command, "clear")) {
            shell_transcript[0] = 0;
            shell_refresh();
            lv_textarea_set_text(shell_input, "");
            return;
        } else if (name && !strcmp(command, compile)) {
            if (c_demo_compile(note_disk_source()))
                result = "Compile error: check the saved C source.";
            else {
                lv_snprintf(output, sizeof output, "Compiled %s to hello", name);
                result = output;
            }
        }
        else if (command[0] == 'c' && command[1] == 'c' &&
                 (command[2] == 0 || command[2] == ' ')) {
            if (name) lv_snprintf(output, sizeof output, "Usage: %s", compile);
            else shell_help(output, sizeof output);
            result = output;
        }
        else if (!strcmp(command, "./hello"))
            result = c_demo_run() < 0 ? "Run failed: compile first or wait for a free process slot." : "Running hello (Tier 3)";
        char entry[256];
        lv_snprintf(entry, sizeof entry, "quota> %s\n%s", command, result);
        shell_append(entry);
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
    if (id == 208) {
        show_page(0);
        select_monitor_tab(SYSTEM_INFO);
        printf("[desktop] system info opened\n");
        return;
    }
    if (id == 209) {
        demo_menu_start(9);
        select_monitor_tab(SYSTEM_TASK);
        printf("[desktop] task manager opened\n");
        return;
    }
    if (id == 203 || id == 204) {
        show_page(0);
        select_monitor_tab(id == 204 ? SYSTEM_TESTS : SYSTEM_TASK);
        printf("[desktop] %s\n", id == 203 ? "task manager opened" : "system tests opened");
        return;
    }
    if (id == 210) {
        show_page(3);
        printf("[desktop] Apps opened\n");
        return;
    }
    if (id >= 205 && id <= 207) {
        lv_obj_add_flag(page_apps, LV_OBJ_FLAG_HIDDEN);
        if (app_cards[id - 205]) {
            int tier = id - 204;
            if (!vehicle_app_running(tier) && vehicle_app_set_running(tier, 1)) return;
            dock_state(APP_SPEED + tier - 1, OPEN);
            for (int i = 0; i < 3; i++)
                lv_obj_set_style_outline_width(app_cards[i], i == id - 205 ? 2 : 0, 0);
            printf("[desktop] app selected %d\n", id - 205 + 1);
        }
        return;
    }
    if (id == 200) {
        show_page(1);
        lv_group_focus_obj(field);
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

static lv_obj_t *desk_label(lv_obj_t *parent, int x, int y, const char *text, uint32_t color)
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

    lv_obj_t *label = desk_label(button, 0, 0, name, 0xf5f8ff);
    lv_obj_set_width(label, 72);
    lv_obj_set_height(label, 20);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 62);
    if (caption) *caption = label;
    return button;
}

static void hide_window(lv_event_t *e, int minimized)
{
    lv_obj_t *win = lv_event_get_user_data(e);
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);
    enum app_key key = win == page_home ? APP_SYSTEM : win == page_input ? APP_NOTES :
                       win == page_apps ? APP_LAUNCHER : win == page_files ? APP_FILES : win == page_shell ? APP_SHELL : APP_COUNT;
    dock_state(key, minimized ? MINIMIZED : CLOSED);
    printf("[desktop] window hidden\n");
}

static void window_close(lv_event_t *e)
{
    hide_window(e, 0);
}

static void window_minimize(lv_event_t *e)
{
    hide_window(e, 1);
}

static void window_drag(lv_event_t *e)
{
    lv_obj_t *win = lv_event_get_user_data(e);
    lv_obj_move_foreground(win);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        printf("[desktop] moved %ld,%ld\n", (long)lv_obj_get_x(win), (long)lv_obj_get_y(win));
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_PRESSING)
        return;
    lv_point_t v;
    lv_indev_get_vect(lv_indev_active(), &v);
    int x = lv_obj_get_x(win) + v.x;
    int y = lv_obj_get_y(win) + v.y;
    int max_x = (app_desktop_mode() ? APP_DESK_EDGE : (int)fb_w) - lv_obj_get_width(win);
    if (x < 0) x = 0;
    if (x > max_x) x = max_x;
    if (y < 44) y = 44;
    int max_y = (int)fb_h - 94 - lv_obj_get_height(win);
    if (y > max_y) y = max_y;
    lv_obj_set_pos(win, x, y);
}

static lv_obj_t *desktop_window(lv_obj_t *screen, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *win = lv_obj_create(screen);
    lv_obj_set_pos(win, x, y);
    lv_obj_set_size(win, w, h);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_style_radius(win, 16, 0);
    lv_obj_set_style_clip_corner(win, true, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(0xf7f9fc), 0);
    lv_obj_set_style_border_color(win, lv_color_hex(0xc4cfdf), 0);
    lv_obj_set_style_border_width(win, 1, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = lv_obj_create(win);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, w, 52);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0xdce4ef), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xebf0f7), 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bar, window_drag, LV_EVENT_PRESSED, win);
    lv_obj_add_event_cb(bar, window_drag, LV_EVENT_PRESSING, win);
    lv_obj_add_event_cb(bar, window_drag, LV_EVENT_RELEASED, win);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *control = lv_button_create(bar);
        lv_obj_set_pos(control, 4 + i * 52, 2);
        lv_obj_set_size(control, 48, 48);
        lv_obj_set_style_radius(control, 8, 0);
        lv_obj_set_style_border_width(control, 0, 0);
        lv_obj_set_style_shadow_width(control, 0, 0);
        lv_obj_set_style_bg_color(control, lv_color_hex(i ? 0xe0e8f2 : 0xf5e5e9), 0);
        lv_obj_set_style_bg_opa(control, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(control, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(control, lv_color_hex(i ? 0xd2dfef : 0xf0cbd3), LV_STATE_PRESSED);
        lv_obj_t *glyph = desk_label(control, 0, 0, i ? LV_SYMBOL_MINUS : LV_SYMBOL_CLOSE,
                                     i ? 0x334563 : 0x982c40);
        lv_obj_center(glyph);
        lv_obj_add_event_cb(control, i ? window_minimize : window_close, LV_EVENT_CLICKED, win);
    }
    lv_obj_t *label = desk_label(bar, 0, 0, title, 0x263552);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return win;
}

static const struct app_entry {
    const char *name;
    const char *glyph;
    uint32_t color;
    int action;
} app_entries[] = {
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

static lv_obj_t *app_card(lv_obj_t *screen, int y, const char *title, const char *detail)
{
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_pos(card, 860, y);
    lv_obj_set_size(card, 380, 140);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xf1f5fc), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *heading = desk_label(card, 30, 16, title, 0x182338);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, 0);
    desk_label(card, 30, 49, detail, 0x475a7a);
    lv_obj_set_style_outline_color(card, lv_color_hex(0x8bf2cb), 0);
    return card;
}

static int leap_year(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static void refresh_clock(void)
{
    static uint32_t last = UINT32_MAX;
    if (!rtc_base)
        return;
    uint32_t seconds = mmio_rd(rtc_base);
    if (seconds == last)
        return;
    last = seconds;
    /* PL031 supplies UTC. This image uses the Asia/Shanghai display zone. */
    uint64_t local = (uint64_t)seconds + 8u * 3600u;
    unsigned days = (unsigned)(local / 86400u), year = 1970, month = 0;
    while (days >= 365u + leap_year(year))
        days -= 365u + leap_year(year++);
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    while (month < 11) {
        unsigned n = month_days[month] + (month == 1 && leap_year(year));
        if (days < n)
            break;
        days -= n;
        month++;
    }
    unsigned time = (unsigned)(local % 86400u);
    lv_label_set_text_fmt(clock_label, "%04u-%02u-%02u  %02u:%02u:%02u",
                          year, month + 1, days + 1, time / 3600,
                          time / 60 % 60, time % 60);
}

static void refresh_app_stats(uint64_t elapsed)
{
    static struct app_display_stats previous;
    struct app_display_stats current;
    app_display_read(&current);
    if (elapsed) {
        for (int i = 0; i < 3; i++) {
            uint32_t frames = current.frames[i] - previous.frames[i];
            unsigned fps = (unsigned)(((uint64_t)frames * 1000000u + elapsed / 2) / elapsed);
            char rate[32];
            lv_snprintf(rate, sizeof rate, "%u FPS (%s)", fps,
                        clock_is_icount() ? "icount" : "guest clock");
            if (strcmp(rate, lv_label_get_text(app_rate[i])))
                lv_label_set_text(app_rate[i], rate);
            const char *status = frames ? "Updating" : "No new frames";
            if (i == 0)
                status = current.speed_misses > previous.speed_misses ? "Deadline missed" :
                         frames ? "No new misses" : "No new frames";
            else if (i && !vehicle_app_running(i + 1))
                status = "Stopped";
            else if (i == 1 && app_desktop_mode() == 1)
                status = "Not running";
            else if (i == 1 && current.video_paused)
                status = "Test pause";
            else if (i == 2 && current.music_held)
                status = "Demo hold (Tier 3)";
            if (strcmp(status, lv_label_get_text(app_status[i])))
                lv_label_set_text(app_status[i], status);
        }
    }
    previous = current;
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
    lv_obj_set_size(top, (int)w, 40);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x152641), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_70, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *about_button = lv_button_create(top);
    lv_obj_set_pos(about_button, 10, 0);
    lv_obj_set_size(about_button, 108, 40);
    lv_obj_set_style_bg_opa(about_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(about_button, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(about_button, 0, 0);
    lv_obj_t *brand = lv_label_create(about_button);
    lv_label_set_text(brand, "Quota OS");
    lv_obj_set_style_text_color(brand, lv_color_hex(0xffffff), 0);
    lv_obj_center(brand);
    lv_obj_add_event_cb(about_button, on_group, LV_EVENT_CLICKED, (void *)(uintptr_t)201);

    if (dtb_find_bases("arm,pl031", &rtc_base, 1) > 0) {
        clock_label = desk_label(top, 0, 0, "", 0xdce8ff);
        lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, -26, 0);
        refresh_clock();
    }

    int apps = app_desktop_mode();
    if (apps) {
        app_cards[0] = app_card(screen, 116, app_speed, "Tier 1  /  Simulated speed (km/h)");
        app_cards[1] = app_card(screen, 281, app_video, apps == 2 ?
                 "Tier 2  /  25 simulated frames per second" : "Optional  /  Not running");
        app_cards[2] = app_card(screen, 446, app_music, "Tier 3  /  EL0 simulated playback");
        for (int i = 0; i < 3; i++) {
            app_rate[i] = desk_label(app_cards[i], 184, 84, "Measuring FPS", 0x263552);
            app_status[i] = desk_label(app_cards[i], 184, 109, "", 0x475a7a);
        }
    }
    page_home = desktop_window(screen, 88, 124, 720, 460, app_monitor);
    menu_list = lv_obj_create(page_home);
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
            "T2  Oversized request", "T2  Approved launch", "T2  CPU saturation",
            "T3  Normal task", "T3  Infinite loop"
        };
        for (int i = 0; i < 5; i++) add_row(menu_list, 12 + i * 54, 0xe0edff, 0xc5d9fa, scenarios[i], 220+i);
    } else {
    load9 = add_row(menu_list, 20, 0xe0edff, 0xc5d9fa, "Scheduler stress", 9);
    add_row(menu_list, 74, 0xffffff, 0xdbe3f0, "Stop stress", 103);
    add_row(menu_list, 128, 0xe0edff, 0xc5d9fa, "Run system checks", 215);
    watch = add_row(menu_list, 236, 0xe0edff, 0xc5d9fa, "Run & view tasks", 209);
    }
    detail = desk_label(page_home, 252, 124, "Select a test", 0x263552);
    lv_obj_set_size(detail, 436, 220);
    approve = add_row(menu_list, 290, 0xe0edff, 0xc5d9fa, "Allow launch", 225);
    cancel_trial = add_row(menu_list, 290, 0xffffff, 0xdbe3f0, "Cancel", 226);
    lv_obj_set_parent(approve, page_home);
    lv_obj_set_parent(cancel_trial, page_home);
    lv_obj_set_pos(approve, 252, 365);
    lv_obj_set_pos(cancel_trial, 455, 365);
    monitor_visible(approve, 0);
    monitor_visible(cancel_trial, 0);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    static const char *const tab_names[] = {"Task", "Info", "Test"};
    static const int tab_ids[] = {203, 208, 204};
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

    page_input = desktop_window(screen, app_desktop_mode() ? 260 : 650, 238, 540, 358, "Notes");
    note_status = desk_label(page_input, 28, 70, "", 0x64748b);
    note_disk_init();
    field = lv_textarea_create(page_input);
    lv_obj_set_style_anim_duration(field, 700, LV_PART_CURSOR);
    lv_obj_set_pos(field, 28, 108);
    lv_obj_set_size(field, 482, 148);
    lv_textarea_set_one_line(field, false);
    lv_textarea_set_placeholder_text(field, "Start typing...");
    lv_obj_set_style_radius(field, 8, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0xd9e1ee), 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(0x263552), 0);
    lv_obj_t *save = add_row(page_input, 283, 0xdcebf6, 0xc5d9fa, "Save", 214);
    lv_obj_set_pos(save, 398, 283);
    lv_obj_set_size(save, 112, 40);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x254e88), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(save, 0), lv_color_white(), 0);
    lv_obj_t *save_source = add_row(page_input, 283, 0xdcebf6, 0xc5d9fa, "Save As...", 232);
    lv_obj_set_pos(save_source, 156, 283);
    lv_obj_set_width(save_source, 132);
    lv_obj_t *example = add_row(page_input, 283, 0xffffff, 0xdce4ef, "C Example", 236);
    lv_obj_set_pos(example, 294, 283);
    lv_obj_set_width(example, 98);
    lv_obj_t *btn = lv_button_create(page_input);
    lv_obj_set_pos(btn, 28, 283);
    lv_obj_set_size(btn, 112, 40);
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
    lv_indev_set_group(keypad, group);
    lv_obj_add_flag(page_input, LV_OBJ_FLAG_HIDDEN);

    page_cover = desktop_window(screen, app_desktop_mode() ? 310 : 390, 205, 500, 348, "About Quota OS");
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
    lv_obj_add_flag(page_cover, LV_OBJ_FLAG_HIDDEN);

    task_body = lv_obj_create(page_home);
    lv_obj_set_pos(task_body, 12, 108);
    lv_obj_set_size(task_body, 694, 336);
    lv_obj_set_style_bg_opa(task_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(task_body, 0, 0);
    lv_obj_set_style_pad_all(task_body, 0, 0);
    lv_obj_remove_flag(task_body, LV_OBJ_FLAG_SCROLLABLE);
    static const char *const tiers[] = {"Tier 1 / Protected", "Tier 2", "Tier 3"};
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
                i == 1 ? "Dashcam  /  Stopped" : "Music Player  /  Stopped", 0x475a7a);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_style_pad_all(row, 8, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_event_cb(row, row_long_press, LV_EVENT_LONG_PRESSED, NULL);
            stopped_rows[i - 1] = row;
        }
        task_lists[i] = list;
        empty_tasks[i] = desk_label(list, 0, 0, "No active tasks", 0x64748b);
    }
    select_monitor_tab(SYSTEM_TASK);

    page_files = desktop_window(screen, 180, 156, 500, 286, "Files");
    desk_label(page_files, 24, 68, "Desktop", 0x536782);
    lv_obj_t *note_title;
    file_entry = files_row(page_files, "Note.txt", 213, &note_title, &file_metadata);
    source_entry = files_row(page_files, "", 233, &source_title, &source_metadata);
    file_empty = desk_label(page_files, 28, 116, "", 0x64748b);
    lv_obj_add_flag(file_entry, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page_files, LV_OBJ_FLAG_HIDDEN);
    source_icon = desktop_file(screen, 12, 66, "", ".c", 233, &source_caption);
    note_icon = desktop_file(screen, 12, 164, "Note.txt", "TXT", 213, NULL);
    monitor_visible(source_entry, 0);
    monitor_visible(source_icon, 0);
    monitor_visible(note_icon, 0);
    page_shell = desktop_window(screen, 160, 148, 670, 390, "Shell");
    shell_history = lv_obj_create(page_shell);
    lv_obj_set_pos(shell_history, 10, 54);
    lv_obj_set_size(shell_history, 650, 324);
    lv_obj_set_style_bg_color(shell_history, lv_color_hex(0x101216), 0);
    lv_obj_set_style_border_width(shell_history, 0, 0);
    lv_obj_set_style_radius(shell_history, 4, 0);
    lv_obj_set_style_pad_all(shell_history, 12, 0);
    lv_obj_set_scroll_dir(shell_history, LV_DIR_VER);
    shell_output = desk_label(shell_history, 0, 0, "", 0xe4edf9);
    lv_obj_set_width(shell_output, 610);
    lv_label_set_long_mode(shell_output, LV_LABEL_LONG_WRAP);
    shell_prompt = desk_label(shell_history, 0, 0, "quota>", 0xe4edf9);
    shell_input = lv_textarea_create(shell_history);
    lv_textarea_set_one_line(shell_input, true);
    lv_textarea_set_max_length(shell_input, 80);
    lv_obj_set_x(shell_input, 58);
    lv_obj_set_size(shell_input, 554, 26);
    lv_obj_set_style_pad_all(shell_input, 4, 0);
    lv_obj_set_style_text_color(shell_input, lv_color_hex(0xe4edf9), 0);
    lv_obj_set_style_bg_opa(shell_input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shell_input, 0, 0);
    lv_obj_set_style_outline_width(shell_input, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(shell_input, 0, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(group, shell_input);
    lv_obj_add_event_cb(shell_input, on_group, LV_EVENT_READY, (void *)(uintptr_t)231);
    shell_refresh();
    monitor_visible(page_shell, 0);
    page_apps = desktop_window(screen, 136, 132, 540, 416, "Apps");
    static const enum app_key catalog[] = {APP_SYSTEM, APP_FILES, APP_NOTES, APP_SPEED, APP_VIDEO, APP_MUSIC, APP_SHELL};
    for (unsigned i = 0; i < sizeof catalog / sizeof catalog[0]; i++) {
        enum app_key key = catalog[i];
        lv_obj_t *entry = app_icon(page_apps, key, 34 + (int)(i % 3) * 174,
                                   70 + (int)(i / 3) * 108, 56);
        if ((key >= APP_SPEED && key <= APP_MUSIC && !apps) || (key == APP_VIDEO && apps != 2)) {
            lv_obj_add_state(entry, LV_STATE_DISABLED);
            lv_obj_set_style_opa(entry, LV_OPA_50, 0);
        }
    }
    lv_obj_add_flag(page_apps, LV_OBJ_FLAG_HIDDEN);
    screen_width = w;
    dock = lv_obj_create(screen);
    int dock_width = (apps ? 6 : 3) * DOCK_STEP + 12;
    lv_obj_set_pos(dock, ((int)w - dock_width) / 2, (int)h - 96);
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
    build_cursor();
    lv_obj_t *cursor = lv_image_create(lv_layer_sys());
    lv_image_set_src(cursor, &cursor_dsc);
    lv_indev_set_cursor(pointer, cursor);
    printf("[desktop] ready: windows, drag title bars, close/minimize, dock restore\n");
    printf("[desktop] size %ux%u\n", w, h);
    lv_obj_update_layout(screen);
    {
        lv_area_t box;
        lv_obj_get_coords(tabs[SYSTEM_TESTS], &box);
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
            if (note_disk_text())
                lv_label_set_text_fmt(file_metadata, "Plain text  /  %u bytes", note_disk_bytes());
            monitor_visible(file_entry, note_disk_text() != NULL);
            monitor_visible(note_icon, note_disk_text() != NULL);
            const char *name = note_disk_source_name();
            if (name) {
                lv_obj_set_y(source_entry, note_disk_text() ? 168 : 96);
                lv_label_set_text(source_title, name);
                lv_label_set_text_fmt(source_metadata, "C source  /  %u bytes",
                                      (unsigned)lv_strlen(note_disk_source()));
                lv_label_set_text(source_caption, name);
            }
            monitor_visible(source_entry, name != NULL);
            monitor_visible(source_icon, note_disk_source() != NULL);
            monitor_visible(file_empty, note_disk_text() == NULL && note_disk_source() == NULL);
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
        take_inputs();
        lv_timer_handler();
        app_desktop_stall_probe();
        app_desktop_poll();
        uint64_t panel_now = now_us();
        if (panel_at && panel_now - panel_at < 100000)
            continue;
        panel_at = panel_now;
        static uint64_t tasks_at;
        if (panel_now - tasks_at >= 500000) {
            refresh_tasks();
            refresh_system_info();
            if (clock_label)
                refresh_clock();
            if (apps)
                refresh_app_stats(tasks_at ? panel_now - tasks_at : 0);
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
