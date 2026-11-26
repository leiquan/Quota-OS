#include "quotaos.h"
#include "desktop.h"

const char app_speed[] = "GPS Speed";
const char app_video[] = "Flight Camera";
const char app_music[] = "Music Player";

static lv_obj_t *app_pages[3], *app_frames[3];
static lv_image_dsc_t app_images[3];
static lv_obj_t *music_history[8];
static lv_obj_t *app_rate[3], *app_status[3];

static lv_obj_t *app_card(lv_obj_t *screen, int y, const char *title, const char *detail, const char *tier, const char *symbol, uint32_t accent)
{
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_size(card, 380, 140);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x526782), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x182338), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(0x22344d), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_HOR, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *heading = desk_label(card, 30, 16, title, 0xf5f8fd);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, 0);
    desk_label(card, 30, 49, detail, 0xb8cadd);
    lv_obj_t *icon = desk_label(card, 332, 18, symbol, accent);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_t *tier_label = desk_label(card, 0, 0, tier, accent);
    lv_obj_align(tier_label, LV_ALIGN_TOP_RIGHT, -22, 49);
    return card;
}

static void refresh_frames(lv_timer_t *timer)
{
    (void)timer;
    for (int i = 0; i < 3; i++)
        if (!lv_obj_has_flag(app_pages[i], LV_OBJ_FLAG_HIDDEN))
            lv_obj_invalidate(app_frames[i]);
}

void instruments_create(lv_obj_t *screen, int apps, int bottom_inset)
{
    const char *titles[] = {app_speed, app_video, app_music};
    const char *details[] = {"Simulated speed (km/h)", apps == 2 ?
                            "Simulated road / 25 fps" : "Optional / Not running",
                            "Entertainment / Simulated playback"};
    const char *tiers[] = {"Tier 1", "Tier 2", "Tier 3"};
    const char *symbols[] = {LV_SYMBOL_CHARGE, LV_SYMBOL_VIDEO, LV_SYMBOL_AUDIO};
    const uint32_t accents[] = {0x69dfbd, 0x8fcdf5, 0xd8afff};
    const unsigned widths[] = {APP_DASH_W, APP_METER_W, APP_MUSIC_SIZE};
    const unsigned heights[] = {APP_DASH_H, APP_METER_H, APP_MUSIC_SIZE};
    lv_obj_t *cards[3];
    for (int i = 0; i < 3; i++) {
        lv_obj_t *page = lv_obj_create(screen);
        app_pages[i] = page;
        lv_obj_set_pos(page, 0, STATUS_BAR_HEIGHT);
        lv_obj_set_size(page, DISPLAY_WIDTH, DISPLAY_HEIGHT - STATUS_BAR_HEIGHT - bottom_inset);
        lv_obj_set_style_bg_color(page, lv_color_hex(0x101a2b), 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_radius(page, 0, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
        cards[i] = app_card(page, 82, titles[i], details[i], tiers[i], symbols[i], accents[i]);
        app_images[i] = (lv_image_dsc_t){
            .header = {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_XRGB8888,
                .w = widths[i],
                .h = heights[i],
                .stride = widths[i] * 4,
            },
            .data_size = widths[i] * heights[i] * 4,
            .data = (const uint8_t *)vehicle_app_pixels(i + TIER1),
        };
        app_frames[i] = lv_image_create(page);
        lv_image_set_src(app_frames[i], &app_images[i]);
        lv_image_set_scale(app_frames[i], 1024);
        lv_image_set_antialias(app_frames[i], false);
        lv_obj_align(app_frames[i], LV_ALIGN_TOP_MID, 0, 340);
    }
    lv_timer_create(refresh_frames, 33, NULL);
    lv_obj_t *scale = lv_scale_create(cards[0]);
    lv_scale_set_mode(scale, LV_SCALE_MODE_HORIZONTAL_TOP);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 9);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x91adb6), LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0xc8e5eb), LV_PART_INDICATOR);
    lv_scale_set_major_tick_every(scale, 4);
    lv_obj_set_pos(scale, 94, 78);
    lv_obj_set_size(scale, 64, 10);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *bar = lv_bar_create(cards[2]);
        lv_obj_set_pos(bar, 75 + i * 10, 89);
        lv_obj_set_size(bar, 6, 32);
        lv_bar_set_range(bar, 0, 200);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x354058), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xd5a6fd), LV_PART_INDICATOR);
        music_history[i] = bar;
    }
    for (int i = 0; i < 3; i++) {
        app_rate[i] = desk_label(cards[i], 184, 84, "Measuring FPS", 0xf1f6ff);
        app_status[i] = desk_label(cards[i], 184, 109, "", 0xc7d7e6);
    }
}

lv_obj_t *instruments_page(int index)
{
    return app_pages[index];
}

void instruments_refresh(void)
{
    static struct app_display_stats previous;
    struct app_display_stats current;
    app_display_read(&current);
    if (previous.sample_tick) {
        for (int i = 0; i < 3; i++) {
            uint32_t frames = current.frames[i] - previous.frames[i];
            uint64_t elapsed = ticks_to_us(current.sample_tick - previous.sample_tick);
            /* Count complete commit intervals. Include a stalled tail if the producer is overdue. */
            if (i < 2 && frames && previous.commit_tick[i] && current.commit_tick[i] > previous.commit_tick[i] &&
                (i == 0 || previous.video_epoch == current.video_epoch) &&
                ticks_to_us(current.sample_tick - current.commit_tick[i]) <= (i ? 41000u : 11000u))
                elapsed = ticks_to_us(current.commit_tick[i] - previous.commit_tick[i]);
            if (!elapsed) continue;
            unsigned fps = (unsigned)(((uint64_t)frames * 1000000u + elapsed / 2) / elapsed);
            if (i == 2) {
                int samples[8], scale = 200;
                for (int j = 0; j < 7; j++)
                    samples[j] = lv_bar_get_value(music_history[j + 1]);
                samples[7] = (int)fps;
                for (int j = 0; j < 8; j++)
                    if (samples[j] > scale) scale = samples[j];
                for (int j = 0; j < 8; j++) {
                    lv_bar_set_range(music_history[j], 0, scale);
                    lv_bar_set_value(music_history[j], samples[j], LV_ANIM_OFF);
                }
            }
            char rate[32];
            lv_snprintf(rate, sizeof rate, "%u FPS (%s)", fps,
                        clock_is_icount() ? "icount" : "guest clock");
            if (strcmp(rate, lv_label_get_text(app_rate[i])))
                lv_label_set_text(app_rate[i], rate);
            const char *status = frames ? "Updating" : "No new frames";
            char deadlines[48];
            if (i == 0) {
                lv_snprintf(deadlines, sizeof deadlines, "Missed %u  /  Long gaps %u", current.speed_misses, current.speed_late_gaps);
                status = deadlines;
            }
            else if (i && !vehicle_app_running(i + 1))
                status = "Stopped";
            else if (i == 1 && app_desktop_mode() == 1)
                status = "Not running";
            else if (i == 1 && current.video_paused)
                status = "Test pause";
            else if (i == 1) {
                lv_snprintf(deadlines, sizeof deadlines, "Missed %u  /  Long gaps %u", current.video_missed, current.video_late_gaps);
                status = deadlines;
            }
            if (strcmp(status, lv_label_get_text(app_status[i])))
                lv_label_set_text(app_status[i], status);
        }
    }
    previous = current;
}

#if defined(SYSTEM_STATUS_ENABLED)
enum status_field { STATUS_CPU, STATUS_RAM, STATUS_TASKS, STATUS_TIER1, STATUS_TIER2, STATUS_FIELDS };
static lv_obj_t *status_values[STATUS_FIELDS];

void system_status_create(lv_obj_t *screen)
{
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, DISPLAY_WIDTH - 72, 248);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT + 26);
    lv_obj_set_style_radius(panel, 20, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x162640), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x596c91), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = desk_label(panel, 28, 20, "System status", 0xf4f7fe);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);

    static const char *const labels[STATUS_FIELDS] = {
        "CPU busy", "Memory reserved / total (KiB)", "Tasks",
        "Tier 1 deadline misses", "Tier 2 quota shortfalls",
    };
    for (unsigned i = 0; i < STATUS_FIELDS; i++) {
        int y = 63 + (int)i * 34;
        desk_label(panel, 28, y, labels[i], 0xc3d0e4);
        status_values[i] = desk_label(panel, 365, y, "--", 0xf4f7fe);
        lv_obj_set_width(status_values[i], DISPLAY_WIDTH - 465);
        lv_obj_set_style_text_align(status_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(status_values[i], &lv_font_unscii_16, 0);
    }
}

void system_status_refresh(void)
{
    static uint64_t previous;
    struct sched_status_snapshot sample;
    if (!sched_system_status_read(&sample) || sample.generation == previous)
        return;
    previous = sample.generation;
    uint64_t busy = sample.window_ticks ? sample.busy_ticks * 1000u / sample.window_ticks : 0;
    lv_label_set_text_fmt(status_values[STATUS_CPU], "%3lu.%lu%%",
                          (unsigned long)(busy / 10), (unsigned long)(busy % 10));
    lv_label_set_text_fmt(status_values[STATUS_RAM], "%lu / %lu",
                          (unsigned long)sample.reserved_kb, (unsigned long)sample.total_kb);
    lv_label_set_text_fmt(status_values[STATUS_TASKS], "%u", sample.tasks);
    lv_label_set_text_fmt(status_values[STATUS_TIER1], "%lu", (unsigned long)sample.tier1_misses);
    lv_label_set_text_fmt(status_values[STATUS_TIER2], "%lu", (unsigned long)sample.tier2_short);
}
#endif
