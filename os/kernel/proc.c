/*
 * EL0 processes. Each live app has its own TTBR0. EL0 can touch only its
 * code, stack, and draw page. Kernel RAM, devices, the global framebuffer,
 * and other apps stay EL1-only or absent.
 *
 * A handle is generation<<16 | (slot+1). The generation bumps when the
 * kernel accepts a kill or an exception, before any page is freed. Reusing
 * the slot does not revive the old handle.
 *
 * Exit order: revoke submit and bump generation, cancel in-flight draws
 * that still name the old generation, and only then unmap and free.
 * A page still referenced by the display queue is not freed. Missing the
 * 2-period target does not skip that rule.
 *
 * LVGL is not involved. Display clients enter through SVC submit.
 */
#include "quotaos.h"
#include "guest_page_services.h"

#if defined(QUOTA_STANDALONE_NODE) || defined(QUOTA_ELECTRON_RUNTIME) || defined(QUOTA_NODE_WEB)
#define QUOTA_LINUX_APP_RUNTIME 1
#endif

#ifdef QUOTA_WAP_FULL_RUNTIME
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
#define EL0_SLOTS 4
#else
#define EL0_SLOTS 10
#endif
#else
#define EL0_SLOTS 2
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
/* Virtual address bounds for sparse V8 reservations. */
#define HIGH_START (2ull << 30)
#define HIGH_LIMIT (1ull << 47)
#define EL0_POOL 65536
#define ELF_INTERP_BASE 0x10000000u
#define ELF_DSO_BASE 0x10200000u
/* Component builds of Blink map many shared ELF segments. Reserve virtual
 * slots independently of the limit on privately committed DSO pages. */
#ifdef QUOTA_NODE_WEB
#define ELF_DSO_PAGES 131072u
#else
#define ELF_DSO_PAGES 32768u
#endif
#define DSO_PRIVATE_PAGE_LIMIT 8192u
#define ELF_STACK_TOP (ELF_DSO_BASE + ELF_DSO_PAGES * 4096u + 0x200000u)
#define ELF_STACK_PAGES 128u
#define ELF_BRK_PAGES 512u
#define ELF_WINDOW (ELF_STACK_TOP + ELF_BRK_PAGES * 4096u)
#else
#if defined(QUOTA_WAP_INTERPRETER) || defined(QUOTA_RUNTIME_VOLUME_BOOT)
#define EL0_POOL 128
#else
#define EL0_POOL 16
#endif
#define ELF_INTERP_BASE 0x300000u
#define ELF_DSO_BASE 0x380000u
#define ELF_DSO_PAGES 32u
#define ELF_STACK_TOP 0x3f0000u
#define ELF_STACK_PAGES 8u
#define ELF_BRK_PAGES 4u
#define ELF_WINDOW 0x200000u
#endif
#define ELF_BRK_BASE ELF_STACK_TOP
struct runtime_file {
    const char *name;
    const unsigned char *begin, *end;
};
extern const struct runtime_file wap_libs_start[] __attribute__((weak));
extern const struct runtime_file wap_libs_end[] __attribute__((weak));
extern const unsigned char wap_interp_start[] __attribute__((weak));
extern const unsigned char wap_interp_end[] __attribute__((weak));
#define EL0_Q_PER 3
#define EL0_Q_GLOBAL 8
#define EL0_FREQ 2
#define EL0_DRAW 4096u
#define DRAW_VA 0x2000ull
#define ELF_BASE 0x200000u
#define ELF_PRIVATE_WRITABLE (1ull << 55)
#define CODE_VA 0x0ull
#define STACK_VA 0x1000ull
#define USER_SP 0x2000ull

#define PTE_VALID 1ull
#define PTE_PAGE 2ull
#define PTE_AF (1ull << 10)
#define PTE_NG (1ull << 11)
#define PTE_PXN (1ull << 53)
#define PTE_UXN (1ull << 54)
#define ATTR_NORMAL 0ull
#define ATTR_DEVICE (1ull << 2)
#define AP_EL0 (1ull << 6)
#define AP_RO (1ull << 7)
#define SH_INNER (3ull << 8)

extern char user_blob[];
extern char user_blob_end[];

static char console_text[497];
static int console_updated;
static uint32_t page_pixels[GUEST_PREVIEW_HEIGHT][GUEST_PREVIEW_WIDTH];
#ifdef QUOTA_LINUX_APP_RUNTIME
static uint32_t electron_frames[2][GUEST_PREVIEW_HEIGHT][GUEST_PREVIEW_WIDTH];
static unsigned electron_generation;
static int64_t backend_owner, backend_ui;
static char backend_line[129];
static int backend_line_ready;
static struct { uint32_t kind, x, y; } window_input[64];
static unsigned input_head, input_count;
#endif
static int64_t page_owner;
static unsigned page_rows;

void proc_page_input(int64_t handle, unsigned kind, unsigned x, unsigned y)
{
#ifdef QUOTA_LINUX_APP_RUNTIME
    uint64_t flags = irq_save();
    if (handle > 0 && handle == page_owner && input_count < 64) {
        window_input[(input_head + input_count) % 64] = (typeof(window_input[0])){kind, x, y};
        ++input_count;
    }
    irq_restore(flags);
#else
    (void)handle; (void)kind; (void)x; (void)y;
#endif
}

int proc_backend_output(int64_t handle, char *out, unsigned size)
{
#ifdef QUOTA_LINUX_APP_RUNTIME
    uint64_t flags = irq_save();
    if (handle > 0 && handle == backend_ui && backend_line_ready && size) {
        unsigned n = 0;
        while (n+1 < size && backend_line[n] && backend_line[n] != '\n' && backend_line[n] != '\r') {
            out[n] = backend_line[n]; ++n;
        }
        out[n] = 0;
        backend_line_ready = 0;
        irq_restore(flags);
        return 1;
    }
    irq_restore(flags);
#else
    (void)handle; (void)out; (void)size;
#endif
    return 0;
}

const uint32_t *proc_page_surface(int64_t handle, unsigned *frame_number)
{
#ifdef QUOTA_LINUX_APP_RUNTIME
    unsigned generation = __atomic_load_n(&electron_generation, __ATOMIC_ACQUIRE);
    if (handle > 0 && handle == page_owner && generation) {
        *frame_number = generation;
        return electron_frames[(generation - 1) & 1][0];
    }
#endif
    if (handle <= 0 || handle != page_owner ||
        __atomic_load_n(&page_rows, __ATOMIC_ACQUIRE) != GUEST_PREVIEW_HEIGHT)
        return NULL;
    *frame_number = 1;
    return page_pixels[0];
}
enum console_abi { CONSOLE_NONE, CONSOLE_QUOTA, CONSOLE_LINUX_BOOTSTRAP, CONSOLE_PAGE, CONSOLE_VOLUME_PROBE };
#ifdef QUOTA_WAP_FULL_RUNTIME
#define WAP_THREADS 32
struct wap_thread {
    struct irq_frame frame;
    uint64_t fp[66] __attribute__((aligned(16)));
    uint64_t tls, clear_tid, mask, wait_addr, cpu_ticks;
    unsigned tid, wait_bits, no_new_privs;
    uint32_t ns_effective, ns_permitted;
    uint64_t poll_deadline, futex_deadline, poll_saved_mask;
    unsigned poll_mask_active;
};
extern void wap_fp_save(void *);
extern void wap_fp_restore(const void *);
#endif

#define NS_CAP_SYS_ADMIN (1u << 21)
#define NS_CAP_SYS_CHROOT (1u << 18)
#ifdef QUOTA_LINUX_APP_RUNTIME
#define WAP_FDS 128
#else
#define WAP_FDS 64
#endif
#define WAP_OBJECT_SLOTS (EL0_SLOTS * 64)
#define FD_EPOLL 0x40000000u
#define FD_PIPE_R 0x20000000u
#define FD_PIPE_W 0x10000000u
#define FD_WAP_NOTE 0x04000000u
#define FD_PACKET 0x08000000u
struct wap_descriptor {
        const struct runtime_file *file;
        struct file_position *position;
        unsigned event_flags, pipe_id, cloexec, access_mode;
#ifdef QUOTA_WAP_FULL_RUNTIME
        struct packet_socket *socket;
#endif
        uint64_t identity;
        struct poll_object *poll;
    };
#ifdef QUOTA_WAP_FULL_RUNTIME
struct packet_socket {
    unsigned refs, shut, packet_head, packets, passcred, stream, nonblock;
    struct packet_socket *peer;
    struct rights_batch { unsigned count, offset; struct wap_descriptor entries[8]; } rights[4];
    union {
        struct { unsigned size, pid, uid, gid, pid_namespace, pid_ancestors[3]; uint8_t data[1024]; } queue[4];
        uint8_t stream_bytes[4096];
    };
};
#define WAP_PIPE_COUNT (EL0_SLOTS * 8)
static struct wap_pipe {
    unsigned head, size, readers, writers;
    uint8_t buf[4096];
} pipes[WAP_PIPE_COUNT];
static void pipe_put(unsigned flags, unsigned id)
{
    if (flags & FD_PIPE_R) --pipes[id].readers;
    if (flags & FD_PIPE_W) --pipes[id].writers;
}
static struct packet_socket packet_sockets[WAP_OBJECT_SLOTS];

static void discard_rights(struct rights_batch *batch);
static void packet_put(struct packet_socket *socket)
{
    if (--socket->refs) return;
    if (socket->peer) socket->peer->peer = NULL;
    socket->peer = NULL;
    for (unsigned i = 0; i < 4; ++i) discard_rights(&socket->rights[i]);
}
#endif
struct poll_watch {
    uint64_t identity, data;
    unsigned events, fd_index, owner_slot;
};
struct poll_object {
    unsigned refs;
    uint64_t count;
    struct poll_watch watches[WAP_FDS];
};
static struct poll_object poll_objects[WAP_OBJECT_SLOTS];
static uint64_t next_fd_identity;

static struct poll_object *poll_alloc(void)
{
    for (unsigned i = 0; i < WAP_OBJECT_SLOTS; ++i) {
        if (poll_objects[i].refs) continue;
        memset(&poll_objects[i], 0, sizeof poll_objects[i]);
        poll_objects[i].refs = 1;
        return &poll_objects[i];
    }
    return NULL;
}

struct file_position {
    const struct runtime_file *backing;
    unsigned refs, offset, status_flags;
    uint64_t namespace_inode;
};
static struct file_position positions[WAP_OBJECT_SLOTS];

#ifdef QUOTA_WAP_FULL_RUNTIME
static void discard_rights(struct rights_batch *batch)
{
    while (batch->count) {
        struct wap_descriptor *entry = &batch->entries[--batch->count];
        if (entry->socket) packet_put(entry->socket);
        if (entry->position) --entry->position->refs;
    }
}

/* Limit nested descriptor ownership so closing a queue has bounded kernel cost. */
static int can_queue_socket(struct packet_socket *socket, struct packet_socket *destination, unsigned depth)
{
    if (socket == destination || depth == 3) return 0;
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < socket->rights[i].count; ++j) {
            struct packet_socket *child = socket->rights[i].entries[j].socket;
            if (child && !can_queue_socket(child, destination, depth+1)) return 0;
        }
    return 1;
}
#endif

#ifdef QUOTA_LINUX_APP_RUNTIME
#define SHM_OBJECTS 24
#endif

#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
#define DISK_ELF_MAX_SEGMENTS 32
/* Invalid L3 entries carry the disk catalog index, page position and protection. */
#define DISK_DSO_PENDING (1ull << 63)
#define DISK_DSO_POSITION_MASK 0xffffffffffull
struct disk_elf_segment {
    uint64_t file_position;
    unsigned address, file_bytes, memory_bytes, permissions, first_frame;
};
#endif

struct proc {
#ifdef QUOTA_WAP_FULL_RUNTIME
    struct wap_thread threads[WAP_THREADS];
    unsigned active_thread, next_tid, live_threads;
    uint64_t thread_charge_base;
    struct proc *fork_child;
    unsigned fork_cursor, fork_init;
    unsigned exec_cursor, exec_frames, exec_stack, exec_dso;
    uint64_t exec_high, release_cursor;
    uint64_t fork_high_va;
    int building;
    int64_t parent_handle;
    unsigned kernel_uid, kernel_gid, pid_namespace, pid_ancestors[3];
    unsigned stdin_null, stdin_cloexec, empty_root;
    unsigned vfork_pid, vfork_record;
    struct irq_frame suspended_frame;
    uint64_t suspended_tls, suspended_fp[66] __attribute__((aligned(16)));
    uint64_t user_namespace;
    int64_t namespace_owner;
    unsigned namespace_depth, parent_uid, parent_gid;
    unsigned namespace_uid, namespace_gid;
    unsigned uid_mapped, gid_mapped, deny_setgroups;
    struct child_status { int pid, status, done; } children[EL0_SLOTS];
    unsigned fork_record;
    int exit_status;
#endif
    enum console_abi console;
    unsigned document_size;
#ifdef QUOTA_LINUX_APP_RUNTIME
    int64_t temp_owner;
    unsigned shm_pages[SHM_OBJECTS];
#endif
#ifdef QUOTA_NODE_WEB
    const struct runtime_file *page_executable;
#endif
    struct runtime_file package_files[2];
    unsigned char package_contents[2][4096];
    unsigned image_pages;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    struct runtime_volume_file disk_elf;
    struct disk_elf_segment disk_segments[DISK_ELF_MAX_SEGMENTS];
    unsigned disk_segment_count, disk_fault_segment, disk_fault_page;
    int disk_elf_waiting, disk_read_waiting;
    unsigned v8_shared, page_ins;
    const char *stop_reason;
    uint64_t fault_file_offset[2];
    const char *fault_file_name[2];
    unsigned clean_cursor, clean_scanned, clean_pages, evicted_pages;
    uint64_t disk_clean[ELF_DSO_PAGES];
#endif
    unsigned linux_traces;
#ifdef QUOTA_WAP_LOADER_TRACE
    unsigned mapping_traces, failed_map_traces, failed_message_traces, ancillary_traces, stream_stall_traces;
    int trace_in_progress;
    unsigned exec_resumes;
#endif
    unsigned linux_brk;
    uint64_t clear_child_tid;
    uint64_t signal_mask;
    struct {
        uint64_t handler, flags, restorer, mask;
    } signal_actions[64];
    unsigned dso_frame;
    unsigned mapped_dso_pages;
#ifdef QUOTA_WAP_FULL_RUNTIME
    unsigned high_frames, private_page_limit;
    uint32_t pending_note;
    unsigned note_writer_tid;
    struct { uint64_t start, end; } reservations[64];
#endif
    struct {
        uint64_t va;
        unsigned checked, next, needed, reserved;
    } mapping;
    uint64_t dso_used[(ELF_DSO_PAGES + 63) / 64];
    uint64_t dso_anonymous[(ELF_DSO_PAGES + 63) / 64];
    uintptr_t dso_source[ELF_DSO_PAGES];
    struct wap_descriptor files[WAP_FDS];
    int slot;
    int alive;
    int draining;
    int submit_ok;
    uint32_t gen;
    int asid;
    int hold;
    int active_draws; /* Pins held by preemptible display copies, outside qq. */
    int fault_pending;
#ifdef QUOTA_NODE_WEB
    uintptr_t fault_code_pa[2];
#endif
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    struct runtime_volume_file volume_file;
    uint64_t volume_offset, volume_data_offset;
    unsigned volume_data_size;
    const uint8_t *volume_page;
    int volume_waiting, volume_exec;
#endif
    struct el0_fault fault_report;
    int pages[3];
    uint64_t pa[3];
    uint64_t ttbr;
    uint16_t vx, vy, vw, vh;
    struct task *task;
    struct running_app *app;
    uint32_t submits, rej_range, rej_freq, rej_depth;
    uint64_t sub_period;
    uint32_t sub_n;
    int unpaced_draw;
    int pid;
    uint64_t reap_t0;
    uint64_t reap_pct0;
};

struct qent {
    int used;
    int slot;
    uint32_t gen;
    uint64_t pa;
    uint16_t x, y, w, h;
    uint32_t bytes;
};

static struct proc procs[EL0_SLOTS];
#ifdef QUOTA_WAP_FULL_RUNTIME
static unsigned next_child_pid = 1000000;
#endif
static struct qent qq[EL0_Q_GLOBAL];
static uint64_t pt_l0[EL0_SLOTS][512] __attribute__((aligned(4096)));
static uint64_t pt_l1[EL0_SLOTS][512] __attribute__((aligned(4096)));
static uint64_t pt_l2[EL0_SLOTS][512] __attribute__((aligned(4096)));
static uint64_t pt_l3[EL0_SLOTS][512] __attribute__((aligned(4096)));
/* One L3 covers the private ELF window for each admitted process. */
static uint64_t image_pt[EL0_SLOTS][ELF_WINDOW / 4096] __attribute__((aligned(4096)));
/* Each DSO virtual page needs an index even when it maps shared executable
 * bytes directly and owns no physical frame. */
#ifdef QUOTA_NODE_WEB
#define IMAGE_FRAME_SLOTS (EL0_POOL + ELF_DSO_PAGES)
#else
#define IMAGE_FRAME_SLOTS EL0_POOL
#endif
static int image_frames[EL0_SLOTS][IMAGE_FRAME_SLOTS];
enum wap_role { WAP_SINGLE, WAP_FRONTEND, WAP_BACKEND, WAP_PAGE_ONLY, WAP_NODE_ONLY };
struct elf_mapping {
    enum wap_role role;
    const unsigned char *backend;
    unsigned backend_size;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    const struct runtime_volume_file *volume_file;
#endif
    const unsigned char *file;
    unsigned pages;
    unsigned load_bias;
    unsigned phdr_va;
    const unsigned char *interpreter;
    unsigned interp_entry;
    unsigned interp_phoff;
    unsigned interp_count;
};
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
static uint8_t *pool[EL0_POOL];
static unsigned pool_capacity;
#else
static uint8_t pool[EL0_POOL][4096] __attribute__((aligned(4096)));
#define pool_capacity EL0_POOL
#endif
static uint8_t pool_used[EL0_POOL];
static unsigned free_frames = EL0_POOL;
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
void proc_bind_color_pool(void)
{
    free_frames = pool_capacity = color_bind_process_frames(pool, EL0_POOL);
    printf("[proc] shared-color frames %u, %u KiB physical capacity\n", pool_capacity, pool_capacity * 4);
}
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
static int pool_index(uintptr_t pa)
{
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
    unsigned lo = 0, hi = pool_capacity;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if ((uintptr_t)pool[mid] < pa) lo = mid + 1;
        else hi = mid;
    }
    return lo < pool_capacity && (uintptr_t)pool[lo] == pa ? (int)lo : -1;
#else
    if (pa < (uintptr_t)pool || pa - (uintptr_t)pool >= sizeof pool)
        return -1;
    return (pa - (uintptr_t)pool) / 4096;
#endif
}
#endif
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
/* Index the immutable pages of this runtime's libv8. Read-only mappings
 * retain references. Writable remaps detach before user access. */
#define V8_CACHE_PAGES 16384
static unsigned v8_frame_plus_one[V8_CACHE_PAGES];
static unsigned v8_first_va[V8_CACHE_PAGES];
static uint8_t v8_first_slot[V8_CACHE_PAGES];
static unsigned short v8_page_plus_one[EL0_POOL];
static unsigned v8_references[EL0_POOL];
static int volume_v8_index = -1;
static void forget_v8_page(unsigned frame)
{
    unsigned page = v8_page_plus_one[frame];
    if (page) v8_frame_plus_one[page - 1] = 0;
    v8_page_plus_one[frame] = 0;
    v8_references[frame] = 0;
}
#endif
static unsigned reserved_frames;
static uint64_t kernel_ttbr;
static struct el0_reap last_reap;
static struct el0_fault last_fault;
static int unmap_refusals;
static int spawn_logs;
static int stale_drain;
static uint64_t irq_off_max;
extern uint64_t gate_kernel_ttbr, gate_user_ttbr, gate_saved_x0;
extern char gate_vectors[];

static uint64_t el0_l2_hi[512] __attribute__((aligned(4096)));
static uint64_t el0_l3_gate[512] __attribute__((aligned(4096)));

static uint64_t cntpct_now(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

struct irq_span { uint64_t start, flags; };

static void irq_off_begin(struct irq_span *span)
{
    span->flags = irq_save();
    span->start = ticks_now();
}

static void irq_off_end(struct irq_span span)
{
    uint64_t d = ticks_now() - span.start;
    if (d > irq_off_max)
        irq_off_max = d;
    irq_restore(span.flags);
}

asm(
    ".section .rodata\n"
    ".global user_blob\n"
    ".global user_blob_end\n"
    "user_blob:\n"
    ".incbin \"build/user/paint.bin\"\n"
    "user_blob_end:\n"
    ".section .text\n"
);

static int pages_free_n(void)
{
    return free_frames;
}

static int pages_used_n(void)
{
    return pool_capacity - pages_free_n();
}

int proc_pages_free(void)
{
    return pages_free_n();
}

int proc_pages_used(void)
{
    return pages_used_n();
}

int proc_queue_depth(void)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        n += qq[i].used ? 1 : 0;
    return n;
}

int proc_holds(void)
{
    int n = 0;
    for (int i = 0; i < EL0_SLOTS; i++)
        if (procs[i].alive)
            n += procs[i].hold;
    return n;
}

int proc_unmap_refusals(void)
{
    return unmap_refusals;
}

int proc_queued_gen(uint32_t gen)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].gen == gen)
            n++;
    return n;
}

int proc_queued_slot(int slot, uint32_t gen)
{
    int n = 0;
    if (slot < 0 || slot >= EL0_SLOTS)
        return -1;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].slot == slot && qq[i].gen == gen)
            n++;
    return n;
}

static int queue_for_gen(int slot, uint32_t gen)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].slot == slot && qq[i].gen == gen)
            n++;
    return n;
}

static struct proc *from_handle(int64_t handle, int live)
{
    if (handle <= 0)
        return 0;
    int slot = (int)(handle & 0xff) - 1;
    uint32_t gen = (uint32_t)((uint64_t)handle >> 16);
    if (slot < 0 || slot >= EL0_SLOTS)
        return 0;
    struct proc *p = &procs[slot];
    if (p->gen != gen)
        return 0;
    if (live && !p->alive)
        return 0;
    return p;
}

static int64_t make_handle(const struct proc *p)
{
    return ((int64_t)p->gen << 16) | (p->slot + 1);
}

static struct proc *proc_by_task(struct task *t)
{
    if (!t)
        return 0;
    for (int i = 0; i < EL0_SLOTS; i++)
        if (procs[i].alive && procs[i].task == t)
            return &procs[i];
    return 0;
}

unsigned proc_task_memory_kib(struct task *t, unsigned *grant)
{
    struct proc *p = proc_by_task(t);
    if (!p)
        return 0;
    *grant = p->app->mem_granted_kb;
    unsigned kib = 0;
    for (unsigned i = 0; i < sizeof p->pages / sizeof p->pages[0]; i++)
        if (p->pages[i] >= 0)
            kib += 4;
    unsigned resident = p->image_pages;
    if (p->linux_brk) resident = p->dso_frame + p->mapped_dso_pages;
    return kib + 4 * resident;
}

int proc_alive(int64_t handle)
{
    return from_handle(handle, 1) != 0;
}

static struct proc *live(int64_t handle)
{
    return from_handle(handle, 1);
}

uint32_t proc_submits(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->submits : 0;
}

uint32_t proc_rej_range(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->rej_range : 0;
}

uint32_t proc_rej_freq(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->rej_freq : 0;
}

uint32_t proc_rej_depth(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->rej_depth : 0;
}

int proc_queue_for(int64_t handle)
{
    struct proc *p = live(handle);
    if (!p)
        return -1;
    return queue_for_gen(p->slot, p->gen);
}

uint64_t proc_draw_pa(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->pa[2] : 0;
}

uint64_t proc_stack_pa(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->pa[1] : 0;
}

int proc_pid(int64_t handle)
{
    struct proc *p = live(handle);
    return p ? p->pid : -1;
}

void proc_last_reap(struct el0_reap *o)
{
    if (o)
        *o = last_reap;
}

void proc_last_fault(struct el0_fault *o)
{
    if (o)
        *o = last_fault;
}

static void tlb_asid(int asid)
{
    uint64_t x = (uint64_t)(uint16_t)asid << 48;
    asm volatile("dsb ish; tlbi aside1, %0; dsb ish; isb" :: "r"(x) : "memory");
}

static uint64_t walk(const struct proc *p, uint64_t va, int *level)
{
    uint64_t d0 = pt_l0[p->slot][(va >> 39) & 511];
    if ((d0 & 1) == 0) { *level = 0; return 0; }
    uint64_t *l1 = (uint64_t *)(uintptr_t)(d0 & ~0xfffull);
    uint64_t d1 = l1[(va >> 30) & 511];
    if ((d1 & 1) == 0) { *level = 1; return 0; }
    if ((d1 & 2) == 0) {
        *level = 1;
        return d1;
    }
    uint64_t *l2 = (uint64_t *)(uintptr_t)(d1 & ~0xfffull);
    uint64_t d2 = l2[(va >> 21) & 511];
    if ((d2 & 1) == 0) { *level = 2; return 0; }
    if ((d2 & 2) == 0) {
        *level = 2;
        return d2;
    }
    uint64_t *l3 = (uint64_t *)(uintptr_t)(d2 & ~0xfffull);
    *level = 3;
    return l3[(va >> 12) & 511];
}

static int range_ok(const struct proc *p, uint64_t va, uint64_t bytes,
                    uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    uint64_t x1, y1;
    if (w == 0 || h == 0 || w > 32 || h > 32)
        return 0;
    if (bytes != w * h * 4 || bytes > EL0_DRAW)
        return 0;
    if (va != DRAW_VA)
        return 0;
    if (x < p->vx || y < p->vy)
        return 0;
    x1 = (uint64_t)p->vx + p->vw;
    y1 = (uint64_t)p->vy + p->vh;
    if (w > x1 - x || h > y1 - y)
        return 0;
    return 1;
}

static int submit_checked(struct proc *p, uint64_t va, uint64_t bytes,
                          uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    int slot_q, all_q, free_q;
    uint64_t period;

    if (!p->alive || !p->submit_ok)
        return 4;
    if (!range_ok(p, va, bytes, x, y, w, h)) {
        p->rej_range++;
        return 1;
    }
    slot_q = queue_for_gen(p->slot, p->gen);
    all_q = proc_queue_depth();
    if (slot_q >= EL0_Q_PER || all_q >= EL0_Q_GLOBAL) {
        p->rej_depth++;
        return 3;
    }
    /* Music relies on normal Tier 3 scheduling and the bounded draw queue. */
    if (!p->unpaced_draw) {
        period = sched_period_now();
        if (period != p->sub_period) {
            p->sub_period = period;
            p->sub_n = 0;
        }
        if (p->sub_n >= EL0_FREQ) {
            p->rej_freq++;
            return 2;
        }
    }
    free_q = -1;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (!qq[i].used) {
            free_q = i;
            break;
        }
    }
    if (free_q < 0) {
        p->rej_depth++;
        return 3;
    }
    qq[free_q].used = 1;
    qq[free_q].slot = p->slot;
    qq[free_q].gen = p->gen;
    qq[free_q].pa = p->pa[2];
    qq[free_q].x = (uint16_t)x;
    qq[free_q].y = (uint16_t)y;
    qq[free_q].w = (uint16_t)w;
    qq[free_q].h = (uint16_t)h;
    qq[free_q].bytes = (uint32_t)bytes;
    if (!p->unpaced_draw) p->sub_n++;
    p->submits++;
    p->hold++;
    (void)va;
    return 0;
}

int proc_submit_handle(int64_t handle)
{
    struct proc *p;
    uint64_t daif;
    int rc;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #2" ::: "memory");
    p = live(handle);
    rc = p ? submit_checked(p, DRAW_VA, EL0_DRAW, p->vx, p->vy, p->vw, p->vh) : -1;
    if ((daif & 0x80ull) == 0)
        asm volatile("msr daifclr, #2" ::: "memory");
    return rc;
}

static void blit(uint64_t pa, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint64_t fb, bytes;
    uint32_t fw = 0, fh = 0;
    uint32_t *dst, *src;

    if (fb_reserved_range(&fb, &bytes) != 0)
        return;
    fb_geometry(&fw, &fh);
    if (fw == 0 || (uint32_t)x + w > fw || (uint32_t)y + h > fh)
        return;
    dst = (uint32_t *)(uintptr_t)fb;
    src = (uint32_t *)(uintptr_t)pa;
    for (uint16_t row = 0; row < h; row++)
        for (uint16_t col = 0; col < w; col++)
            dst[((uint32_t)y + row) * fw + x + col] = src[(uint32_t)row * w + col];
}

int disp_drain(int max)
{
    int n = 0;
    /* Pixels must never be copied in exception/IRQ-masked context. */
    uint64_t flags;
    asm volatile("mrs %0, daif" : "=r"(flags));
    if (max <= 0 || (flags & 0x80ull))
        return 0;
    for (int i = 0; i < EL0_Q_GLOBAL && n < max; i++) {
        struct irq_span span;
        /* Empty slots can wait until the next drain. Recheck under the lock. */
        if (!__atomic_load_n(&qq[i].used, __ATOMIC_RELAXED)) continue;
        struct qent work;
        struct proc *p;
        irq_off_begin(&span);
        if (!qq[i].used) {
            irq_off_end(span);
            continue;
        }
        work = qq[i];
        p = &procs[work.slot];
        qq[i].used = 0;
        if (!p->alive || p->gen != work.gen || work.pa != p->pa[2]) {
            /* A stale queue item cannot debit the next generation. */
            stale_drain++;
            irq_off_end(span);
            continue;
        }
        /* Transfer the existing queue hold to a draw pin before unmasking.
         * Kill may cancel queued work, but cannot free a pinned draw page. */
        p->active_draws++;
        irq_off_end(span);
        blit(work.pa, work.x, work.y, work.w, work.h);
        irq_off_begin(&span);
        p->active_draws--;
        p->hold--;
        irq_off_end(span);
        n++;
    }
    return n;
}

static int slot_entries(int slot)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++)
        if (qq[i].used && qq[i].slot == slot)
            n++;
    return n;
}

static int cancel_gen(struct proc *p, uint32_t gen)
{
    int n = 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (!qq[i].used || qq[i].slot != p->slot || qq[i].gen != gen)
            continue;
        qq[i].used = 0;
        if (p->hold)
            p->hold--;
        n++;
    }
    /* An active display copy still owns its pin after leaving the queue.
     * Revocation must not turn that live reference into a free page. */
    if (slot_entries(p->slot) == 0)
        p->hold = p->active_draws;
    return n;
}

static void mark_dead(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint64_t fb, bytes;
    uint32_t fw = 0, fh = 0;
    uint32_t *dst;
    if (fb_reserved_range(&fb, &bytes) != 0 || w == 0 || h == 0)
        return;
    fb_geometry(&fw, &fh);
    if ((uint32_t)x + w > fw || (uint32_t)y + h > fh)
        return;
    dst = (uint32_t *)(uintptr_t)fb;
    for (uint16_t row = 0; row < h; row++)
        for (uint16_t col = 0; col < w; col++)
            dst[((uint32_t)y + row) * fw + x + col] = 0x00600000u;
}

static int pa_still_queued(uint64_t pa)
{
    if (!pa)
        return 0;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (!qq[i].used)
            continue;
        if (qq[i].pa >= pa && qq[i].pa < pa + 4096)
            return 1;
    }
    return 0;
}

static void reap_timed(struct proc *p, const char *why, uint64_t t0, uint64_t p0)
{
    struct irq_span off0;
    uint64_t pct0;
    uint32_t gen_before;

    if (!p || !p->alive)
        return;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    p->stop_reason = why;
#endif
    pct0 = cntpct_now();
    irq_off_begin(&off0);

#ifdef QUOTA_WAP_FULL_RUNTIME
    struct proc *parent = from_handle(p->parent_handle, 1);
    if (parent) {
        for (unsigned i = 0; i < EL0_SLOTS; ++i) {
            if (parent->children[i].pid != p->pid) continue;
            parent->children[i].status = p->exit_status;
            parent->children[i].done = 1;
            break;
        }
    }
#endif
    reserved_frames -= p->mapping.reserved;
    memset(&p->mapping, 0, sizeof p->mapping);
    gen_before = p->gen;
    p->submit_ok = 0;
    p->gen++;

    memset(&last_reap, 0, sizeof last_reap);
    last_reap.valid = 1;
    last_reap.pid = p->pid;
    last_reap.hold_before = p->hold;
    last_reap.free_before = pages_free_n();
    last_reap.submits = p->submits;
    last_reap.rej_range = p->rej_range;
    last_reap.rej_freq = p->rej_freq;
    last_reap.rej_depth = p->rej_depth;
    last_reap.gen_before = gen_before;
    last_reap.gen_after = p->gen;
    last_reap.vx = p->vx;
    last_reap.vy = p->vy;
    last_reap.vw = p->vw;
    last_reap.vh = p->vh;
    if (p->pa[2])
        last_reap.draw0 = *(uint32_t *)(uintptr_t)p->pa[2];
    str_copy(last_reap.why, why, (int)sizeof last_reap.why);

    last_reap.cancelled = cancel_gen(p, gen_before);
    if (p->task) {
        p->task->user_ttbr = 0;
        task_dead(p->task);
        p->task = 0;
    }
    p->ttbr = 0;
    p->reap_t0 = t0;
    p->reap_pct0 = pct0;
    p->draining = 1;
    irq_off_end(off0);
    (void)p0;
}

static void free_idx(int idx);

#ifdef QUOTA_WAP_FULL_RUNTIME
static void release_high(struct proc *, uint64_t, uint64_t);
#endif
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
#define VOLUME_PROBE_VA 0x3000ull
static void clean_code(void *p, uint64_t n);
static void finish_volume_wait(struct proc *p)
{
    if (!p->alive || p->draining || !p->volume_waiting || !p->task)
        return;
    const uint8_t *page = NULL;
    int writable = p->volume_waiting == 2;
    int result = writable
        ? runtime_volume_fill_segment_page(&p->volume_file, p->volume_data_offset, 0,
                                            p->volume_data_size, (uint8_t *)(uintptr_t)p->pa[2])
        : runtime_volume_acquire(p->volume_offset, &page);
    if (!result)
        return;
    if (result > 0 && !writable && p->volume_exec)
        clean_code((void *)page, 4096);
    uint64_t flags = irq_save();
    p->volume_waiting = 0;
    if (result < 0) {
        reap_timed(p, "volume-read", ticks_now(), sched_period_now());
    } else if (writable) {
        pt_l3[p->slot][DRAW_VA >> 12] = p->pa[2] | PTE_VALID | PTE_PAGE | PTE_AF |
            SH_INNER | ATTR_NORMAL | PTE_NG | AP_EL0 | PTE_UXN | PTE_PXN;
        asm volatile("dsb ishst" ::: "memory");
        tlb_asid(p->asid);
        task_ready(p->task);
    } else {
        p->volume_page = page;
        pt_l3[p->slot][VOLUME_PROBE_VA >> 12] =
            (uintptr_t)page | PTE_VALID | PTE_PAGE | PTE_AF | SH_INNER |
            ATTR_NORMAL | PTE_NG | AP_EL0 | AP_RO | PTE_PXN | (p->volume_exec ? 0 : PTE_UXN);
        asm volatile("dsb ishst" ::: "memory");
        tlb_asid(p->asid);
        task_ready(p->task);
    }
    irq_restore(flags);
    if (result > 0 && writable)
        printf("[volume-pager] private data mapped pid=%d\n", p->pid);
    else if (result > 0)
        printf("[volume-pager] resumed pid=%d asid=%d page=0x%lx\n", p->pid, p->asid,
               (uintptr_t)page);
}
#endif

#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
static void clean_code(void *p, uint64_t n);
static uint64_t code_pte(uint64_t pa);
static uint64_t data_pte(uint64_t pa);
static int alloc_page(uint64_t *pa);
static unsigned private_pages_left(const struct proc *p);
static const struct runtime_volume_file *disk_catalog_extent(unsigned index);
static const char *disk_extent_name(unsigned index);

static int block_on_disk_elf(struct proc *p, uint64_t far, uint64_t esr)
{
    unsigned ec = (esr >> 26) & 0x3f;
    if (!p || p->task->tier != TIER3 || (esr & 0x3f) != 7 ||
        (esr & (1u << 10)) || (ec != 0x20 && ec != 0x24) ||
        far < ELF_BASE || far >= ELF_DSO_BASE + (uint64_t)ELF_DSO_PAGES * 4096)
        return 0;
    unsigned page = far & ~4095u;
    uint64_t entry = image_pt[p->slot][(page - ELF_BASE) >> 12];
    if (entry & PTE_VALID)
        return 0;
    if (page >= ELF_DSO_BASE && (entry & DISK_DSO_PENDING)) {
        unsigned prot = (entry >> 1) & 7;
        if (!prot || (ec == 0x20 && !(prot & 4)) ||
            (ec == 0x24 && (esr & 0x40) && !(prot & 2)) ||
            (ec == 0x24 && !(esr & 0x40) && !(prot & 1)))
            return 0;
        p->disk_fault_page = page;
        p->disk_elf_waiting = 2;
        p->task->state = TASK_BLOCKED;
        return 1;
    }
    for (unsigned i = 0; i < p->disk_segment_count; ++i) {
        const struct disk_elf_segment *segment = &p->disk_segments[i];
        unsigned first = segment->address & ~4095u;
        if (page < first || page >= segment->address + segment->memory_bytes)
            continue;
        if ((ec == 0x20 && !(segment->permissions & 1)) ||
            (ec == 0x24 && (esr & 0x40) && !(segment->permissions & 2)))
            return 0;
        p->disk_fault_segment = i;
        p->disk_fault_page = page;
        p->disk_elf_waiting = 1;
        p->task->state = TASK_BLOCKED;
        return 1;
    }
    return 0;
}

/* Issue at most one reclaim per fault and bound the scan per service poll.
 * Only pristine read-only file mappings can be reconstructed from disk. */
static int reclaim_disk_page(struct proc *p, unsigned skip, unsigned length)
{
    for (unsigned n = 0; n < 256; ++n) {
        unsigned idx = p->clean_cursor++ % ELF_DSO_PAGES;
        ++p->clean_scanned;
        uint64_t pending = p->disk_clean[idx];
        if (!pending || (idx >= skip && idx - skip < length)) continue;
        unsigned page = (ELF_DSO_BASE - ELF_BASE) / 4096 + idx;
        int *slot = &image_frames[p->slot][p->dso_frame + idx];
        uint64_t irq = irq_save();
        image_pt[p->slot][page] = pending;
        tlb_asid(p->asid);
        int frame = *slot;
        *slot = -1;
        p->dso_source[idx] = p->disk_clean[idx] = 0;
        --p->mapped_dso_pages;
        --p->clean_pages;
        ++p->evicted_pages;
        p->clean_scanned = 0;
        irq_restore(irq);
        free_idx(frame);
        return 1;
    }
    return 0;
}

static void finish_disk_dso_fault(struct proc *p)
{
    unsigned page = p->disk_fault_page;
    unsigned index = (page - ELF_DSO_BASE) >> 12;
    uint64_t *pte = &image_pt[p->slot][(page - ELF_BASE) >> 12];
    uint64_t pending = *pte;
    unsigned protection = (pending >> 1) & 7;
    unsigned file_index = (pending >> 44) & 1023;
    uint64_t position = ((pending >> 4) & DISK_DSO_POSITION_MASK) << 12;
    const struct runtime_volume_file *file = disk_catalog_extent(file_index);
    uint64_t pa = 0;
    if (p->clean_pages && p->clean_scanned < ELF_DSO_PAGES &&
        (private_pages_left(p) < 1024 || free_frames - reserved_frames < 512) &&
        !reclaim_disk_page(p, 0, 0))
        return;
    p->clean_scanned = 0;
    unsigned number = position / 4096;
    int eligible = (int)file_index == volume_v8_index && !(protection & 2) &&
                   position / 4096 < V8_CACHE_PAGES;
    unsigned cached = eligible ? v8_frame_plus_one[number] : 0;
    int frame = cached && private_pages_left(p) ? (int)cached - 1 :
                file && position < file->size && private_pages_left(p)
        ? alloc_page(&pa) : -1;
    int result = -1;
    if (cached && frame >= 0) {
        ++v8_references[frame];
        pa = (uintptr_t)pool[frame];
        result = 1;
        struct proc *peer = &procs[v8_first_slot[number]];
        int level;
        unsigned peer_va = v8_first_va[number];
        uint64_t peer_pte = walk(peer, peer_va, &level);
        if (!p->v8_shared && (protection & 4) && p != peer && peer->alive && !peer->draining &&
            level == 3 && (peer_pte & (PTE_VALID | AP_RO | PTE_UXN)) == (PTE_VALID | AP_RO) &&
            (peer_pte & 0x0000fffffffff000ull) == pa) {
            p->v8_shared = 1;
            printf("[wap-volume] libv8 executable shared pid=%d asid=%u va=%x peer_pid=%d peer_asid=%u peer_va=%x offset=%lu pa=%lx\n",
                   p->pid, p->asid, page, peer->pid, peer->asid, peer_va, position, pa);
        }
    } else if (frame >= 0) {
        uint64_t bytes = file->size - position;
        result = runtime_volume_fill_segment_page(file, position, 0,
                                                  bytes < 4096 ? bytes : 4096,
                                                  (uint8_t *)(uintptr_t)pa);
        if (result != 1)
            free_idx(frame);
        else if (eligible) {
            v8_frame_plus_one[number] = frame + 1;
            v8_first_va[number] = page;
            v8_first_slot[number] = p->slot;
            v8_page_plus_one[frame] = number + 1;
            v8_references[frame] = 1;
        }
    }
    if (!result)
        return;
    if (result > 0 && (protection & 4))
        clean_code((void *)(uintptr_t)pa, 4096);
    uint64_t irq = irq_save();
    p->disk_elf_waiting = 0;
    if (result < 0) {
        reap_timed(p, "dso-page-read", ticks_now(), sched_period_now());
    } else {
        image_frames[p->slot][p->dso_frame + index] = frame;
        p->mapped_dso_pages++;
        p->dso_source[index] = pa;
        ++p->page_ins;
        if (!(protection & 2)) {
            p->disk_clean[index] = pending;
            ++p->clean_pages;
        }
        *pte = protection & 4 ? code_pte(pa) | PTE_PXN :
               data_pte(pa) | ((protection & 2) ? 0 : AP_RO);
        asm volatile("dsb ishst" ::: "memory");
        tlb_asid(p->asid);
        task_ready(p->task);
    }
    irq_restore(irq);
    if (result > 0 && !(p->page_ins % 8192))
        printf("[wap-volume] pid=%d page_ins=%u clean=%u evicted=%u resident=%u high=%u free=%u\n",
               p->pid, p->page_ins, p->clean_pages, p->evicted_pages, p->mapped_dso_pages,
               p->high_frames, free_frames);
}

static void finish_disk_elf_fault(struct proc *p)
{
    if (!p->alive || p->draining || !p->disk_elf_waiting)
        return;
    if (p->disk_elf_waiting == 2) {
        finish_disk_dso_fault(p);
        return;
    }
    const struct disk_elf_segment *segment = &p->disk_segments[p->disk_fault_segment];
    unsigned page = p->disk_fault_page;
    unsigned first = segment->address & ~4095u;
    unsigned frame = segment->first_frame + (page - first) / 4096;
    uint8_t *destination = pool[image_frames[p->slot][frame]];
    unsigned start = page < segment->address ? segment->address : page;
    unsigned end = segment->address + segment->file_bytes;
    if (end > page + 4096)
        end = page + 4096;
    unsigned bytes = end > start ? end - start : 0;
    uint64_t position = segment->file_position + (bytes ? start - segment->address : 0);
    int result = runtime_volume_fill_segment_page(&p->disk_elf, position, start - page,
                                                  bytes, destination);
    if (!result)
        return;
    if (result > 0 && (segment->permissions & 1))
        clean_code(destination, 4096);
    uint64_t irq = irq_save();
    p->disk_elf_waiting = 0;
    if (result < 0) {
        reap_timed(p, "elf-page-read", ticks_now(), sched_period_now());
    } else {
        uintptr_t pa = (uintptr_t)destination;
        image_pt[p->slot][(page - ELF_BASE) >> 12] = segment->permissions & 1
            ? code_pte(pa) | PTE_PXN
            : data_pte(pa) | ((segment->permissions & 2) ? ELF_PRIVATE_WRITABLE : AP_RO);
        asm volatile("dsb ishst" ::: "memory");
        tlb_asid(p->asid);
        task_ready(p->task);
    }
    irq_restore(irq);
}
#endif

static void finish_one(struct proc *p)
{
    int refused = 0;
    uint64_t t1, pct1;
    if (!p->draining || p->active_draws)
        return;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    if (p->stop_reason) {
        printf("[wap-volume] process stopped pid=%d reason=%s dso=%u high=%u private_limit=%u\n",
               p->pid, p->stop_reason, p->mapped_dso_pages, p->high_frames, p->private_page_limit);
        p->stop_reason = NULL;
    }
#endif
    if (p->hold != 0 || slot_entries(p->slot) != 0) {
        refused = 1;
        unmap_refusals++;
    } else {
        memset(&pt_l2[p->slot][ELF_BASE >> 21], 0, (ELF_WINDOW >> 21) * sizeof(uint64_t));
#ifdef QUOTA_WAP_FULL_RUNTIME
        if (p->fork_child) {
            struct proc *c = p->fork_child;
            if (p->fork_init > sizeof *c) {
                if (c->high_frames) release_high(c, HIGH_START, HIGH_LIMIT);
                unsigned n = p->fork_cursor < c->image_pages ? p->fork_cursor : c->image_pages;
                while (n) free_idx(image_frames[c->slot][--n]);
                for (unsigned i = 0; i < 3; ++i) free_idx(c->pages[i]);
                if (c->app) resource_release(c->app);
            }
            c->building = 0;
            p->fork_child = NULL;
        }
        for (unsigned i = 0; i < WAP_FDS; ++i) {
            pipe_put(p->files[i].event_flags, p->files[i].pipe_id);
            p->files[i].event_flags &= ~(FD_PIPE_R | FD_PIPE_W);
            if (!p->files[i].socket) continue;
            packet_put(p->files[i].socket);
            p->files[i].socket = NULL;
        }
        release_high(p, HIGH_START, HIGH_LIMIT);
#endif
        for (unsigned i = 0; i < WAP_FDS; ++i) {
            if (p->files[i].poll) { --p->files[i].poll->refs; p->files[i].poll = NULL; }
            if (p->files[i].position) {
                --p->files[i].position->refs;
                p->files[i].position = NULL;
                p->files[i].file = NULL;
            }
        }
        uint64_t pas[3];
        for (int i = 0; i < 3; i++)
            pas[i] = p->pa[i];
        for (int i = 0; i < 3; i++)
            pt_l3[p->slot][i] = 0;
        asm volatile("dsb ish" ::: "memory");
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
        pt_l3[p->slot][VOLUME_PROBE_VA >> 12] = 0;
        asm volatile("dsb ishst" ::: "memory");
#endif
        tlb_asid(p->asid);
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
        if (p->volume_page) {
            runtime_volume_release(p->volume_page);
            p->volume_page = NULL;
        }
        p->volume_waiting = 0;
#endif
        while (p->image_pages) {
            int pg = image_frames[p->slot][--p->image_pages];
            if (pg < 0) continue;
            free_idx(pg);
        }
        for (int i = 0; i < 3; i++) {
            int pg = p->pages[i];
            if (pg < 0 || pg >= EL0_POOL)
                continue;
            if (pa_still_queued(pas[i]) || slot_entries(p->slot) != 0) {
                refused = 1;
                unmap_refusals++;
                break;
            }
            free_idx(pg);
            p->pages[i] = -1;
            p->pa[i] = 0;
        }
        if (!refused) {
            if (p->app) {
                resource_release(p->app);
                p->app = 0;
            }
            p->alive = 0;
            p->draining = 0;
        }
    }
    t1 = ticks_now();
    pct1 = cntpct_now();
    if (last_reap.pid == p->pid && last_reap.gen_after == p->gen) {
        last_reap.hold_after = p->hold;
        last_reap.free_after = pages_free_n();
        last_reap.unmap_refused = refused;
        last_reap.ticks = t1 >= p->reap_t0 ? t1 - p->reap_t0 : 0;
        last_reap.us = ticks_to_us(last_reap.ticks);
        last_reap.cntpct = pct1 >= p->reap_pct0 ? pct1 - p->reap_pct0 : 0;
    }
    if (!refused)
        mark_dead(p->vx, p->vy, p->vw, p->vh);
}

static void report_fault(struct proc *p)
{
    const struct el0_fault *f = &p->fault_report;
    if (!p->fault_pending)
        return;
    p->fault_pending = 0;
    printf("[fault] deferred from_el=%u pid=%d ec=0x%x esr=0x%lx elr=0x%lx far=0x%lx spsr=0x%lx pte=0x%lx level=%d ap_el0=%d class=%s gen %u->%u\n",
           (unsigned)((f->spsr & 0xfu) == 0 ? 0 : (f->spsr & 0xfu)),
           f->pid, f->ec, f->esr, f->elr, f->far, f->spsr, f->pte, f->level,
           f->ap_el0, f->kind, p->gen - 1, p->gen);
#ifdef QUOTA_NODE_WEB
    for (unsigned i = 0; i < 2; ++i) {
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
        if (p->fault_file_name[i])
            printf("[wap-volume] fault %s=%s+%lx\n", i ? "lr" : "pc",
                   p->fault_file_name[i], p->fault_file_offset[i]);
#endif
        uintptr_t pa = p->fault_code_pa[i];
        for (const struct runtime_file *file = wap_libs_start; file < wap_libs_end; ++file) {
            if (pa >= (uintptr_t)file->begin && pa < (uintptr_t)file->end) {
                printf("[fault] %s file=%s offset=0x%lx\n", i ? "lr" : "pc",
                       file->name, pa - (uintptr_t)file->begin);
                break;
            }
        }
    }
#endif
}

void proc_finish_pending(void)
{
    uint64_t flags;
    asm volatile("mrs %0, daif" : "=r"(flags));
    /* Page clearing, framebuffer writes and UART are thread work. */
    if (flags & 0x80ull)
        return;
    for (int i = 0; i < EL0_SLOTS; i++) {
        if (procs[i].draining)
            finish_one(&procs[i]);
        report_fault(&procs[i]);
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
        if (sched_current() && sched_current()->tier == TIER3) {
            struct proc *p = &procs[i];
            finish_disk_elf_fault(p);
            if (p->alive && !p->draining && p->disk_read_waiting &&
                !runtime_volume_io_pending()) {
                uint64_t irq = irq_save();
                p->disk_read_waiting = 0;
                task_ready(p->task);
                irq_restore(irq);
            }
        }
#endif
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
        if (sched_current() && sched_current()->tier == TIER3)
            finish_volume_wait(&procs[i]);
#endif
    }
}

static void classify(struct proc *self, uint64_t far, uint64_t pte, char *out)
{
    uint64_t fb0 = 0, fbn = 0;

    if (far >= 0x08000000ull && far < 0x0c000000ull) {
        str_copy(out, "device", 16);
        return;
    }
    if (fb_reserved_range(&fb0, &fbn) == 0 && far >= fb0 && far < fb0 + fbn) {
        str_copy(out, "framebuffer", 16);
        return;
    }
    for (int i = 0; i < EL0_SLOTS; i++) {
        struct proc *o = &procs[i];
        if (o == self || !o->alive)
            continue;
        for (int k = 0; k < 3; k++) {
            if (o->pa[k] && far >= o->pa[k] && far < o->pa[k] + 4096) {
                str_copy(out, "other-app", 16);
                return;
            }
        }
    }
    if (far < 0x4000ull && (pte & 1ull) == 0) {
        str_copy(out, "unmapped", 16);
        return;
    }
    if (far >= 0x40000000ull && far < 0x48000000ull) {
        str_copy(out, "kernel", 16);
        return;
    }
    str_copy(out, "other", 16);
}

void proc_el0_abort(struct irq_frame *f, uint64_t esr)
{
    uint64_t t0 = ticks_now();
    uint64_t p0 = sched_period_now();
    uint64_t far = 0;
    struct proc *p = proc_by_task(sched_current());
    int level = 0;
    uint64_t pte = 0;

    asm volatile("mrs %0, far_el1" : "=r"(far));
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    if (block_on_disk_elf(p, far, esr))
        return;
#endif
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    unsigned ec = (esr >> 26) & 0x3f;
    if (p && p->console == CONSOLE_VOLUME_PROBE && p->task == sched_current() &&
        p->task->tier == TIER3 && (esr & 0x3f) == 7) {
        uint64_t target = far & ~4095ull;
        if (!p->volume_page && ec == (p->volume_exec ? 0x20u : 0x24u) &&
            !(esr & 0x40) && target == VOLUME_PROBE_VA)
            p->volume_waiting = 1;
        else if (p->volume_data_offset && ec == 0x24 && target == DRAW_VA &&
                 !pt_l3[p->slot][DRAW_VA >> 12])
            p->volume_waiting = 2;
        if (p->volume_waiting) {
            p->task->state = TASK_BLOCKED;
            return;
        }
    }
#endif
    memset(&last_fault, 0, sizeof last_fault);
    last_fault.valid = 1;
    last_fault.ec = (uint32_t)((esr >> 26) & 0x3f);
    last_fault.esr = esr;
    last_fault.far = far;
    last_fault.elr = f->elr;
    last_fault.spsr = f->spsr;
    if (p) {
#ifdef QUOTA_NODE_WEB
        for (unsigned i = 0; i < 2; ++i) {
            uint64_t pc = i ? f->r[30] : f->elr;
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
            p->fault_file_name[i] = NULL;
            if (pc >= ELF_DSO_BASE && pc < ELF_DSO_BASE + (uint64_t)ELF_DSO_PAGES * 4096) {
                uint64_t src = p->disk_clean[(pc - ELF_DSO_BASE) / 4096];
                if (src) {
                    p->fault_file_name[i] = disk_extent_name((src >> 44) & 1023);
                    p->fault_file_offset[i] = (((src >> 4) & DISK_DSO_POSITION_MASK) << 12) + (pc & 4095);
                }
            }
#endif
            uint64_t leaf = walk(p, pc, &level);
            p->fault_code_pa[i] = (leaf & 1) ? ((leaf & 0x0000fffffffff000ull) | (pc & 4095)) : 0;
        }
#endif
        pte = walk(p, far, &level);
        last_fault.pid = p->pid;
        last_fault.pte = pte;
        last_fault.level = level;
        last_fault.ap_el0 = (int)((pte >> 6) & 1ull);
        classify(p, far, pte, last_fault.kind);
        reap_timed(p, "fault", t0, p0);
    } else {
        str_copy(last_fault.kind, "no-proc", 16);
        struct task *t = sched_current();
        if (t)
            task_dead(t);
    }
    if (p) {
        p->fault_report = last_fault;
        p->fault_pending = 1;
    }

}

/* Resolve each user page through the calling process's tables. */
static int copy_process_bytes(struct proc *p, uint64_t va, void *buffer, unsigned count, int to_user)
{
    unsigned copied = 0;
    while (copied < count) {
        uint64_t at = va + copied;
        if (at < va) return -1;
        if (at >= ELF_BASE + ELF_WINDOW) {
#ifdef QUOTA_WAP_FULL_RUNTIME
            if (at < HIGH_START || at >= HIGH_LIMIT) return -1;
#else
            return -1;
#endif
        }
        int level;
        uint64_t pte = walk(p, at, &level);
        if (!(pte & PTE_VALID) || level != 3 || !(pte & AP_EL0)) return -1;
        unsigned offset = at & 4095;
        unsigned chunk = 4096 - offset;
        if (chunk > count - copied) chunk = count - copied;
        uintptr_t pa = (pte & 0x0000fffffffff000ull) + offset;
        if (to_user) {
            if (pte & AP_RO) return -1;
            memcpy((void *)pa, (unsigned char *)buffer + copied, chunk);
        } else memcpy((unsigned char *)buffer + copied, (const void *)pa, chunk);
        copied += chunk;
    }
    return 0;
}

#ifdef QUOTA_WAP_FULL_RUNTIME
struct socket_iovec { uint64_t addr, len; };

static int64_t transfer_stream(struct proc *p, struct irq_frame *f, struct packet_socket *sock,
                               int read, const struct socket_iovec *iov, unsigned count, unsigned flags)
{
    if (read && ((sock->shut & 1) || (!sock->packets && (!sock->peer || (sock->peer->shut & 2))))) return 0;
    if (!read && (!sock->peer || (sock->peer->shut & 1) || (sock->shut & 2))) return -32;
    struct packet_socket *end = read ? sock : sock->peer;
    unsigned capacity = sizeof end->stream_bytes;
    unsigned room = read ? end->packets : capacity - end->packets;
    /* A recvmsg can carry one queued control batch. Leave later batches
     * and their associated bytes for the following receive. */
    if (read) {
        unsigned first = capacity + 1, second = capacity + 1;
        for (unsigned i = 0; i < 4; ++i) {
            if (!end->rights[i].count) continue;
            unsigned off = end->rights[i].offset;
            if (off < first) { second = first; first = off; }
            else if (off < second) second = off;
        }
        if (room > second) room = second;
    }
    unsigned used = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!iov[i].len) continue;
        if (!room && !used) {
            if (sock->nonblock || (flags & 0x40)) return -11;
            f->elr -= 4;
            return f->r[0];
        }
        unsigned n = iov[i].len < room ? iov[i].len : room;
        unsigned pos = (end->packet_head + (read ? 0 : end->packets) + used) % capacity;
        unsigned first = n < capacity-pos ? n : capacity-pos;
        if (copy_process_bytes(p, iov[i].addr, end->stream_bytes + pos, first, read) ||
            (n > first && copy_process_bytes(p, iov[i].addr + first, end->stream_bytes, n-first, read))) return -14;
        used += n;
        room -= n;
        if (!room) break;
    }
    if (read) {
        end->packet_head = (end->packet_head + used) % capacity;
        end->packets -= used;
    } else end->packets += used;
    return used;
}

/* Offsets are relative to the next unread byte on a stream. */
static struct rights_batch *consume_stream_rights(struct packet_socket *sock, unsigned bytes)
{
    struct rights_batch *delivered = NULL;
    for (unsigned i = 0; i < 4; ++i) {
        struct rights_batch *batch = &sock->rights[i];
        if (!batch->count) continue;
        if (batch->offset < bytes) delivered = batch;
        else batch->offset -= bytes;
    }
    return delivered;
}

static int rotate_thread(struct proc *p, struct irq_frame *f)
{
    unsigned old = p->active_thread;
    for (unsigned i = 1; i < WAP_THREADS; ++i) {
        unsigned next = (old + i) % WAP_THREADS;
        struct wap_thread *to = &p->threads[next];
        if (!to->tid) continue;
        if (to->wait_addr && to->futex_deadline && ticks_now() >= to->futex_deadline) {
            to->wait_addr = 0;
            to->futex_deadline = 0;
            to->frame.r[0] = -110;
        }
        if (to->wait_addr) continue;
        struct wap_thread *from = &p->threads[old];
        from->frame = *f;
        asm volatile("mrs %0,tpidr_el0" : "=r"(from->tls));
        wap_fp_save(from->fp);
        from->clear_tid = p->clear_child_tid;
        from->mask = p->signal_mask;
        *f = to->frame;
        p->clear_child_tid = to->clear_tid;
        p->signal_mask = to->mask;
        asm volatile("msr tpidr_el0,%0" :: "r"(to->tls));
        wap_fp_restore(to->fp);
        uint64_t charged = p->task->used_total;
        from->cpu_ticks += charged - p->thread_charge_base;
        p->thread_charge_base = charged;
        p->active_thread = next;
        return 1;
    }
    return 0;
}

static int clone_thread(struct proc *p, struct irq_frame *f)
{
    if (f->r[0] != 0x3d0f00) return -22;
    if (!f->r[1] || (f->r[1] & 15) || !f->r[3]) return -22;
    unsigned slot = 0;
    while (slot < WAP_THREADS && p->threads[slot].tid) ++slot;
    if (slot == WAP_THREADS) return -11;
    unsigned tid = ++p->next_tid;
    if (copy_process_bytes(p, f->r[2], &tid, sizeof tid, 1)) return -14;
    struct wap_thread *child = &p->threads[slot];
    memset(child, 0, sizeof *child);
    child->frame = *f;
    child->frame.r[0] = 0;
    child->frame.pad = f->r[1];
    child->mask = p->signal_mask;
    child->ns_effective = p->threads[p->active_thread].ns_effective;
    child->ns_permitted = p->threads[p->active_thread].ns_permitted;
    child->no_new_privs = p->threads[p->active_thread].no_new_privs;
    child->tls = f->r[3];
    child->clear_tid = f->r[4];
    child->tid = tid;
    wap_fp_save(child->fp);
    ++p->live_threads;
#ifdef QUOTA_WAP_LOADER_TRACE
    printf("[wap] cloned tid=%u sp=%lx tls=%lx group=%u\n", tid, child->frame.pad, child->tls, p->pid);
#endif
    return tid;
}

static unsigned wake_threads(struct proc *p, uint64_t addr, unsigned count, unsigned bits)
{
    unsigned n = 0;
    for (unsigned i = 0; i < WAP_THREADS && n < count; ++i) {
        struct wap_thread *t = &p->threads[i];
        if (t->tid && t->wait_addr == addr && (t->wait_bits & bits)) {
            t->wait_addr = 0;
            t->futex_deadline = 0;
            ++n;
        }
    }
    return n;
}
#endif

static int64_t runtime_syscall(struct proc *, struct irq_frame *);

void proc_el0_svc(struct irq_frame *f)
{
    struct proc *p = proc_by_task(sched_current());
    uint64_t op;

    if (!p) {
        f->r[0] = ~0ull;
        return;
    }
    op = f->r[8];
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    /* A pathname literal may first be touched by the kernel in openat or
       stat, before EL0 has issued a load from its file-backed page. */
    if (p->disk_segment_count && (op == 56 || op == 79 || op == 78 || op == 48)) {
        uint64_t path = f->r[1];
        unsigned head = 4096 - (path & 4095);
        if (block_on_disk_elf(p, path, (0x24u << 26) | 7)) {
            f->elr -= 4;
            return;
        }
        if (head < 256) {
            char prefix[256];
            if (!copy_process_bytes(p, path, prefix, head, 0)) {
                unsigned i = 0;
                while (i < head && prefix[i]) ++i;
                if (i == head && block_on_disk_elf(p, path + head, (0x24u << 26) | 7)) {
                    f->elr -= 4;
                    return;
                }
            }
        }
    }
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (p->vfork_pid) {
        /* The single-threaded parent is suspended. The child uses its own
           register frame and TLS, sharing the VM and root with its parent. */
        if (op == 93 || op == 94) {
            unsigned pid = p->vfork_pid;
            p->children[p->vfork_record] = (struct child_status){ pid, (f->r[0] & 255) << 8, 1 };
            *f = p->suspended_frame;
            f->r[0] = pid;
            p->vfork_pid = 0;
            asm volatile("msr tpidr_el0,%0" :: "r"(p->suspended_tls));
            wap_fp_restore(p->suspended_fp);
            return;
        }
        if (op == 51 || op == 49) {
            static const char dir[] = "/proc/self/fdinfo/";
            char path[sizeof dir];
            unsigned length = op == 51 ? sizeof dir : 2;
            if (copy_process_bytes(p, f->r[0], path, length, 0)) f->r[0] = -14;
            else if (path[length-1] || strcmp(path, op == 51 ? dir : "/")) f->r[0] = -2;
            else if (op == 51 && (!p->user_namespace || !(p->threads[p->active_thread].ns_effective & NS_CAP_SYS_CHROOT) || p->empty_root)) f->r[0] = -1;
            else {
                if (op == 51) p->empty_root = 1;
                f->r[0] = 0;
            }
            return;
        }
        f->r[0] = op == 172 || op == 178 ? p->vfork_pid : (uint64_t)-38;
        return;
    }
#endif
    if (p->console == CONSOLE_PAGE && op == GUEST_LINUX_SVC_TILE) {
        unsigned row = page_rows;
        if (make_handle(p) != page_owner || row >= GUEST_PREVIEW_HEIGHT ||
            f->r[0] != 0 || f->r[1] != row ||
            f->r[2] != GUEST_PREVIEW_WIDTH || f->r[3] != 1) {
            f->r[0] = (uint64_t)-22;
            return;
        }
        _Static_assert(sizeof page_pixels[0] <= GUEST_TILE_BUFFER_BYTES, "tile row too large");
        memcpy(page_pixels[row], (const void *)(uintptr_t)(p->pa[2] + GUEST_TILE_OFFSET),
               sizeof page_pixels[row]);
        __atomic_store_n(&page_rows, row + 1, __ATOMIC_RELEASE);
        f->r[0] = 0;
        return;
    }
    if (p->console == CONSOLE_PAGE && p->linux_brk && op == GUEST_LINUX_SVC_STACK_BOUNDS) {
        f->r[0] = ELF_STACK_TOP;
        f->r[1] = ELF_STACK_TOP - ELF_STACK_PAGES * 4096;
        return;
    }
    if (p->console == CONSOLE_PAGE && op == GUEST_LINUX_SVC_DOCUMENT) {
        f->r[0] = DRAW_VA;
        f->r[1] = p->document_size;
        return;
    }
    if (p->console == CONSOLE_LINUX_BOOTSTRAP || p->console == CONSOLE_PAGE) {
        uint64_t fd = f->r[0], address = f->r[1], count = f->r[2];
#ifdef QUOTA_WAP_FULL_RUNTIME
        if (op == 93 && p->live_threads > 1) {
            uint32_t zero = 0;
            if (p->clear_child_tid && !copy_process_bytes(p, p->clear_child_tid, &zero, 4, 1))
                wake_threads(p, p->clear_child_tid, 1, ~0u);
            p->threads[p->active_thread].tid = 0;
            --p->live_threads;
            if (!rotate_thread(p, f)) reap_timed(p, "thread-group-idle", ticks_now(), sched_period_now());
            return;
        }
#endif
        if (op == 93 || op == 94) {
            if (p->clear_child_tid) {
                uint32_t zero = 0;
                copy_process_bytes(p, p->clear_child_tid, &zero, sizeof zero, 1);
            }
#ifdef QUOTA_WAP_FULL_RUNTIME
            p->exit_status = (fd & 255) << 8;
#endif
            if (p->console == CONSOLE_PAGE) printf("[wap] linux exit code=%lu pid=%d\n", fd, p->pid);
            reap_timed(p, "linux-exit", ticks_now(), sched_period_now());
            return;
        }
#ifdef QUOTA_WAP_LOADER_TRACE
        if (op == 64 && fd == 2 && p->mapping_traces == 128) p->mapping_traces = 0;
        int trace = p->linux_brk && !p->mapping.va && p->mapping_traces < 128;
        if (!p->mapping.va) p->trace_in_progress = trace;
        if (trace) {
            ++p->mapping_traces;
            printf("[wap-abi] op=%lu arg0=%lx arg1=%lx arg2=%lx arg3=%lx\n",
                   op, fd, address, count, f->r[3]);
        }
#endif
#ifdef QUOTA_WAP_LOADER_TRACE
        uint64_t measured_start = ticks_now();
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
        uint64_t resume_pc = f->elr;
#endif
        int runtime_abi = p->linux_brk != 0;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB) && defined(QUOTA_RUNTIME_VOLUME_PAGER_TEST)
        /* The static test image exercises the same file syscalls as ld.so. */
        if (p->disk_segment_count && (op == 56 || op == 57 || op == 62 ||
                                     op == 63 || op == 67 || op == 80 || op == 222 ||
                                     op == 215 || op == 226))
            runtime_abi = 1;
#endif
        int64_t result = runtime_abi ? runtime_syscall(p, f) : -38;
#if defined(QUOTA_WAP_FULL_RUNTIME) && defined(QUOTA_WAP_LOADER_TRACE)
        if ((op == 19 || op == 20 || op == 21 || op == 59 || op == 199) && result < 0)
            printf("[wap] fd setup pid=%d tid=%u svc=%lu flags=%lx arg1=%lx errno=%ld\n",
                   p->pid, p->threads[p->active_thread].tid, op, fd, address, -result);
        if ((op == 222 || op == 226) && result < 0 && p->failed_map_traces < 64) {
            ++p->failed_map_traces;
            printf("[wap] mapping refused pid=%d tid=%u op=%lu va=%lx len=%lx prot=%lx flags=%lx fd=%lx off=%lx errno=%ld private=%u free=%u reserved=%u\n",
                   p->pid, p->threads[p->active_thread].tid, op, fd, address, count, f->r[3],
                   f->r[4], f->r[5], -result, p->mapped_dso_pages, free_frames, reserved_frames);
        }
        if ((op == 211 || op == 212) && result < 0 && result != -11 && p->failed_message_traces < 16) {
            ++p->failed_message_traces;
            printf("[wap] message refused pid=%d op=%lu fd=%lu flags=%lx errno=%ld\n", p->pid, op, fd, count, -result);
        }
        if (op == 211 && result == -11 && fd >= 3 && fd - 3 < WAP_FDS &&
            p->stream_stall_traces < 8 && p->files[fd-3].socket) {
            struct packet_socket *peer = p->files[fd-3].socket->peer;
            if (peer && peer->stream) {
                ++p->stream_stall_traces;
                printf("[wap] stream send retry pid=%d fd=%lu peer=%ld bytes=%u rights=%u,%u,%u,%u\n",
                       p->pid, fd, (long)(peer-packet_sockets), peer->packets,
                       peer->rights[0].count, peer->rights[1].count, peer->rights[2].count, peer->rights[3].count);
            }
        }
        uint64_t measured = ticks_now() - measured_start;
        if (measured > us_to_ticks(1000))
            printf("[wap] long svc pid=%d op=%lu ticks=%lu addr=%lx len=%lx prot=%lx flags=%lx\n", p->pid, op, measured, fd, address, count, f->r[3]);
        if (op == 220 && (fd == 0x1200011 || fd == 0x20000011) && result < 0)
            printf("[wap] process clone error=%ld mapping=%lx\n", result, p->mapping.va);
#endif
        if (op == 214 && p->linux_brk) {
            if (fd >= ELF_BRK_BASE && fd <= ELF_BRK_BASE + ELF_BRK_PAGES * 4096)
                p->linux_brk = fd;
            result = p->linux_brk;
        }
        if (op == 64 && (fd <= 2 || !p->linux_brk)) {
            unsigned length = count < sizeof console_text ? (unsigned)count : sizeof console_text - 1;
            result = -9; /* EBADF */
            if (fd == 1 || fd == 2) {
                result = 0;
                if (length) {
                    result = -11; /* EAGAIN */
                    if (!console_updated) {
                        result = -14; /* EFAULT */
                        if (!copy_process_bytes(p, address, console_text, length, 0)) {
                            console_text[length] = 0;
#ifdef QUOTA_LINUX_APP_RUNTIME
                            if (fd == 1 && make_handle(p) == backend_owner) {
                                memcpy(backend_line, console_text, length+1);
                                backend_line_ready = 1;
                            }
#endif
                            if (p->console == CONSOLE_PAGE) printf("[wap] %s\n", console_text);
                            else console_updated = 1;
                            result = length;
                        }
                    }
                }
            }
        }
        if (p->console == CONSOLE_PAGE && result == -38 && p->linux_traces < 8) {
            ++p->linux_traces;
            printf("[wap] unsupported Linux syscall=%lu pc=0x%lx\n", op, f->elr);
        }
#ifdef QUOTA_WAP_LOADER_TRACE
        if (p->trace_in_progress && !p->mapping.va) {
            printf("[wap-abi] return=%lx\n", (uint64_t)result);
            p->trace_in_progress = 0;
        }
#endif
        f->r[0] = (uint64_t)result;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
        if (p->disk_read_waiting)
            return;
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
        if (p->live_threads > 1 && !p->fork_child && !p->mapping.va && (op == 220 || op == 98 || op == 22 || op == 73 || op == 124 ||
            ((op == 63 || op == 64 || op == 206 || op == 207 || op == 211 || op == 212) && f->elr + 4 == resume_pc)))
            rotate_thread(p, f);
#endif
        return;
    }
    if (op == 0) {
        f->r[0] = 0;
        return;
    }
    if (op == 1) {
        if (p->console == CONSOLE_QUOTA) printf("[cap] exit=%ld\n", (long)f->r[0]);
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
        if (p->console == CONSOLE_VOLUME_PROBE)
            printf("[volume-pager] exit pid=%d byte=%lu\n", p->pid, f->r[0]);
#endif
        reap_timed(p, "exit", ticks_now(), sched_period_now());
        return;
    }
    if (op == 2) {
        f->r[0] = (uint64_t)submit_checked(p, f->r[0], f->r[1], f->r[2], f->r[3], f->r[4], f->r[5]);
        return;
    }
    if (op == 4 && p->console == CONSOLE_QUOTA) {
        /* Read the shell volume's text file into the caller-owned data page. */
        const char *path = (const char *)(uintptr_t)p->pa[2];
        if (f->r[0] != DRAW_VA || path[9] || strcmp(path, "hello.txt")) { f->r[0] = -2; return; }
        const char *text = note_disk_hello();
        if (!text) { f->r[0] = -2; return; }
        unsigned length = 0;
        while (text[length]) ++length;
        memcpy((void *)(uintptr_t)p->pa[2], text, length);
        f->r[0] = length;
        return;
    }
    if (op == 3 && p->console == CONSOLE_QUOTA && !console_updated) {
        uint64_t off = f->r[0], len = f->r[1];
        if ((off == DRAW_VA && len < sizeof console_text) ||
            (off < 4096 && len <= 128 && len <= 4096 - off)) {
            uintptr_t src = off == DRAW_VA ? p->pa[2] : p->pa[0] + off;
            memcpy(console_text, (const void *)src, len);
            console_text[len] = 0;
            console_updated = 1;
            f->r[0] = len;
            return;
        }
    }
    f->r[0] = ~0ull;
}

const char *proc_console_text(void)
{
    if (!console_updated) return 0;
    console_updated = 0;
    return console_text;
}

int proc_kill(int64_t handle)
{
    struct proc *p = live(handle);
    if (!p)
        return -1;
    reap_timed(p, "kill", ticks_now(), sched_period_now());
    proc_finish_pending();
    return 0;
}

static int alloc_page(uint64_t *pa)
{
    static unsigned cursor;
    for (unsigned scanned = 0; scanned < pool_capacity; ++scanned) {
        uint64_t flags = irq_save();
        if (free_frames <= reserved_frames) {
            irq_restore(flags);
            return -1;
        }
        unsigned i = cursor++ % pool_capacity;
        if (pool_used[i]) {
            irq_restore(flags);
            continue;
        }
        pool_used[i] = 1;
        --free_frames;
        irq_restore(flags);
        memset(pool[i], 0, 4096);
        *pa = (uint64_t)(uintptr_t)pool[i];
        return i;
    }
    return -1;
}

static void free_idx(int idx)
{
    if (idx < 0 || idx >= EL0_POOL)
        return;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    if (v8_page_plus_one[idx]) {
        if (--v8_references[idx]) return;
        forget_v8_page(idx);
    }
#endif
    memset(pool[idx], 0, 4096);
    uint64_t flags = irq_save();
    pool_used[idx] = 0;
    ++free_frames;
    irq_restore(flags);
}

#ifdef QUOTA_RUNTIME_VOLUME_BOOT
int proc_runtime_cache_alloc(uint8_t **page)
{
    uint64_t pa;
    int frame = alloc_page(&pa);
    if (frame >= 0)
        *page = (uint8_t *)(uintptr_t)pa;
    return frame;
}

void proc_runtime_cache_free(int frame)
{
    free_idx(frame);
}
#endif

static uint64_t data_pte(uint64_t pa)
{
    return (pa & ~0xfffull) | PTE_VALID | PTE_PAGE | PTE_AF | SH_INNER | ATTR_NORMAL | PTE_NG |
           AP_EL0 | PTE_UXN | PTE_PXN;
}

static uint64_t code_pte(uint64_t pa)
{
    return (pa & ~0xfffull) | PTE_VALID | PTE_PAGE | PTE_AF | SH_INNER | ATTR_NORMAL | PTE_NG |
           AP_EL0 | AP_RO;
}

static void build_pt(struct proc *p)
{
    memset(pt_l0[p->slot], 0, 4096);
    memset(pt_l1[p->slot], 0, 4096);
    memset(pt_l2[p->slot], 0, 4096);
    memset(pt_l3[p->slot], 0, 4096);
    pt_l3[p->slot][0] = code_pte(p->pa[0]);
    pt_l3[p->slot][1] = data_pte(p->pa[1]);
    pt_l3[p->slot][2] = data_pte(p->pa[2]);
    pt_l2[p->slot][0] = (uint64_t)(uintptr_t)pt_l3[p->slot] | PTE_VALID | PTE_PAGE;
    pt_l1[p->slot][0] = (uint64_t)(uintptr_t)pt_l2[p->slot] | PTE_VALID | PTE_PAGE;
    /* RAM half is not the kernel L2. Only the shared gate page is present. */
    pt_l1[p->slot][1] = (uint64_t)(uintptr_t)el0_l2_hi | PTE_VALID | PTE_PAGE;
    pt_l0[p->slot][0] = (uint64_t)(uintptr_t)pt_l1[p->slot] | PTE_VALID | PTE_PAGE;
    p->ttbr = (uint64_t)(uintptr_t)pt_l0[p->slot] | ((uint64_t)p->asid << 48);
    for (int i = 0; i < 512; i += 8) {
        asm volatile("dc civac, %0" :: "r"(&pt_l0[p->slot][i]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&pt_l1[p->slot][i]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&pt_l2[p->slot][i]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&pt_l3[p->slot][i]) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
}

static void clean_code(void *p, uint64_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)p + n;
    for (; a < end; a += 64)
        asm volatile("dc cvau, %0" :: "r"(a) : "memory");
    asm volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory");
}

#include "../port/wap/linux_process.inc"

static int64_t spawn_image(const struct elf_mapping *image, const char *code, unsigned bytes, const unsigned char *data, unsigned data_size, const unsigned char *headers, unsigned header_count, unsigned entry, enum console_abi console, const char *name, uint64_t role, uint64_t limit, uint64_t foreign_pa,
                   uint64_t fb_pa, uint64_t kern_va, uint16_t vx, uint16_t vy,
                   uint16_t vw, uint16_t vh)
{
    struct proc *p = 0;
    struct app_manifest man;
    uint64_t blen, pack;
    int pg[3];

    proc_finish_pending();
    for (int i = 0; i < EL0_SLOTS; i++) {
        if (!procs[i].alive
#ifdef QUOTA_WAP_FULL_RUNTIME
            && !procs[i].building
#endif
        ) {
            p = &procs[i];
            p->slot = i;
            break;
        }
    }
    if (!p)
        return -1;
    if (p->gen == 0)
        p->gen = 1;
    for (int i = 0; i < EL0_Q_GLOBAL; i++) {
        if (qq[i].used && qq[i].slot == p->slot)
            return -1;
    }
    blen = bytes;
    if (!image && (blen < 4 || blen > 4096 || (entry & 3) || entry > blen - 4))
        return -1;
    for (int i = 0; i < 3; i++)
        pg[i] = -1;
    for (int i = 0; i < 3; i++) {
        uint64_t pa = 0;
        pg[i] = alloc_page(&pa);
        if (pg[i] < 0) {
            for (int k = 0; k < i; k++)
                free_idx(pg[k]);
            return -1;
        }
        p->pages[i] = pg[i];
        p->pa[i] = pa;
    }
    man.name = name;
    man.role = "el0-draw";
    man.mem_min_kb = 16 + (image ? image->pages * 4 : 0);
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (image && image->interp_entry) {
        /* The virtual DSO window also contains shared read-only and PROT_NONE pages. */
        man.mem_min_kb -= (ELF_DSO_PAGES - DSO_PRIVATE_PAGE_LIMIT) * 4;
        man.mem_min_kb += HIGH_FRAME_LIMIT * 4;
        p->private_page_limit = HIGH_FRAME_LIMIT + DSO_PRIVATE_PAGE_LIMIT;
#ifndef QUOTA_RUNTIME_VOLUME_BOOT
        if (image->role == WAP_BACKEND || image->role == WAP_NODE_ONLY) {
            p->private_page_limit -= 4096;
            man.mem_min_kb -= 4096 * 4;
        }
#endif
        if (image->role == WAP_FRONTEND || image->role == WAP_PAGE_ONLY) {
#ifdef QUOTA_NODE_WEB
            /* Blink's component build consumes over 52 MiB of private DSO
             * pages before Oilpan creates its first normal allocation page. */
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
            const unsigned extra_pages = 8192; /* 80 MiB on the external runtime candidate. */
#else
            const unsigned extra_pages = 12288; /* 96 MiB total private cap */
#endif
#else
            const unsigned extra_pages = 2048;
#endif
            p->private_page_limit += extra_pages;
            man.mem_min_kb += extra_pages * 4;
        }
    }
#endif
    man.mem_sugg_kb = man.mem_min_kb;
    man.cpu_min_us = 0;
    man.cpu_sugg_us = 0;
    if (resource_check(&man) != ADMIT_OK) {
        for (int i = 0; i < 3; i++)
            free_idx(p->pages[i]);
        printf("%s", resource_reason());
        return -1;
    }
    p->app = resource_commit(&man);
    if (!p->app) {
        for (int i = 0; i < 3; i++)
            free_idx(p->pages[i]);
        return -1;
    }
    pack = (uint64_t)vx | ((uint64_t)vy << 16) | ((uint64_t)vw << 32) | ((uint64_t)vh << 48);
    uint64_t user_sp = (console == CONSOLE_LINUX_BOOTSTRAP || console == CONSOLE_PAGE)
        ? linux_bootstrap_stack(p->pa[1], entry, headers, header_count, image) : USER_SP;
    if (image) {
        unsigned initial = image->pages - (image->interp_entry ? ELF_DSO_PAGES : 0);
        unsigned allocated = 0;
        for (; allocated < initial; ++allocated) {
            uint64_t pa;
            int pg = alloc_page(&pa);
            if (pg < 0) break;
            image_frames[p->slot][allocated] = pg;
        }
        if (allocated != initial) {
            while (allocated) free_idx(image_frames[p->slot][--allocated]);
            for (int i = 0; i < 3; ++i) free_idx(p->pages[i]);
            resource_release(p->app);
            p->app = NULL;
            return -1;
        }
    }
    p->task = task_try_create(name, TIER3, KIND_SPIN);
    if (!p->task || task_arm_el0(p->task, image && image->interp_entry ? image->interp_entry : entry, user_sp, role, limit, foreign_pa, fb_pa, kern_va, pack)) {
        if (p->task) {
            task_kill(p->task);
            p->task = 0;
        }
        if (image)
            for (unsigned i = 0; i < image->pages - (image->interp_entry ? ELF_DSO_PAGES : 0); ++i) free_idx(image_frames[p->slot][i]);
        resource_release(p->app);
        p->app = 0;
        for (int i = 0; i < 3; i++) {
            free_idx(p->pages[i]);
            p->pages[i] = -1;
            p->pa[i] = 0;
        }
        return -1;
    }
#ifdef QUOTA_LINUX_APP_RUNTIME
    memset(p->shm_pages, 0, sizeof p->shm_pages);
    p->temp_owner = make_handle(p);
    for (unsigned j = 0; j < SHM_OBJECTS; ++j) {
        struct shm_object *obj = &shm_objects[j];
        if (!obj->owner) continue;
        unsigned live = 0;
        for (unsigned k = 0; k < EL0_SLOTS; ++k)
            if ((procs[k].alive || procs[k].building) && procs[k].temp_owner == obj->owner) live = 1;
        if (!live) { obj->owner = 0; obj->path[0] = 0; }
    }
    for (unsigned j = 0; j < sizeof temp_dirs / sizeof *temp_dirs; ++j) {
        if (!temp_dirs[j].owner) continue;
        unsigned live = 0;
        for (unsigned k = 0; k < EL0_SLOTS; ++k)
            if ((procs[k].alive || procs[k].building) && procs[k].temp_owner == temp_dirs[j].owner) live = 1;
        if (!live) temp_dirs[j].owner = 0;
    }
#endif
    p->console = console;
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    p->volume_offset = console == CONSOLE_VOLUME_PROBE ? role : 0;
    p->volume_page = NULL;
    p->volume_waiting = 0;
    p->volume_exec = console == CONSOLE_VOLUME_PROBE && limit;
    p->volume_data_offset = p->volume_exec ? foreign_pa : 0;
    if (p->volume_exec) {
        p->volume_file = *(const struct runtime_volume_file *)kern_va;
        p->volume_data_size = vx;
    }
#endif
#ifdef QUOTA_NODE_WEB
    p->page_executable = image && (image->role == WAP_FRONTEND || image->role == WAP_PAGE_ONLY)
                         ? blink_executable() : NULL;
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
    if (image && image->volume_file &&
        (image->role == WAP_FRONTEND || image->role == WAP_PAGE_ONLY))
        p->page_executable = disk_catalog_lookup("quota-blink");
#endif
#endif
    p->document_size = console == CONSOLE_PAGE ? data_size : 0;
    memset(p->package_files, 0, sizeof p->package_files);
    if (console == CONSOLE_PAGE && image) {
        const unsigned char *contents[] = {data, image->backend};
        const unsigned sizes[] = {data_size, image->backend_size};
        const char *paths[] = {"/app/app.html", "/app/app.js"};
        for (unsigned i = 0; i < 2; ++i) {
            if (!contents[i]) continue;
            unsigned char *copy = p->package_contents[i];
            memcpy(copy, contents[i], sizes[i]);
            p->package_files[i] = (struct runtime_file){ paths[i], copy, copy + sizes[i] };
        }
    }
    p->asid = p->slot + 1;
    p->vx = vx;
    p->vy = vy;
    p->vw = vw;
    p->vh = vh;
    p->submit_ok = 1;
    p->hold = 0;
    p->submits = p->rej_range = p->rej_freq = p->rej_depth = 0;
    p->sub_period = ~0ull;
    p->sub_n = 0;
    p->unpaced_draw = role == EL0_PAINT_MUSIC;
    p->image_pages = 0;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    p->disk_segment_count = 0;
    memset(p->disk_clean, 0, sizeof p->disk_clean);
    p->v8_shared = p->page_ins = 0;
    p->clean_pages = p->clean_cursor = p->clean_scanned = p->evicted_pages = 0;
    p->disk_elf_waiting = 0;
    p->disk_read_waiting = 0;
#endif
    build_pt(p);
#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
    if (p->volume_data_offset)
        pt_l3[p->slot][DRAW_VA >> 12] = 0;
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
    memset(p->threads, 0, sizeof p->threads);
    p->fork_child = NULL;
    p->exec_cursor = 0;
    p->parent_handle = 0;
    p->empty_root = p->vfork_pid = 0;
    p->pid_namespace = 0;
    memset(p->pid_ancestors, 0, sizeof p->pid_ancestors);
    p->stdin_null = p->stdin_cloexec = 0;
    p->kernel_uid = p->kernel_gid = 1000 + p->slot;
    p->user_namespace = p->namespace_owner = 0;
    p->namespace_depth = 0;
    p->uid_mapped = p->gid_mapped = p->deny_setgroups = 0;
    memset(p->children, 0, sizeof p->children);
    p->exit_status = 9;
    p->thread_charge_base = 0;
    p->active_thread = 0;
    p->live_threads = 1;
    p->threads[0].tid = p->task->id;
    p->next_tid = 10000 + p->slot * 100000;
#endif
    p->linux_traces = 0;
#ifdef QUOTA_WAP_LOADER_TRACE
    p->mapping_traces = 0;
#endif
    p->linux_brk = 0;
    p->clear_child_tid = 0;
    p->signal_mask = 0;
    memset(p->signal_actions, 0, sizeof p->signal_actions);
    if (image) {
        memset(image_pt[p->slot], 0, sizeof image_pt[0]);
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
        if (image->volume_file)
            map_disk_image(p, image, headers, header_count);
        else
#endif
            map_image(p, image->file, image->load_bias, headers, header_count);
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB) && defined(QUOTA_RUNTIME_VOLUME_PAGER_TEST)
        if (image->volume_file && !image->interp_entry) {
            p->dso_frame = p->image_pages;
            p->mapped_dso_pages = 0;
            p->private_page_limit = 16;
            for (unsigned i = 0; i < ELF_DSO_PAGES; ++i)
                image_frames[p->slot][p->dso_frame + i] = -1;
            p->image_pages += ELF_DSO_PAGES;
            memset(p->dso_source, 0, sizeof p->dso_source);
            memset(p->dso_used, 0, sizeof p->dso_used);
            memset(p->dso_anonymous, 0, sizeof p->dso_anonymous);
        }
#endif
        if (image->interp_entry) {
            map_image(p, image->interpreter, ELF_INTERP_BASE,
                      image->interpreter + image->interp_phoff, image->interp_count);
            for (unsigned va = ELF_STACK_TOP - ELF_STACK_PAGES * 4096;
                 va < ELF_BRK_BASE + ELF_BRK_PAGES * 4096; va += 4096) {
                uint64_t pa = p->pa[1];
                if (va != ELF_STACK_TOP - 4096)
                    pa = (uintptr_t)pool[image_frames[p->slot][p->image_pages++]];
                image_pt[p->slot][(va - ELF_BASE) >> 12] = data_pte(pa);
            }
            pt_l3[p->slot][1] = 0;
            p->linux_brk = ELF_BRK_BASE;
            p->dso_frame = p->image_pages;
            p->mapped_dso_pages = 0;
#ifdef QUOTA_WAP_FULL_RUNTIME
            memset(p->reservations, 0, sizeof p->reservations);
#endif
            for (unsigned i = 0; i < ELF_DSO_PAGES; ++i)
                image_frames[p->slot][p->dso_frame + i] = -1;
            p->image_pages += ELF_DSO_PAGES;
            memset(p->dso_source, 0, sizeof p->dso_source);
            memset(p->dso_used, 0, sizeof p->dso_used);
            memset(p->dso_anonymous, 0, sizeof p->dso_anonymous);
            memset(p->files, 0, sizeof p->files);
        }
    }
    else memcpy((void *)(uintptr_t)p->pa[0], code, (size_t)blen);
    if (data_size) memcpy((void *)(uintptr_t)p->pa[2], data, data_size);
    if (!image) clean_code((void *)(uintptr_t)p->pa[0], blen);
    tlb_asid(p->asid);
    p->task->user_ttbr = p->ttbr;
    p->pid = p->task->id;
    if (spawn_logs < 2) {
        int lv = 0;
        uint64_t pc = walk(p, CODE_VA, &lv);
        uint64_t ps = walk(p, STACK_VA, &lv);
        uint64_t pd = walk(p, DRAW_VA, &lv);
        uint64_t pkern = walk(p, kern_va, &lv);
        uint64_t pdev = walk(p, 0x09000000ull, &lv);
        uint64_t pfb = walk(p, fb_pa, &lv);
        printf("[map] pid=%d handle=0x%lx gen=%u asid=%d ttbr=0x%lx draw_pa=0x%lx\n",
               p->pid, (unsigned long)make_handle(p), p->gen, p->asid, p->ttbr, p->pa[2]);
        printf("[map] pte code=0x%lx stack=0x%lx draw=0x%lx ap_el0=%d/%d/%d\n",
               pc, ps, pd, (int)((pc >> 6) & 1), (int)((ps >> 6) & 1), (int)((pd >> 6) & 1));
        printf("[map] walk kern=0x%lx pte=0x%lx ap_el0=%d dev pte=0x%lx ap_el0=%d fb pte=0x%lx ap_el0=%d\n",
               kern_va, pkern, (int)((pkern >> 6) & 1), pdev, (int)((pdev >> 6) & 1),
               pfb, (int)((pfb >> 6) & 1));
    } else {
        printf("[map] pid=%d handle=0x%lx gen=%u asid=%d draw_pa=0x%lx\n",
               p->pid, (unsigned long)make_handle(p), p->gen, p->asid, p->pa[2]);
    }
    spawn_logs++;
    {
        struct irq_span off0;
        uint64_t published;
        irq_off_begin(&off0);
        if (console == CONSOLE_PAGE && (!image || image->role == WAP_SINGLE || image->role == WAP_PAGE_ONLY)) {
            page_owner = make_handle(p);
            page_rows = 0;
#ifdef QUOTA_LINUX_APP_RUNTIME
        electron_generation = input_count = input_head = 0;
#endif
        }
        p->alive = 1;
        if (!image || image->role == WAP_SINGLE || image->role == WAP_PAGE_ONLY ||
            image->role == WAP_NODE_ONLY) task_ready(p->task);
        published = make_handle(p);
        irq_off_end(off0);
        return (int64_t)published;
    }
}

int64_t proc_spawn(const char *name, uint64_t role, uint64_t limit, uint64_t foreign_pa,
                   uint64_t fb_pa, uint64_t kern_va, uint16_t vx, uint16_t vy,
                   uint16_t vw, uint16_t vh)
{
    return spawn_image(NULL, user_blob, user_blob_end - user_blob, NULL, 0, NULL, 0, 0, CONSOLE_NONE, name, role, limit,
                       foreign_pa, fb_pa, kern_va, vx, vy, vw, vh);
}

int64_t proc_spawn_console(const char *code, unsigned bytes)
{
    console_updated = 0;
    return spawn_image(NULL, code, bytes, NULL, 0, NULL, 0, 0, CONSOLE_QUOTA, "hello", 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

#ifdef QUOTA_RUNTIME_VOLUME_PAGER_TEST
int64_t proc_spawn_volume_probe(const char *name, const struct runtime_volume_file *file,
                                 uint64_t code_offset, uint64_t data_offset, unsigned data_size)
{
    /* mov x1,#0x3000; br x1 */
    static const uint32_t jump[] = { 0xd2860001, 0xd61f0020 };
    if ((code_offset & 4095) || code_offset > file->size || file->size - code_offset < 4096 ||
        !data_offset || data_offset > file->size || data_size > 4096 ||
        data_size > file->size - data_offset)
        return -1;
    return spawn_image(NULL, (const char *)jump, sizeof jump, NULL, 0, NULL, 0, 0,
                       CONSOLE_VOLUME_PROBE, name, file->offset + code_offset, 1,
                       data_offset, 0, (uintptr_t)file, data_size, 0, 0, 0);
}
#endif

/* The existing single RX page process layout supports this ELF subset.
 * Linux runtime executables need a separate mapper and process ABI. */
static uint64_t elf_number(const unsigned char *bytes, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static int parse_interpreter(struct elf_mapping *image, const unsigned char *file, size_t bytes)
{
    if (!file || bytes < 64) return -1;
    if (elf_number(file, 4) != 0x464c457f || file[4] != 2 || file[5] != 1 ||
        file[6] != 1 || elf_number(file + 16, 2) != 3 || elf_number(file + 18, 2) != 183 ||
        elf_number(file + 20, 4) != 1 || elf_number(file + 52, 2) != 64 ||
        elf_number(file + 54, 2) != 56) return -1;
    uint64_t entry = elf_number(file + 24, 8), off = elf_number(file + 32, 8);
    unsigned count = elf_number(file + 56, 2);
    if (off < 64 || off > bytes || !count || count > 32 || bytes - off < count * 56 ||
        (entry & 3)) return -1;
    uint64_t next = 0;
    int valid_entry = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *ph = file + off + i * 56;
        unsigned type = elf_number(ph, 4);
        if (type == 3) return -1;
        if (type != 1) continue;
        unsigned flags = elf_number(ph + 4, 4);
        uint64_t src = elf_number(ph + 8, 8), va = elf_number(ph + 16, 8);
        uint64_t len = elf_number(ph + 32, 8), mem = elf_number(ph + 40, 8);
        uint64_t align = elf_number(ph + 48, 8);
        if ((flags != 4 && flags != 5 && flags != 6) || src > bytes || len > bytes - src ||
            !mem || mem < len || va < next || va >= ELF_DSO_BASE - ELF_INTERP_BASE ||
            mem > ELF_DSO_BASE - ELF_INTERP_BASE - va ||
            (align > 1 && ((align & (align - 1)) || ((src ^ va) & (align - 1)) ||
                           (ELF_INTERP_BASE & (align - 1))))) return -1;
        next = (va + mem + 4095) & ~4095ull;
        image->pages += ((next - (va & ~4095ull)) >> 12) - shared_image_pages(file, flags, src, va, len);
        if (flags == 5 && entry >= va && entry <= va + len && va + len - entry >= 4)
            valid_entry = 1;
    }
    if (!valid_entry) return -1;
    image->interpreter = file;
    image->interp_entry = ELF_INTERP_BASE + entry;
    image->interp_phoff = off;
    image->interp_count = count;
    return 0;
}

#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
/* The desktop loads one page per poll and publishes only a validated ELF.
 * The published image is immutable for the lifetime of the boot. */
static unsigned char volume_interpreter[256 * 1024] __attribute__((aligned(4096)));
static unsigned interpreter_read;
static int interpreter_ready;

int proc_prepare_volume_interpreter(const struct runtime_volume_file *file)
{
    if (interpreter_ready)
        return 1;
    if (!file || file->size < 64 || file->size > sizeof volume_interpreter) {
        interpreter_read = 0;
        return -1;
    }
    unsigned remaining = file->size - interpreter_read;
    unsigned length = remaining < 4096 ? remaining : 4096;
    int result = runtime_volume_read_file(file, interpreter_read,
                                          volume_interpreter + interpreter_read, length);
    if (result != 1) {
        if (result < 0)
            interpreter_read = 0;
        return result;
    }
    interpreter_read += length;
    if (interpreter_read != file->size)
        return 0;
    struct elf_mapping layout = {0};
    if (parse_interpreter(&layout, volume_interpreter, interpreter_read)) {
        interpreter_read = 0;
        return -1;
    }
    interpreter_ready = 1;
    printf("[wap-volume] interpreter ready bytes=%u entry=%x\n",
           interpreter_read, layout.interp_entry);
    return 1;
}
#endif

static int prepare_interpreter(struct elf_mapping *image)
{
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    if (interpreter_ready)
        return parse_interpreter(image, volume_interpreter, interpreter_read);
#endif
    uintptr_t start = (uintptr_t)wap_interp_start, end = (uintptr_t)wap_interp_end;
    if (end < start)
        return -1;
    return parse_interpreter(image, wap_interp_start, end - start);
}

static int64_t spawn_elf_source(const unsigned char *elf, unsigned bytes, const unsigned char *document, unsigned size,
                         const unsigned char *backend, unsigned backend_size, enum wap_role role,
                         const unsigned char *program_headers
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
                         , const struct runtime_volume_file *disk_file
#endif
                         )
{
    if (!elf || bytes < 64 || elf_number(elf, 4) != 0x464c457f ||
        elf[4] != 2 || elf[5] != 1 || elf[6] != 1 || (elf[7] != 0 && elf[7] != 3) ||
        (elf_number(elf + 16, 2) != 2 && elf_number(elf + 16, 2) != 3) || elf_number(elf + 18, 2) != 183 ||
        elf_number(elf + 20, 4) != 1 || elf_number(elf + 52, 2) != 64 ||
        elf_number(elf + 54, 2) != 56)
        return -1;

    uint64_t entry = elf_number(elf + 24, 8), phoff = elf_number(elf + 32, 8);
    unsigned bias = elf_number(elf + 16, 2) == 3 ? ELF_BASE : 0;
    if (entry > UINT64_MAX - bias) return -1;
    entry += bias;
    unsigned count = elf_number(elf + 56, 2);
    if (!count || count > 32 || phoff < 64 || phoff > bytes || bytes - phoff < count * 56)
        return -1;
    if (!program_headers)
        program_headers = elf + phoff;
    if (entry >= ELF_BASE && entry < ELF_BASE + ELF_WINDOW) {
        if (size > sizeof procs[0].package_contents[0] || backend_size > sizeof procs[0].package_contents[1] || (!backend && backend_size)) return -1;
        struct elf_mapping image = { .role = role, .file = elf, .load_bias = bias, .backend = backend, .backend_size = backend_size };
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
        image.volume_file = disk_file;
#endif
        uint64_t next_page = ELF_BASE;
        int executable_entry = 0, interpreter = 0;
        /* WAP roles open private package files; the legacy tile ABI shares
         * document storage with pixel rows in the same page. */
        if ((entry & 3) || (role == WAP_SINGLE && size > GUEST_TILE_OFFSET)) return -1;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned char *ph = program_headers + 56 * i;
            unsigned type = elf_number(ph, 4), flags = elf_number(ph + 4, 4);
            if (type == 3) {
                static const char name[] = "/lib/ld-linux-aarch64.so.1";
                uint64_t pos = elf_number(ph + 8, 8);
                if (interpreter || pos > bytes || sizeof name > bytes - pos ||
                    elf_number(ph + 32, 8) != sizeof name) return -1;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
                /* The volume parser validates PT_INTERP before publishing its headers. */
                if (!disk_file)
#endif
                    if (elf[pos + sizeof name - 1] || strcmp((const char *)elf + pos, name)) return -1;
                interpreter = 1;
                continue;
            }
            if (!type || type == 2 || type == 7 || type == 0x6474e553 || type == 4 || type == 6 || type == 0x6474e550 ||
                type == 0x6474e552 || (type == 0x6474e551 && !(flags & 1))) continue;
            if (type != 1 || (flags != 4 && flags != 5 && flags != 6)) return -1;
            uint64_t off = elf_number(ph + 8, 8), va = elf_number(ph + 16, 8);
            uint64_t len = elf_number(ph + 32, 8), mem = elf_number(ph + 40, 8);
            uint64_t align = elf_number(ph + 48, 8);
            if (va > UINT64_MAX - bias || (align > 1 && (bias & (align - 1)))) return -1;
            va += bias;
            if (off > bytes || len > bytes - off || len > mem || !mem ||
                va < next_page || va >= ELF_BASE + ELF_WINDOW ||
                mem > ELF_BASE + ELF_WINDOW - va ||
                (align > 1 && ((align & (align - 1)) || ((off ^ va) & (align - 1)))))
                return -1;
            if (phoff >= off && phoff - off <= len && count * 56u <= len - (phoff - off))
                image.phdr_va = va + (phoff - off);
            next_page = (va + mem + 4095) & ~4095ull;
            image.pages += (next_page - (va & ~4095ull)) >> 12;
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
            if (!disk_file)
#endif
                image.pages -= shared_image_pages(elf, flags, off, va, len);
            if (flags == 5 && entry >= va && entry + 4 <= va + len) executable_entry = 1;
        }
        if (bias && !interpreter) return -1;
        if (interpreter && (!image.phdr_va || next_page > ELF_INTERP_BASE ||
                            prepare_interpreter(&image))) return -1;
        if (interpreter) image.pages += ELF_STACK_PAGES - 1 + ELF_BRK_PAGES + ELF_DSO_PAGES;
        unsigned private_pages = image.pages - (interpreter ? ELF_DSO_PAGES : 0);
        if (!executable_entry || image.pages > IMAGE_FRAME_SLOTS - 3 ||
            private_pages > EL0_POOL - 3) return -1;
#ifdef QUOTA_LINUX_APP_RUNTIME
        if (elf == wap_worker_start) {
            system_layout = image;
            system_layout.role = WAP_SINGLE;
        }
#endif
        return spawn_image(&image, NULL, 0, document, size, program_headers, count, entry,
                           document ? CONSOLE_PAGE : CONSOLE_LINUX_BOOTSTRAP, (role == WAP_BACKEND || role == WAP_NODE_ONLY) ? "wap-backend" : "elf-page",
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
    }
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
    if (disk_file) return -1;
#endif
    if (role != WAP_SINGLE) return -1;
    const unsigned char *code = NULL, *data = NULL;
    unsigned code_size = 0, data_size = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *load = elf + phoff + i * 56;
        uint64_t offset = elf_number(load + 8, 8), va = elf_number(load + 16, 8);
        uint64_t length = elf_number(load + 32, 8), memory = elf_number(load + 40, 8);
        unsigned flags = elf_number(load + 4, 4);
        if (elf_number(load, 4) != 1 || offset > bytes || length > bytes - offset ||
            length > memory || !memory || memory > 4096)
            return -1;
        if (va == CODE_VA && flags == 5 && !code && length == memory) {
            code = elf + offset;
            code_size = length;
        } else if (va == DRAW_VA && flags == 6 && !data) {
            data = elf + offset;
            data_size = length;
        } else {
            return -1;
        }
    }
    if (code_size < 4 || entry > code_size - 4 || (entry & 3))
        return -1;
    if (document) {
        /* The bootstrap address space reserves the draw page for document IPC. */
        if (data || size > GUEST_TILE_OFFSET) return -1;
        data = document;
        data_size = size;
    }
    console_updated = 0;
    return spawn_image(NULL, (const char *)code, code_size, data, data_size, elf + phoff, count, (unsigned)entry,
                       document ? CONSOLE_PAGE : CONSOLE_LINUX_BOOTSTRAP, "elf-bootstrap", 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

static int64_t spawn_elf(const unsigned char *elf, unsigned bytes, const unsigned char *document, unsigned size,
                         const unsigned char *backend, unsigned backend_size, enum wap_role role)
{
    return spawn_elf_source(elf, bytes, document, size, backend, backend_size, role, NULL
#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
                            , NULL
#endif
                            );
}

#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB) && defined(QUOTA_RUNTIME_VOLUME_PAGER_TEST)
int64_t proc_spawn_disk_elf_test(const struct runtime_volume_file *file,
                                 const void *header, const void *program_headers)
{
    if (file->size > UINT32_MAX)
        return -1;
    static const unsigned char document[] = "";
    return spawn_elf_source(header, file->size, document, 0, NULL, 0,
                            WAP_PAGE_ONLY, program_headers, file);
}
#endif


int64_t proc_spawn_bootstrap_elf(const unsigned char *elf, unsigned bytes)
{
    return spawn_elf(elf, bytes, NULL, 0, NULL, 0, WAP_SINGLE);
}

int64_t proc_spawn_page_elf(const unsigned char *elf, unsigned bytes,
                            const unsigned char *document, unsigned size)
{
    return document ? spawn_elf(elf, bytes, document, size, NULL, 0, WAP_SINGLE) : -1;
}

#ifdef QUOTA_LINUX_APP_RUNTIME
static int64_t connect_wap_pair(int64_t ui, int64_t js)
{
    uint64_t flags = irq_save();
    struct proc *front = from_handle(ui, 1), *back = from_handle(js, 1);
    struct packet_socket *ends[2];
    unsigned n = 0;
    for (unsigned i = 0; i < WAP_OBJECT_SLOTS && n < 2; ++i)
        if (!packet_sockets[i].refs) ends[n++] = &packet_sockets[i];
    if (!front || !back || n != 2) {
        irq_restore(flags);
        proc_kill(ui); proc_kill(js);
        return -1;
    }
    struct proc *members[] = {front, back};
    for (unsigned i = 0; i < 2; ++i) {
        ends[i]->refs = 1;
        ends[i]->stream = 0;
        ends[i]->nonblock = 0x800;
        ends[i]->passcred = ends[i]->shut = ends[i]->packets = ends[i]->packet_head = 0;
        ends[i]->peer = ends[i^1];
        members[i]->files[0].identity = ++next_fd_identity;
        members[i]->files[0].socket = ends[i];
        members[i]->files[0].event_flags = FD_PACKET;
    }
    backend_owner = js; backend_ui = ui; backend_line_ready = 0;
    back->files[1].event_flags = FD_WAP_NOTE;
    back->pending_note = back->note_writer_tid = 0;
    page_owner = ui;
    page_rows = 0;
#ifdef QUOTA_LINUX_APP_RUNTIME
    electron_generation = input_count = input_head = 0;
#endif
    task_ready(front->task);
    task_ready(back->task);
    irq_restore(flags);
    printf("[wap] launched pair frontend=%ld backend=%ld ipc_fd=3\n", ui, js);
    return ui;
}
#endif

#if defined(QUOTA_RUNTIME_VOLUME_BOOT) && defined(QUOTA_NODE_WEB)
int proc_volume_users_active(void)
{
    for (unsigned i = 0; i < EL0_SLOTS; ++i)
        if ((procs[i].alive || procs[i].draining) && procs[i].disk_segment_count)
            return 1;
    return 0;
}

static int64_t spawn_volume_role(const struct runtime_volume_elf *elf,
                                 const unsigned char *html, unsigned html_size,
                                 const unsigned char *backend, unsigned backend_size,
                                 enum wap_role role)
{
    if (elf->file.size > UINT32_MAX)
        return -1;
    return spawn_elf_source(elf->header, elf->file.size, html, html_size,
                            backend, backend_size, role, elf->programs, &elf->file);
}

int64_t proc_spawn_volume_wap(const struct runtime_volume_elf *blink,
                              const struct runtime_volume_elf *node,
                              const unsigned char *html, unsigned html_size,
                              const unsigned char *backend, unsigned backend_size)
{
    int64_t ui = spawn_volume_role(blink, html, html_size, backend, backend_size,
                                   backend ? WAP_FRONTEND : WAP_PAGE_ONLY);
    if (ui <= 0 || !backend)
        return ui;
    int64_t js = spawn_volume_role(node, html, html_size, backend, backend_size, WAP_BACKEND);
    if (js <= 0) {
        proc_kill(ui);
        return js;
    }
    return connect_wap_pair(ui, js);
}
#endif

int64_t proc_spawn_wap_elf(const unsigned char *elf, unsigned bytes,
                           const unsigned char *html, unsigned html_size,
                           const unsigned char *backend, unsigned backend_size)
{
    if (!html) return -1;
#ifdef QUOTA_STANDALONE_NODE
    if (!backend || !backend_size) return -1;
    return spawn_elf(elf, bytes, html, html_size, backend, backend_size, WAP_NODE_ONLY);
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
    const unsigned char *ui_elf = elf;
    unsigned ui_bytes = bytes;
#ifdef QUOTA_NODE_WEB
    const struct runtime_file *renderer = blink_executable();
    if (!renderer || renderer->end - renderer->begin > 0xffffffffu) return -1;
    ui_elf = renderer->begin;
    ui_bytes = renderer->end - renderer->begin;
#endif
    if (backend) {
        int64_t ui = spawn_elf(ui_elf, ui_bytes, html, html_size, backend, backend_size, WAP_FRONTEND);
        if (ui <= 0) return ui;
        int64_t js = spawn_elf(elf, bytes, html, html_size, backend, backend_size, WAP_BACKEND);
        if (js <= 0) { proc_kill(ui); return js; }
        return connect_wap_pair(ui, js);
    }
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (!backend) return spawn_elf(ui_elf, ui_bytes, html, html_size, NULL, 0, WAP_PAGE_ONLY);
#endif
    return spawn_elf(elf, bytes, html, html_size, backend, backend_size, WAP_SINGLE);
}

void proc_ledger(const char *tag)
{
    int procs_n = 0;
    for (int i = 0; i < EL0_SLOTS; i++)
        procs_n += procs[i].alive ? 1 : 0;
    printf("[ledger] %s procs=%d pages_free=%d pages_used=%d queue=%d holds=%d mem_kb=%lu running=%d refusals=%d stale_drain=%d\n",
           tag, procs_n, pages_free_n(), pages_used_n(), proc_queue_depth(), proc_holds(),
           resource_mem_min_sum_kb(), resource_running_count(), unmap_refusals, stale_drain);
    for (int i = 0; i < EL0_SLOTS; i++) {
        struct proc *p = &procs[i];
        if (!p->alive)
            continue;
        printf("[ledger] proc pid=%d gen=%u asid=%d handle=0x%lx ttbr=0x%lx draw=0x%lx submits=%u range=%u freq=%u depth=%u hold=%d q=%d\n",
               p->pid, p->gen, p->asid, (unsigned long)make_handle(p), p->ttbr, p->pa[2],
               p->submits, p->rej_range, p->rej_freq, p->rej_depth, p->hold,
               queue_for_gen(p->slot, p->gen));
    }
}

void proc_on_switch(struct task *next)
{
#ifdef QUOTA_WAP_FULL_RUNTIME
    /* A different EL0 process must resume its own TLS and SIMD state even
     * when its last switch was a timer preemption rather than a syscall. */
    static struct proc *owner;
    static uint32_t owner_gen;
    static uint64_t kernel_tls;
    static uint64_t kernel_fp[66] __attribute__((aligned(16)));
    struct proc *incoming = next && next->user_ttbr ? proc_by_task(next) : NULL;
#ifdef QUOTA_WAP_LOADER_TRACE
    if (incoming && incoming->exec_resumes) {
        const struct irq_frame *saved = (const void *)(uintptr_t)next->sp;
        printf("[wap] exec resume pid=%d pc=%lx sp=%lx psr=%lx\n",
               incoming->pid, saved->elr, saved->pad, saved->spsr);
        --incoming->exec_resumes;
    }
#endif
    if (incoming != owner || (incoming && incoming->gen != owner_gen)) {
        if (!owner) {
            asm volatile("mrs %0,tpidr_el0" : "=r"(kernel_tls));
            wap_fp_save(kernel_fp);
        } else if (owner->alive && owner->gen == owner_gen) {
            struct wap_thread *thread = &owner->threads[owner->active_thread];
            asm volatile("mrs %0,tpidr_el0" : "=r"(thread->tls));
            wap_fp_save(thread->fp);
        }
        struct wap_thread *thread = incoming ? &incoming->threads[incoming->active_thread] : NULL;
        uint64_t tls = thread ? thread->tls : kernel_tls;
        asm volatile("msr tpidr_el0,%0" :: "r"(tls));
        wap_fp_restore(thread ? thread->fp : kernel_fp);
        owner = incoming;
        owner_gen = incoming ? incoming->gen : 0;
    }
#endif
    /* Do not switch TTBR0 here. Kernel code is still running at its link address.
     * gate_eret installs this value on the way back to EL0. */
    if (next && next->user_ttbr)
        gate_user_ttbr = next->user_ttbr;
    else
        gate_user_ttbr = 0;
}

static void init_gate_map(void)
{
    uint64_t gva = (uint64_t)(uintptr_t)gate_vectors & ~4095ull;
    uint64_t l2i = (gva >> 21) & 511ull;
    uint64_t l3i = (gva >> 12) & 511ull;
    memset(el0_l2_hi, 0, sizeof el0_l2_hi);
    memset(el0_l3_gate, 0, sizeof el0_l3_gate);
    el0_l3_gate[l3i] = (gva & ~4095ull) | PTE_VALID | PTE_PAGE | PTE_AF | SH_INNER | PTE_UXN;
    el0_l2_hi[l2i] = (uint64_t)(uintptr_t)el0_l3_gate | PTE_VALID | PTE_PAGE;
    printf("[proc] EL0 table maps the app pages plus one EL1-only gate page at 0x%lx. It does not map the kernel RAM block, devices, or the framebuffer.\n",
           gva);
    printf("[proc] permission-only sharing of the kernel L2 is not what this table does.\n");
}

void proc_init(void)
{
    memset(procs, 0, sizeof procs);
    memset(qq, 0, sizeof qq);
    memset(pool_used, 0, sizeof pool_used);
    free_frames = pool_capacity;
    for (int i = 0; i < EL0_SLOTS; i++) {
        procs[i].slot = i;
        for (int k = 0; k < 3; k++)
            procs[i].pages[k] = -1;
    }
    kernel_ttbr = mmu_kernel_ttbr();
    gate_kernel_ttbr = kernel_ttbr;
    gate_user_ttbr = 0;
    init_gate_map();
    printf("[proc] EL0 slots %d pool %d draw %u queue %d/%d freq %d/period. target %u us is a goal, not a measured bound.\n",
           EL0_SLOTS, EL0_POOL, EL0_DRAW, EL0_Q_PER, EL0_Q_GLOBAL, EL0_FREQ, EL0_REAP_TARGET_US);
    printf("[proc] kernel ttbr 0x%lx. Handle is sampled before the task can run. Reap time is CNTVCT, not the IRQ period counter.\n",
           kernel_ttbr);
}

uint64_t proc_irq_off_max_us(void)
{
    return ticks_to_us(irq_off_max);
}
