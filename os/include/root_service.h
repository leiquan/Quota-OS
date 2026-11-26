#ifndef QUOTA_ROOT_SERVICE_H
#define QUOTA_ROOT_SERVICE_H

#include "ext4_ro.h"

enum root_operation { ROOT_LIST, ROOT_READ, ROOT_SAVE, ROOT_REMOVE };
enum { ROOT_SERVICE_BYTES = 4096 };

/* One Tier 3 frontend owns this mailbox. Requests are copied on submit.
 * Submit returns 0 on acceptance, 1 if busy, or -1 for invalid/no task.
 * Take returns 1 when it consumes a result, 0 while waiting, or -1 when
 * the caller must supply more output space. ext4_result is set on success. */
int root_service_submit(enum root_operation, const char *, const void *, size_t);
int root_service_take(void *, size_t, size_t *, int *);

#endif
