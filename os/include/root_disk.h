#ifndef QUOTA_ROOT_DISK_H
#define QUOTA_ROOT_DISK_H

#include "ext4_ro.h"

/* Open the sole read-only VirtIO disk. Only call on the Tier 3 shell task. */
/* Mount and read from a single Tier 3 caller. I/O waits may span periods. */
int root_disk_mount(struct ext4_volume *);

#endif
