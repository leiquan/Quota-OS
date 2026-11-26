#include "quotaos.h"
#include "desktop.h"

const char app_speed[] = "GPS Speed";
const char app_video[] = "Flight Camera";
const char app_music[] = "Music Player";

static lv_obj_t *app_cards[3];
static lv_obj_t *music_history[8];
static lv_obj_t *app_rate[3], *app_status[3];

static lv_obj_t *app_card(lv_obj_t *screen, int y, const char *title, const char *detail, const char *tier, const char *symbol, uint32_t accent)
{
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_pos(card, 860, y);
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
    lv_obj_set_style_outline_color(card, lv_color_hex(0x8bf2cb), 0);
    return card;
}

static lv_obj_t *status_panel, *status_values[5];
static struct sched_status_snapshot displayed_status;
static int status_pending;

static void status_create(lv_obj_t *screen)
{
    status_panel = app_card(screen, 48, "System status", "", "Tier 1", LV_SYMBOL_SETTINGS, 0x69dfbd);
    lv_obj_set_height(status_panel, 139);
    lv_obj_set_y(lv_obj_get_child(status_panel, 0), 5);
    lv_obj_set_y(lv_obj_get_child(status_panel, 2), 12);
    lv_obj_align(lv_obj_get_child(status_panel, 3), LV_ALIGN_TOP_RIGHT, -50, 9);
    static const char *const titles[] = {"CPU busy (%)", "Reserved / total (KiB)", "Tasks",
                                         "Tier 1 misses", "Tier 2 shortfalls"};
    for (unsigned i = 0; i < 5; ++i) {
        int y = 39 + (int)i * 18;
        lv_obj_t *key = desk_label(status_panel, 16, y, titles[i], 0xc7d7e6);
        lv_obj_set_style_text_font(key, &lv_font_montserrat_14, 0);
        status_values[i] = desk_label(status_panel, 170, y, "--", 0xd8f7ef);
        lv_obj_set_width(status_values[i], 190);
        lv_obj_set_style_text_font(status_values[i], &lv_font_unscii_16, 0);
        lv_obj_set_style_text_align(status_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

void instruments_status_refresh(void)
{
    struct sched_status_snapshot sample;
    if (!status_panel || !sched_system_status_read(&sample) ||
        sample.generation == displayed_status.generation || !sample.window_ticks)
        return;
    lv_label_set_text_fmt(status_values[0], "%u", (unsigned)(100 * sample.busy_ticks / sample.window_ticks));
    lv_label_set_text_fmt(status_values[1], "%u / %u", (unsigned)sample.reserved_kb, (unsigned)sample.total_kb);
    lv_label_set_text_fmt(status_values[2], "%u", sample.tasks);
    char value[24];
    lv_snprintf(value, sizeof value, "%lu", (unsigned long)sample.tier1_misses);
    lv_label_set_text(status_values[3], value);
    lv_snprintf(value, sizeof value, "%lu", (unsigned long)sample.tier2_short);
    lv_label_set_text(status_values[4], value);
    displayed_status = sample;
    status_pending = 1;
    lv_obj_invalidate(status_panel);
}

void instruments_status_flushed(void)
{
    if (!status_pending) return;
    uint64_t commit = ticks_now();
    status_pending = 0;
    const struct sched_status_snapshot *s = &displayed_status;
    printf("[system-status-frame] generation=%lu release=%lu sample=%lu commit=%lu busy=%lu window=%lu reserved_kib=%lu total_kib=%lu tasks=%u t1_misses=%lu t2_short=%lu sample_misses=%u peak_ticks=%lu\n",
           s->generation, s->release_ticks, s->sample_ticks, commit, s->busy_ticks, s->window_ticks,
           s->reserved_kb, s->total_kb, s->tasks, s->tier1_misses, s->tier2_short, s->sample_misses, s->task_peak_ticks);
}

void instruments_create(lv_obj_t *screen, int apps)
{
    status_create(screen);
    app_cards[0] = app_card(screen, 194, app_speed, "Simulated speed (km/h)", "Tier 1", LV_SYMBOL_CHARGE, 0x69dfbd);
    app_cards[1] = app_card(screen, 359, app_video, apps == 2 ?
             "Simulated road  /  25 fps" : "Optional  /  Not running", "Tier 2", LV_SYMBOL_VIDEO, 0x8fcdf5);
    app_cards[2] = app_card(screen, 524, app_music, "Entertainment  /  Simulated playback", "Tier 3", LV_SYMBOL_AUDIO, 0xd8afff);
    lv_obj_t *scale = lv_scale_create(app_cards[0]);
    lv_scale_set_mode(scale, LV_SCALE_MODE_HORIZONTAL_TOP);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 9);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x91adb6), LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0xc8e5eb), LV_PART_INDICATOR);
    lv_scale_set_major_tick_every(scale, 4);
    lv_obj_set_pos(scale, 94, 78);
    lv_obj_set_size(scale, 64, 10);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *bar = lv_bar_create(app_cards[2]);
        lv_obj_set_pos(bar, 75 + i * 10, 89);
        lv_obj_set_size(bar, 6, 32);
        lv_bar_set_range(bar, 0, 200);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x354058), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xd5a6fd), LV_PART_INDICATOR);
        music_history[i] = bar;
    }
    for (int i = 0; i < 3; i++) {
        app_rate[i] = desk_label(app_cards[i], 184, 84, "Measuring FPS", 0xf1f6ff);
        app_status[i] = desk_label(app_cards[i], 184, 109, "", 0xc7d7e6);
    }
}

void instruments_select(int index)
{
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_outline_width(app_cards[i], i == index ? 2 : 0, 0);
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
