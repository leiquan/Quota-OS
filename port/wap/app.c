#include "app_zip.h"
#include "quotaos.h"
#include "wap_app.h"

extern const unsigned char wap_worker_start[] __attribute__((weak));
extern const unsigned char wap_worker_end[] __attribute__((weak));
extern const unsigned char demo_manifest[], demo_manifest_end[];
extern const unsigned char demo_html[], demo_html_end[];
extern const unsigned char demo_node[], demo_node_end[];

enum { PACKAGE_CAPACITY = 2032 };
static unsigned char running_package[PACKAGE_CAPACITY];
static unsigned running_size;

static int same_package(const unsigned char *data, unsigned size)
{
    if (size != running_size) return 0;
    for (unsigned i = 0; i < size; ++i)
        if (running_package[i] != data[i]) return 0;
    return 1;
}

enum wap_open_result wap_open_saved(int64_t *handle)
{
    unsigned size;
    const unsigned char *data = note_disk_package(&size);
    static struct app_zip archive;
    const struct app_zip_entry *ui, *backend;
    if (app_zip_read(data, size, &archive) || app_zip_launch_entries(&archive, &ui, &backend))
        return WAP_INVALID;
    printf("[wap] launch entries tier=3 ui=%s bytes=%u backend=%s bytes=%u\n",
           ui->name, ui->size, backend ? backend->name : "-", backend ? backend->size : 0);
    if (*handle > 0 && proc_pid(*handle) >= 0 && same_package(data, size))
        return WAP_RESTORED;
    uintptr_t start = (uintptr_t)wap_worker_start, end = (uintptr_t)wap_worker_end;
    running_size = 0;
    *handle = -1;
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

int wap_export_demo(void)
{
    const struct app_zip_entry entries[] = {
        {"app.json", demo_manifest, (uintptr_t)demo_manifest_end - (uintptr_t)demo_manifest},
        {"app.html", demo_html, (uintptr_t)demo_html_end - (uintptr_t)demo_html},
        {"app.js", demo_node, (uintptr_t)demo_node_end - (uintptr_t)demo_node}
    };
    static unsigned char bytes[PACKAGE_CAPACITY];
    int size = app_zip_write(entries, sizeof entries / sizeof entries[0], bytes, sizeof bytes);
    int ok = size > 0 && note_disk_save_package(bytes, size) == 0;
    printf("[zip-app] export queued=%d bytes=%d\n", ok, size);
    return ok;
}
