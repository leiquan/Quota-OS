#include "ext4_ro.h"
#include "quotaos.h"

enum {
    ELF_LOAD = 1, ELF_STACK = 0x6474e551,
    ELF_X = 1, ELF_R = 4, ELF_EXEC = 2, ELF_AARCH64 = 183,
    ELF_MAX_HEADERS = 8,
};

struct elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_segment {
    uint32_t type, flags;
    uint64_t offset, address, physical, filesz, memsz, align;
};

_Static_assert(sizeof(struct elf_header) == 64, "ELF64 header");
_Static_assert(sizeof(struct elf_segment) == 56, "ELF64 program header");

/* Quota OS console modules execute at VA 0 in a single read/execute page. */
int cli_read_root_elf(const struct ext4_volume *volume, const char *name,
                      char *image, unsigned capacity, unsigned *size)
{
    struct ext4_file file;
    struct elf_header header;
    static const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0};
    int rc = ext4_open_root(volume, name, &file);
    if (rc) return rc;
    if (file.size < sizeof header) return EXT4_UNSUPPORTED;
    rc = ext4_read_file(volume, &file, 0, &header, sizeof header);
    if (rc) return rc;
    for (unsigned i = 0; i < sizeof ident; ++i)
        if (header.ident[i] != ident[i]) return EXT4_UNSUPPORTED;
    if (header.type != ELF_EXEC || header.machine != ELF_AARCH64 ||
        header.version != 1 || header.entry || header.flags ||
        header.ehsize != sizeof header || header.phentsize != sizeof(struct elf_segment) ||
        !header.phnum || header.phnum > ELF_MAX_HEADERS ||
        header.phoff < sizeof header || header.phoff > file.size ||
        header.phnum * sizeof(struct elf_segment) > file.size - header.phoff)
        return EXT4_UNSUPPORTED;

    unsigned end = 0;
    int entry_mapped = 0;
    memset(image, 0, capacity);
    for (unsigned i = 0; i < header.phnum; ++i) {
        struct elf_segment segment;
        rc = ext4_read_file(volume, &file, header.phoff + i * sizeof segment,
                            &segment, sizeof segment);
        if (rc) return rc;
        if (segment.type == ELF_STACK && !(segment.flags & ELF_X)) continue;
        if (segment.type != ELF_LOAD ||
            (segment.flags != ELF_R && segment.flags != (ELF_R | ELF_X)) ||
            !segment.filesz || segment.filesz != segment.memsz ||
            (segment.align > 1 && ((segment.align & (segment.align - 1)) ||
             ((segment.offset ^ segment.address) & (segment.align - 1)))) ||
            segment.address < end || segment.address >= capacity ||
            segment.memsz > capacity - segment.address ||
            segment.offset > file.size || segment.filesz > file.size - segment.offset)
            return EXT4_UNSUPPORTED;
        rc = ext4_read_file(volume, &file, segment.offset,
                            image + segment.address, segment.filesz);
        if (rc) return rc;
        if (segment.address == 0 && (segment.flags & ELF_X) && segment.filesz >= 4)
            entry_mapped = 1;
        end = segment.address + segment.memsz;
    }
    if (!entry_mapped) return EXT4_UNSUPPORTED;
    *size = end;
    return EXT4_OK;
}
