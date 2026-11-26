#include "quotaos.h"
#include "root_disk.h"
#include "root_service.h"

enum { EMPTY, PENDING, COMPLETE };
static volatile unsigned state;
static enum root_operation operation;
static struct ext4_volume volume;
static char file[256];
static char data[ROOT_SERVICE_BYTES];
static size_t length;
static int result;

static int list_entry(void *context, const char *name, unsigned bytes)
{
    (void)context;
    if (bytes + 1 > sizeof data - length) {
        result = EXT4_NO_SPACE;
        return 1;
    }
    memcpy(data + length, name, bytes);
    length += bytes;
    data[length++] = '\n';
    return 0;
}

static void root_worker(void)
{
    result = volume.read ? EXT4_OK : root_disk_mount(&volume);
    if (!result) {
        switch (operation) {
        case ROOT_LIST: {
            int rc = ext4_list_root(&volume, list_entry, NULL);
            if (rc) result = rc;
            break;
        }
        case ROOT_READ:
            result = ext4_read_root(&volume, file, data, sizeof data, &length);
            break;
        case ROOT_SAVE:
            result = root_disk_overwrite(&volume, file, data, length);
            length = 0;
            break;
        case ROOT_REMOVE:
            result = root_disk_remove(&volume, file);
            break;
        }
    }
    if (result) length = 0;
    /* Publish and retire on this CPU before the frontend can reuse the slot. */
    irq_save();
    asm volatile("" ::: "memory");
    state = COMPLETE;
    task_dead(sched_current());
    asm volatile("svc #0" ::: "memory");
    for (;;) asm volatile("wfi");
}

int root_service_submit(enum root_operation op, const char *name,
                        const void *input, size_t bytes)
{
    if (sched_current()->tier != TIER3 || (unsigned)op > ROOT_REMOVE ||
        (op != ROOT_LIST && !name) || (op != ROOT_SAVE && bytes) ||
        (op == ROOT_SAVE && (!input || !bytes || bytes > sizeof data)))
        return -1;
    if (state != EMPTY) return 1;
    if (op != ROOT_LIST) {
        unsigned n = 0;
        while (n < sizeof file && name[n]) ++n;
        if (!n || n == sizeof file) return -1;
        memcpy(file, name, n + 1);
    }
    struct task *task = task_try_create("root-io", TIER3, KIND_SPIN);
    if (!task) return -1;
    task_set_entry(task, root_worker);
    if (bytes) memcpy(data, input, bytes);
    operation = op;
    length = bytes;
    asm volatile("" ::: "memory");
    state = PENDING;
    task_start(task);
    return 0;
}

int root_service_take(void *output, size_t capacity, size_t *bytes, int *ext4_result)
{
    if (sched_current()->tier != TIER3 || !bytes || !ext4_result) return -1;
    if (state != COMPLETE) return 0;
    asm volatile("" ::: "memory");
    if (length > capacity || (length && !output)) return -1;
    if (length) memcpy(output, data, length);
    *bytes = length;
    *ext4_result = result;
    asm volatile("" ::: "memory");
    state = EMPTY;
    return 1;
}
