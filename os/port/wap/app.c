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
static const char *const runtime_components[] = {"quota-blink", "libv8.so", "node"};
static unsigned next_component;
static int volume_pending;

int wap_volume_prepare_poll(void)
{
    if (!volume_pending)
        return 0;

    uint64_t offset, size;
    int result = runtime_volume_find(runtime_components[next_component], &offset, &size);
    if (!result)
        return 0;
    if (result > 0) {
        printf("[wap-volume] found %s bytes=%lu\n", runtime_components[next_component], size);
        if (++next_component < sizeof runtime_components / sizeof runtime_components[0])
            return 0;
    }
    volume_pending = 0;
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
