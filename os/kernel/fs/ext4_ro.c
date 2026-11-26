#include "ext4_ro.h"
#include "quotaos.h"

enum {
    BLOCK_BYTES = 4096, DESCRIPTOR_BYTES = 32, ROOT_INODE = 2,
    EXTENTS = 0x40, FILETYPE = 0x2, CLEAN = 1, HAS_EXTENTS = 0x80000,
    MODE_MASK = 0xf000, REGULAR = 0x8000, DIRECTORY = 0x4000,
};

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static int same_name(const char *name, const char *entry, unsigned length)
{
    for (unsigned i = 0; i < length; ++i)
        if (!name[i] || name[i] != entry[i]) return 0;
    return name[length] == 0;
}

static int walk_root(const struct ext4_volume *, ext4_dir_fn, void *,
                     const char *, uint32_t *);

const char *ext4_result_text(int result)
{
    switch (result) {
    case EXT4_OK: return "ok";
    case EXT4_UNSUPPORTED: return "unsupported layout or recovery required";
    case EXT4_CORRUPT: return "invalid ext4 metadata";
    case EXT4_IO: return "block read failed";
    case EXT4_READ_ONLY: return "read-only root disk";
    case EXT4_NOT_FOUND: return "not found";
    default: return "unknown error";
    }
}

int ext4_mount_ro(struct ext4_volume *v, ext4_read_fn read, void *context, uint64_t bytes)
{
    uint8_t sb[1024];
    memset(v, 0, sizeof *v);
    if (bytes < 2 * BLOCK_BYTES) return EXT4_CORRUPT;
    if (read(context, 1024, sb, sizeof sb)) return EXT4_IO;
    if (le16(sb + 56) != 0xef53) return EXT4_CORRUPT;
    uint32_t incompatible = le32(sb + 96);
    /* Accept only the layouts this reader understands. A volume requiring
     * journal recovery advertises an incompatible feature and is rejected. */
    if (le32(sb + 76) != 1 || le16(sb + 58) != CLEAN || le32(sb + 24) != 2 || le32(sb + 28) != 2 ||
        le32(sb + 20) || !(incompatible & EXTENTS) ||
        (incompatible & ~(EXTENTS | FILETYPE)) || (le32(sb + 100) & ~3u))
        return EXT4_UNSUPPORTED;
    uint32_t blocks = le32(sb + 4), blocks_per_group = le32(sb + 32);
    uint32_t inodes = le32(sb), inodes_per_group = le32(sb + 40);
    uint32_t inode_size = le16(sb + 88);
    if (blocks < 2 || blocks > bytes / BLOCK_BYTES ||
        !blocks_per_group || blocks_per_group > 8 * BLOCK_BYTES ||
        inodes < ROOT_INODE || !inodes_per_group ||
        inodes_per_group > 8 * BLOCK_BYTES || inode_size < 128 ||
        inode_size > BLOCK_BYTES || (inode_size & (inode_size - 1)))
        return EXT4_CORRUPT;
    uint64_t groups = ((uint64_t)blocks + blocks_per_group - 1) / blocks_per_group;
    if (inodes > groups * inodes_per_group ||
        groups * DESCRIPTOR_BYTES > (uint64_t)(blocks - 1) * BLOCK_BYTES)
        return EXT4_CORRUPT;
    *v = (struct ext4_volume){read, context, blocks, blocks_per_group,
                              inodes, inodes_per_group, inode_size,
                              groups == 1 && incompatible == (EXTENTS | FILETYPE) &&
                              !le32(sb + 92) && !le32(sb + 100)};
    int rc = walk_root(v, NULL, NULL, NULL, NULL);
    if (rc) memset(v, 0, sizeof *v);
    return rc;
}

static int read_inode(const struct ext4_volume *v, uint32_t number, uint8_t inode[128])
{
    if (!number || number > v->inodes) return EXT4_CORRUPT;
    uint32_t index = number - 1, group = index / v->inodes_per_group;
    uint8_t descriptor[DESCRIPTOR_BYTES];
    uint64_t offset = BLOCK_BYTES + (uint64_t)group * DESCRIPTOR_BYTES;
    if (v->read(v->context, offset, descriptor, sizeof descriptor)) return EXT4_IO;
    uint32_t table = le32(descriptor + 8);
    offset = (uint64_t)table * BLOCK_BYTES + (uint64_t)(index % v->inodes_per_group) * v->inode_size;
    if (!table || offset + 128 > (uint64_t)v->blocks * BLOCK_BYTES) return EXT4_CORRUPT;
    if (v->read(v->context, offset, inode, 128)) return EXT4_IO;
    return le16(inode + 26) ? EXT4_OK : EXT4_CORRUPT;
}

static int extent_block(const struct ext4_volume *v, const uint8_t inode[128],
                        uint32_t logical, uint32_t *block)
{
    const uint8_t *tree = inode + 40;
    if (!(le32(inode + 32) & HAS_EXTENTS) || le16(tree + 6))
        return EXT4_UNSUPPORTED;
    unsigned entries = le16(tree + 2), capacity = le16(tree + 4);
    if (le16(tree) != 0xf30a || entries > capacity || capacity > 4) return EXT4_CORRUPT;
    uint64_t previous_end = 0;
    *block = 0;
    for (unsigned i = 0; i < entries; ++i) {
        const uint8_t *e = tree + 12 + i * 12;
        uint32_t first = le32(e), length = le16(e + 4), physical = le32(e + 8);
        if (length > 32768 || le16(e + 6)) return EXT4_UNSUPPORTED;
        if (!length || first < previous_end || !physical ||
            (uint64_t)physical + length > v->blocks) return EXT4_CORRUPT;
        previous_end = (uint64_t)first + length;
        if (logical >= first && logical < previous_end) *block = physical + (logical - first);
    }
    return EXT4_OK;
}

static int walk_root(const struct ext4_volume *v, ext4_dir_fn callback, void *context,
                     const char *name, uint32_t *found)
{
    uint8_t inode[128], data[BLOCK_BYTES];
    if (!v->read) return EXT4_CORRUPT;
    int rc = read_inode(v, ROOT_INODE, inode);
    if (rc) return rc;
    uint32_t size = le32(inode + 4);
    if ((le16(inode) & MODE_MASK) != DIRECTORY || le32(inode + 108) ||
        !size || size % BLOCK_BYTES)
        return EXT4_CORRUPT;
    /* Limit enumeration to 64 blocks in the first milestone. */
    if (size > 64 * BLOCK_BYTES || (le32(inode + 32) & 0x1000))
        return EXT4_UNSUPPORTED;
    for (uint32_t i = 0; i < size / BLOCK_BYTES; ++i) {
        uint32_t block;
        rc = extent_block(v, inode, i, &block);
        if (rc) return rc;
        if (!block) return EXT4_CORRUPT;
        if (v->read(v->context, (uint64_t)block * BLOCK_BYTES, data, sizeof data)) return EXT4_IO;
        for (unsigned pos = 0; pos < BLOCK_BYTES;) {
            uint8_t *e = data + pos;
            if (BLOCK_BYTES - pos < 8) return EXT4_CORRUPT;
            uint32_t number = le32(e);
            unsigned length = le16(e + 4), chars = e[6];
            if (length < 8 || (length & 3) || length > BLOCK_BYTES - pos ||
                chars > length - 8 || number > v->inodes) return EXT4_CORRUPT;
            if (number && chars) {
                const char *entry = (const char *)e + 8;
                if (name && same_name(name, entry, chars)) {
                    *found = number;
                    return EXT4_OK;
                }
                if (callback && callback(context, entry, chars)) return EXT4_OK;
            }
            pos += length;
        }
    }
    return name ? EXT4_NOT_FOUND : EXT4_OK;
}

int ext4_list_root(const struct ext4_volume *v, ext4_dir_fn callback, void *context)
{
    return walk_root(v, callback, context, NULL, NULL);
}

int ext4_open_root(const struct ext4_volume *v, const char *name, struct ext4_file *file)
{
    if (*name == '/') ++name;
    uint32_t number;
    int rc = walk_root(v, NULL, NULL, name, &number);
    if (rc) return rc;
    rc = read_inode(v, number, file->inode);
    if (rc) return rc;
    if ((le16(file->inode) & MODE_MASK) != REGULAR || le32(file->inode + 108))
        return EXT4_UNSUPPORTED;
    file->size = le32(file->inode + 4);
    return EXT4_OK;
}

int ext4_read_file(const struct ext4_volume *v, const struct ext4_file *file,
                   uint32_t offset, void *output, size_t bytes)
{
    if (offset > file->size || bytes > file->size - offset) return EXT4_CORRUPT;
    uint8_t *dst = output;
    while (bytes) {
        uint32_t block, start = offset % BLOCK_BYTES;
        size_t length = BLOCK_BYTES - start;
        if (length > bytes) length = bytes;
        int rc = extent_block(v, file->inode, offset / BLOCK_BYTES, &block);
        if (rc) return rc;
        if (!block) memset(dst, 0, length);
        else if (v->read(v->context, (uint64_t)block * BLOCK_BYTES + start, dst, length))
            return EXT4_IO;
        dst += length;
        offset += length;
        bytes -= length;
    }
    return EXT4_OK;
}

int ext4_read_root(const struct ext4_volume *v, const char *name, void *output,
                   size_t capacity, size_t *bytes)
{
    *bytes = 0;
    struct ext4_file file;
    int rc = ext4_open_root(v, name, &file);
    if (rc) return rc;
    if (file.size > capacity) return EXT4_UNSUPPORTED;
    rc = ext4_read_file(v, &file, 0, output, file.size);
    if (!rc) *bytes = file.size;
    return rc;
}

/* In-place overwrite for the single-group, no-journal test profile. */
int ext4_overwrite_location(const struct ext4_volume *v, const char *name, size_t bytes,
                            uint64_t *location)
{
    if (!v->overwrite_supported || !bytes || bytes > BLOCK_BYTES)
        return EXT4_UNSUPPORTED;
    struct ext4_file file;
    int rc = ext4_open_root(v, name, &file);
    if (rc) return rc;
    const uint8_t *tree = file.inode + 40;
    if (file.size != bytes || le32(file.inode + 32) != HAS_EXTENTS ||
        le16(tree + 2) != 1 || le16(tree + 16) != 1 || le32(tree + 12) != 0)
        return EXT4_UNSUPPORTED;

    uint32_t block;
    rc = extent_block(v, file.inode, 0, &block);
    if (rc) return rc;
    uint8_t descriptor[DESCRIPTOR_BYTES], allocated;
    if (v->read(v->context, BLOCK_BYTES, descriptor, sizeof descriptor)) return EXT4_IO;
    uint32_t bitmap = le32(descriptor), inode_bitmap = le32(descriptor + 4);
    uint32_t table = le32(descriptor + 8);
    uint64_t table_end = table + ((uint64_t)v->inodes_per_group * v->inode_size +
                                  BLOCK_BYTES - 1) / BLOCK_BYTES;
    if (bitmap < 2 || bitmap >= v->blocks || !table || table_end > v->blocks ||
        block < 2 || block == bitmap || block == inode_bitmap ||
        (block >= table && block < table_end))
        return EXT4_CORRUPT;
    if (v->read(v->context, (uint64_t)bitmap * BLOCK_BYTES + block / 8, &allocated, 1))
        return EXT4_IO;
    if (!(allocated & (1u << (block % 8)))) return EXT4_CORRUPT;
    *location = (uint64_t)block * BLOCK_BYTES;
    return EXT4_OK;
}
