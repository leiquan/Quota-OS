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
static lv_obj_t *legend;
static lv_obj_t *detail;
static lv_obj_t *page_home;
static lv_obj_t *page_input;
static lv_obj_t *page_cover;
static lv_obj_t *page_tasks;
static lv_obj_t *task_rows;

static void show_page(int which)
{
    lv_obj_t *win = which == 0 ? page_home : which == 1 ? page_input : which == 2 ? page_cover : page_tasks;
    lv_obj_remove_flag(win, LV_OBJ_FLAG_HIDDEN);
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
static uint64_t click_stamp, key_stamp, scroll_stamp;
static uint64_t click_dt[SAMP_N], key_dt[SAMP_N], scroll_dt[SAMP_N];
static int n_click, n_key, n_scroll;
static uint32_t flush_n, flush_max_bytes;

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
    int32_t h = lv_area_get_height(area);
    uint32_t stride_px = fb_stride / 4u;
    uint32_t bytes;
    int mode = app_desktop_mode();
    for (int32_t y = area->y1; y <= area->y2; y++) {
        uint32_t *dst = fb_px + (uint32_t)y * stride_px;
        int end = area->x2 + 1;
        int reserved = 0;
        if (mode) {
            if ((y >= APP_DASH_Y && y < APP_DASH_Y + APP_METER_H) ||
                (mode == 2 && y >= APP_VIDEO_Y && y < APP_VIDEO_Y + APP_METER_H))
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
    bytes = (uint32_t)w * (uint32_t)h * 4u;
    flush_n++;
    if (bytes > flush_max_bytes)
        flush_max_bytes = bytes;
    take_dt(&click_stamp, click_dt, &n_click, "click");
    take_dt(&key_stamp, key_dt, &n_key, "key");
    take_dt(&scroll_stamp, scroll_dt, &n_scroll, "scroll");
    lv_display_flush_ready(disp);
}

static uint32_t map_key(uint16_t code)
{
    static const char row1[] = "qwertyuiop";
    static const char row2[] = "asdfghjkl";
    static const char row3[] = "zxcvbnm";
    if (code >= 2 && code <= 10)
        return (uint32_t)('1' + (code - 2));
    if (code == 11)
        return '0';
    if (code >= 16 && code <= 25)
        return (uint32_t)row1[code - 16];
    if (code >= 30 && code <= 38)
        return (uint32_t)row2[code - 30];
    if (code >= 44 && code <= 50)
        return (uint32_t)row3[code - 44];
    if (code == 57)
        return ' ';
    if (code == 28)
        return LV_KEY_ENTER;
    if (code == 14)
        return LV_KEY_BACKSPACE;
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

static void take_inputs(void)
{
    /* Keep events LVGL has not read yet. A refresh can be later than the virtio poll. */
    compact(keys, &nkeys, &key_i);
    compact(ptrs, &nptrs, &ptr_i);
    input_service_poll();
    struct quota_input ev;
    while (input_pop(&ev)) {
        if (ev.kind == QUOTA_INPUT_KEY && nkeys < 32)
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
    data->key = last;
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    while (key_i < nkeys) {
        struct quota_input ev = keys[key_i++];
        uint32_t mapped = map_key(ev.code);
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

static void on_group(lv_event_t *e)
{
    int id = (int)(uintptr_t)lv_event_get_user_data(e);
    printf("[menu] button %d\n", id);
    if (id == 100) {
        scroll_stamp = now_us();
        if (menu_list)
            lv_obj_scroll_by(menu_list, 0, -36, LV_ANIM_OFF);
        printf("[lvgl] scroll once, one row, animation off\n");
        return;
    }
    if (id == 101) {
        printf("[lvgl] runaway not available: no protected process. An unbounded loop is not started.\n");
        return;
    }
    if (id == 102) {
        printf("[lvgl] ram crash not available. Menu 4 refuses an oversize quota. It does not exhaust RAM.\n");
        return;
    }
    if (id == 103) {
        demo_menu_stop();
        return;
    }
    if (id == 104) {
        printf("[lvgl] click and key samples are taken from real events. Panel light is not measured.\n");
        return;
    }
    if (id == 203) {
        show_page(3);
        printf("[desktop] task manager opened\n");
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
    lv_obj_set_style_radius(item, 8, 0);
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

static void window_control(lv_event_t *e)
{
    lv_obj_t *win = lv_event_get_user_data(e);
    lv_obj_add_flag(win, LV_OBJ_FLAG_HIDDEN);
    printf("[desktop] window hidden\n");
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
    lv_obj_set_style_radius(win, 12, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(0xf5f7fc), 0);
    lv_obj_set_style_border_color(win, lv_color_hex(0xb4bfd9), 0);
    lv_obj_set_style_border_width(win, 1, 0);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = lv_obj_create(win);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, w, 52);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xe6ebf5), 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bar, window_drag, LV_EVENT_PRESSED, win);
    lv_obj_add_event_cb(bar, window_drag, LV_EVENT_PRESSING, win);
    lv_obj_add_event_cb(bar, window_drag, LV_EVENT_RELEASED, win);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *dot = lv_button_create(bar);
        lv_obj_set_pos(dot, 4 + i * 52, 2);
        lv_obj_set_size(dot, 48, 48);
        lv_obj_set_style_radius(dot, 10, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_shadow_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i ? 0xdce4f3 : 0xf8dce1), 0);
        lv_obj_t *glyph = desk_label(dot, 0, 0, i ? LV_SYMBOL_MINUS : LV_SYMBOL_CLOSE,
                                     i ? 0x334563 : 0x982c40);
        lv_obj_center(glyph);
        lv_obj_add_event_cb(dot, window_control, LV_EVENT_CLICKED, win);
    }
    lv_obj_t *label = desk_label(bar, 120, 18, title, 0x263552);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return win;
}

static lv_obj_t *dock_item(lv_obj_t *dock, int x, uint32_t color, const char *icon, const char *name, int id)
{
    lv_obj_t *b = lv_button_create(dock);
    lv_obj_set_pos(b, x, 7);
    lv_obj_set_size(b, 80, 56);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_add_event_cb(b, on_group, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, icon);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *n = lv_label_create(b);
    lv_label_set_text(n, name);
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, 2);
    lv_obj_set_style_text_color(n, lv_color_hex(0xffffff), 0);
    return b;
}

static void app_card(lv_obj_t *screen, int y, const char *title, const char *detail)
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
}

static void refresh_tasks(void)
{
    if (lv_obj_has_flag(page_tasks, LV_OBJ_FLAG_HIDDEN))
        return;
    static char rows[3072];
    static const char *const heading[] = {
        "Tier 1  /  Fixed slots", "Tier 2  /  Reserved minimum", "Tier 3  /  Spare CPU"
    };
    size_t used = 0;
    for (int tier = TIER1; tier <= TIER3; tier++) {
        used += lv_snprintf(rows + used, sizeof rows - used, "%s\n", heading[tier - TIER1]);
        int count = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            const struct task *t = sched_task_at(i);
            if (!t || t->tier != tier || t->state == TASK_EMPTY || t->state == TASK_DEAD)
                continue;
            const char *state = t->state == TASK_RUNNING ? "Running" :
                                t->state == TASK_READY ? "Ready" : "Waiting";
            if (used + 64 >= sizeof rows)
                return;
            if (tier == TIER2)
                used += lv_snprintf(rows + used, sizeof rows - used,
                    "  %s   %s   %lu us / 10 ms\n", t->name, state,
                    (unsigned long)ticks_to_us(t->min_t));
            else
                used += lv_snprintf(rows + used, sizeof rows - used,
                    "  %s   %s\n", t->name, state);
            count++;
        }
        used += lv_snprintf(rows + used, sizeof rows - used, "%s\n", count ? "" : "  None\n");
    }
    if (strcmp(rows, lv_label_get_text(task_rows)))
        lv_label_set_text(task_rows, rows);
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
    lv_obj_set_style_bg_color(top, lv_color_hex(0x101d39), 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    desk_label(top, 26, 13, "Quota OS", 0xffffff);

    int apps = app_desktop_mode();
    if (apps) {
        app_card(screen, 116, "Dashboard", "Tier 1  /  100 updates per virtual second");
        app_card(screen, 281, "Video", apps == 2 ?
                 "Tier 2  /  25 simulated frames per second" : "Optional  /  Not running");
        app_card(screen, 446, "Music", "Tier 3  /  EL0 simulated playback");
    }
    page_home = desktop_window(screen, 88, 124, 720, 460, "Activity Monitor");
    menu_list = lv_obj_create(page_home);
    lv_obj_set_pos(menu_list, 0, 52);
    lv_obj_set_size(menu_list, 220, 408);
    lv_obj_set_style_bg_color(menu_list, lv_color_hex(0xeaf0fa), 0);
    lv_obj_set_style_border_width(menu_list, 0, 0);
    lv_obj_set_style_pad_all(menu_list, 12, 0);
    lv_obj_set_style_radius(menu_list, 0, 0);
    lv_obj_remove_flag(menu_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *inb = add_row(menu_list, 36, 0xffffff, 0xdbe3f0, "Notes", 200);
    lv_obj_t *load9 = add_row(menu_list, 90, 0xe0edff, 0xc5d9fa, "Run workload", 9);
    add_row(menu_list, 144, 0xffffff, 0xdbe3f0, "Stop workload", 103);
    add_row(menu_list, 198, 0xffffff, 0xdbe3f0, "Service check", 5);
    add_row(menu_list, 252, 0xffffff, 0xdbe3f0, "App reservation", 2);
    lv_obj_t *covb = add_row(menu_list, 306, 0xffffff, 0xdbe3f0, "About this system", 201);
    detail = desk_label(page_home, 252, 84, "No workload results", 0x263552);
    lv_obj_set_size(detail, 436, 338);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t *tasks_button = add_row(page_home, 409, 0xe4ecff, 0xc5d9fa, "Task Manager", 203);
    lv_obj_set_pos(tasks_button, 252, 409);
    lv_obj_set_width(tasks_button, 180);
    legend = lv_label_create(page_home);
    lv_obj_add_flag(legend, LV_OBJ_FLAG_HIDDEN);

    page_input = desktop_window(screen, app_desktop_mode() ? 260 : 650, 238, 540, 358, "Notes");
    desk_label(page_input, 28, 70, "Notes are cleared on restart.", 0x64748b);
    field = lv_textarea_create(page_input);
    lv_obj_set_pos(field, 28, 108);
    lv_obj_set_size(field, 482, 148);
    lv_textarea_set_one_line(field, false);
    lv_textarea_set_placeholder_text(field, "Start typing...");
    lv_obj_set_style_radius(field, 8, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0xd9e1ee), 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(0x263552), 0);
    lv_obj_t *btn = lv_button_create(page_input);
    lv_obj_set_pos(btn, 28, 283);
    lv_obj_set_size(btn, 112, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x5275d8), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Clear");
    lv_obj_center(bl);
    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, field);
    lv_indev_set_group(keypad, group);
    lv_obj_add_flag(page_input, LV_OBJ_FLAG_HIDDEN);

    page_cover = desktop_window(screen, app_desktop_mode() ? 310 : 390, 205, 500, 348, "About Quota OS");
    desk_label(page_cover, 28, 70, "Version " QUOTAOS_VERSION "    ARM64", 0x374e90);
    lv_obj_t *about = desk_label(page_cover, 28, 111,
        "1280 x 800 display\n"
        "Desktop priority: Tier 3\n"
        "Timing results: Activity Monitor", 0x64748b);
    lv_obj_set_width(about, 440);
    lv_obj_t *back = add_row(page_cover, 233, 0xe4ecff, 0xc5d9fa, "Activity Monitor", 202);
    lv_obj_set_pos(back, 28, 280);
    lv_obj_set_width(back, 440);
    lv_obj_add_flag(page_cover, LV_OBJ_FLAG_HIDDEN);

    page_tasks = desktop_window(screen, app_desktop_mode() ? 250 : 575, 145, 570, 522, "Task Manager");
    lv_obj_t *task_body = lv_obj_create(page_tasks);
    lv_obj_set_pos(task_body, 12, 63);
    lv_obj_set_size(task_body, 544, 444);
    lv_obj_set_style_bg_opa(task_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(task_body, 0, 0);
    lv_obj_set_scroll_dir(task_body, LV_DIR_VER);
    task_rows = desk_label(task_body, 0, 0, "", 0x263552);
    lv_obj_set_width(task_rows, 498);
    lv_obj_add_flag(page_tasks, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dock = lv_obj_create(screen);
    lv_obj_set_pos(dock, ((int)w - 306) / 2, (int)h - 84);
    lv_obj_set_size(dock, 306, 72);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_set_style_radius(dock, 16, 0);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0xc9c9e6), 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(0xe1e2f7), 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    dock_item(dock, 12, 0x4a6bc2, LV_SYMBOL_BARS, "Monitor", 202);
    dock_item(dock, 112, 0xc28c4d, LV_SYMBOL_EDIT, "Notes", 200);
    dock_item(dock, 212, 0x7160b5, LV_SYMBOL_LIST, "About", 201);
    build_cursor();
    lv_obj_t *cursor = lv_image_create(lv_layer_sys());
    lv_image_set_src(cursor, &cursor_dsc);
    lv_indev_set_cursor(pointer, cursor);
    printf("[desktop] ready: windows, drag title bars, close/minimize, dock restore\n");
    printf("[desktop] size %ux%u\n", w, h);
    lv_obj_update_layout(screen);
    {
        lv_area_t box;
        lv_obj_get_coords(load9, &box);
        printf("[lvgl] load9 box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(inb, &box);
        printf("[lvgl] input box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(covb, &box);
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
    static char leg_shown[160];
    static char text[480];
    static struct menu_detail d;
    uint64_t panel_at = 0;
    shown[0] = 0;
    leg_shown[0] = 0;
    /* The UI is a service, not a 45-second demonstration. IRQs remain enabled
     * and the scheduler still charges all rendering/input to Tier 3. */
    for (;;) {
        take_inputs();
        lv_timer_handler();
        app_desktop_poll();
        uint64_t panel_now = now_us();
        if (panel_at && panel_now - panel_at < 100000)
            continue;
        panel_at = panel_now;
        static uint64_t tasks_at;
        if (panel_now - tasks_at >= 500000) {
            refresh_tasks();
            tasks_at = panel_now;
        }
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
                "No workload results.\nSelect Run workload to measure deadlines.");
        }
        if (strcmp(text, shown) != 0) {
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
        {
            char leg[160];
            demo_menu_legend(leg, (int)sizeof leg);
            if (strcmp(leg, leg_shown) != 0) {
                lv_label_set_text(legend, leg);
                printf("[lvgl] legend %s\n", leg);
                int n = 0;
                while (leg[n] && n < (int)sizeof leg_shown - 1) {
                    leg_shown[n] = leg[n];
                    n++;
                }
                leg_shown[n] = 0;
            }
        }
    }
    return 0;
}
