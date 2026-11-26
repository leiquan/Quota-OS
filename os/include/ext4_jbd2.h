#ifndef QUOTA_EXT4_JBD2_H
#define QUOTA_EXT4_JBD2_H

#include "ext4_ro.h"

enum { EXT4_BLOCK = 4096, EXT4_TX_BLOCKS = 6 };
enum ext4_cut_point { EXT4_CUT_DATA = 1, EXT4_CUT_LOG, EXT4_CUT_COMMIT, EXT4_CUT_CHECKPOINT };

struct ext4_block_io {
    int (*read)(uint32_t, void *);
    int (*write)(uint32_t, const void *);
    int (*flush)(void);
    int (*cut)(enum ext4_cut_point);
};
struct ext4_journal {
    const struct ext4_block_io *io;
    uint32_t blocks, base, length, first, sequence;
    uint8_t uuid[16];
};
struct ext4_transaction {
    uint32_t count, target[EXT4_TX_BLOCKS];
    uint8_t data[EXT4_TX_BLOCKS][EXT4_BLOCK];
};

int ext4_journal_open(struct ext4_journal *, const struct ext4_block_io *, uint32_t);
int ext4_journal_commit(struct ext4_journal *, const struct ext4_transaction *);
int ext4_journal_change(struct ext4_journal *, const char *, const char *, size_t, int);

#endif
