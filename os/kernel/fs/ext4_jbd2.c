#include "ext4_jbd2.h"
#include "quotaos.h"

enum {
    SUPER = 1024, INCOMPAT = SUPER + 96, RECOVER = 4,
    MAGIC = 0xc03b3998, DESCRIPTOR = 1, COMMIT = 2, SUPER_V2 = 4,
    ESCAPE = 1, SAME_UUID = 2, LAST = 8,
};
static uint8_t buffer[EXT4_BLOCK];
static uint8_t journal_super[EXT4_BLOCK];

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static void header(uint32_t type, uint32_t sequence)
{
    memset(buffer, 0, sizeof buffer);
    put_be32(buffer, MAGIC);
    put_be32(buffer + 4, type);
    put_be32(buffer + 8, sequence);
}

static int target_ok(const struct ext4_journal *j, uint32_t target)
{
    return target < j->blocks && (target < j->base || target >= j->base + j->length);
}

static int set_recovery(const struct ext4_journal *j, int enable)
{
    if (j->io->read(0, buffer)) return EXT4_IO;
    if (enable)
        buffer[INCOMPAT] |= RECOVER;
    else
        buffer[INCOMPAT] &= ~RECOVER;
    return j->io->write(0, buffer) || j->io->flush() ? EXT4_IO : EXT4_OK;
}

static int retire(struct ext4_journal *j)
{
    put_be32(journal_super + 24, ++j->sequence);
    put_be32(journal_super + 28, 0);
    if (j->io->write(j->base, journal_super) || j->io->flush()) return EXT4_IO;
    return set_recovery(j, 0);
}

static int recover(struct ext4_journal *j, uint32_t start)
{
    if (!start) return set_recovery(j, 0);
    if (start != j->first) return EXT4_UNSUPPORTED;
    if (j->io->read(j->base + start, buffer)) return EXT4_IO;
    if (be32(buffer) != MAGIC || be32(buffer + 8) != j->sequence)
        return retire(j);
    if (be32(buffer + 4) != DESCRIPTOR) return EXT4_UNSUPPORTED;
    uint32_t target[EXT4_TX_BLOCKS], flags[EXT4_TX_BLOCKS], count = 0;
    unsigned offset = 12;
    for (; count < EXT4_TX_BLOCKS; ++count) {
        target[count] = be32(buffer + offset);
        flags[count] = be32(buffer + offset + 4);
        offset += 8;
        if (!target_ok(j, target[count]) || flags[count] & ~(ESCAPE | SAME_UUID | LAST))
            return EXT4_CORRUPT;
        for (unsigned i = 0; i < count; ++i)
            if (target[i] == target[count]) return EXT4_CORRUPT;
        if (!(flags[count] & SAME_UUID)) {
            for (unsigned i = 0; i < sizeof j->uuid; ++i)
                if (buffer[offset + i] != j->uuid[i]) return EXT4_UNSUPPORTED;
            offset += sizeof j->uuid;
        } else if (!count) return EXT4_CORRUPT;
        if (flags[count] & LAST) break;
    }
    if (count == EXT4_TX_BLOCKS) return EXT4_UNSUPPORTED;
    ++count;
    uint32_t commit = j->base + start + count + 1;
    if (j->io->read(commit, buffer)) return EXT4_IO;
    if (be32(buffer) != MAGIC || be32(buffer + 8) != j->sequence)
        return retire(j);
    if (be32(buffer + 4) != COMMIT) return EXT4_UNSUPPORTED;
    if (j->io->read(commit + 1, buffer)) return EXT4_IO;
    if (be32(buffer) == MAGIC && be32(buffer + 8) == j->sequence + 1)
        return EXT4_UNSUPPORTED;
    for (unsigned i = 0; i < count; ++i) {
        if (j->io->read(j->base + start + i + 1, buffer)) return EXT4_IO;
        if (flags[i] & ESCAPE) put_be32(buffer, MAGIC);
        if (!target[i]) buffer[INCOMPAT] |= RECOVER;
        if (j->io->write(target[i], buffer)) return EXT4_IO;
    }
    if (j->io->flush()) return EXT4_IO;
    return retire(j);
}

int ext4_journal_open(struct ext4_journal *j, const struct ext4_block_io *io,
                      uint32_t capacity)
{
    memset(j, 0, sizeof *j);
    if (io->read(0, buffer)) return EXT4_IO;
    const uint8_t *sb = buffer + SUPER;
    if (!(le32(sb + 92) & 4)) return EXT4_NOT_FOUND;
    uint32_t blocks = le32(sb + 4), inodes = le32(sb);
    uint32_t incompat = le32(sb + 96);
    if (le16(sb + 56) != 0xef53 || le32(sb + 76) != 1 ||
        le16(sb + 58) != 1 || le32(sb + 20) != 0 ||
        le32(sb + 24) != 2 || le32(sb + 28) != 2 ||
        le32(sb + 92) != 4 || le32(sb + 100) || (incompat & ~RECOVER) != 0x42 ||
        le32(sb + 224) != 8 || le32(sb + 228) || le32(sb + 232) ||
        !blocks || blocks > capacity || blocks > 32768 ||
        blocks > le32(sb + 32) || inodes < 11 || inodes != le32(sb + 40) ||
        inodes > 32768 || le16(sb + 88) != 256 || le32(sb + 84) != 11)
        return EXT4_UNSUPPORTED;
    if (io->read(1, buffer)) return EXT4_IO;
    uint32_t block_bitmap = le32(buffer), inode_bitmap = le32(buffer + 4);
    uint32_t table = le32(buffer + 8);
    uint32_t table_blocks = (inodes + 15) / 16;
    if (block_bitmap < 2 || inode_bitmap < 2 || block_bitmap == inode_bitmap ||
        block_bitmap >= table || inode_bitmap >= table || table >= blocks ||
        table_blocks > blocks - table || (le16(buffer + 18) & ~4u))
        return EXT4_CORRUPT;
    if (io->read(table, buffer)) return EXT4_IO;
    const uint8_t *inode = buffer + 7 * 256;
    uint32_t base = le32(inode + 60), length = le16(inode + 56);
    if ((le16(inode) & 0xf000) != 0x8000 || le32(inode + 32) != 0x80000 ||
        le16(inode + 40) != 0xf30a || le16(inode + 42) != 1 || le16(inode + 46) ||
        le32(inode + 52) || le16(inode + 58) || length < 16 || length > 32768 ||
        base < table + table_blocks || base >= blocks || length > blocks - base ||
        le32(inode + 4) != length * EXT4_BLOCK || le32(inode + 108))
        return EXT4_UNSUPPORTED;
    const uint8_t *root = buffer + 256;
    uint32_t directory = le32(root + 60);
    if ((le16(root) & 0xf000) != 0x4000 || le32(root + 32) != 0x80000 ||
        le32(root + 4) != EXT4_BLOCK || le16(root + 40) != 0xf30a ||
        le16(root + 42) != 1 || le16(root + 46) || le32(root + 52) ||
        le16(root + 56) != 1 || le16(root + 58) ||
        directory < table + table_blocks || directory >= blocks ||
        (directory >= base && directory < base + length))
        return EXT4_UNSUPPORTED;
    if (io->read(base, journal_super)) return EXT4_IO;
    uint32_t first = be32(journal_super + 20), start = be32(journal_super + 28);
    if (be32(journal_super) != MAGIC || be32(journal_super + 4) != SUPER_V2 ||
        be32(journal_super + 12) != EXT4_BLOCK || be32(journal_super + 16) != length ||
        first != 1 || be32(journal_super + 32) || be32(journal_super + 36) ||
        (be32(journal_super + 40) & ~1u) || be32(journal_super + 44) ||
        be32(journal_super + 64) != 1)
        return EXT4_UNSUPPORTED;
    *j = (struct ext4_journal){io, blocks, base, length, first, be32(journal_super + 24), {0}};
    memcpy(j->uuid, journal_super + 48, sizeof j->uuid);
    if (incompat & RECOVER) return recover(j, start);
    return start ? EXT4_CORRUPT : EXT4_OK;
}

int ext4_journal_commit(struct ext4_journal *j, const struct ext4_transaction *tx)
{
    if (!tx->count || tx->count > EXT4_TX_BLOCKS) return EXT4_UNSUPPORTED;
    for (unsigned i = 0; i < tx->count; ++i)
        if (!target_ok(j, tx->target[i])) return EXT4_CORRUPT;
    int rc = set_recovery(j, 1);
    if (rc) return rc;
    uint32_t descriptor = j->base + j->first;
    uint32_t commit = descriptor + tx->count + 1;
    /* Invalidate stale log headers before making this transaction discoverable. */
    memset(buffer, 0, sizeof buffer);
    if (j->io->write(descriptor, buffer) || j->io->write(commit, buffer) ||
        j->io->write(commit + 1, buffer) || j->io->flush()) return EXT4_IO;
    put_be32(journal_super + 28, j->first);
    if (j->io->write(j->base, journal_super) || j->io->flush()) return EXT4_IO;

    header(DESCRIPTOR, j->sequence);
    unsigned offset = 12;
    for (unsigned i = 0; i < tx->count; ++i) {
        uint32_t flags = i ? SAME_UUID : 0;
        if (i + 1 == tx->count) flags |= LAST;
        if (be32(tx->data[i]) == MAGIC) flags |= ESCAPE;
        put_be32(buffer + offset, tx->target[i]);
        put_be32(buffer + offset + 4, flags);
        offset += 8;
        if (!i) {
            memcpy(buffer + offset, j->uuid, sizeof j->uuid);
            offset += sizeof j->uuid;
        }
    }
    if (j->io->write(descriptor, buffer)) return EXT4_IO;
    for (unsigned i = 0; i < tx->count; ++i) {
        memcpy(buffer, tx->data[i], sizeof buffer);
        if (!tx->target[i]) buffer[INCOMPAT] |= RECOVER;
        if (be32(buffer) == MAGIC) put_be32(buffer, 0);
        if (j->io->write(descriptor + i + 1, buffer)) return EXT4_IO;
    }
    if (j->io->flush() || j->io->cut(EXT4_CUT_LOG)) return EXT4_IO;
    header(COMMIT, j->sequence);
    if (j->io->write(commit, buffer) || j->io->flush() || j->io->cut(EXT4_CUT_COMMIT))
        return EXT4_IO;
    for (unsigned i = 0; i < tx->count; ++i) {
        memcpy(buffer, tx->data[i], sizeof buffer);
        if (!tx->target[i]) buffer[INCOMPAT] |= RECOVER;
        if (j->io->write(tx->target[i], buffer) || (!i && j->io->cut(EXT4_CUT_CHECKPOINT)))
            return EXT4_IO;
    }
    if (j->io->flush()) return EXT4_IO;
    return retire(j);
}
