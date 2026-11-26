#pragma once

#include <stddef.h>
#include "root_service.h"

#define TOUCH_EXT4_BOOTARGS "apps-touch-ext4"
#define TOUCH_EXT4_PRODUCT_BOOTARGS "apps-ext4"
int touch_ext4_enabled(void);
void touch_ext4_scroll_step(void);
int touch_ext4_busy(void);
void touch_ext4_poll(void);
int touch_ext4_shell_command(const char *command, char *reply, size_t capacity);
const char *touch_ext4_shell_poll(void);
int touch_ext4_files_submit(enum root_operation op, const char *name, const char *text, size_t bytes);
const char *touch_ext4_files_poll(enum root_operation *op, int *result);

void touch_ext4_record_io(unsigned operation, unsigned bytes, unsigned long begin, unsigned long end);
void touch_ext4_record_frame(unsigned generation);
