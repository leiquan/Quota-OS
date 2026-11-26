#ifndef QUOTA_EXT4_RO_H
#define QUOTA_EXT4_RO_H

#include <stddef.h>
#include <stdint.h>

/* Byte reads return zero on success. Each volume is owned by one caller. */
typedef int (*ext4_read_fn)(void *, uint64_t, void *, size_t);
typedef int (*ext4_dir_fn)(void *, const char *, unsigned);

enum ext4_result {
    EXT4_OK, EXT4_UNSUPPORTED, EXT4_CORRUPT, EXT4_IO, EXT4_NOT_FOUND,
};

struct ext4_volume {
    ext4_read_fn read;
    void *context;
    uint32_t blocks, blocks_per_group, inodes, inodes_per_group, inode_size;
};

struct ext4_file {
    uint8_t inode[128];
    uint32_t size;
};

int ext4_open_root(const struct ext4_volume *, const char *, struct ext4_file *);
int ext4_read_file(const struct ext4_volume *, const struct ext4_file *, uint32_t,
                   void *, size_t);
const char *ext4_result_text(int);

int ext4_mount_ro(struct ext4_volume *, ext4_read_fn, void *, uint64_t);
int ext4_list_root(const struct ext4_volume *, ext4_dir_fn, void *);
int ext4_read_root(const struct ext4_volume *, const char *, void *, size_t, size_t *);

#endif
