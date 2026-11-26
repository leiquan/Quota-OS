#pragma once

#include <stddef.h>

#define TOUCH_EXT4_BOOTARGS "apps-touch-ext4"
void touch_ext4_scroll_step(void);
int touch_ext4_busy(void);
void touch_ext4_poll(void);
int touch_ext4_shell_command(const char *command, char *reply, size_t capacity);
const char *touch_ext4_shell_poll(void);
