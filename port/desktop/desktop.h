#pragma once
#include "lvgl.h"

extern const char app_speed[], app_video[], app_music[];
lv_obj_t *desk_label(lv_obj_t *parent, int x, int y, const char *text, uint32_t color);
void instruments_create(lv_obj_t *screen, int mode);
void instruments_select(int index);
void instruments_refresh(void);
