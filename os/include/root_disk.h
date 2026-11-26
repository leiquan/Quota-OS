#ifndef QUOTA_ROOT_DISK_H
#define QUOTA_ROOT_DISK_H

#include "ext4_ro.h"

/* QEMU virt bus 30 is reserved for the opt-in writable root test disk. */
#define ROOT_RW_MMIO 0x0a003c00ull
#define ROOT_RW_BOOTARGS "cli ext4rw"

/* Single Tier 3 caller. I/O waits may span periods. */
int root_disk_mount(struct ext4_volume *);
int root_disk_overwrite(const struct ext4_volume *, const char *, const char *, size_t);

#endif
