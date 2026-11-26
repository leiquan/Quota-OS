#include "app_zip.h"
#include "quotaos.h"
#include "wap_app.h"

extern const unsigned char wap_worker_start[] __attribute__((weak));
extern const unsigned char wap_worker_end[] __attribute__((weak));
extern const unsigned char todo_package[], todo_package_end[];

static unsigned char running_package[NOTE_DISK_PACKAGE_CAPACITY];
static unsigned running_size;

static int same_package(const unsigned char *data, unsigned size)
{
    if (size != running_size) return 0;
    for (unsigned i = 0; i < size; ++i)
        if (running_package[i] != data[i]) return 0;
    return 1;
}

#ifdef QUOTA_RUNTIME_VOLUME_BOOT
static const char *const runtime_components[] = {
    "quota-blink", "libv8.so", "node", "ld-linux-aarch64.so.1",
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    "quota-pager-test",
#endif
};
/* ELF64 little-endian records from the ARM64 runtime volume. */
struct runtime_elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct runtime_elf_segment {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

_Static_assert(sizeof(struct runtime_elf_header) == 64, "ELF64 header size");
_Static_assert(sizeof(struct runtime_elf_segment) == 56, "ELF64 program header size");

static unsigned next_component;
static int volume_pending;
static struct runtime_volume_file file;
static uint64_t text_offset;
static struct runtime_elf_header header;
static unsigned next_segment, load_segments;
static int interpreter_seen;
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
static uint64_t probe_data_offset;
static int64_t probe_handle;
#endif

static int prepare_component(void)
{
    if (!file.size) {
        int result = runtime_volume_find(runtime_components[next_component], &file);
        if (result != 1)
            return result;
    }
    if (!next_segment) {
        if (file.size < sizeof header)
            return -1;
        int result = runtime_volume_read_file(&file, 0, &header, sizeof header);
        if (result != 1)
            return result;
        static const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
        for (unsigned i = 0; i < sizeof ident; ++i)
            if (header.ident[i] != ident[i])
                return -1;
        if (header.machine != 183 ||
            header.version != 1 || (header.type != 2 && header.type != 3) ||
            header.ehsize != sizeof header || header.phentsize != sizeof(struct runtime_elf_segment) ||
            !header.phnum || header.phnum > 32 || header.phoff < sizeof header ||
            header.phoff > file.size || header.phnum * sizeof(struct runtime_elf_segment) > file.size - header.phoff ||
            (header.entry & 3) || (!header.entry && next_component != 1))
            return -1;
    }
    if (next_segment < header.phnum) {
        struct runtime_elf_segment segment;
        uint64_t position = header.phoff + next_segment * sizeof segment;
        int result = runtime_volume_read_file(&file, position, &segment, sizeof segment);
        if (result != 1)
            return result;
        if (segment.type == 3) {
            static const char expected[] = "/lib/ld-linux-aarch64.so.1";
            char interpreter[sizeof expected];
            if (interpreter_seen || segment.offset > file.size ||
                segment.filesz != sizeof interpreter ||
                sizeof interpreter > file.size - segment.offset)
                return -1;
            result = runtime_volume_read_file(&file, segment.offset, interpreter,
                                         sizeof interpreter);
            if (result != 1)
                return result;
            if (interpreter[sizeof interpreter - 1] || strcmp(interpreter, expected))
                return -1;
            interpreter_seen = 1;
        }
        if (segment.type == 1) {
            if (segment.offset > file.size || segment.filesz > file.size - segment.offset ||
                segment.filesz > segment.memsz || segment.memsz > UINT64_MAX - segment.vaddr ||
                (segment.flags != 4 && segment.flags != 5 && segment.flags != 6) ||
                (segment.align > 1 && ((segment.align & (segment.align - 1)) ||
                                      ((segment.offset ^ segment.vaddr) & (segment.align - 1)))))
                return -1;
            ++load_segments;
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
            if (!strcmp(runtime_components[next_component], "quota-pager-test")) {
                if (segment.filesz != 4096 || segment.memsz != 4096 ||
                    (segment.offset & 4095) ||
                    !((segment.flags == 5 && segment.vaddr == 0x3000) ||
                      (segment.flags == 6 && segment.vaddr == 0x2000 && segment.offset)))
                    return -1;
                if (segment.flags == 6)
                    probe_data_offset = segment.offset;
            }
#endif
            if (segment.flags == 5 && segment.filesz >= 4) {
                if (!header.entry)
                    text_offset = segment.offset + 1;
                else if (header.entry >= segment.vaddr &&
                         header.entry - segment.vaddr <= segment.filesz - 4)
                    text_offset = segment.offset + (header.entry - segment.vaddr) + 1;
            }
        }
        ++next_segment;
        return 0;
    }

    if (!text_offset || (next_component == 0 && !interpreter_seen) ||
        (next_component == 2 && !interpreter_seen))
        return -1;
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    if (!strcmp(runtime_components[next_component], "quota-pager-test")) {
        if (load_segments != 2 || header.entry != 0x3000 || !probe_data_offset)
            return -1;
        probe_handle = proc_spawn_volume_probe("volume-elf-test", file.offset + text_offset - 1,
                                              file.offset + probe_data_offset, 1);
        return probe_handle > 0 ? 1 : -1;
    }
#endif
    uint32_t instruction;
    int result = runtime_volume_read_file(&file, text_offset - 1, &instruction, sizeof instruction);
    if (result == 1)
        printf("[wap-volume] ELF %s bytes=%lu entry=%lx loads=%u text=%lx sample=%x\n",
               runtime_components[next_component], file.size, header.entry, load_segments,
               text_offset - 1, instruction);
    return result;
}

static void reset_component(void)
{
    file = (struct runtime_volume_file){0};
    text_offset = 0;
    next_segment = 0;
    load_segments = 0;
    interpreter_seen = 0;
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    probe_data_offset = 0;
#endif
}

int wap_volume_prepare_poll(void)
{
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    if (probe_handle > 0 && proc_pid(probe_handle) < 0 && runtime_volume_trim())
        probe_handle = 0;
#endif
    if (!volume_pending)
        return 0;

    int result = prepare_component();
    if (!result)
        return 0;
    reset_component();
    if (result > 0 && ++next_component < sizeof runtime_components / sizeof runtime_components[0])
        return 0;
    volume_pending = 0;
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    if (probe_handle > 0)
        return result;
#endif
    runtime_volume_trim();
    return result;
}
#endif

enum wap_open_result wap_open_saved(int64_t *handle)
{
    unsigned size;
    const unsigned char *data = note_disk_package(&size);
    if (*handle > 0 && proc_pid(*handle) >= 0 && same_package(data, size))
        return WAP_RESTORED;
    static struct app_zip archive;
    const struct app_zip_entry *ui, *backend;
    if (app_zip_read(data, size, &archive) || app_zip_launch_entries(&archive, &ui, &backend))
        return WAP_INVALID;
    printf("[wap] launch entries tier=3 ui=%s bytes=%u backend=%s bytes=%u\n",
           ui->name, ui->size, backend ? backend->name : "-", backend ? backend->size : 0);
    uintptr_t start = (uintptr_t)wap_worker_start, end = (uintptr_t)wap_worker_end;
    running_size = 0;
    *handle = -1;
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
    if (!start) {
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
        if (probe_handle > 0)
            return WAP_VOLUME_PENDING;
#endif
        if (!volume_pending) {
            next_component = 0;
            runtime_volume_init();
            volume_pending = 1;
        }
        return WAP_VOLUME_PENDING;
    }
#endif
    if (!start) return WAP_MISSING_RUNTIME;
    if (end <= start || end - start > UINT32_MAX) return WAP_LOAD_FAILED;
    *handle = proc_spawn_wap_elf(wap_worker_start, end - start, ui->data, ui->size,
                                 backend ? backend->data : NULL, backend ? backend->size : 0);
    if (*handle <= 0) return WAP_LOAD_FAILED;
    if (size <= sizeof running_package) {
        memcpy(running_package, data, size);
        running_size = size;
    }
    return WAP_STARTED;
}

int wap_export_todo(void)
{
    unsigned size = (uintptr_t)todo_package_end - (uintptr_t)todo_package;
    int ok = size <= NOTE_DISK_PACKAGE_CAPACITY && note_disk_save_package(todo_package, size) == 0;
    printf("[zip-app] export queued=%d bytes=%u\n", ok, size);
    return ok;
}
