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
static lv_obj_t *count_label;
static lv_obj_t *field;
static unsigned clicks;

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

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int32_t w = lv_area_get_width(area);
    uint32_t stride_px = fb_stride / 4u;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        uint32_t *dst = fb_px + (uint32_t)y * stride_px + (uint32_t)area->x1;
        memcpy(dst, px, (size_t)w * 4u);
        px += w * 4;
    }
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
    clicks++;
    lv_label_set_text_fmt(count_label, "Count: %u", clicks);
    printf("[lvgl] click count %u\n", clicks);
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x101820), 0);
    build_cursor();
    lv_obj_t *cursor = lv_image_create(screen);
    lv_image_set_src(cursor, &cursor_dsc);
    lv_indev_set_cursor(pointer, cursor);
    printf("[lvgl] white background, dark text\n");
    printf("[lvgl] software cursor %dx%d black fill white outline hotspot 0,0\n", CUR_W, CUR_H);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Quota OS");
    lv_obj_set_pos(title, 24, 16);
    lv_obj_set_style_text_color(title, lv_color_hex(0x101820), 0);

    lv_obj_t *btn = lv_button_create(screen);
    lv_obj_set_pos(btn, 24, 70);
    lv_obj_set_size(btn, 180, 56);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x101820), 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "+1");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x101820), 0);
    lv_obj_center(bl);

    count_label = lv_label_create(screen);
    lv_label_set_text(count_label, "Count: 0");
    lv_obj_set_pos(count_label, 24, 150);
    lv_obj_set_style_text_color(count_label, lv_color_hex(0x101820), 0);

    field = lv_textarea_create(screen);
    lv_obj_set_pos(field, 24, 210);
    lv_obj_set_size(field, 400, 120);
    lv_textarea_set_placeholder_text(field, "type here");
    lv_obj_set_style_bg_color(field, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(field, 2, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(0x101820), 0);

    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, field);
    lv_indev_set_group(keypad, group);

    printf("[lvgl] button box 24,70 180x56\n");
    printf("[lvgl] ready\n");

    uint64_t t0 = now_us();
    uint64_t next = t0;
    while (now_us() - t0 < 12000000ull) {
        take_inputs();
        lv_timer_handler();
        if (now_us() >= next) {
            printf("[lvgl] alive count %u text [%s]\n", clicks, lv_textarea_get_text(field));
            next = now_us() + 1000000ull;
        }
    }
    printf("[lvgl] result clicks %u text [%s] drops %u\n", clicks, lv_textarea_get_text(field),
           input_drop_count());
    return clicks > 0 ? 0 : 1;
}
