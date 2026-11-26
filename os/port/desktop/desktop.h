#pragma once
#include "lvgl.h"

enum { STATUS_BAR_HEIGHT = 40 };

extern const char app_speed[], app_video[], app_music[];
lv_obj_t *desk_label(lv_obj_t *parent, int x, int y, const char *text, uint32_t color);
void instruments_create(lv_obj_t *screen, int mode, int bottom_inset);
lv_obj_t *instruments_page(int index);
void instruments_refresh(void);
