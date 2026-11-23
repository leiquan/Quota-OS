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

void instruments_create(lv_obj_t *screen, int apps)
{
    app_cards[0] = app_card(screen, 116, app_speed, "Simulated speed (km/h)", "Tier 1", LV_SYMBOL_CHARGE, 0x69dfbd);
    app_cards[1] = app_card(screen, 281, app_video, apps == 2 ?
             "Simulated road  /  25 fps" : "Optional  /  Not running", "Tier 2", LV_SYMBOL_VIDEO, 0x8fcdf5);
    app_cards[2] = app_card(screen, 446, app_music, "Entertainment  /  Simulated playback", "Tier 3", LV_SYMBOL_AUDIO, 0xd8afff);
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
                for (int j = 0; j < 7; j++)
                    lv_bar_set_value(music_history[j], lv_bar_get_value(music_history[j + 1]), LV_ANIM_OFF);
                lv_bar_set_value(music_history[7], fps > 200 ? 200 : (int)fps, LV_ANIM_OFF);
            }
            char rate[32];
            lv_snprintf(rate, sizeof rate, "%u FPS (%s)", fps,
                        clock_is_icount() ? "icount" : "guest clock");
            if (strcmp(rate, lv_label_get_text(app_rate[i])))
                lv_label_set_text(app_rate[i], rate);
            const char *status = frames ? "Updating" : "No new frames";
            char deadlines[48];
            if (i == 0) {
                lv_snprintf(deadlines, sizeof deadlines, "%u deadline misses", (unsigned)current.speed_misses);
                status = deadlines;
            }
            else if (i && !vehicle_app_running(i + 1))
                status = "Stopped";
            else if (i == 1 && app_desktop_mode() == 1)
                status = "Not running";
            else if (i == 1 && current.video_paused)
                status = "Test pause";
            if (strcmp(status, lv_label_get_text(app_status[i])))
                lv_label_set_text(app_status[i], status);
        }
    }
    previous = current;
}
