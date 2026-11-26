#include "ext4_jbd2.h"
#include "quotaos.h"

enum {
    SUPER = 1024, INODE = 256, PER_BLOCK = 16,
    SB = 0, GROUP, BLOCKS, INODES, NODE, DIRECTORY,
};
static struct ext4_transaction tx;
static uint8_t payload[EXT4_BLOCK];

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static unsigned name_length(const char *name)
{
    unsigned n = 0;
    while (name[n]) {
        unsigned char c = name[n];
        if (n == 255 || c < 32 || c == 127 || c == '/') return 0;
        ++n;
    }
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    return n;
}

static unsigned entry_size(unsigned n)
{
    return (8 + n + 3) & ~3u;
}

static int matching_name(const uint8_t *entry, const char *name, unsigned len)
{
    if (entry[6] != len) return 0;
    for (unsigned i = 0; i < len; ++i)
        if (entry[8 + i] != (uint8_t)name[i]) return 0;
    return 1;
}

static int is_set(const uint8_t *map, unsigned n)
{
    return map[n / 8] & (1 << (n % 8));
}

static void mark(uint8_t *map, unsigned n, int used)
{
    if (used)
        map[n / 8] |= 1 << (n % 8);
    else
        map[n / 8] &= ~(1 << (n % 8));
}

static uint32_t find_free(const uint8_t *map, uint32_t first, uint32_t limit)
{
    for (uint32_t i = first; i < limit; ++i)
        if (!is_set(map, i)) return i;
    return 0;
}

static uint32_t single_block(const uint8_t *inode)
{
    if (le32(inode + 32) != 0x80000 || le16(inode + 40) != 0xf30a ||
        le16(inode + 42) != 1 || le16(inode + 44) != 4 ||
        le16(inode + 46) || le32(inode + 52) ||
        le16(inode + 56) != 1 || le16(inode + 58))
        return 0;
    return le32(inode + 60);
}

static int is_data_block(const struct ext4_journal *j, uint32_t block, uint32_t table_end)
{
    return block >= table_end && block < j->blocks &&
           (block < j->base || block >= j->base + j->length);
}

static void write_entry(uint8_t *entry, uint32_t inode, const char *name,
                        unsigned len, unsigned size)
{
    put32(entry, inode);
    put16(entry + 4, size);
    entry[6] = len;
    entry[7] = 1;
    memcpy(entry + 8, name, len);
}

int ext4_journal_change(struct ext4_journal *j, const char *name, const char *data,
                        size_t bytes, int remove)
{
    if (*name == '/') ++name;
    unsigned len = name_length(name);
    if (!len || (!remove && (!bytes || bytes > EXT4_BLOCK))) return EXT4_UNSUPPORTED;
    const struct ext4_block_io *io = j->io;
    tx.count = EXT4_TX_BLOCKS;
    tx.target[SB] = 0;
    tx.target[GROUP] = 1;
    if (io->read(0, tx.data[SB]) || io->read(1, tx.data[GROUP])) return EXT4_IO;
    uint8_t *sb = tx.data[SB] + SUPER, *gd = tx.data[GROUP];
    uint32_t inodes = le32(sb), table = le32(gd + 8);
    uint32_t table_end = table + (inodes + PER_BLOCK - 1) / PER_BLOCK;
    uint32_t bb = le32(gd), ib = le32(gd + 4);
    if (bb < 2 || ib < 2 || bb == ib || bb >= table || ib >= table ||
        table_end > j->base || le16(gd + 18) & ~4u)
        return EXT4_UNSUPPORTED;
    tx.target[BLOCKS] = bb;
    tx.target[INODES] = ib;
    if (io->read(bb, tx.data[BLOCKS]) || io->read(ib, tx.data[INODES]) ||
        io->read(table, tx.data[NODE])) return EXT4_IO;
    const uint8_t *root = tx.data[NODE] + INODE;
    uint32_t dir_block = single_block(root);
    uint8_t *blockmap = tx.data[BLOCKS], *inodemap = tx.data[INODES];
    if ((le16(root) & 0xf000) != 0x4000 || le32(root + 4) != EXT4_BLOCK ||
        !is_data_block(j, dir_block, table_end) || !is_set(blockmap, dir_block))
        return EXT4_UNSUPPORTED;
    tx.target[DIRECTORY] = dir_block;
    if (io->read(dir_block, tx.data[DIRECTORY])) return EXT4_IO;

    uint8_t *dir = tx.data[DIRECTORY], *entry = NULL, *available = NULL;
    for (unsigned off = 0; off < EXT4_BLOCK;) {
        uint8_t *p = dir + off;
        if (EXT4_BLOCK - off < 8) return EXT4_CORRUPT;
        unsigned rec = le16(p + 4), used = le32(p) ? entry_size(p[6]) : 0;
        if (rec < 8 || (rec & 3) || rec > EXT4_BLOCK - off || used > rec ||
            le32(p) > inodes) return EXT4_CORRUPT;
        if (used && matching_name(p, name, len)) {
            if (entry) return EXT4_CORRUPT;
            entry = p;
        }
        if (!available && rec - used >= entry_size(len)) available = p;
        off += rec;
    }
    if (remove && !entry) return EXT4_NOT_FOUND;
    if (!entry && !available) return EXT4_NO_SPACE;

    uint32_t free_inodes = le32(sb + 16), free_blocks = le32(sb + 12);
    if (free_inodes != le16(gd + 14) || free_blocks != le16(gd + 12) ||
        free_inodes > inodes || free_blocks >= j->blocks)
        return EXT4_CORRUPT;
    uint32_t number = entry ? le32(entry) : find_free(inodemap, 10, inodes) + 1;
    if (!entry && (!free_inodes || number == 1)) return EXT4_NO_SPACE;
    if (number < 11 || number > inodes) return EXT4_UNSUPPORTED;
    unsigned index = number - 1;
    uint32_t inode_block = table + index / PER_BLOCK;
    if (inode_block != table && io->read(inode_block, tx.data[NODE])) return EXT4_IO;
    tx.target[NODE] = inode_block;
    uint8_t *inode = tx.data[NODE] + (index % PER_BLOCK) * INODE;
    uint32_t old = entry ? single_block(inode) : 0;
    if (entry && (!is_set(inodemap, index) || entry[7] != 1 ||
        (le16(inode) & 0xf000) != 0x8000 || le16(inode + 26) != 1 ||
        le32(inode + 104) || le16(inode + 118) ||
        !le32(inode + 4) || le32(inode + 4) > EXT4_BLOCK || le32(inode + 108) ||
        !is_data_block(j, old, table_end) || old == dir_block || !is_set(blockmap, old)))
        return EXT4_UNSUPPORTED;

    if (remove) {
        put32(entry, 0);
        memset(inode, 0, INODE);
        mark(inodemap, index, 0);
        ++free_inodes;
    } else {
        uint32_t fresh = find_free(blockmap, table_end, j->blocks);
        if (!free_blocks || !fresh) return EXT4_NO_SPACE;
        if (!is_data_block(j, fresh, table_end) || fresh == dir_block)
            return EXT4_CORRUPT;
        if (!entry) {
            unsigned reserved = le32(available) ? entry_size(available[6]) : 0;
            unsigned size = le16(available + 4) - reserved;
            if (reserved) put16(available + 4, reserved);
            write_entry(available + reserved, number, name, len, size);
            memset(inode, 0, INODE);
            put16(inode, 0100644);
            put16(inode + 26, 1);
            put32(inode + 28, EXT4_BLOCK / 512);
            put32(inode + 32, 0x80000);
            put16(inode + 40, 0xf30a);
            put16(inode + 42, 1);
            put16(inode + 44, 4);
            put16(inode + 56, 1);
            put16(inode + 128, 32);
            mark(inodemap, index, 1);
            --free_inodes;
        }
        put32(inode + 4, bytes);
        put32(inode + 60, fresh);
        mark(blockmap, fresh, 1);
        --free_blocks;
        memset(payload, 0, sizeof payload);
        memcpy(payload, data, bytes);
        if (io->write(fresh, payload) || io->flush() || io->cut(EXT4_CUT_DATA)) return EXT4_IO;
    }
    if (old) {
        mark(blockmap, old, 0);
        ++free_blocks;
    }
    put32(sb + 12, free_blocks);
    put32(sb + 16, free_inodes);
    put16(gd + 12, free_blocks);
    put16(gd + 14, free_inodes);
    return ext4_journal_commit(j, &tx);
}
