#pragma once
#include <stdint.h>

enum wap_open_result { WAP_INVALID, WAP_MISSING_RUNTIME, WAP_LOAD_FAILED, WAP_STARTED, WAP_RESTORED, WAP_VOLUME_PENDING };
enum wap_open_result wap_open_saved(int64_t *handle);
int wap_export_todo(void);
void wap_install_default_todo(void);
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
int wap_volume_prepare_poll(int64_t *handle);
void wap_volume_end_session(void);
#endif
