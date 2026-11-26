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
#define EL0_SLOTS 10
#else
#define EL0_SLOTS 2
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
/* Dedicated large-memory build. The static pool is covered by system_reserve_kb. */
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
#ifdef QUOTA_WAP_INTERPRETER
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
enum console_abi { CONSOLE_NONE, CONSOLE_QUOTA, CONSOLE_LINUX_BOOTSTRAP, CONSOLE_PAGE };
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
    const unsigned char *file;
    unsigned pages;
    unsigned load_bias;
    unsigned phdr_va;
    unsigned interp_entry;
    unsigned interp_phoff;
    unsigned interp_count;
};
static uint8_t pool[EL0_POOL][4096] __attribute__((aligned(4096)));
static uint8_t pool_used[EL0_POOL];
static unsigned free_frames = EL0_POOL;
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
    return EL0_POOL - pages_free_n();
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
    period = sched_period_now();
    if (period != p->sub_period) {
        p->sub_period = period;
        p->sub_n = 0;
    }
    if (p->sub_n >= EL0_FREQ) {
        p->rej_freq++;
        return 2;
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
    p->sub_n++;
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
static void finish_one(struct proc *p)
{
    int refused = 0;
    uint64_t t1, pct1;
    if (!p->draining || p->active_draws)
        return;
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
        tlb_asid(p->asid);
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
        int64_t result = p->linux_brk ? runtime_syscall(p, f) : -38;
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
    for (unsigned scanned = 0; scanned < EL0_POOL; ++scanned) {
        uint64_t flags = irq_save();
        if (free_frames <= reserved_frames) {
            irq_restore(flags);
            return -1;
        }
        unsigned i = cursor++ % EL0_POOL;
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
    memset(pool[idx], 0, 4096);
    uint64_t flags = irq_save();
    pool_used[idx] = 0;
    ++free_frames;
    irq_restore(flags);
}

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

#ifdef QUOTA_LINUX_APP_RUNTIME
#if defined(QUOTA_STANDALONE_NODE) || defined(QUOTA_NODE_WEB)
#define SYSTEM_RUNTIME_PATH "/usr/lib/quota-runtime/node"
#else
#define SYSTEM_RUNTIME_PATH "/usr/lib/quota-runtime/electron"
#endif
extern const unsigned char wap_worker_start[], wap_worker_end[];
static const struct runtime_file note_path = { .name = "/app/data/note.txt" };
static const struct runtime_file window_device = { .name = "/dev/quota-window" };
static const struct runtime_file system_executable = { SYSTEM_RUNTIME_PATH, wap_worker_start, wap_worker_end };
#ifdef QUOTA_NODE_WEB
#define BLINK_RUNTIME_PATH "/usr/lib/quota-runtime/quota-blink"
static const struct runtime_file *blink_executable(void)
{
    for (const struct runtime_file *file = wap_libs_start; file && file < wap_libs_end; ++file)
        if (!strcmp(file->name, "quota-blink")) return file;
    return NULL;
}
#endif
static const struct runtime_file runtime_dirs[] = {
    { .name = "/" }, { .name = "/usr" }, { .name = "/usr/lib" }, { .name = "/usr/lib/quota-runtime" }, { .name = "/usr/lib/quota-runtime/resources" },
    { .name = "/usr/lib/quota-runtime/fonts" }, { .name = "/usr/lib/quota-runtime/resources/app" }, { .name = "/usr/lib/quota-runtime/locales" }, { .name = "/tmp" }
};
/* Shared-memory objects remain pinned throughout their application instance. */
#define SHM_CAPACITY (1024u * 1024u)
static const struct runtime_file shm_dir = { .name = "/dev/shm" };
static struct shm_object {
    struct runtime_file file;
    int64_t owner;
    char path[128];
} shm_objects[SHM_OBJECTS];
static unsigned char shm_data[SHM_OBJECTS][SHM_CAPACITY] __attribute__((aligned(4096)));

static int shm_slot_busy(unsigned index)
{
    struct shm_object *obj = &shm_objects[index];
    if (!obj->owner) return 0;
    if (obj->path[0]) return 1;
    for (unsigned i = 0; i < EL0_SLOTS; ++i)
        if ((procs[i].alive || procs[i].building || procs[i].draining) && procs[i].shm_pages[index]) return 1;
    for (unsigned i = 0; i < WAP_OBJECT_SLOTS; ++i)
        if (positions[i].refs && positions[i].backing == &obj->file) return 1;
    return 0;
}

static void shm_account(struct proc *p, uintptr_t pa, int adding)
{
    uintptr_t offset = pa - (uintptr_t)shm_data;
    if (offset >= sizeof shm_data) return;
    if (adding) ++p->shm_pages[offset / SHM_CAPACITY];
    else --p->shm_pages[offset / SHM_CAPACITY];
}

static struct shm_object *shm_for_file(const struct runtime_file *file)
{
    for (unsigned i = 0; i < SHM_OBJECTS; ++i)
        if (file == &shm_objects[i].file) return &shm_objects[i];
    return NULL;
}
static int shm_path(struct proc *p, uint64_t address, char path[128])
{
    static const char prefix[] = "/dev/shm/";
    if (p->empty_root) return -2;
    for (unsigned i = 0; i < 128; ++i) {
        if (copy_process_bytes(p, address+i, path+i, 1, 0)) return -14;
        if (i < sizeof prefix-1 && path[i] != prefix[i]) return -2;
        if (i >= sizeof prefix-1 && path[i] == '/') return -2;
        if (!path[i]) {
            const char *name = path + sizeof prefix-1;
            return !*name || !strcmp(name,".") || !strcmp(name,"..") ? -2 : 0;
        }
    }
    return -36;
}
struct temp_directory {
    struct runtime_file file;
    int64_t owner;
    unsigned mode;
    char path[192];
};
static struct temp_directory temp_dirs[EL0_SLOTS * 8];

static int temp_mkdir(struct proc *p, uint64_t address, unsigned mode)
{
    char path[sizeof temp_dirs[0].path];
    unsigned n = 0, slash = 0;
    for (; n < sizeof path; ++n) {
        if (copy_process_bytes(p, address + n, path+n, 1, 0)) return -14;
        if (!path[n]) break;
        if (path[n] == '/') slash = n;
    }
    if (n == sizeof path) return -36;
    if (p->empty_root) return -2;
    if (n < 5 || path[0] != '/' || path[1] != 't' || path[2] != 'm' || path[3] != 'p' || path[4] != '/') return -30;
    unsigned parent = slash == 4;
    struct temp_directory *slot = NULL;
    for (unsigned i = 0; i < sizeof temp_dirs / sizeof *temp_dirs; ++i) {
        struct temp_directory *d = &temp_dirs[i];
        if (!d->owner) { if (!slot) slot = d; continue; }
        if (d->owner != p->temp_owner) continue;
        if (!strcmp(path, d->path)) return -17;
        path[slash] = 0;
        if (!strcmp(path, d->path)) parent = 1;
        path[slash] = '/';
    }
    if (!parent) return -2;
    if (!path[slash+1] || !strcmp(path+slash+1, ".") || !strcmp(path+slash+1, "..")) return -22;
    if (!slot) return -28;
    memcpy(slot->path, path, n+1);
    slot->file.name = slot->path;
    slot->owner = p->temp_owner;
    slot->mode = mode & 0777;
    return 0;
}

#endif
static const struct runtime_file app_dir = { .name = "/app" };
static const struct runtime_file fd_dir = { .name = "/proc/self/fd" };
static const struct runtime_file task_dir = { .name = "/proc/self/task" };
static const struct runtime_file proc_dir = { .name = "/proc" };
static const struct runtime_file null_file = { .name = "/dev/null" };
static const struct runtime_file urandom_file = { .name = "/dev/urandom" };

#ifdef QUOTA_WAP_FULL_RUNTIME
static const struct runtime_file pid_ns_file = { .name = "/proc/self/ns/pid" };
static const struct runtime_file user_ns_file = { .name = "/proc/self/ns/user" };
static const struct runtime_file uid_map = { .name = "/proc/self/uid_map" };
static const struct runtime_file gid_map = { .name = "/proc/self/gid_map" };
static const struct runtime_file setgroups_file = { .name = "/proc/self/setgroups" };

static int parse_id(const char **cursor, unsigned *value)
{
    const char *s = *cursor;
    unsigned v = 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s < '0' || *s > '9') return -1;
    do {
        unsigned d = (unsigned)(*s++ - '0');
        if (v > (0xffffffffu - d) / 10) return -1;
        v = 10 * v + d;
    } while (*s >= '0' && *s <= '9');
    *cursor = s;
    *value = v;
    return 0;
}

static void enter_user_namespace(struct proc *p, unsigned depth, unsigned uid, unsigned gid)
{
    static uint64_t next_namespace;
    p->user_namespace = ++next_namespace;
    struct wap_thread *self = &p->threads[p->active_thread];
    self->ns_effective = self->ns_permitted = NS_CAP_SYS_ADMIN | NS_CAP_SYS_CHROOT;
    p->namespace_owner = make_handle(p);
    p->namespace_depth = depth;
    p->parent_uid = uid;
    p->parent_gid = gid;
    p->uid_mapped = p->gid_mapped = 0;
}
static int write_self_map(struct proc *p, const struct runtime_file *file, uint64_t address, uint64_t size)
{
    char text[64];
    if (p->namespace_owner != make_handle(p)) return -1;
    if (!size || size >= sizeof text) return -22;
    if (copy_process_bytes(p, address, text, size, 0)) return -14;
    for (unsigned i = 0; i < size; ++i) if (!text[i]) return -22;
    text[size] = 0;
    if (text[size - 1] == '\n') text[size - 1] = 0;
    if (file == &setgroups_file) {
        if (strcmp(text, "deny") || p->gid_mapped) return -1;
        p->deny_setgroups = 1;
        return size;
    }
    const char *cursor = text;
    unsigned inside, outside, length;
    if (parse_id(&cursor, &inside) || parse_id(&cursor, &outside) ||
        parse_id(&cursor, &length) || *cursor || inside == 0xffffffffu || length != 1) return -22;
    if (file == &uid_map) {
        if (p->uid_mapped || outside != p->parent_uid) return -1;
        p->namespace_uid = inside;
        p->uid_mapped = 1;
    } else {
        if (p->gid_mapped || !p->deny_setgroups || outside != p->parent_gid) return -1;
        p->namespace_gid = inside;
        p->gid_mapped = 1;
    }
    return size;
}
#endif

static const struct runtime_file *runtime_lookup(struct proc *p, uint64_t address)
{
    char path[256];
    unsigned i;
    for (i = 0; i < sizeof path; ++i) {
        if (copy_process_bytes(p, address + i, path + i, 1, 0)) return NULL;
        if (!path[i]) break;
    }
    if (i == sizeof path) return NULL;
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (p->empty_root) {
#ifdef QUOTA_WAP_LOADER_TRACE
        if (!strcmp(path, "/dev/urandom")) printf("[wap] entropy path unavailable in chroot pid=%d\n", p->pid);
#endif
        return NULL;
    }
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
    for (unsigned j = 0; j < sizeof temp_dirs / sizeof *temp_dirs; ++j)
        if (temp_dirs[j].owner == p->temp_owner && temp_dirs[j].owner && !strcmp(path, temp_dirs[j].path))
            return &temp_dirs[j].file;
    if (p->files[1].event_flags == FD_WAP_NOTE && !strcmp(path, note_path.name)) return &note_path;
    if (!strcmp(path, shm_dir.name)) return &shm_dir;
    for (unsigned j = 0; j < SHM_OBJECTS; ++j)
        if (shm_objects[j].owner == p->temp_owner && shm_objects[j].path[0] && !strcmp(path, shm_objects[j].path))
            return &shm_objects[j].file;
    if (make_handle(p) == page_owner && !strcmp(path, window_device.name)) return &window_device;
    if (!strcmp(path, "/etc/fonts/fonts.conf")) {
        for (const struct runtime_file *f = wap_libs_start; f < wap_libs_end; ++f)
            if (!strcmp(f->name, "fonts.conf")) return f;
    }
#ifdef QUOTA_NODE_WEB
    if (p->page_executable && !strcmp(path, "/proc/self/exe")) return p->page_executable;
#endif
    if (!strcmp(path, SYSTEM_RUNTIME_PATH) || !strcmp(path, "/proc/self/exe")) return &system_executable;
    for (unsigned j = 0; j < sizeof runtime_dirs / sizeof *runtime_dirs; ++j)
        if (!strcmp(path, runtime_dirs[j].name)) return &runtime_dirs[j];
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (!strcmp(path, pid_ns_file.name)) return &pid_ns_file;
    if (!strcmp(path, user_ns_file.name)) return &user_ns_file;
    if (!strcmp(path, task_dir.name) || !strcmp(path, "/proc/self/task/")) return &task_dir;
    if (!strcmp(path, "/proc") || !strcmp(path, "/proc/")) return &proc_dir;
    if (p->user_namespace) {
        if (!strcmp(path, uid_map.name)) return &uid_map;
        if (!strcmp(path, gid_map.name)) return &gid_map;
    }
    if (!strcmp(path, setgroups_file.name)) return &setgroups_file;
#endif
    if (p->package_files[0].name && (!strcmp(path, "/app") || !strcmp(path, "/app/"))) return &app_dir;
    for (unsigned j = 0; j < 2; ++j) {
        const struct runtime_file *entry = &p->package_files[j];
        if (entry->name && !strcmp(path, entry->name)) return entry;
    }
    if (!strcmp(path, null_file.name)) return &null_file;
    if (!strcmp(path, urandom_file.name)) return &urandom_file;
    static const char *const roots[] = {
#ifdef QUOTA_LINUX_APP_RUNTIME
        "/usr/lib/quota-runtime/",
#endif
        "/lib/aarch64-linux-gnu/", "/usr/lib/aarch64-linux-gnu/", "/lib/", "/usr/lib/"};
    for (unsigned r = 0; r < sizeof roots / sizeof roots[0]; r++) {
        const char *prefix = roots[r];
        unsigned n = 0;
        while (prefix[n] && path[n] == prefix[n]) ++n;
        if (prefix[n]) continue;
        for (const struct runtime_file *f = wap_libs_start; f && f < wap_libs_end; f++)
            if (!strcmp(path + n, f->name)) return f;
    }
    return NULL;
}

static uint64_t runtime_pte(uint64_t pa, unsigned prot)
{
    if (!prot) return 0;
    if (prot & 4) return code_pte(pa) | PTE_PXN;
    return data_pte(pa) | ((prot & 2) ? 0 : AP_RO);
}

/* Return the first occupied page in a proposed range, or its end. */
static unsigned first_mapped_page(struct proc *p, unsigned first, unsigned end)
{
    while (first < end) {
        uint64_t bits = p->dso_used[first / 64] & (~0ull << (first % 64));
        if (bits) {
            unsigned page = (first & ~63u) + __builtin_ctzll(bits);
            return page < end ? page : end;
        }
        first = (first & ~63u) + 64;
    }
    return end;
}

static void reserve_empty_pages(struct proc *p, unsigned first, unsigned pages)
{
    while (pages) {
        unsigned shift = first % 64;
        unsigned n = pages < 64 - shift ? pages : 64 - shift;
        uint64_t mask = (~0ull >> (64 - n)) << shift;
        p->dso_used[first / 64] |= mask;
        p->dso_anonymous[first / 64] |= mask;
        first += n;
        pages -= n;
    }
}

#ifdef QUOTA_WAP_FULL_RUNTIME
/* Uncommitted Chromium address pools occupy virtual space only. The high
 * window is disjoint from the desktop, gate and ELF loader mappings. */

#define HIGH_FRAME_LIMIT 4096u
/* Both anonymous high pages and writable DSO pages consume this process grant. */
static unsigned private_pages_left(const struct proc *p)
{
    unsigned used = p->high_frames + p->mapped_dso_pages;
    unsigned limit = p->private_page_limit;
    return used < limit ? limit-used : 0;
}

#define FRAME_ADDR(e) ((e) & 0x0000fffffffff000ull)
static void drop_high_frame(struct proc *p, uint64_t *entry)
{
    uintptr_t pa = FRAME_ADDR(*entry);
    *entry = 0;
    tlb_asid(p->asid);
    free_idx((pa - (uintptr_t)pool) / 4096);
    --p->high_frames;
}

static void release_high(struct proc *p, uint64_t lo, uint64_t hi)
{
    for (unsigned a = lo >> 30; a <= (hi - 1) >> 30; a++) {
        uint64_t *root = &pt_l0[p->slot][a >> 9];
        if (!*root) {
            a |= 511;
            continue;
        }
        uint64_t *l1_table = (void *)(uintptr_t)FRAME_ADDR(*root);
        uint64_t *l1 = &l1_table[a & 511];
        if (!*l1) continue;
        uint64_t *l2 = (void *)(uintptr_t)FRAME_ADDR(*l1);
        unsigned live2 = 0;
        for (unsigned b = 0; b < 512; b++) {
            if (!l2[b]) continue;
            uint64_t base = ((uint64_t)a << 30) + ((uint64_t)b << 21);
            if (base + (1u << 21) > lo && base < hi) {
                uint64_t *l3 = (void *)(uintptr_t)FRAME_ADDR(l2[b]);
                unsigned live3 = 0;
                for (unsigned c = 0; c < 512; c++) {
                    uint64_t va = base + c * 4096;
                    if (l3[c] && va >= lo && va < hi) drop_high_frame(p, &l3[c]);
                    if (l3[c]) live3++;
                }
                if (!live3) drop_high_frame(p, &l2[b]);
            }
            if (l2[b]) live2++;
        }
        if (!live2) {
            drop_high_frame(p, l1);
            if (a >= 512) {
                unsigned k = 0;
                while (k < 512 && !l1_table[k]) ++k;
                if (k == 512) drop_high_frame(p, root);
            }
        }
    }
}

static int64_t protect_high(struct proc *p, struct irq_frame *f, uint64_t va, unsigned pages, unsigned prot)
{
    if (prot > 7 || (prot & 6) == 6) return -22;
    /* Small first commits, including any translation tables, are charged together. */
    p->mapping.va = va;
    unsigned stop = p->mapping.checked + 128;
    if (stop > pages) stop = pages;
    for (unsigned i = p->mapping.checked; i < stop; ++i) {
        uint64_t at = va + i * 4096ull;
        uint64_t e0 = pt_l0[p->slot][at >> 39];
        uint64_t e1 = e0 ? ((uint64_t *)(uintptr_t)FRAME_ADDR(e0))[(at >> 30) & 511] : 0;
        if (prot && !e0 && (!i || !(at & ((1ull << 39) - 1)))) p->mapping.needed++;
        uint64_t e2 = e1 ? ((uint64_t *)(uintptr_t)FRAME_ADDR(e1))[(at >> 21) & 511] : 0;
        uint64_t e3 = e2 ? ((uint64_t *)(uintptr_t)FRAME_ADDR(e2))[(at >> 12) & 511] : 0;
        if (prot && !e1 && (!i || !(at & ((1ull << 30)-1)))) p->mapping.needed++;
        if (prot && !e2 && (!i || !(at & ((1ull << 21)-1)))) p->mapping.needed++;
        if (prot && !e3) p->mapping.needed++;
    }
    if (p->mapping.checked < pages) {
        p->mapping.checked = stop;
        if (stop < pages) { f->elr -= 4; return va; }
        if (p->mapping.needed > private_pages_left(p) ||
            p->mapping.needed > free_frames - reserved_frames) {
            memset(&p->mapping, 0, sizeof p->mapping);
            return -12;
        }
        p->mapping.reserved = p->mapping.needed;
        reserved_frames += p->mapping.reserved;
    }
    uint64_t start = ticks_now();
    while (p->mapping.next < pages) {
        unsigned i = p->mapping.next++;
        uint64_t at = va + i * 4096ull, pa;
        uint64_t *table = pt_l0[p->slot];
        for (unsigned shift = 39; shift >= 12; shift -= 9) {
            uint64_t *e = &table[(at >> shift) & 511];
            if (!*e) {
                if (!prot) break;
                --reserved_frames;
                --p->mapping.reserved;
                if (alloc_page(&pa) < 0) return -12;
                ++p->high_frames;
                *e = pa | PTE_VALID | PTE_PAGE;
            }
            pa = FRAME_ADDR(*e);
            if (shift == 12) {
                if (prot & 4) clean_code((void *)(uintptr_t)pa, 4096);
                *e = prot ? runtime_pte(pa, prot) : pa;
            } else table = (void *)(uintptr_t)pa;
        }
        if (p->mapping.next < pages && ticks_now() - start >= us_to_ticks(40)) {
            tlb_asid(p->asid);
            f->elr -= 4;
            return va;
        }
    }
    memset(&p->mapping, 0, sizeof p->mapping);
    tlb_asid(p->asid);
    return 0;
}
/* Skip empty subtrees and release populated leaves in scheduler-sized slices. */
static int release_mapping_slice(struct proc *p, struct irq_frame *f, uint64_t addr, uint64_t end)
{
    uint64_t start = ticks_now();
    if (!p->mapping.va) {
        p->mapping.va = addr;
        p->release_cursor = addr;
    }
    while (p->release_cursor < end && p->high_frames) {
        uint64_t va = p->release_cursor;
        uint64_t *table = pt_l0[p->slot];
        for (int shift = 39; shift >= 12; shift -= 9) {
            uint64_t leaf = table[(va >> shift) & 511];
            if (!leaf) {
                p->release_cursor = ((va >> shift) + 1) << shift;
                break;
            }
            if (shift == 12) {
                release_high(p, va, va + 4096);
                p->release_cursor += 4096;
                break;
            }
            table = (void *)(uintptr_t)FRAME_ADDR(leaf);
        }
        if (ticks_now() - start >= us_to_ticks(32) && p->release_cursor < end) {
            f->elr -= 4;
            return 0;
        }
    }
    p->release_cursor = p->mapping.va = 0;
    return 1;
}
static int64_t high_mapping(struct proc *p, struct irq_frame *f)
{
    uint64_t addr = f->r[0], len = f->r[1], op = f->r[8];
    if (!len || len > HIGH_LIMIT - HIGH_START) return -22;
    len = (len + 4095) & ~4095ull;
    if (op == 222 && !(f->r[3] & 16)) {
        if ((f->r[3] & ~0x4020ull) != 2 || !(f->r[3] & 32) || f->r[2] || f->r[5]) return -12;
        addr &= ~4095ull;
        if (addr < HIGH_START || addr > HIGH_LIMIT - len) addr = HIGH_START;
        int empty = -1;
        for (unsigned i = 0; i < 64; ++i) {
            if (!p->reservations[i].end) { empty = i; continue; }
            if (addr < p->reservations[i].end && p->reservations[i].start < addr + len) {
                addr = p->reservations[i].end;
                if (addr > HIGH_LIMIT - len) return -12;
                i = ~0u;
            }
        }
        if (empty < 0) return -12;
        p->reservations[empty].start = addr;
        p->reservations[empty].end = addr + len;
        return addr;
    }
    if ((addr & 4095) || addr < HIGH_START || addr > HIGH_LIMIT - len) return -22;
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t start = p->reservations[i].start, end = p->reservations[i].end;
        if (addr < start || addr + len > end) continue;
        if (op == 222 && f->r[3] == 0x32 && !f->r[2] && !f->r[5]) {
            release_mapping_slice(p, f, addr, addr + len);
            return addr;
        }
        if (op == 226) {
            return protect_high(p, f, addr, len / 4096, f->r[2]);
        }
        if (op != 215) return -12;
        unsigned j = 0;
        if (addr != start && addr + len != end) {
            while (j < 64 && p->reservations[j].end) ++j;
            if (j == 64) return -12;
        }
        if (!release_mapping_slice(p, f, addr, addr + len)) return addr;
        if (addr == start && addr + len == end) p->reservations[i].end = 0;
        else if (addr == start) p->reservations[i].start += len;
        else if (addr + len == end) p->reservations[i].end = addr;
        else {
            p->reservations[j].start = addr + len;
            p->reservations[j].end = end;
            p->reservations[i].end = addr;
        }
        return 0;
    }
    return -12;
}
#endif

static int64_t runtime_map(struct proc *p, struct irq_frame *f, const struct runtime_file *file)
{
    uint64_t va = f->r[0], len = f->r[1], prot = f->r[2], flags = f->r[3], off = f->r[5];
    int op = f->r[8], map = op == 222;
#ifdef QUOTA_WAP_FULL_RUNTIME
    /* glibc reserves an aligned DSO span with MAP_DENYWRITE before replacing
     * it with file-backed PT_LOAD segments. Keep that span in the DSO window. */
    if (va >= HIGH_START || (map && !(flags & (16 | 0x800)) && !prot && len >= (32ull << 20)))
        return high_mapping(p, f);
#endif
    if (!len || len > ELF_DSO_PAGES * 4096 || (op != 215 && (prot > 7 || (prot & 6) == 6))) return -22;
    unsigned pages = (len + 4095) / 4096;
    int shared_readonly = (flags & 3) == 1 && !(flags & 32) && file && prot == 1;
    int share_writable = 0;
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (map && (flags & 3) == 1 && shm_for_file(file)) {
        if (prot & 4) return -13;
        if ((prot & 2) && p->files[f->r[4]-3].access_mode != 2) return -13;
        uint64_t size = file->end-file->begin;
        if (off >= size || len > ((size + 4095) & ~4095ull) - off) return -22;
        share_writable = 1;
    }
#endif
    if (map && (((flags & 3) != 2 && !shared_readonly && !share_writable) || (!(flags & 32) && !file) || (off & 4095))) return -22;
    if (p->mapping.va) va = p->mapping.va;
    else if (map && !(flags & 16)) {
        unsigned first = 0;
        while (first + pages <= ELF_DSO_PAGES) {
            unsigned occupied = first_mapped_page(p, first, first + pages);
            if (occupied == first + pages) break;
            first = occupied;
            while (first < ELF_DSO_PAGES) {
                uint64_t available = ~p->dso_used[first / 64] & (~0ull << (first % 64));
                if (available) {
                    first = (first & ~63u) + __builtin_ctzll(available);
                    break;
                }
                first = (first & ~63u) + 64;
            }
        }
        if (first + pages > ELF_DSO_PAGES) return -12;
        va = ELF_DSO_BASE + first * 4096;
        if ((flags & 32) && !prot) {
            reserve_empty_pages(p, first, pages);
            return va;
        }
    }
    /* glibc seals the executable and interpreter RELRO after relocation. */
    if (op == 226 && (prot == 1 || prot == 3) && !(va & 4095) && va >= ELF_BASE &&
        va < ELF_DSO_BASE && pages <= (ELF_DSO_BASE - va) / 4096) {
        unsigned first = (va - ELF_BASE) / 4096;
        for (unsigned i = 0; i < pages; ++i) {
            uint64_t entry = image_pt[p->slot][first + i];
            if (!(entry & PTE_VALID)) return -12;
            if (prot == 3 && !(entry & ELF_PRIVATE_WRITABLE)) return -13;
        }
        for (unsigned i = 0; i < pages; ++i) {
            uint64_t *entry = &image_pt[p->slot][first + i];
            if (prot == 3) *entry = (*entry | PTE_UXN) & ~AP_RO;
            else *entry |= AP_RO | PTE_UXN;
        }
        tlb_asid(p->asid);
        return 0;
    }
    if ((va & 4095) || va < ELF_DSO_BASE || va - ELF_DSO_BASE > (ELF_DSO_PAGES - pages) * 4096) return -22;
    unsigned first = (va - ELF_DSO_BASE) / 4096;
    /* Re-enter the same SVC between bounded slices. No user instruction
     * observes partial updates: the sole address-space owner resumes at svc. */
    p->mapping.va = va;
    unsigned stop = p->mapping.checked + 128;
    if (stop > pages) stop = pages;
    for (unsigned i = p->mapping.checked; i < stop; ++i) {
        unsigned idx = first + i;
        if (op == 226 && !(p->dso_used[idx / 64] & (1ull << (idx % 64)))) {
            memset(&p->mapping, 0, sizeof p->mapping);
            return -12;
        }
        const uint8_t *source = map ? NULL : (const uint8_t *)p->dso_source[idx];
        if (map && !(flags & 32) && off <= (uint64_t)(file->end - file->begin) &&
            i * 4096ull + 4096 <= (uint64_t)(file->end - file->begin) - off)
            source = file->begin + off + i * 4096u;
        if (share_writable) source = file->begin + off + i * 4096u;
        int share = source && !((uintptr_t)source & 4095) && (!(prot & 2) || share_writable);
        /* Reapplying PROT_NONE to a reserved hole needs no physical backing. */
        int reserve = !prot && ((map && (flags & 32)) || (!map && !source));
        if (op != 215 && !reserve && !share &&
            image_frames[p->slot][p->dso_frame + idx] < 0) ++p->mapping.needed;
    }
    if (p->mapping.checked < pages) {
        p->mapping.checked = stop;
        if (stop != pages) {
            f->elr -= 4;
            return f->r[0];
        }
        if (p->mapping.needed > free_frames - reserved_frames
#ifdef QUOTA_WAP_FULL_RUNTIME
            || p->mapping.needed > private_pages_left(p)
#endif
        ) {
            memset(&p->mapping, 0, sizeof p->mapping);
            return -12;
        }
        p->mapping.reserved = p->mapping.needed;
        reserved_frames += p->mapping.reserved;
    }
    uint64_t slice_start = ticks_now();
    while (p->mapping.next < pages) {
        unsigned i = p->mapping.next++;
        unsigned idx = first + i;
        int *slot = &image_frames[p->slot][p->dso_frame + idx];
        uintptr_t pa = 0;
        if (op != 215) {
            const uint8_t *src = (const uint8_t *)p->dso_source[idx];
            size_t bytes = src ? 4096 : 0;
            if (map) {
                src = NULL;
                bytes = 0;
                uint64_t pos = off + i * 4096ull;
                if (!(flags & 32) && pos >= off && pos < (uint64_t)(file->end - file->begin)) {
                    src = file->begin + pos;
                    bytes = file->end - src;
                    if (bytes > 4096) bytes = 4096;
                }
            }
            if (share_writable) {
                src = file->begin + off + i * 4096u;
                bytes = 4096;
            }
            if (src && bytes == 4096 && !((uintptr_t)src & 4095) && (!(prot & 2) || share_writable) &&
                (map || *slot < 0)) {
                pa = (uintptr_t)src;
            } else if (prot || (map ? !(flags & 32) : src != NULL)) {
                if (*slot < 0) {
                    uint64_t frame_pa;
                    --p->mapping.reserved;
                    --reserved_frames;
                    *slot = alloc_page(&frame_pa);
                    if (*slot < 0) return -12;
                    p->mapped_dso_pages++;
                    if (!map && bytes) memcpy(pool[*slot], src, bytes);
                }
                pa = (uintptr_t)pool[*slot];
                if (map) {
                    if (bytes) memcpy((void *)pa, src, bytes);
                    if (bytes < 4096) memset((void *)(pa + bytes), 0, 4096 - bytes);
                }
                if (prot & 4) clean_code((void *)pa, 4096);
            }
        }
#ifdef QUOTA_LINUX_APP_RUNTIME
        shm_account(p, p->dso_source[idx], 0);
        shm_account(p, pa, 1);
#endif
        p->dso_source[idx] = pa;
        uint64_t bit = 1ull << (idx % 64);
        if (map && (flags & 32)) p->dso_anonymous[idx / 64] |= bit;
        else if (map || op == 215) p->dso_anonymous[idx / 64] &= ~bit;
        if (op == 215) p->dso_used[idx / 64] &= ~bit;
        else p->dso_used[idx / 64] |= bit;
        image_pt[p->slot][(va - ELF_BASE) / 4096 + i] = pa ? runtime_pte(pa, prot) : 0;
        if (*slot >= 0 && pa != (uintptr_t)pool[*slot]) {
            tlb_asid(p->asid);
            free_idx(*slot);
            *slot = -1;
            p->mapped_dso_pages--;
        }
        if (p->mapping.next < pages && ticks_now() - slice_start >= us_to_ticks(40)) {
            tlb_asid(p->asid);
            f->elr -= 4;
            return f->r[0];
        }
    }
    memset(&p->mapping, 0, sizeof p->mapping);
    tlb_asid(p->asid);
#ifdef QUOTA_NODE_WEB
    if (map && !(flags & 32) && (prot & 4) && file && !strcmp(file->name, "libv8.so")) {
        /* Emit only after every PTE in this segment has been installed. */
        uint64_t pte = image_pt[p->slot][(va - ELF_BASE) / 4096];
        uint64_t first_pa = FRAME_ADDR(pte);
        printf("[wap-v8] runtime=%s pid=%d asid=%u file_offset=%lu bytes=%lu va=0x%lx first_pa=0x%lx shared_first_page=%d\n",
               p->page_executable ? "blink" : "node", p->pid, (unsigned)p->asid,
               off, len, va, first_pa,
               off < (uint64_t)(file->end - file->begin) &&
               (pte & (PTE_VALID | AP_EL0 | AP_RO | PTE_UXN)) == (PTE_VALID | AP_EL0 | AP_RO) &&
               first_pa == (uintptr_t)file->begin + off);
    }
#endif
    return map ? (int64_t)va : 0;
}

static int64_t runtime_advise(struct proc *p, struct irq_frame *f)
{
    uint64_t va = f->r[0], len = f->r[1], advice = f->r[2];
    if ((va & 4095) || (advice != 3 && advice != 4)) return -22;
    if (!len) return 0;
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (va >= HIGH_START) {
        if (va >= HIGH_LIMIT || len > HIGH_LIMIT - va) return -12;
        unsigned j = 0;
        while (j < 64 && (va < p->reservations[j].start || va + len > p->reservations[j].end)) j++;
        if (j == 64) return -12;
        if (advice == 3) return 0;
        uint64_t at = p->mapping.va ? p->mapping.va : va;
        uint64_t begin = ticks_now();
        while (at < va + len) {
            int level = 0;
            uint64_t pte = walk(p, at, &level);
            if (level == 3 && FRAME_ADDR(pte))
                memset((void *)(uintptr_t)FRAME_ADDR(pte), 0, 4096);
            if (level == 0) at = ((at >> 39) + 1) << 39;
            else if (level == 1) at = ((at >> 30) + 1) << 30;
            else if (level != 3) at = ((at >> 21) + 1) << 21;
            else at += 4096;
            if (at < va + len && ticks_now() - begin > us_to_ticks(40)) {
                p->mapping.va = at;
                f->elr -= 4;
                return va;
            }
        }
        p->mapping.va = 0;
        return 0;
    }
#endif
    if (va < ELF_DSO_BASE || len > ELF_DSO_PAGES * 4096 ||
        va - ELF_DSO_BASE > ELF_DSO_PAGES * 4096 - len) return -12;
    unsigned first = (va - ELF_DSO_BASE) / 4096, count = (len + 4095) / 4096;
    p->mapping.va = va;
    unsigned stop = p->mapping.checked + 128;
    if (stop > count) stop = count;
    for (unsigned i = p->mapping.checked; i < stop; i++) {
        unsigned idx = first + i;
        uint64_t bit = 1ull << (idx % 64);
        if (!(p->dso_used[idx / 64] & bit) ||
            (advice == 4 && !(p->dso_anonymous[idx / 64] & bit))) {
            memset(&p->mapping, 0, sizeof p->mapping);
            return advice == 4 && (p->dso_used[idx / 64] & bit) ? -95 : -12;
        }
    }
    p->mapping.checked = stop;
    if (stop < count) {
        f->elr -= 4;
        return va;
    }
    /* All committed pages are resident. DONTNEED on anonymous private
     * memory discards its contents, including pages currently protected. */
    uint64_t start = ticks_now();
    while (advice == 4 && p->mapping.next < count) {
        int frame = image_frames[p->slot][p->dso_frame + first + p->mapping.next++];
        if (frame >= 0) memset(pool[frame], 0, 4096);
        if (p->mapping.next < count && ticks_now() - start >= us_to_ticks(40)) {
            f->elr -= 4;
            return va;
        }
    }
    memset(&p->mapping, 0, sizeof p->mapping);
    return 0;
}

#ifdef QUOTA_WAP_FULL_RUNTIME
/* Copy the calling address space over successive scheduler entries. Only immutable
 * image pages may retain the parent's physical address. */
static int64_t fork_private(struct proc *parent, struct irq_frame *f)
{
    struct proc *child = parent->fork_child;
    if (!child) {
        if (f->r[0] == 0x20000011 && (!parent->user_namespace ||
            !(parent->threads[parent->active_thread].ns_effective & NS_CAP_SYS_ADMIN))) return -1;
        if ((f->r[0] & 0x20000000u) && parent->pid_ancestors[2]) return -28;
        if ((f->r[0] & 0x10000000u) && parent->user_namespace) return -95;
        unsigned record = 0;
        while (record < EL0_SLOTS && parent->children[record].pid) ++record;
        if (record == EL0_SLOTS) return -11;
        parent->fork_record = record;
        if (parent->mapping.va) return -16;
        for (unsigned i = 0; i < WAP_FDS; ++i)
            if (parent->files[i].event_flags && !(parent->files[i].event_flags & (FD_PIPE_R | FD_PIPE_W)) && !parent->files[i].poll && !parent->files[i].socket) return -95;
        for (int i = 0; i < EL0_SLOTS; ++i)
            if (!procs[i].alive && !procs[i].draining && !procs[i].building) { child = &procs[i]; break; }
        if (!child) {
            for (unsigned i = 0; i < EL0_SLOTS; ++i) {
                if (procs[i].draining && !procs[i].active_draws) {
                    f->elr -= 4;
                    return f->r[0];
                }
            }
            return -11;
        }
        int slot = child - procs;
        uint32_t gen = child->gen ? child->gen : 1;
        parent->fork_child = child;
        child->building = 1;
        child->slot = slot;
        child->gen = gen;
        parent->fork_init = parent->fork_cursor = 0;
        parent->fork_high_va = HIGH_START;
    }
    int slot = child - procs;
    /* The slot stays reserved while its large ELF accounting arrays are prepared. */
    if (parent->fork_init < sizeof *child) {
        unsigned at = parent->fork_init;
        unsigned n = sizeof *child - at;
        if (n > 1024) n = 1024;
        uint32_t gen = child->gen;
        memset((uint8_t *)child + at, 0, n);
        child->slot = slot;
        child->gen = gen;
        child->building = 1;
        parent->fork_init += n;
        f->elr -= 4;
        return f->r[0];
    }
    if (parent->fork_init == sizeof *child) {
        child->asid = slot + 1;
        child->parent_handle = make_handle(parent);
#ifdef QUOTA_LINUX_APP_RUNTIME
        child->temp_owner = parent->temp_owner;
#endif
        child->empty_root = parent->empty_root;
        child->stdin_null = parent->stdin_null;
        child->stdin_cloexec = parent->stdin_cloexec;
        child->kernel_uid = parent->kernel_uid;
        child->kernel_gid = parent->kernel_gid;
        if (f->r[0] & 0x10000000u) {
            enter_user_namespace(child, 1, parent->kernel_uid, parent->kernel_gid);
        } else {
            child->user_namespace = parent->user_namespace;
            child->threads[0].ns_effective = parent->threads[parent->active_thread].ns_effective;
            child->threads[0].ns_permitted = parent->threads[parent->active_thread].ns_permitted;
            child->namespace_owner = parent->namespace_owner;
            child->namespace_depth = parent->namespace_depth;
            child->parent_uid = parent->parent_uid;
            child->parent_gid = parent->parent_gid;
            child->deny_setgroups = parent->deny_setgroups;
            child->uid_mapped = parent->uid_mapped;
            child->gid_mapped = parent->gid_mapped;
            child->namespace_uid = parent->namespace_uid;
            child->namespace_gid = parent->namespace_gid;
        }
        for (unsigned i = 0; i < 3; ++i) child->pages[i] = -1;
#ifdef QUOTA_NODE_WEB
        child->page_executable = parent->page_executable;
#endif
        char resource_name[] = "wap-child-0";
        resource_name[sizeof resource_name - 2] += slot;
        unsigned inherited = parent->mapped_dso_pages + parent->high_frames;
        unsigned child_limit = inherited + 2048;
        if (child_limit < 7168) child_limit = 7168;
        if (child_limit > parent->private_page_limit) child_limit = parent->private_page_limit;
        child->private_page_limit = child_limit;
        unsigned required_kib = parent->app->mem_min_kb - 4*(parent->private_page_limit-child_limit);
        struct app_manifest manifest = { resource_name, "el0-draw", required_kib, required_kib, 0, 0 };
        child->app = resource_commit(&manifest);
        if (!child->app) { printf("[wap] fork admission: %s", resource_reason()); goto failed; }
        for (unsigned i = 0; i < 3; ++i) {
            child->pages[i] = alloc_page(&child->pa[i]);
            if (child->pages[i] < 0) goto failed;
            memcpy((void *)(uintptr_t)child->pa[i], (void *)(uintptr_t)parent->pa[i], 4096);
        }
        build_pt(child);
        pt_l3[slot][1] = 0;
        for (unsigned i = 0; i < ELF_WINDOW >> 21; ++i)
            pt_l2[slot][(ELF_BASE >> 21) + i] = (uintptr_t)&image_pt[slot][512*i] | 3;
        child->console = parent->console;
        child->linux_brk = parent->linux_brk;
        child->document_size = parent->document_size;
        child->signal_mask = parent->signal_mask;
        memcpy(child->signal_actions, parent->signal_actions, sizeof child->signal_actions);
        memcpy(child->package_contents, parent->package_contents, sizeof child->package_contents);
        for (unsigned i = 0; i < 2; ++i) {
            const struct runtime_file *r = &parent->package_files[i];
            if (r->name) child->package_files[i] = (struct runtime_file){r->name, child->package_contents[i],
                child->package_contents[i] + (r->end - r->begin)};
        }
        child->dso_frame = parent->dso_frame;
        child->image_pages = child->dso_frame + ELF_DSO_PAGES;
        parent->fork_init++;
        child->mapped_dso_pages = parent->mapped_dso_pages;
        memcpy(child->reservations, parent->reservations, sizeof child->reservations);
    }
    /* The allocation list preserves DSO holes and aliases. First build an old
     * frame to new frame index, then translate the leaf table in bounded batches. */
    static int replacement[EL0_SLOTS][EL0_POOL];
    uint64_t start = ticks_now();
    unsigned leaves = ELF_WINDOW >> 12;
    while (parent->fork_cursor < child->image_pages + leaves) {
        unsigned index = parent->fork_cursor++;
        if (index < child->image_pages) {
            image_frames[child->slot][index] = -1;
            if (index >= child->dso_frame) {
                unsigned d = index - child->dso_frame;
                child->dso_source[d] = parent->dso_source[d];
#ifdef QUOTA_LINUX_APP_RUNTIME
                shm_account(child, child->dso_source[d], 1);
#endif
                if (!(d % 64)) {
                    child->dso_used[d/64] = parent->dso_used[d/64];
                    child->dso_anonymous[d/64] = parent->dso_anonymous[d/64];
                }
            }
            int old = image_frames[parent->slot][index];
            if (old >= 0) {
                uint64_t pa;
                int fresh = alloc_page(&pa);
                if (fresh < 0) goto failed;
                image_frames[child->slot][index] = fresh;
                replacement[child->slot][old] = fresh;
                memcpy((void *)(uintptr_t)pa, pool[old], 4096);
            }
        } else {
            unsigned i = index - child->image_pages;
            uint64_t pte = image_pt[parent->slot][i], pa = FRAME_ADDR(pte);
            if (pa == parent->pa[1]) pa = child->pa[1];
            else if (pa >= (uintptr_t)pool && pa < (uintptr_t)pool + sizeof pool) {
                unsigned old = (pa - (uintptr_t)pool) >> 12;
                pa = (uintptr_t)pool[replacement[child->slot][old]];
            }
            image_pt[child->slot][i] = (pte & ~0x0000fffffffff000ull) | pa;
        }
        if (ticks_now() - start >= us_to_ticks(32)) { f->elr -= 4; return f->r[0]; }
    }
    /* Skip empty subtrees instead of walking individual pages of V8 reservations. */
    while (parent->high_frames && parent->fork_high_va < HIGH_LIMIT) {
        uint64_t va = parent->fork_high_va;
        uint64_t *source = pt_l0[parent->slot];
        uint64_t *target = pt_l0[child->slot];
        for (int shift = 39; shift >= 12; shift -= 9) {
            unsigned idx = (va >> shift) & 511;
            uint64_t entry = source[idx];
            if (!entry) {
                parent->fork_high_va = ((va >> shift) + 1) << shift;
                break;
            }
            if (!target[idx]) {
                uint64_t pa;
                if (!private_pages_left(child) || alloc_page(&pa) < 0) goto failed;
                ++child->high_frames;
                target[idx] = pa | (entry & ~0x0000fffffffff000ull);
                if (shift == 12) {
                    memcpy((void *)(uintptr_t)pa, (void *)(uintptr_t)FRAME_ADDR(entry), 4096);
                    if ((entry & PTE_VALID) && !(entry & PTE_UXN)) clean_code((void *)(uintptr_t)pa, 4096);
                }
            }
            if (shift == 12) { parent->fork_high_va = va + 4096; break; }
            source = (void *)(uintptr_t)FRAME_ADDR(entry);
            target = (void *)(uintptr_t)FRAME_ADDR(target[idx]);
        }
        if (ticks_now() - start >= us_to_ticks(32)) { f->elr -= 4; return f->r[0]; }
    }
    child->pid = ++next_child_pid;
    memcpy(child->pid_ancestors, parent->pid_ancestors, sizeof child->pid_ancestors);
    if (f->r[0] & 0x20000000u) {
        for (unsigned i = 2; i > 0; --i) child->pid_ancestors[i] = parent->pid_ancestors[i-1];
        child->pid_ancestors[0] = parent->pid_namespace;
    }
    child->pid_namespace = f->r[0] & 0x20000000u ? (unsigned)child->pid : parent->pid_namespace;
    if (f->r[0] & 0x1000000u) {
        uint32_t tid = child->pid;
        if ((f->r[4] & 3) || copy_process_bytes(child, f->r[4], &tid, sizeof tid, 1)) goto failed;
    }
    if (f->r[0] & 0x200000u) child->clear_child_tid = child->threads[0].clear_tid = f->r[4];
    child->task = task_try_create("wap-child", TIER3, KIND_SPIN);
    if (!child->task) goto failed;
    struct irq_frame *frame = (void *)(uintptr_t)child->task->sp;
    *frame = *f;
    frame->r[0] = 0;
    if (f->r[1]) frame->pad = f->r[1];
    parent->children[parent->fork_record] = (struct child_status){ .pid = child->pid };
    child->exit_status = 9;
    child->threads[0].no_new_privs = parent->threads[parent->active_thread].no_new_privs;
    child->threads[0].tid = child->pid_namespace == (unsigned)child->pid ? 1 : child->pid;
    child->live_threads = 1;
    child->next_tid = 10000 + child->slot * 100000;
    asm volatile("mrs %0,tpidr_el0" : "=r"(child->threads[0].tls));
    wap_fp_save(child->threads[0].fp);
    for (unsigned i = 0; i < WAP_FDS; ++i) {
        if (parent->files[i].position) {
            child->files[i].position = parent->files[i].position;
            ++child->files[i].position->refs;
            const struct runtime_file *source = parent->files[i].file;
            child->files[i].file = source == &parent->package_files[0] ? &child->package_files[0]
                                 : source == &parent->package_files[1] ? &child->package_files[1] : source;
        }
        child->files[i].cloexec = parent->files[i].cloexec;
        child->files[i].access_mode = parent->files[i].access_mode;
        child->files[i].identity = parent->files[i].identity;
        if (parent->files[i].poll) {
            child->files[i].poll = parent->files[i].poll;
            ++child->files[i].poll->refs;
            child->files[i].event_flags = parent->files[i].event_flags;
        }
        unsigned flags = parent->files[i].event_flags;
        if (flags & (FD_PIPE_R | FD_PIPE_W)) {
            unsigned id = parent->files[i].pipe_id;
            child->files[i].pipe_id = id;
            child->files[i].event_flags = flags;
            if (flags & FD_PIPE_R) ++pipes[id].readers;
            else ++pipes[id].writers;
        }
        if (!parent->files[i].socket) continue;
        child->files[i].socket = parent->files[i].socket;
        child->files[i].event_flags = parent->files[i].event_flags;
        ++child->files[i].socket->refs;
    }
    tlb_asid(child->asid);
    child->task->user_ttbr = child->ttbr;
    child->alive = 1;
    child->building = 0;
    parent->fork_child = NULL;
    task_ready(child->task);
    printf("[wap] fork parent=%d child=%d asid=%d private_dso_pages=%u grant_kib=%u\n", parent->pid, child->pid, child->asid, child->mapped_dso_pages, child->app->mem_min_kb);
    return child->pid;
failed:
    if (child->high_frames) release_high(child, HIGH_START, HIGH_LIMIT);
    if (parent->fork_cursor < child->image_pages) child->image_pages = parent->fork_cursor;
    while (child->image_pages) free_idx(image_frames[child->slot][--child->image_pages]);
    for (unsigned i = 0; i < 3; ++i) free_idx(child->pages[i]);
    if (child->app) resource_release(child->app);
    child->app = NULL;
    child->building = 0;
    parent->fork_child = NULL;
    return -12;
}

static unsigned fd_ready(struct proc *p, unsigned i)
{
    if (p->files[i].file) return 1;
    if (!p->files[i].event_flags) return 32;
    uint64_t count = p->files[i].poll ? p->files[i].poll->count : 0;
    unsigned ready = (count ? 1u : 0u) | (count < UINT64_MAX - 1 ? 4u : 0u);
    unsigned flags = p->files[i].event_flags;
    if (flags & (FD_PIPE_R | FD_PIPE_W)) {
        struct wap_pipe *pipe = &pipes[p->files[i].pipe_id];
        ready = flags & FD_PIPE_R ? (pipe->size ? 1u : 0u) | (!pipe->writers ? 16u : 0u)
                                  : (pipe->size < sizeof pipe->buf ? 4u : 0u) | (!pipe->readers ? 8u : 0u);
    }
    if (flags & FD_PACKET) {
        struct packet_socket *sock = p->files[i].socket;
        struct packet_socket *peer = sock->peer;
        ready = sock->packets || !peer || (peer->shut & 2) ? 1u : 0u;
        if (!(sock->shut & 2) && peer && !(peer->shut & 1) && peer->packets < (peer->stream ? sizeof peer->stream_bytes : 4)) ready |= 4u;
        if (!peer) ready |= 16u;
    }
    return ready;
}
/* Resolve a watched open description even after the registering process
 * closes its local fd. A reused fd never matches the old identity. */
static int watch_ready(struct poll_watch *w)
{
    struct proc *p = &procs[w->owner_slot];
    if (p->alive && !p->draining && p->files[w->fd_index].identity == w->identity)
        return fd_ready(p, w->fd_index);
    for (unsigned slot = 0; slot < EL0_SLOTS; ++slot) {
        p = &procs[slot];
        if (!p->alive || p->draining) continue;
        for (unsigned i = 0; i < WAP_FDS; ++i)
            if (p->files[i].identity == w->identity) return fd_ready(p, i);
    }
    w->events = 0;
    return 0;
}

#endif

#ifdef QUOTA_LINUX_APP_RUNTIME
static int64_t exec_system_runtime(struct proc *, struct irq_frame *);
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
static int capture_rights(struct proc *p, struct packet_socket *peer, struct rights_batch *batch,
                          uint64_t address, uint64_t space)
{

    struct { uint64_t len; uint32_t level, type; int fds[8]; } control;
    if (space < 20 || space > sizeof control) return -22;
    if (copy_process_bytes(p, address, &control, space, 0)) return -14;
    if (control.level != 1 || control.type != 1 || control.len < 20 ||
        control.len > space || ((control.len - 16) & 3) ||
        space > ((control.len+7) & ~7ull)) return -22;
    unsigned n = (control.len-16)/4;
    for (unsigned k = 0; k < n; ++k) {
        unsigned slot = (unsigned)control.fds[k]-3;
        if (slot >= WAP_FDS) return -9;
        struct wap_descriptor *entry = &p->files[slot];
        if (entry->socket) {
            if (!can_queue_socket(entry->socket, peer, 0)) return -40;
        } else {
#ifdef QUOTA_LINUX_APP_RUNTIME
            uintptr_t addr = (uintptr_t)entry->file;
            if (!entry->position || (!shm_for_file(entry->file) &&
                !(addr >= (uintptr_t)wap_libs_start && addr < (uintptr_t)wap_libs_end))) return -95;
#else
            return -95;
#endif
        }
        batch->entries[k] = *entry;
    }
    for (unsigned k = 0; k < n; ++k) {
        if (batch->entries[k].socket) ++batch->entries[k].socket->refs;
        else ++batch->entries[k].position->refs;
    }
    batch->count = n;
    return 0;
}
#endif
static int64_t runtime_syscall(struct proc *p, struct irq_frame *f)
{
    uint64_t op = f->r[8], fd = f->r[0], index = fd - 3;
    const struct runtime_file *file = index < WAP_FDS ? p->files[index].file : NULL;
#ifdef QUOTA_WAP_LOADER_TRACE
    if ((op == 220 || op == 435) && !p->fork_child)
        printf("[wap] clone svc=%lu flags_or_args=%lx stack_or_size=%lx ptid=%lx tls=%lx ctid=%lx\n",
               op, fd, f->r[1], f->r[2], f->r[3], f->r[4]);
    if (op == 131 && f->r[2] == 6 && fd == (unsigned)p->pid) {
        uint64_t fp = f->r[29];
        printf("[wap] abort caller=%lx", f->r[30]);
        for (unsigned depth = 0; depth < 24; depth++) {
            uint64_t record[2];
            if ((fp & 15) || copy_process_bytes(p, fp, record, sizeof record, 0)) break;
            printf(" %lx", record[1]);
            if (record[0] <= fp) break;
            fp = record[0];
        }
        printf("\n");
    }
#endif
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (op == 23 || op == 24 || (op == 25 && (f->r[1] == 0 || f->r[1] == 1030))) {
        uint64_t newfd = f->r[1];
        unsigned cloexec = op == 25 && f->r[1] == 1030;
        if (op == 24) {
            if (fd == newfd || (f->r[2] & ~0x80000ull)) return -22;
            cloexec = !!f->r[2];
        } else {
            newfd = op == 23 ? 3 : f->r[2];
            if (newfd >= WAP_FDS + 3) return -22;
            if (newfd < 3) newfd = 3;
            while (newfd < WAP_FDS + 3 &&
                   (p->files[newfd-3].file || p->files[newfd-3].event_flags)) ++newfd;
            if (newfd == WAP_FDS + 3) return -24;
        }
        if (index >= WAP_FDS || (!file && !p->files[index].event_flags)) return -9;
        if (newfd == 0 && file == &null_file && !p->files[index].access_mode) {
            p->stdin_null = 1;
            p->stdin_cloexec = !!f->r[2];
            return 0;
        }
        if (newfd < 3) return -95;
        if (newfd - 3 >= WAP_FDS) return -9;
        typeof(p->files[0]) *from = &p->files[index], *to = &p->files[newfd - 3];
        if ((from->event_flags | to->event_flags) & FD_WAP_NOTE) return -1;
        if (to->position) --to->position->refs;
        if (to->poll) --to->poll->refs;
        if (to->socket) packet_put(to->socket);
        pipe_put(to->event_flags, to->pipe_id);
        *to = *from;
        to->cloexec = cloexec;
        if (to->position) ++to->position->refs;
        if (to->poll) ++to->poll->refs;
        if (to->socket) ++to->socket->refs;
        if (to->event_flags & FD_PIPE_R) ++pipes[to->pipe_id].readers;
        if (to->event_flags & FD_PIPE_W) ++pipes[to->pipe_id].writers;
        return newfd;
    }
    if (op == 25 && (f->r[1] == 1 || f->r[1] == 2)) {
        if (!fd && p->stdin_null) {
            if (f->r[1] == 1) return p->stdin_cloexec;
            p->stdin_cloexec = f->r[2] & 1;
            return 0;
        }
        if (index >= WAP_FDS || (!file && !p->files[index].event_flags)) return -9;
        if (f->r[1] == 1) return p->files[index].cloexec;
        p->files[index].cloexec = f->r[2] & 1;
        return 0;
    }
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (file == &window_device && (op == 63 || op == 64)) {
        if (make_handle(p) != page_owner) return -13;
        if (op == 63) {
            if (f->r[2] != sizeof window_input[0]) return -22;
            if (!input_count) return -11;
            if (copy_process_bytes(p, f->r[1], &window_input[input_head], sizeof window_input[0], 1)) return -14;
            input_head = (input_head + 1) % 64;
            --input_count;
            return sizeof window_input[0];
        }
        unsigned row = p->files[index].position->offset;
        if (row >= GUEST_PREVIEW_HEIGHT || f->r[2] != sizeof page_pixels[0]) return -22;
        unsigned generation = electron_generation;
        if (copy_process_bytes(p, f->r[1], electron_frames[generation & 1][row], sizeof page_pixels[0], 0)) return -14;
        if (++row == GUEST_PREVIEW_HEIGHT) {
            __atomic_store_n(&electron_generation, generation+1, __ATOMIC_RELEASE);
            row = 0;
        }
        p->files[index].position->offset = row;
        return sizeof page_pixels[0];
    }
#endif
    if (file == &null_file) {
        if (op == 63) return p->files[index].access_mode == 1 ? -9 : 0;
        if (op == 64) return p->files[index].access_mode == 0 ? -9 : (int64_t)f->r[2];
    }
    if ((op == 63 || op == 64) && index < WAP_FDS && p->files[index].event_flags == FD_WAP_NOTE) {
        if (op == 63) {
            unsigned size = 0;
            const unsigned char *data = note_disk_wap_note(&size);
            if (!data) return 0;
            unsigned n = size < f->r[2] ? size : (unsigned)f->r[2];
            return copy_process_bytes(p, f->r[1], (void *)data, n, 1) ? -14 : (int)n;
        }
        if (op == 64) {
            unsigned tid = p->threads[p->active_thread].tid;
            if (!p->note_writer_tid) {
                unsigned char bytes[496];
                if (f->r[2] > sizeof bytes) return -27;
                if (copy_process_bytes(p, f->r[1], bytes, f->r[2], 0)) return -14;
                if (note_disk_save_wap_note(bytes, f->r[2], &p->pending_note)) return -5;
                p->note_writer_tid = tid;
            } else if (p->note_writer_tid != tid) return -16;
            int committed = note_disk_wap_note_committed(p->pending_note);
            if (!committed) { f->elr -= 4; return fd; }
            p->note_writer_tid = 0;
            if (committed < 0) return -5;
            printf("[wap] backend disk flush generation=%u bytes=%lu\n", p->pending_note, f->r[2]);
            return f->r[2];
        }
    }
    if (op == 73) {
        struct { int fd; int16_t events, revents; } item;
        if (f->r[1] > WAP_FDS || (f->r[3] && f->r[4] != sizeof(uint64_t))) return -22;
        struct wap_thread *self = &p->threads[p->active_thread];
        uint64_t mask = 0;
        if (self->poll_mask_active) {
            p->signal_mask = self->poll_saved_mask;
            self->poll_mask_active = 0;
        }
        if (f->r[3] && copy_process_bytes(p, f->r[3], &mask, sizeof mask, 0)) return -14;
        unsigned ready = 0;
        for (unsigned i = 0; i < f->r[1]; ++i) {
            uint64_t addr = fd + sizeof item * i;
            if (copy_process_bytes(p, addr, &item, sizeof item, 0)) return -14;
            unsigned state = item.fd < 0 ? 0 : item.fd < 3 ? (item.fd ? 4 : 1)
                           : (unsigned)item.fd - 3 < WAP_FDS ? fd_ready(p, item.fd - 3) : 32;
            item.revents = state & (item.events | 0x38);
            if (copy_process_bytes(p, addr, &item, sizeof item, 1)) return -14;
            if (item.revents) ++ready;
        }
        if (!ready && f->r[2] && !self->poll_deadline) {
            uint64_t ts[2];
            if (copy_process_bytes(p, f->r[2], ts, sizeof ts, 0)) return -14;
            if ((int64_t)ts[0] < 0 || ts[1] >= 1000000000) return -22;
            uint64_t freq = timer_freq(), now = ticks_now(), sub = ts[1] * freq / 1000000000;
            self->poll_deadline = ts[0] > (UINT64_MAX - now - sub) / freq
                                ? UINT64_MAX : now + ts[0] * freq + sub;
        }
        if (!ready && (!f->r[2] || ticks_now() < self->poll_deadline)) {
            if (f->r[3]) {
                self->poll_saved_mask = p->signal_mask;
                self->poll_mask_active = 1;
                p->signal_mask = mask & ~((1ull << 8) | (1ull << 18));
            }
            f->elr -= 4;
            return fd;
        }
        self->poll_deadline = 0;
        return ready;
    }
    if (op == 199) {
        uint64_t type = f->r[1] & ~0x80800ull;
        if (fd != 1 || (type != 1 && type != 5) || f->r[2]) return -93;
        int pair[2];
        unsigned n = 0;
        for (unsigned i = 0; i < WAP_FDS && n < 2; i++)
            if (!p->files[i].file && !p->files[i].event_flags) pair[n++] = i + 3;
        if (n != 2) return -24;
        struct packet_socket *ends[2];
        n = 0;
        for (unsigned i = 0; i < WAP_OBJECT_SLOTS && n < 2; ++i)
            if (!packet_sockets[i].refs) ends[n++] = &packet_sockets[i];
        if (n < 2) return -23;
        if (copy_process_bytes(p, f->r[3], pair, sizeof pair, 1)) return -14;
        for (unsigned i = 0; i < 2; i++) {
            p->files[pair[i]-3].identity = ++next_fd_identity;
            p->files[pair[i]-3].cloexec = !!(f->r[1] & 0x80000);
            p->files[pair[i]-3].event_flags = FD_PACKET | (f->r[1] & 0x80800);
            ends[i]->refs = 1;
            ends[i]->stream = type == 1;
            ends[i]->nonblock = f->r[1] & 0x800;
            ends[i]->passcred = ends[i]->shut = ends[i]->packets = ends[i]->packet_head = 0;
            ends[i]->peer = ends[i^1];
            p->files[pair[i]-3].socket = ends[i];
        }
        return 0;
    }
    if (index < WAP_FDS && (p->files[index].event_flags & FD_PACKET)) {
        struct packet_socket *sock = p->files[index].socket;
        struct packet_socket *peer = sock->peer;
        if (op == 25 && f->r[1] == 3) return 2 | sock->nonblock;
        if (op == 25 && f->r[1] == 4) {
            if (f->r[2] & ~(uint64_t)0x803) return -22;
            sock->nonblock = f->r[2] & 0x800;
            return 0;
        }
        if (op == 208) {
            int enabled;
            if (f->r[1] != 1 || f->r[2] != 16) return -92;
            if (f->r[4] < sizeof enabled) return -22;
            if (copy_process_bytes(p, f->r[3], &enabled, sizeof enabled, 0)) return -14;
            sock->passcred = !!enabled;
            return 0;
        }
        if (op == 211 || op == 212) {
            struct {
                uint64_t name, namelen, iov, count, control, space;
                uint32_t flags, pad;
            } msg;
            struct socket_iovec vec[16];
            int receive = op == 212;
            if (f->r[2] & ~(0x40ull | 0x4000ull | 0x40000000ull)) return -95;
            if (copy_process_bytes(p, f->r[1], &msg, sizeof msg, 0)) return -14;
#ifdef QUOTA_WAP_LOADER_TRACE
            if (!receive && msg.space && p->ancillary_traces < 32) {
                ++p->ancillary_traces;
                struct { uint64_t length; uint32_t level, type; } header;
                if (msg.space >= sizeof header && !copy_process_bytes(p, msg.control, &header, sizeof header, 0)) {
                    printf("[wap] ancillary pid=%d fd=%lu stream=%u level=%u type=%u len=%lu", p->pid, fd,
                           sock->stream, header.level, header.type, header.length);
                    if (header.level == 1 && header.type == 1 && header.length <= msg.space)
                        for (unsigned i = 0; i < 12 && sizeof header + 4*(i+1) <= header.length; ++i) {
                            uint32_t sent;
                            if (copy_process_bytes(p, msg.control + sizeof header + 4*i, &sent, 4, 0)) break;
                            if (sent < 3 || sent-3 >= WAP_FDS) continue;
                            typeof(p->files[0]) *entry = &p->files[sent-3];
                            printf(" [%u:%x:%s]", sent, entry->event_flags, entry->file ? entry->file->name : "");
                        }
                    printf("\n");
                }
            }
#endif
            if ((!receive && msg.name) || msg.count > 16) return -95;
            if (msg.count && copy_process_bytes(p, msg.iov, vec, msg.count * sizeof *vec, 0)) return -14;
            int64_t stream_result = 0;
            struct rights_batch *stream_batch = NULL;
            if (sock->stream && !receive) {
                if (msg.space) {
                    if (!peer) return -32;
                    for (unsigned i = 0; i < 4; ++i)
                        if (!peer->rights[i].count) { stream_batch = &peer->rights[i]; break; }
                    if (!stream_batch) return -11;
                    int error = capture_rights(p, peer, stream_batch, msg.control, msg.space);
                    if (error) return error;
                    stream_batch->offset = peer->packets;
                }
                uint64_t pc = f->elr;
                stream_result = transfer_stream(p, f, sock, 0, vec, msg.count, f->r[2]);
                if (msg.space && (stream_result <= 0 || pc != f->elr)) discard_rights(stream_batch);
                return stream_result;
            }
            if (sock->stream) {
                uint64_t pc = f->elr;
                stream_result = transfer_stream(p, f, sock, 1, vec, msg.count, f->r[2]);
                if (stream_result <= 0 || f->elr != pc) return stream_result;
                stream_batch = consume_stream_rights(sock, (unsigned)stream_result);
                if (!stream_batch) {
                    msg.space = msg.namelen = msg.flags = 0;
                    if (copy_process_bytes(p, f->r[1], &msg, sizeof msg, 1)) return -14;
                    return stream_result;
                }
            }
            if (!sock->stream) {
            if (receive && ((sock->shut & 1) || (!sock->packets && (!peer || (peer->shut & 2))))) return 0;
            if (!receive && (!peer || (peer->shut & 1) || (sock->shut & 2))) return -32;
            if (receive ? !sock->packets : peer->packets == 4) {
                if ((f->r[2] & 0x40) || sock->nonblock) return -11;
                f->elr -= 4;
                return fd;
            }
            typeof(sock->queue[0]) *packet = receive ? &sock->queue[sock->packet_head] :
                &peer->queue[(peer->packet_head + peer->packets) % 4];
            unsigned used = 0, limit = receive ? packet->size : sizeof packet->data;
            for (unsigned j = 0; j < msg.count; ++j) {
                if (!receive && vec[j].len > limit - used) return -90;
                unsigned n = vec[j].len > limit - used ? limit - used : vec[j].len;
                if (n && copy_process_bytes(p, vec[j].addr, packet->data + used, n, receive)) return -14;
                used += n;
            }
            if (!receive) {
                struct rights_batch *batch = &peer->rights[(peer->packet_head + peer->packets) % 4];
                if (msg.space) {
                    int error = capture_rights(p, peer, batch, msg.control, msg.space);
                    if (error) return error;
                }
                packet->size = used;
                packet->pid_namespace = p->pid_namespace;
            memcpy(packet->pid_ancestors, p->pid_ancestors, sizeof packet->pid_ancestors);
            packet->pid = p->pid;
                packet->uid = p->kernel_uid;
                packet->gid = p->kernel_gid;
                ++peer->packets;
                return used;
            }
            stream_result = used;
            msg.flags = used < packet->size ? 0x20 : 0;
            } else msg.flags = 0;
            uint64_t space = msg.space;
            msg.space = msg.namelen = 0;
            if (sock->passcred && !sock->stream) {
                typeof(sock->queue[0]) *packet = &sock->queue[sock->packet_head];
                unsigned uid = packet->uid, gid = packet->gid;
                if (p->user_namespace) {
                    uid = p->uid_mapped && uid == p->kernel_uid ? p->namespace_uid : 65534;
                    gid = p->gid_mapped && gid == p->kernel_gid ? p->namespace_gid : 65534;
                }
                unsigned sender = packet->pid;
                if (p->pid_namespace) {
                    if (p->pid_namespace != packet->pid_namespace) {
                        sender = 0;
                        for (unsigned i = 0; i < 3; ++i)
                            if (packet->pid_ancestors[i] == p->pid_namespace) sender = packet->pid;
                    }
                    else if (sender == p->pid_namespace) sender = 1;
                }
                struct { uint64_t len; uint32_t level, type, pid, uid, gid, pad; }
                    cred = { 28, 1, 2, sender, uid, gid, 0 };
                if (space >= cred.len) {
                    msg.space = space < sizeof cred ? space : sizeof cred;
                    if (copy_process_bytes(p, msg.control, &cred, msg.space, 1)) return -14;
                } else msg.flags |= 8;
            }
            struct rights_batch *batch = sock->stream ? stream_batch : &sock->rights[sock->packet_head];
            struct { uint64_t len; uint32_t level, type; int fds[8]; } out = {16, 1, 1, {0}};
            unsigned install = 0;
            if (batch->count) {
                unsigned slot = 0;
                for (; install < batch->count && space-msg.space >= 20+4*install; ++install) {
                    while (slot < WAP_FDS && (p->files[slot].file || p->files[slot].event_flags)) ++slot;
                    if (slot == WAP_FDS) break;
                    out.fds[install] = 3+slot++;
                }
                if (install) {
                    out.len += 4*install;
                    uint64_t bytes = (out.len+7) & ~7ull;
                    if (bytes > space-msg.space) bytes = space-msg.space;
                    if (copy_process_bytes(p, msg.control+msg.space, &out, bytes, 1)) return -14;
                    msg.space += bytes;
                }
                if (install < batch->count) msg.flags |= 8;
            }
            if (copy_process_bytes(p, f->r[1], &msg, sizeof msg, 1)) return -14;
            for (unsigned k = 0; k < install; ++k) {
                struct wap_descriptor *dest = &p->files[out.fds[k]-3];
                *dest = batch->entries[k];
                dest->cloexec = !!(f->r[2] & 0x40000000ull);
                batch->entries[k].socket = NULL;
                batch->entries[k].position = NULL;
            }
            if (install) printf("[wap] received rights pid=%d count=%u\n", p->pid, install);
            discard_rights(batch);
            if (sock->stream) return stream_result;
            sock->packet_head = (sock->packet_head + 1) % 4;
            --sock->packets;
            return stream_result;
        }
        if (op == 210) {
            if (f->r[1] > 2) return -22;
            sock->shut |= f->r[1] == 2 ? 3u : 1u << f->r[1];
            return 0;
        }
        if (op == 63 || op == 64 || op == 206 || op == 207) {
            int read = op == 63 || op == 207;
            if (op >= 206 && (f->r[3] & ~(0x40ull | 0x4000ull))) return -95;
            if ((op == 206 || op == 207) && f->r[4]) return -95;
            if (read && ((sock->shut & 1) || (!sock->packets && (!peer || (peer->shut & 2))))) return 0;
            if (!read && (!peer || (peer->shut & 1) || (sock->shut & 2))) return -32;
            if (sock->stream) {
                struct socket_iovec vec = { f->r[1], f->r[2] };
                uint64_t pc = f->elr;
                int64_t n = transfer_stream(p, f, sock, read, &vec, 1, op >= 206 ? f->r[3] : 0);
                if (read && f->elr == pc && n > 0) {
                    struct rights_batch *batch = consume_stream_rights(sock, (unsigned)n);
                    if (batch) discard_rights(batch);
                }
                return n;
            }
            if (!read && f->r[2] > sizeof sock->queue[0].data) return -90;
            if (read ? !sock->packets : peer->packets == 4) {
                if (sock->nonblock || (op >= 206 && (f->r[3] & 0x40))) return -11;
                f->elr -= 4;
                return fd;
            }
            if (read) {
                typeof(sock->queue[0]) *packet = &sock->queue[sock->packet_head];
                unsigned n = f->r[2] < packet->size ? f->r[2] : packet->size;
                if (copy_process_bytes(p, f->r[1], packet->data, n, 1)) return -14;
                discard_rights(&sock->rights[sock->packet_head]);
                sock->packet_head = (sock->packet_head + 1) % 4;
                --sock->packets;
                return n;
            }
            typeof(sock->queue[0]) *packet = &peer->queue[(peer->packet_head + peer->packets) % 4];
            if (copy_process_bytes(p, f->r[1], packet->data, f->r[2], 0)) return -14;
            packet->pid_namespace = p->pid_namespace;
            memcpy(packet->pid_ancestors, p->pid_ancestors, sizeof packet->pid_ancestors);
            packet->pid = p->pid;
            packet->uid = p->kernel_uid;
            packet->gid = p->kernel_gid;
            packet->size = f->r[2];
            ++peer->packets;
            return packet->size;
        }
    }
    unsigned uid = p->user_namespace ? (p->uid_mapped ? p->namespace_uid : 65534) : p->kernel_uid;
    unsigned gid = p->user_namespace ? (p->gid_mapped ? p->namespace_gid : 65534) : p->kernel_gid;
    if (op == 167 && (fd == 38 || fd == 39)) {
        if (f->r[2] || f->r[3] || f->r[4]) return -22;
        struct wap_thread *thread = &p->threads[p->active_thread];
        if (fd == 39) return f->r[1] ? -22 : (int)thread->no_new_privs;
        if (f->r[1] != 1) return -22;
        thread->no_new_privs = 1;
        return 0;
    }
    if (op == 97) {
        if (!fd) return 0;
        if (fd != 0x10000000u) return -22;
        if (p->live_threads != 1) return -22;
        if (p->namespace_depth >= 32) return -28;
        if (p->user_namespace && (!p->uid_mapped || !p->gid_mapped)) return -1;
        for (unsigned i = 0; i < WAP_FDS; ++i) {
            const struct runtime_file *open = p->files[i].file;
            if (open == &uid_map || open == &gid_map || open == &setgroups_file) return -16;
        }
        enter_user_namespace(p, p->namespace_depth + 1, uid, gid);
        return 0;
    }
    if (op == 174 || op == 175) return uid;
    if (op == 176 || op == 177) return gid;
    if (op == 148 || op == 150) {
        uint32_t id = op == 148 ? uid : gid;
        for (unsigned i = 0; i < 3; ++i)
            if (copy_process_bytes(p, f->r[i], &id, sizeof id, 1)) return -14;
        return 0;
    }
    if (op == 158) {
        if (!fd) return 0;
        if ((int)fd < 0) return -22;
        return 0; /* No supplementary groups. */
    }
    if (op == 90 || op == 91) { /* Linux capability ABI v3. Only namespace-local operations have capability bits. */
        struct { uint32_t version; int32_t pid; } header;
        if (copy_process_bytes(p, fd, &header, sizeof header, 0)) return -14;
        if (header.version != 0x20080522u) {
            header.version = 0x20080522u;
            return copy_process_bytes(p, fd, &header, sizeof header, 1) ? -14 : -22;
        }
        if (header.pid && header.pid != p->pid && header.pid != (int)p->threads[p->active_thread].tid) return -3;
        struct wap_thread *self = &p->threads[p->active_thread];
        uint32_t caps[6] = {self->ns_effective, self->ns_permitted, 0, 0, 0, 0};
        if (op == 91) {
            if (copy_process_bytes(p, f->r[1], caps, sizeof caps, 0)) return -14;
            if ((caps[1] & ~self->ns_permitted) || (caps[0] & ~caps[1]) ||
                caps[2] || caps[3] || caps[4] || caps[5]) return -1;
            self->ns_effective = caps[0];
            self->ns_permitted = caps[1];
            return 0;
        }
        return copy_process_bytes(p, f->r[1], caps, sizeof caps, 1) ? -14 : 0;
    }
    if (op == 179) { /* Linux AArch64 sysinfo, used by glibc _SC_PHYS_PAGES. */
        struct {
            int64_t uptime;
            uint64_t loads[3], totalram, freeram, sharedram, bufferram;
            uint64_t totalswap, freeswap;
            uint16_t procs, pad;
            uint64_t totalhigh, freehigh;
            uint32_t mem_unit;
        } info = {0};
        _Static_assert(sizeof info == 112, "AArch64 sysinfo layout");
        info.uptime = ticks_now() / timer_freq();
        info.mem_unit = 4096;
        info.totalram = resource_ram_kb() / 4;
        info.freeram = free_frames - reserved_frames;
        for (unsigned i = 0; i < EL0_SLOTS; ++i)
            if (procs[i].task && procs[i].task->state != TASK_DEAD) ++info.procs;
        return copy_process_bytes(p, fd, &info, sizeof info, 1) ? -14 : 0;
    }
    if (op == 160) {
        static const char identity[6][65] = {
            "Quota OS", "quota", "0.1.0", "Quota OS extended", "aarch64", "(none)"
        };
        return copy_process_bytes(p, fd, (void *)identity, sizeof identity, 1) ? -14 : 0;
    }
    if (op == 59) {
        if (f->r[1] & ~(0x80000u | 0x800u)) return -22;
        unsigned pipe_id = 0;
        while (pipe_id < WAP_PIPE_COUNT && (pipes[pipe_id].readers || pipes[pipe_id].writers)) ++pipe_id;
        if (pipe_id == WAP_PIPE_COUNT) return -23;
        int pair[2];
        unsigned n = 0;
        for (unsigned i = 0; i < WAP_FDS && n < 2; i++) {
            if (!p->files[i].file && !p->files[i].event_flags) pair[n++] = i + 3;
        }
        if (n != 2) return -24;
        if (copy_process_bytes(p, fd, pair, sizeof pair, 1)) return -14;
        struct wap_pipe *pipe = &pipes[pipe_id];
        pipe->size = pipe->head = 0;
        pipe->readers = pipe->writers = 1;
        for (unsigned i = 0; i < 2; i++) {
            p->files[pair[i]-3].pipe_id = pipe_id;
            p->files[pair[i]-3].identity = ++next_fd_identity;
            p->files[pair[i]-3].cloexec = !!(f->r[1] & 0x80000);
            p->files[pair[i]-3].event_flags = (i ? FD_PIPE_W : FD_PIPE_R) | f->r[1];
        }
        return 0;
    }
    if (index < WAP_FDS && (p->files[index].event_flags & (FD_PIPE_R | FD_PIPE_W))) {
        unsigned flags = p->files[index].event_flags;
        struct wap_pipe *pipe = &pipes[p->files[index].pipe_id];
        if (op == 57) {
            pipe_put(flags, p->files[index].pipe_id);
        }
        if (op == 63 || op == 64) {
            int read = op == 63;
            if (read != !!(flags & FD_PIPE_R)) return -9;
            if (!read && !pipe->readers) return -32;
            unsigned available = read ? pipe->size : sizeof pipe->buf - pipe->size;
            if (!available && f->r[2] && (!read || pipe->writers)) {
                if (flags & 0x800) return -11;
                f->elr -= 4;
                return fd;
            }
            unsigned n = f->r[2] < available ? f->r[2] : available;
            unsigned pos = (pipe->head + (read ? 0 : pipe->size)) % sizeof pipe->buf;
            unsigned first = n < sizeof pipe->buf - pos ? n : sizeof pipe->buf - pos;
            if (copy_process_bytes(p, f->r[1], pipe->buf + pos, first, read) ||
                copy_process_bytes(p, f->r[1] + first, pipe->buf, n - first, read)) return -14;
            if (read) {
                pipe->head = (pos + n) % sizeof pipe->buf;
                pipe->size -= n;
            } else pipe->size += n;
            return n;
        }
    }
    if (op == 20) {
        if (fd & ~0x80000u) return -22;
        for (unsigned i = 0; i < WAP_FDS; ++i) {
            if (p->files[i].file || p->files[i].event_flags) continue;
            if (!(p->files[i].poll = poll_alloc())) return -23;
            p->files[i].identity = ++next_fd_identity;
            p->files[i].cloexec = !!(fd & 0x80000);
            p->files[i].event_flags = FD_EPOLL;
            return i + 3;
        }
        return -24;
    }
    if (op == 21 || op == 22) {
        if (index >= WAP_FDS || p->files[index].event_flags != FD_EPOLL) return -9;
        struct { uint32_t events, padding; uint64_t data; } event;
        if (op == 21) {
            unsigned target = f->r[2] - 3;
            if (target >= WAP_FDS || !p->files[target].event_flags) return -9;
            if (p->files[target].event_flags == FD_EPOLL) return -22;
            struct poll_watch *w = NULL, *available = NULL;
            for (unsigned i = 0; i < WAP_FDS; ++i) {
                struct poll_watch *entry = &p->files[index].poll->watches[i];
                if (entry->events && entry->identity == p->files[target].identity &&
                    entry->fd_index == target) { w = entry; break; }
                if (entry->events) watch_ready(entry);
                if (!entry->events && !available) available = entry;
            }
            if (!w) w = available;
            if (!w) return f->r[1] == 1 ? -28 : -2;
            if (f->r[1] == 1 && w->events) return -17;
            if (f->r[1] != 1 && !w->events) return -2;
            if (f->r[1] == 2) { w->events = 0; return 0; }
            if (f->r[1] != 1 && f->r[1] != 3) return -22;
            if (copy_process_bytes(p, f->r[3], &event, sizeof event, 0)) return -14;
            w->events = event.events | 0x18;
            w->identity = p->files[target].identity;
            w->fd_index = target;
            w->owner_slot = p->slot;
            w->data = event.data;
            return 0;
        }
        if (!(int)f->r[2] || (int)f->r[2] < 0) return -22;
        if (f->r[4]) return -95;
        unsigned count = 0;
        struct wap_thread *thread = &p->threads[p->active_thread];
        for (unsigned i = 0; i < WAP_FDS && count < f->r[2]; i++) {
            struct poll_watch *w = &p->files[index].poll->watches[i];
            if (!w->events) continue;
            unsigned ready = watch_ready(w);
            if (!(ready & w->events)) continue;
            event.events = ready & w->events;
            event.padding = 0;
            event.data = w->data;
            if (copy_process_bytes(p, f->r[1] + sizeof event * count, &event, sizeof event, 1)) return -14;
            ++count;
        }
        int timeout = (int)f->r[3];
        if (!count && timeout > 0 && !thread->poll_deadline)
            thread->poll_deadline = ticks_now() + (uint64_t)timeout * timer_freq() / 1000;
        if (!count && (timeout < 0 || (timeout > 0 && ticks_now() < thread->poll_deadline))) {
            f->elr -= 4;
            return fd;
        }
        thread->poll_deadline = 0;
        return count;
    }
#endif
    if (op == 19) { /* eventfd2, used by the GLib main context */
        unsigned flags = f->r[1];
        if (flags & ~(1u | 0x800u | 0x80000u)) return -22;
        if (!(flags & 0x800u)) return -95; /* nonblocking endpoints only */
        for (unsigned i = 0; i < WAP_FDS; ++i) {
            if (p->files[i].file || p->files[i].event_flags) continue;
            if (!(p->files[i].poll = poll_alloc())) return -23;
            p->files[i].identity = ++next_fd_identity;
            p->files[i].cloexec = !!(flags & 0x80000);
            p->files[i].event_flags = flags;
            p->files[i].poll->count = (uint32_t)fd;
            return i + 3;
        }
        return -24;
    }
    if (index < WAP_FDS && p->files[index].event_flags) {
        uint64_t value;
        if (op == 57) {
            if (p->files[index].poll) --p->files[index].poll->refs;
#ifdef QUOTA_WAP_FULL_RUNTIME
            if (p->files[index].socket) packet_put(p->files[index].socket);
#endif
            memset(&p->files[index], 0, sizeof p->files[index]);
            return 0;
        }
        if (op == 63 || op == 64) {
            if (!p->files[index].poll || p->files[index].event_flags == FD_EPOLL) return -22;
            uint64_t *count = &p->files[index].poll->count;
            if (f->r[2] < sizeof value) return -22;
            if (op == 63) {
                if (!*count) return -11;
                value = p->files[index].event_flags & 1 ? 1 : *count;
                if (copy_process_bytes(p, f->r[1], &value, sizeof value, 1)) return -14;
                *count -= value;
            } else {
                if (copy_process_bytes(p, f->r[1], &value, sizeof value, 0)) return -14;
                if (value == UINT64_MAX) return -22;
                if (value >= UINT64_MAX - *count) return -11;
                *count += value;
            }
            return sizeof value;
        }
    }
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (op == 129) {
        if ((int)fd <= 0 || (f->r[1] != 0 && f->r[1] != 9)) return -95;
        for (unsigned i = 0; i < EL0_SLOTS; ++i) {
            if (p->children[i].pid != (int)fd) continue;
            if (p->children[i].done) return 0;
            for (unsigned j = 0; j < EL0_SLOTS; ++j) {
                struct proc *child = &procs[j];
                if (!child->alive || child->pid != (int)fd || child->parent_handle != make_handle(p)) continue;
                if (child->kernel_uid != p->kernel_uid) return -1;
                if (f->r[1]) {
                    child->exit_status = 9;
                    reap_timed(child, "child-signal", ticks_now(), sched_period_now());
                }
                return 0;
            }
        }
        return -3;
    }
    if (op == 260) {
        if ((int)fd != -1 && (int)fd <= 0) return -95;
        if (f->r[2] & ~1ull || f->r[3]) return -95;
        unsigned matches = 0;
        for (unsigned i = 0; i < EL0_SLOTS; ++i) {
            struct child_status *child = &p->children[i];
            if (!child->pid || ((int)fd != -1 && (int)fd != child->pid)) continue;
            ++matches;
            if (!child->done) continue;
            if (f->r[1] && copy_process_bytes(p, f->r[1], &child->status, 4, 1)) return -14;
            int pid = child->pid;
            *child = (struct child_status){0};
            return pid;
        }
        if (!matches) return -10;
        if (f->r[2] & 1) return 0;
        f->elr -= 4;
        return fd;
    }
    if (op == 220 && fd == 0x84311) {
        if (p->live_threads != 1 || !p->user_namespace || !p->uid_mapped ||
            !f->r[1] || (f->r[1] & 15) || !f->r[3]) return -22;
        unsigned slot = 0;
        while (slot < EL0_SLOTS && p->children[slot].pid) ++slot;
        if (slot == EL0_SLOTS) return -11;
        p->vfork_record = slot;
        p->vfork_pid = ++next_child_pid;
        p->suspended_frame = *f;
        asm volatile("mrs %0,tpidr_el0" : "=r"(p->suspended_tls));
        wap_fp_save(p->suspended_fp);
        asm volatile("msr tpidr_el0,%0" :: "r"(f->r[3]));
        f->pad = f->r[1];
        return 0;
    }
    if (op == 220 && (fd == 17 || fd == 0x1200011 || fd == 0x10000011 || fd == 0x20000011 || fd == 0x30000011)) return fork_private(p, f);
    if (op == 220) return clone_thread(p, f);
    if (op == 172) return p->pid_namespace == (unsigned)p->pid ? 1 : p->pid;
    if (op == 173) {
        struct proc *parent = from_handle(p->parent_handle, 1);
        if (!parent || parent->pid_namespace != p->pid_namespace) return 0;
        return parent->pid_namespace == (unsigned)parent->pid ? 1 : parent->pid;
    }
    if (op == 178) return p->threads[p->active_thread].tid;
    if (op == 124) return 0;
#endif
    if (op == 98) {
        if (fd & 3) return -22;
        uint32_t word;
        if (copy_process_bytes(p, fd, &word, sizeof word, 0)) return -14;
        unsigned cmd = f->r[1] & ~128u;
        /* Only private, single-task address spaces are supported here. */
        if (cmd == 1 || cmd == 10) {
            if (cmd == 10 && !(uint32_t)f->r[5]) return -22;
#ifdef QUOTA_WAP_FULL_RUNTIME
            return wake_threads(p, fd, f->r[2], cmd == 10 ? f->r[5] : ~0u);
#else
            return 0;
#endif
        }
#ifdef QUOTA_WAP_FULL_RUNTIME
        int realtime = !!(cmd & 256);
        cmd &= ~256u;
        if (cmd == 0 || cmd == 9) {
            struct wap_thread *self = &p->threads[p->active_thread];
            unsigned bits = cmd == 0 ? ~0u : (unsigned)f->r[5];
            if (!bits || (cmd == 0 && realtime)) return -22;
            if (word != (uint32_t)f->r[2]) {
                self->futex_deadline = 0;
                return -11;
            }
            uint64_t now = ticks_now(), freq = timer_freq();
            if (f->r[3] && !self->futex_deadline) {
                uint64_t ts[2];
                if (copy_process_bytes(p, f->r[3], ts, sizeof ts, 0)) return -14;
                if ((int64_t)ts[0] < 0 || ts[1] >= 1000000000) return -22;
                uint64_t base = cmd == 0 ? now : 0;
                if (realtime) {
                    uint64_t wall[2];
                    if (timer_realtime(wall)) return -19;
                    if (ts[0] < wall[0] || (ts[0] == wall[0] && ts[1] <= wall[1])) return -110;
                    ts[0] -= wall[0];
                    if (ts[1] < wall[1]) { --ts[0]; ts[1] += 1000000000; }
                    ts[1] -= wall[1];
                    base = now;
                }
                uint64_t sub = (ts[1] * freq + 999999999) / 1000000000;
                self->futex_deadline = ts[0] > (UINT64_MAX - base - sub) / freq
                                     ? UINT64_MAX : base + ts[0] * freq + sub;
                if (!self->futex_deadline || now >= self->futex_deadline) {
                    self->futex_deadline = 0;
                    return -110;
                }
            }
            if (self->futex_deadline && now >= self->futex_deadline) {
                self->futex_deadline = 0;
                return -110;
            }
            for (unsigned i = 0; i < WAP_THREADS; ++i) {
                struct wap_thread *other = &p->threads[i];
                if (i == p->active_thread || !other->tid) continue;
                if (other->wait_addr && (!other->futex_deadline || now < other->futex_deadline)) continue;
                self->wait_addr = fd;
                self->wait_bits = bits;
                return 0;
            }
            f->elr -= 4;
            return fd;
        }
#endif
        if ((cmd == 0 || cmd == 9) && word != (uint32_t)f->r[2]) return -11;
        return -38;
    }
    if (op == 96) {
        p->clear_child_tid = fd;
#ifdef QUOTA_WAP_FULL_RUNTIME
        return p->threads[p->active_thread].tid;
#else
        return p->pid;
#endif
    }
    if (op == 172 || op == 178) return p->pid;
    if (op == 261) {
        if (fd && fd != (uint64_t)p->pid) return -3;
        uint64_t bytes;
        switch (f->r[1]) {
        case 2: bytes = (uint64_t)ELF_BRK_PAGES * 4096; break;
        case 3: bytes = (uint64_t)ELF_STACK_PAGES * 4096; break;
#ifdef QUOTA_WAP_FULL_RUNTIME
        case 6: bytes = WAP_THREADS; break;
#endif
        case 7: bytes = WAP_FDS + 3; break;
        default: return -38;
        }
        uint64_t limits[2] = {bytes, bytes};
        if (f->r[2]) {
            uint64_t requested[2];
            if (copy_process_bytes(p, f->r[2], requested, sizeof requested, 0)) return -14;
            if (requested[0] > requested[1]) return -22;
            if (requested[1] > bytes) return -1;
            /* Fixed enforced bounds already satisfy an idempotent setrlimit. */
            if (requested[0] != bytes || requested[1] != bytes) return -95;
        }
        return f->r[3] && copy_process_bytes(p, f->r[3], limits, sizeof limits, 1) ? -14 : 0;
    }
    if (op == 123) { /* sched_getaffinity, one QEMU vCPU */
        uint64_t mask = 1;
        if (fd && fd != (uint64_t)p->pid) return -3;
        if (f->r[1] < sizeof mask || (f->r[1] % sizeof mask)) return -22;
        return copy_process_bytes(p, f->r[2], &mask, sizeof mask, 1) ? -14 : sizeof mask;
    }
    if (op == 134) {
        if (fd < 1 || fd > 64 || f->r[3] != 8) return -22;
        typeof(p->signal_actions[0]) new_action;
        if (f->r[1]) {
            if (fd == 9 || fd == 19) return -22;
            if (copy_process_bytes(p, f->r[1], &new_action, sizeof new_action, 0)) return -14;
        }
        if (f->r[2] && copy_process_bytes(p, f->r[2], &p->signal_actions[fd - 1], sizeof new_action, 1))
            return -14;
        if (f->r[1]) p->signal_actions[fd - 1] = new_action;
        return 0;
    }
    if (op == 135) {
        uint64_t mask = 0;
        if (f->r[3] != sizeof mask) return -22;
        if (f->r[1]) {
            if (fd > 2) return -22;
            if (copy_process_bytes(p, f->r[1], &mask, sizeof mask, 0)) return -14;
            mask &= ~((1ull << 8) | (1ull << 18));
        }
        if (f->r[2] && copy_process_bytes(p, f->r[2], &p->signal_mask, sizeof mask, 1)) return -14;
        if (f->r[1]) {
            if (fd == 0) p->signal_mask |= mask;
            else if (fd == 1) p->signal_mask &= ~mask;
            else p->signal_mask = mask;
        }
        return 0;
    }
    if (op == 278 || (op == 63 && file == &urandom_file)) {
        uint8_t buf[256];
        if (op == 278 && (f->r[2] & ~3ull)) return -22;
        uint64_t size = f->r[op == 278 ? 1 : 2];
        unsigned n = size < sizeof buf ? size : sizeof buf;
        int result = rng_read(buf, n);
        if (result == -11 && (op == 63 || !(f->r[2] & 1))) {
            f->elr -= 4;
            return fd;
        }
        if (result > 0 && copy_process_bytes(p, f->r[op == 278 ? 0 : 1], buf, result, 1)) result = -14;
        memset(buf, 0, sizeof buf);
        return result;
    }
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (op == 221) return exec_system_runtime(p, f);
    if (op == 78) {
        if (p->empty_root) return -2;
        static const char link[] = "/proc/self/exe";
        char path[sizeof link];
        if (copy_process_bytes(p, f->r[1], path, sizeof path, 0)) return -14;
        if (path[sizeof path - 1] || strcmp(path, link)) return runtime_lookup(p, f->r[1]) ? -22 : -2;
        if (!f->r[3]) return -22;
        const char *exe_path = SYSTEM_RUNTIME_PATH;
        unsigned len = sizeof SYSTEM_RUNTIME_PATH - 1;
#ifdef QUOTA_NODE_WEB
        if (p->page_executable) { exe_path = BLINK_RUNTIME_PATH; len = sizeof BLINK_RUNTIME_PATH - 1; }
#endif
        unsigned n = f->r[3] < len ? f->r[3] : len;
        return copy_process_bytes(p, f->r[2], (void *)exe_path, n, 1) ? -14 : (int)n;
    }
#endif
    if (op == 169) {
        uint64_t tv[2], utc = 0;
        if (timer_realtime(tv)) return -19;
        tv[1] /= 1000;
        if (fd && copy_process_bytes(p, fd, tv, sizeof tv, 1)) return -14;
        return f->r[1] && copy_process_bytes(p, f->r[1], &utc, sizeof utc, 1) ? -14 : 0;
    }
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (op == 113 && (fd == 2 || fd == 3)) {
        uint64_t charged = p->task->used_total;
        uint64_t ticks = fd == 2 ? charged :
            p->threads[p->active_thread].cpu_ticks + (charged - p->thread_charge_base);
        uint64_t freq = timer_freq();
        uint64_t ts[2] = {ticks / freq, (ticks % freq) * 1000000000ull / freq};
        return copy_process_bytes(p, f->r[1], ts, sizeof ts, 1) ? -14 : 0;
    }
#endif
    if (op == 113 && (fd == 0 || fd == 5)) {
        uint64_t ts[2];
        if (timer_realtime(ts)) return -19;
        return copy_process_bytes(p, f->r[1], ts, sizeof ts, 1) ? -14 : 0;
    }
    if (op == 113 && (fd == 1 || fd == 4 || fd == 6 || fd == 7)) {
        uint64_t freq = timer_freq();
        uint64_t ticks = ticks_now();
        uint64_t ts[2] = {ticks / freq, ((ticks % freq) * 1000000000ull) / freq};
        return copy_process_bytes(p, f->r[1], ts, sizeof ts, 1) ? -14 : 0;
    }
    if (op == 66 && (fd == 1 || fd == 2)) {
        if (f->r[2] > 16) return -22;
        unsigned total = 0;
        for (unsigned i = 0; i < f->r[2]; i++) {
            uint64_t vec[2];
            char buf[129];
            if (copy_process_bytes(p, f->r[1] + sizeof vec * i, vec, sizeof vec, 0)) return -14;
            unsigned n = vec[1] < sizeof buf ? vec[1] : sizeof buf - 1;
            if (copy_process_bytes(p, vec[0], buf, n, 0)) return -14;
            buf[n] = 0;
            printf("%s", buf);
            total += n;
            if (n < vec[1]) break;
        }
        return total;
    }
#ifdef QUOTA_WAP_FULL_RUNTIME
    if (op == 64 && (file == &uid_map || file == &gid_map || file == &setgroups_file))
        return write_self_map(p, file, f->r[1], f->r[2]);
#endif
    if (op == 48) {
        unsigned mode = (unsigned)f->r[2];
        if (mode & ~7u) return -22;
        const struct runtime_file *target = runtime_lookup(p, f->r[1]);
        if (!target) return -2;
        unsigned allowed = 4;
        if (target == &null_file) allowed |= 2;
        if (target == &app_dir || target == &proc_dir || target == &fd_dir || target == &task_dir)
            allowed |= 1;
#ifdef QUOTA_LINUX_APP_RUNTIME
        for (unsigned i = 0; i < sizeof temp_dirs / sizeof *temp_dirs; ++i)
            if (target == &temp_dirs[i].file) allowed = (temp_dirs[i].mode >> 6) & 7;
        if (target == &shm_dir) allowed = 7;
        if (shm_for_file(target)) allowed = 6;
        if (target == &system_executable) allowed |= 1;
        for (unsigned i = 0; i < sizeof runtime_dirs / sizeof *runtime_dirs; ++i)
            if (target == &runtime_dirs[i]) allowed |= 1;
#endif
        return (mode & ~allowed) ? -13 : 0;
    }
#ifdef QUOTA_WAP_LOADER_TRACE
    if (op == 34) {
        char path[192];
        unsigned i;
        for (i = 0; i + 1 < sizeof path; ++i) {
            if (copy_process_bytes(p, f->r[1] + i, path+i, 1, 0) || !path[i]) break;
        }
        path[i] = 0;
        printf("[wap] mkdirat pid=%d path=%s mode=%x\n", p->pid, path, (unsigned)f->r[2]);
    }
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (file == &note_path && (op == 63 || op == 64)) {
        struct file_position *position = p->files[index].position;
        unsigned size = 0, at = position->offset;
        const unsigned char *old = note_disk_wap_note(&size);
        if (op == 63) {
            if (p->files[index].access_mode == 1) return -9;
            if (at >= size) return 0;
            unsigned count = size-at < f->r[2] ? size-at : (unsigned)f->r[2];
            if (copy_process_bytes(p, f->r[1], (void *)(old + at), count, 1)) return -14;
            position->offset += count;
            return count;
        }
        if (!p->files[index].access_mode) return -9;
        if (!f->r[2]) return 0;
        unsigned tid = p->threads[p->active_thread].tid;
        if (!p->note_writer_tid) {
            unsigned char bytes[496] = {0};
            if (at > sizeof bytes || f->r[2] > sizeof bytes-at) return -27;
            if (size) memcpy(bytes, old, size);
            if (copy_process_bytes(p, f->r[1], bytes+at, f->r[2], 0)) return -14;
            unsigned end = at+(unsigned)f->r[2];
            if (note_disk_save_wap_note(bytes, end > size ? end : size, &p->pending_note)) return -5;
            p->note_writer_tid = tid;
        } else if (p->note_writer_tid != tid) return -16;
        int committed = note_disk_wap_note_committed(p->pending_note);
        if (!committed) { f->elr -= 4; return fd; }
        p->note_writer_tid = 0;
        if (committed < 0) return -5;
        position->offset += f->r[2];
#ifdef QUOTA_NODE_WEB
        printf("[wap] backend disk flush generation=%u bytes=%lu pid=%d\n",
               p->pending_note, f->r[2], p->pid);
#endif
        return f->r[2];
    }
    if (op == 17) {
        /* Guest runtime processes start in the root of their mount namespace. */
        static const char cwd[] = "/";
        if (f->r[1] < sizeof cwd) return -34;
        return copy_process_bytes(p, fd, (void *)cwd, sizeof cwd, 1) ? -14 : (int)sizeof cwd;
    }
    if (op == 35) {
        struct shm_object *obj = shm_for_file(runtime_lookup(p, f->r[1]));
        if (!obj) return -2;
        if (f->r[2]) return -22;
        obj->path[0] = 0;
        return 0;
    }
    if ((op == 46 || op == 47 || op == 68) && shm_for_file(file)) {
        if (!p->files[index].access_mode) return -9;
        uint64_t old = file->end - file->begin, size = f->r[1];
        if (op != 46) {
            if (op == 47 && f->r[1]) return -95;
            uint64_t offset = f->r[op == 47 ? 2 : 3], count = f->r[op == 47 ? 3 : 2];
            if (offset > SHM_CAPACITY || count > SHM_CAPACITY-offset) return -28;
            if (!count) return op == 47 ? -22 : 0;
            size = offset + count;
            if (size < old) size = old;
        }
        if (size > SHM_CAPACITY) return -28;
        if (size < old) return -95;
        if (size > old) memset((void *)(file->begin + old), 0, ((size+4095) & ~4095ull)-old);
        if (op == 68 && copy_process_bytes(p, f->r[1], (void *)(file->begin+f->r[3]), f->r[2], 0)) return -14;
        shm_for_file(file)->file.end = file->begin + size;
        return op == 68 ? (int64_t)f->r[2] : 0;
    }
    if (op == 34) return temp_mkdir(p, f->r[1], f->r[2]);
#endif
    if (op == 56
#ifdef QUOTA_LINUX_APP_RUNTIME
        || op == 0x5152
#endif
    ) {
        const struct runtime_file *directory = file;
#ifdef QUOTA_LINUX_APP_RUNTIME
        if (op == 0x5152) {
            if (fd & ~0x80000ull) return -22;
            f->r[2] = fd;
            file = &urandom_file;
        } else
#endif
        file = runtime_lookup(p, f->r[1]);
#ifdef QUOTA_LINUX_APP_RUNTIME
        if (file == &note_path) {
            unsigned size, flags = f->r[2], tid = p->threads[p->active_thread].tid;
            int exists = note_disk_wap_note(&size) != NULL;
            if (flags & ~(3u | 64u | 128u | 512u | 0x80000u | 0x8000u)) return -95;
            if ((flags & 3) == 3) return -22;
            if (!exists && !(flags & 64)) return -2;
            if (exists && (flags & 192) == 192 && p->note_writer_tid != tid) return -17;
            if (!exists || (flags & 512) || p->note_writer_tid == tid) {
                if (!p->note_writer_tid) {
                    if (note_disk_save_wap_note((const unsigned char *)"", 0, &p->pending_note)) return -5;
                    p->note_writer_tid = tid;
                } else if (p->note_writer_tid != tid) return -16;
                int committed = note_disk_wap_note_committed(p->pending_note);
                if (!committed) { f->elr -= 4; return fd; }
                p->note_writer_tid = 0;
                if (committed < 0) return -5;
            }
        } else if (f->r[2] & 64) {
            if (file && (f->r[2] & 128)) return -17;
            char path[128];
            if (!file && !shm_path(p, f->r[1], path)) {
                unsigned j = 0;
                while (j < SHM_OBJECTS && shm_slot_busy(j)) ++j;
                if (j == SHM_OBJECTS) {
                    printf("[wap] shared object slots full pid=%d\n", p->pid);
                    return -28;
                }
                struct shm_object *obj = &shm_objects[j];
                unsigned k = 0;
                do { obj->path[k] = path[k]; } while (path[k++]);
                obj->owner = p->temp_owner;
                obj->file = (struct runtime_file){obj->path, shm_data[j], shm_data[j]};
                file = &obj->file;
            }
        }
#endif
        if (!file && directory == &proc_dir) {
            static const char relative[] = "self/fd/";
            char path[sizeof relative];
            for (unsigned j = 0; j < sizeof relative; ++j) {
                if (copy_process_bytes(p, f->r[1]+j, &path[j], 1, 0)) return -14;
                if (path[j] != relative[j]) break;
                if (j+1 == sizeof relative) file = &fd_dir;
            }
        }
        if (!file && (directory == &proc_dir || directory == &task_dir || directory == &app_dir)) {
            char dot[2];
            if (copy_process_bytes(p, f->r[1], dot, 1, 0)) return -14;
            if (dot[0] == '.') {
                if (copy_process_bytes(p, f->r[1] + 1, dot + 1, 1, 0)) return -14;
                if (!dot[1]) file = directory;
            }
        }
        if (!file) return -2;
#ifdef QUOTA_WAP_FULL_RUNTIME
        if (file == &uid_map || file == &gid_map || file == &setgroups_file) {
            if ((f->r[2] & 3) != 1 || !p->user_namespace) return -13;
        } else
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
        if (shm_for_file(file)) {
            if (f->r[2] & 512) return -95;
        } else if (file == &window_device) {
            if ((f->r[2] & 3) != 2) return -13;
        } else
#endif
        if (file != &null_file &&
#ifdef QUOTA_LINUX_APP_RUNTIME
            file != &note_path &&
#endif
            (f->r[2] & 3)) return -30;
        if ((f->r[2] & 3) == 3) return -22;
        for (unsigned i = 0; i < WAP_FDS; ++i) {
            if (p->files[i].file || p->files[i].event_flags) continue;
            unsigned j = 0;
            while (j < WAP_OBJECT_SLOTS && positions[j].refs) ++j;
            if (j == WAP_OBJECT_SLOTS) return -23;
            positions[j] = (struct file_position){ .backing = file, .refs = 1, .status_flags = f->r[2] & (3u | 0x4000u | 0x800u) };
#ifdef QUOTA_WAP_FULL_RUNTIME
            if (file == &pid_ns_file) positions[j].namespace_inode = (1ull << 32) + p->pid_namespace;
            if (file == &user_ns_file) positions[j].namespace_inode = 1 + p->user_namespace;
#endif
            p->files[i].cloexec = !!(f->r[2] & 0x80000);
            p->files[i].access_mode = f->r[2] & 3;
            p->files[i].file = file;
            p->files[i].position = &positions[j];
            printf("[wap] opened runtime library %s\n", file->name);
            return i + 3;
        }
        return -24;
    }
    if (op == 57) {
        if (!file) return -9;
        --p->files[index].position->refs;
        p->files[index].position = NULL;
        p->files[index].file = NULL;
        return 0;
    }
    /* Standard streams are inherited by the page process. Input is an empty
       stream and output goes to the Quota console. */
    if (op == 80 && fd <= 2) {
        uint64_t st[16] = {0};
        st[0] = 1;
        st[1] = fd + 1;
        st[2] = (1ull << 32) | (fd ? 020200 : 020400);
        st[7] = 4096;
        return copy_process_bytes(p, f->r[1], st, sizeof st, 1) ? -14 : 0;
    }
    if (op == 63 && !fd) return 0;
    if (op == 25 && f->r[1] == 3 && file && p->files[index].position) return p->files[index].position->status_flags;
    if (op == 25 && fd <= 2 && f->r[1] == 3) return fd ? 1 : 0;
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (op == 61 && file && !strcmp(file->name, "/usr/lib/quota-runtime/fonts")) {
        unsigned *cursor = &p->files[index].position->offset;
        unsigned total = 0;
        while (*cursor < (unsigned)(wap_libs_end - wap_libs_start)) {
            unsigned pos = *cursor;
            const char *name = wap_libs_start[pos].name;
            unsigned matched = 0;
            while (matched < 6 && name[matched] == "fonts/"[matched]) ++matched;
            if (matched != 6) { ++*cursor; continue; }
            name += 6;
            struct { uint64_t ino, next; uint16_t len; uint8_t type; char name[128]; } rec = {0};
            unsigned size = 1;
            while (size <= sizeof rec.name && name[size-1]) ++size;
            if (size > sizeof rec.name) return -36;
            rec.ino = pos + 1;
            rec.next = pos + 1;
            rec.len = (19 + size + 7) & ~7u;
            rec.type = 8;
            memcpy(rec.name, name, size);
            if (rec.len > f->r[2] - total) return total ? (int)total : -22;
            if (copy_process_bytes(p, f->r[1]+total, &rec, rec.len, 1)) return total ? (int)total : -14;
            ++*cursor;
            total += rec.len;
        }
        return total;
    }
#endif
    if (op == 61 && file == &fd_dir) {
        unsigned total = 0;
        unsigned *cursor = &p->files[index].position->offset;
        while (*cursor < WAP_FDS + 5) {
            unsigned pos = *cursor, num = pos - 2;
            if (pos >= 5 && !p->files[num-3].file && !p->files[num-3].event_flags) {
                ++*cursor;
                continue;
            }
            struct {
                uint64_t ino, next;
                uint16_t len;
                uint8_t type;
                char name[5];
            } entry = { pos+1, pos+1, 24, pos < 2 ? 4 : 10, {0} };
            if (pos < 2) {
                entry.name[0] = '.';
                if (pos) entry.name[1] = '.';
            } else if (num < 10) entry.name[0] = '0' + num;
            else {
                unsigned at = 0;
                if (num >= 100) entry.name[at++] = '0' + num/100;
                entry.name[at++] = '0' + num/10%10;
                entry.name[at] = '0' + num%10;
            }
            if (f->r[2] - total < sizeof entry) return total ? (int)total : -22;
            if (copy_process_bytes(p, f->r[1]+total, &entry, sizeof entry, 1)) return total ? (int)total : -14;
            ++*cursor;
            total += sizeof entry;
        }
        return total;
    }
    if (op == 79 || op == 80) {
        if (op == 79) {
            char first;
            if (copy_process_bytes(p, f->r[1], &first, 1, 0)) return -14;
            if (file == &fd_dir && first >= '0' && first <= '9') {
                unsigned num = 0;
                char c = first;
                for (unsigned j = 0; c; ++j) {
                    if (c < '0' || c > '9' || j >= 3) return -2;
                    num = num*10 + c-'0';
                    if (copy_process_bytes(p, f->r[1]+j+1, &c, 1, 0)) return -14;
                }
                if (num >= WAP_FDS+3) return -2;
                file = num < 3 ? NULL : p->files[num-3].file;
                unsigned kind = num < 3 ? 0020000 : 0;
                if (num >= 3 && p->files[num-3].event_flags) {
                    unsigned flags = p->files[num-3].event_flags;
                    kind = flags & FD_PACKET ? 0140000 : flags & (FD_PIPE_R|FD_PIPE_W) ? 0010000 : 0100000;
                }
                if (!file && !kind) return -2;
                if ((f->r[3] & 0x100) || !file) {
                    uint64_t st[16] = {1, num+1, (1ull<<32) | ((f->r[3] & 0x100) ? 0120777 : kind|0600)};
                    return copy_process_bytes(p,f->r[2],st,sizeof st,1) ? -14 : 0;
                }
            } else if (file == &proc_dir && first && first != '/') {
                char path[12];
                unsigned j;
                for (j = 0; j < sizeof path; ++j) {
                    if (copy_process_bytes(p, f->r[1] + j, &path[j], 1, 0)) return -14;
                    if (!path[j]) break;
                }
                file = j < sizeof path && (!strcmp(path, "self/task/") || !strcmp(path, "self/task")) ? &task_dir : NULL;
            } else if (first || !(f->r[3] & 0x1000)) file = runtime_lookup(p, f->r[1]);
        }
        if (!file) return op == 79 ? -2 : -9;
        /* Linux asm-generic stat on AArch64. */
        uint64_t st[16] = { 0 };
#ifdef QUOTA_WAP_FULL_RUNTIME
        if (file == &user_ns_file || file == &pid_ns_file) {
            st[0] = 2;
            st[1] = op == 80 ? p->files[index].position->namespace_inode :
                    file == &pid_ns_file ? (1ull << 32) + p->pid_namespace : 1 + p->user_namespace;
            st[2] = (1ull << 32) | (op == 79 && (f->r[3] & 0x100) ? 0120777 : 0100444);
            st[7] = 4096;
            return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
        }
#endif
        unsigned dir_inode = 0;
#ifdef QUOTA_LINUX_APP_RUNTIME
        struct shm_object *shm = shm_for_file(file);
        if (file == &shm_dir || shm) {
            st[0] = 1; st[1] = shm ? 0x41001 + (shm - shm_objects) : 0x41000;
            st[2] = (1ull<<32) | (shm ? 0100600 : 0041777);
            st[6] = shm ? file->end-file->begin : 0; st[7] = 4096;
            return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
        }
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
        for (unsigned i = 0; i < sizeof temp_dirs / sizeof *temp_dirs; ++i) {
            if (file != &temp_dirs[i].file) continue;
            st[0] = 1; st[1] = 0x40000 + i; st[2] = (2ull << 32) | 0040000 | temp_dirs[i].mode;
            st[3] = p->kernel_uid | ((uint64_t)p->kernel_gid << 32);
            st[7] = 4096;
            return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
        }
        for (unsigned i = 0; i < sizeof runtime_dirs / sizeof *runtime_dirs; ++i)
            if (file == &runtime_dirs[i]) dir_inode = 0x10010 + i;
#endif
        if (dir_inode || file == &fd_dir || file == &app_dir || file == &proc_dir || file == &task_dir) {
            st[0] = 1;
            st[1] = dir_inode ? dir_inode : file == &fd_dir ? 0x10004 : file == &task_dir ? 0x20000u + (unsigned)p->pid : file == &proc_dir ? 0x10003 : 0x10000;
            uint64_t links = 2;
#ifdef QUOTA_WAP_FULL_RUNTIME
            if (file == &task_dir) links += p->live_threads;
#endif
            st[2] = (links << 32) | 0040555;
            st[7] = 4096;
            return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
        }
#ifdef QUOTA_LINUX_APP_RUNTIME
        if (file == &system_executable) {
            st[0] = 1; st[1] = 0x10020; st[2] = (1ull << 32) | 0100555;
            st[6] = file->end - file->begin; st[7] = 4096; st[8] = (st[6] + 511) / 512;
            return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
        }
#endif
#ifdef QUOTA_LINUX_APP_RUNTIME
        if (file == &note_path) {
            unsigned size = 0;
            if (!note_disk_wap_note(&size)) return -2;
            st[0] = 1; st[1] = 0x20003; st[2] = (1ull << 32) | 0100600;
            st[6] = size; st[7] = 512; st[8] = (size+511)/512;
            return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
        }
#endif
        if (file == &urandom_file || file == &null_file) {
            st[0] = 1;
            st[2] = (1ull << 32) | (file == &null_file ? 020666 : 020444);
            st[3] = file == &null_file ? 0x103 : 0x109;
            st[7] = 4096;
            return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
        }
        st[0] = 1; st[1] = file == &p->package_files[0] ? 0x10001 :
                             file == &p->package_files[1] ? 0x10002 : file - wap_libs_start + 1;
        st[2] = (1ull << 32) | 0100444; st[6] = file->end - file->begin;
        st[7] = 4096; st[8] = (st[6] + 511) / 512;
        return copy_process_bytes(p, f->r[op == 79 ? 2 : 1], st, sizeof st, 1) ? -14 : 0;
    }
    if (op == 62) { /* lseek on an image-backed regular file */
        if (index >= WAP_FDS) return fd <= 2 ? -29 : -9;
        if (!file) return p->files[index].identity ? -29 : -9;
        if (!file->begin || !p->files[index].position) return -29;
        int64_t base, next;
        switch (f->r[2]) {
        case 0: base = 0; break;
        case 1: base = p->files[index].position->offset; break;
        case 2: base = file->end - file->begin; break;
        default: return -22;
        }
        if (__builtin_add_overflow(base, (int64_t)f->r[1], &next)) return -75;
        if (next < 0) return -22;
        if ((uint64_t)next > (unsigned)-1) return -75;
        p->files[index].position->offset = (unsigned)next;
        return next;
    }
    if (op == 63 || op == 67) {
        if (!file) return -9;
        if (file == &app_dir || file == &proc_dir || file == &task_dir) return -21;
        if (file == &urandom_file) return -29;
        uint64_t pos = op == 63 ? p->files[index].position->offset : f->r[3];
        uint64_t size = file->end - file->begin;
        if (pos >= size) return 0;
        unsigned bytes = f->r[2] < size - pos ? f->r[2] : size - pos;
#ifdef QUOTA_LINUX_APP_RUNTIME
        /* Bound a regular-file transfer while IRQs are masked. Linux callers
         * can continue a short read from the returned file position. */
        if (bytes > 4096) bytes = 4096;
#endif
        if (copy_process_bytes(p, f->r[1], (void *)(file->begin + pos), bytes, 1)) return -14;
        if (op == 63) p->files[index].position->offset += bytes;
        return bytes;
    }
    if (op == 233) return runtime_advise(p, f);
    if (op == 222 || op == 215 || op == 226) {
        index = f->r[4] - 3;
        if (op == 222 && !(f->r[3] & 32) && index < WAP_FDS && (p->files[index].file == &urandom_file || p->files[index].file == &app_dir || p->files[index].file == &proc_dir || p->files[index].file == &task_dir)) return -19;
        return runtime_map(p, f, index < WAP_FDS ? p->files[index].file : NULL);
    }
    return -38;
}

/* Entry stack for the fixed-address, freestanding Linux ELF subset. */
static uint64_t linux_bootstrap_stack(uint64_t pa, unsigned entry, const unsigned char *headers, unsigned count, const struct elf_mapping *image)
{
#ifdef QUOTA_LINUX_APP_RUNTIME
    const char name[] = SYSTEM_RUNTIME_PATH;
#else
    const char name[] = "elf-bootstrap";
#endif
    const char *executable_name = name;
    unsigned name_size = sizeof name;
#ifdef QUOTA_NODE_WEB
    if (image && (image->role == WAP_FRONTEND || image->role == WAP_PAGE_ONLY)) {
        executable_name = BLINK_RUNTIME_PATH;
        name_size = sizeof BLINK_RUNTIME_PATH;
    }
#endif
    unsigned name_offset = 4096 - name_size;
    unsigned headers_offset = (name_offset - count * 56u) & ~7u;
    unsigned stack_base = image && image->interp_entry ? ELF_STACK_TOP - 4096 : STACK_VA;
    uint64_t name_va = stack_base + name_offset;
    unsigned args = headers_offset;
#ifdef QUOTA_LINUX_APP_RUNTIME
#ifdef QUOTA_STANDALONE_NODE
    const int backend = image && image->role == WAP_NODE_ONLY;
#else
    const int backend = image && image->role == WAP_BACKEND;
#endif
    const char jit[] = "--jitless", entry_path[] = "/app/app.js";
    unsigned rpc_at = 0, jit_at = 0, entry_at = 0, env_at = 0, uv_at = 0;
    const int frontend = image && (image->role == WAP_FRONTEND || image->role == WAP_PAGE_ONLY);
    if (backend || frontend) {
        static const char uv_pool[] = "UV_THREADPOOL_SIZE=1";
        uv_at = args -= sizeof uv_pool;
        memcpy((void *)(uintptr_t)(pa + uv_at), uv_pool, sizeof uv_pool);
    }
    if (frontend) {
#ifndef QUOTA_NODE_WEB
        static const char preload[] = "LD_PRELOAD=/usr/lib/quota-runtime/libquota-entropy.so";
        env_at = args -= sizeof preload;
        memcpy((void *)(uintptr_t)(pa + env_at), preload, sizeof preload);
#endif
#ifdef QUOTA_NODE_WEB
        const char *display_platform = image->role == WAP_FRONTEND ? "--wap-rpc" : "--wap-page";
        unsigned display_size = image->role == WAP_FRONTEND ? sizeof "--wap-rpc" : sizeof "--wap-page";
#else
        static const char display_platform[] = "--ozone-platform=headless";
        unsigned display_size = sizeof display_platform;
#endif
        entry_at = args -= display_size;
        memcpy((void *)(uintptr_t)(pa + entry_at), display_platform, display_size);
        static const char browser_js[] = "--js-flags=--jitless";
        jit_at = args -= sizeof browser_js;
        memcpy((void *)(uintptr_t)(pa + jit_at), browser_js, sizeof browser_js);

    }
    if (backend) {
#ifndef QUOTA_STANDALONE_NODE
        static const char rpc_preload[] = "--require=/usr/lib/quota-runtime/preload.cjs";
        rpc_at = args -= sizeof rpc_preload;
        memcpy((void *)(uintptr_t)(pa + rpc_at), rpc_preload, sizeof rpc_preload);
#endif
#if !defined(QUOTA_STANDALONE_NODE) && !defined(QUOTA_NODE_WEB)
        static const char env[] = "ELECTRON_RUN_AS_NODE=1";
        env_at = args -= sizeof env;
        memcpy((void *)(uintptr_t)(pa + env_at), env, sizeof env);
#endif
        jit_at = args -= sizeof jit;
        entry_at = args -= sizeof entry_path;
        memcpy((void *)(uintptr_t)(pa + jit_at), jit, sizeof jit);
        memcpy((void *)(uintptr_t)(pa + entry_at), entry_path, sizeof entry_path);
    }
#endif
    unsigned seed_offset = args - 16;
    unsigned char *page = (unsigned char *)(uintptr_t)pa;
    int seed_bytes = image && image->interp_entry ? rng_read(page + seed_offset, 16) : 0;
    const uint64_t aux[] = {
        23, 0,               /* AT_SECURE: the immutable executable has no set-ID transition. */
        6, 4096,             /* AT_PAGESZ */
        3, image && image->phdr_va ? image->phdr_va : stack_base + headers_offset, /* AT_PHDR */
        4, 56,               /* AT_PHENT */
        5, count,            /* AT_PHNUM */
        7, image && image->interp_entry ? ELF_INTERP_BASE : 0, /* AT_BASE */
        9, entry,            /* AT_ENTRY */
        seed_bytes == 16 ? 25u : 1u, seed_bytes == 16 ? stack_base + seed_offset : 0, /* AT_RANDOM or AT_IGNORE */
        31, name_va,         /* AT_EXECFN */
        0, 0                 /* AT_NULL */
    };
    uint64_t words[9 + sizeof aux / sizeof aux[0]];
    unsigned n = 0;
    words[n++] = 1;
    words[n++] = name_va;
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (frontend) {
        words[0] = 3;
        words[n++] = stack_base + jit_at;
        words[n++] = stack_base + entry_at;
    } else if (backend) {
        words[0] = rpc_at ? 4 : 3;
        if (rpc_at) words[n++] = stack_base + rpc_at;
        words[n++] = stack_base + jit_at;
        words[n++] = stack_base + entry_at;
    }
#endif
    words[n++] = 0;
#ifdef QUOTA_LINUX_APP_RUNTIME
    if (backend || frontend) {
        if (env_at) words[n++] = stack_base + env_at;
        words[n++] = stack_base + uv_at;
    }
#endif
    words[n++] = 0;
    memcpy(words+n, aux, sizeof aux);
    unsigned bytes = n * sizeof words[0] + sizeof aux;
    unsigned stack_offset = (seed_offset - bytes) & ~15u;
    memcpy(page + name_offset, executable_name, name_size);
    memcpy(page + headers_offset, headers, count * 56u);
    memcpy(page + stack_offset, words, bytes);
    return stack_base + stack_offset;
}

static uint64_t elf_number(const unsigned char *, unsigned);

extern const unsigned char wap_worker_start[] __attribute__((weak));

/* Only complete immutable pages of the system runtime can be borrowed. */
static unsigned shared_image_pages(const unsigned char *file, unsigned flags,
                                   uint64_t off, uint64_t va, uint64_t len)
{
    int immutable = file == wap_worker_start || file == wap_interp_start;
#ifdef QUOTA_NODE_WEB
    if (!immutable && !(flags & 2)) {
        const struct runtime_file *renderer = blink_executable();
        immutable = renderer && file == renderer->begin;
    }
#endif
    if (!immutable || (flags & 2) ||
        (((uintptr_t)file + off) & 4095) != (va & 4095)) return 0;
    unsigned prefix = (-va) & 4095;
    return len > prefix ? (len - prefix) / 4096 : 0;
}

static void map_image_range(struct proc *p, const unsigned char *file, unsigned bias,
                      const unsigned char *headers, unsigned count, unsigned low, unsigned high, unsigned *frames)
{
    uint64_t *table = image_pt[p->slot];
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *ph = headers + i * 56;
        if (elf_number(ph, 4) != 1) continue;
        unsigned flags = elf_number(ph + 4, 4);
        unsigned offset = bias + elf_number(ph + 16, 8) - ELF_BASE;
        unsigned file_size = elf_number(ph + 32, 8);
        unsigned end = offset + elf_number(ph + 40, 8);
        unsigned first = offset & ~4095u;
        unsigned limit = (end + 4095u) & ~4095u;
        unsigned file_end = offset + file_size;
        unsigned shared_start = (offset + 4095) & ~4095u;
        unsigned off = elf_number(ph + 8, 8);
        unsigned shared_end = shared_start + 4096 * shared_image_pages(file, flags, off, offset, file_size);
        if (first < low) first = low;
        if (limit > high) limit = high;
        for (unsigned at = first; at < limit; at += 4096) {
            if (at >= shared_start && at < shared_end) {
                uintptr_t pa = (uintptr_t)file + off + at - offset;
                table[at >> 12] = flags & 1 ? code_pte(pa) | PTE_PXN : data_pte(pa) | AP_RO;
                continue;
            }
            uint8_t *frame = pool[image_frames[p->slot][(*frames)++]];
            unsigned lo = at > offset ? at : offset;
            unsigned hi = at + 4096 < file_end ? at + 4096 : file_end;
            memset(frame, 0, 4096);
            if (hi > lo)
                memcpy(frame + (lo - at), file + elf_number(ph + 8, 8) + (lo - offset), hi - lo);
            uint64_t pa = (uintptr_t)frame;
            table[at >> 12] = flags & 1 ? code_pte(pa) | PTE_PXN :
                              data_pte(pa) | ((flags & 2) ? ELF_PRIVATE_WRITABLE : AP_RO);
            if (flags & 1) clean_code(frame, 4096);
        }
    }
}

static void map_image(struct proc *p, const unsigned char *file, unsigned bias,
                      const unsigned char *headers, unsigned count)
{
    map_image_range(p, file, bias, headers, count, 0, ELF_WINDOW, &p->image_pages);
    uint64_t *table = image_pt[p->slot];
    for (unsigned i = 0; i < (ELF_WINDOW >> 21); ++i)
        pt_l2[p->slot][(ELF_BASE >> 21) + i] = (uintptr_t)(table + i * 512) | PTE_VALID | PTE_PAGE;
    asm volatile("dsb ishst" ::: "memory");
}

#ifdef QUOTA_LINUX_APP_RUNTIME
/* Only the immutable system image that passed spawn_elf validation is executable. */
static struct elf_mapping system_layout;
static int64_t exec_system_runtime(struct proc *p, struct irq_frame *f)
{
    if (!p->exec_cursor && runtime_lookup(p, f->r[0]) != &system_executable) return -2;
    if (!system_layout.interp_entry || p->live_threads != 1 || p->fork_child ||
        p->mapping.va || !p->parent_handle) return -16;
    if (p->image_pages != system_layout.pages || p->dso_frame + ELF_DSO_PAGES != p->image_pages)
        return -8;
    const unsigned char *file = system_layout.file;
    unsigned count = elf_number(file + 56, 2);
    unsigned entry = system_layout.load_bias + elf_number(file + 24, 8);
    const unsigned char *headers = file + elf_number(file + 32, 8);
    static unsigned char staging[EL0_SLOTS][4096] __attribute__((aligned(16)));
    unsigned char *page = staging[p->slot];
    const unsigned base = ELF_STACK_TOP - 4096;
    if (p->exec_cursor) goto remap;
    memset(page, 0, 4096);
    unsigned boot = linux_bootstrap_stack((uintptr_t)page, entry, headers, count, &system_layout) - base;
    unsigned top = boot;
    /* WAP_SINGLE bootstrap stack is argc, argv[0], NULL, envp NULL, then auxv. */
    uint64_t words[96] = {0};
    unsigned n = 1;
    for (unsigned vector = 1; vector <= 2; ++vector) {
        uint64_t array = f->r[vector];
        for (unsigned j = 0; array; ++j) {
            uint64_t addr;
            if (copy_process_bytes(p, array + 8ull*j, &addr, sizeof addr, 0)) return -14;
            if (!addr) break;
            if (n >= 74) return -7;
            unsigned length = 0;
            char ch;
            do {
                if (++length > top) return -7;
                if (copy_process_bytes(p, addr + length - 1, &ch, 1, 0)) return -14;
            } while (ch);
            top -= length;
            if (copy_process_bytes(p, addr, page + top, length, 0)) return -14;
            words[n++] = base + top;
        }
        if (vector == 1) words[0] = n-1;
        words[n++] = 0;
    }
    /* Use the auxv from staging, whose random seed remains at the referenced VA. */
    const uint64_t *aux = (const uint64_t *)(page + boot) + 4;
    for (;;) {
        if (n + 2 > sizeof words / sizeof words[0]) return -7;
        words[n++] = aux[0];
        words[n++] = aux[1];
        if (!aux[0]) break;
        aux += 2;
    }
    unsigned bytes = n * sizeof(uint64_t);
    if (top < bytes) return -7;
    unsigned sp = (top - bytes) & ~15u;
    memcpy(page + sp, words, bytes);
    p->exec_dso = 0;
    p->exec_high = HIGH_START;
    p->exec_stack = sp;
    p->exec_frames = 0;
    p->exec_cursor = 1;
    /* The old caller text is replaced below. Resume only this syscall from an
       immutable EL0 SVC instruction between bounded mapping slices. */
    *(uint32_t *)(uintptr_t)p->pa[0] = 0xd4000001;
    clean_code((void *)(uintptr_t)p->pa[0], 4);
remap:;
    uint64_t slice = ticks_now();
    f->elr = CODE_VA;
    while (p->high_frames && p->exec_high < HIGH_LIMIT) {
        uint64_t va = p->exec_high;
        uint64_t *table = pt_l0[p->slot];
        for (int shift = 39; shift >= 12; shift -= 9) {
            uint64_t leaf = table[(va >> shift) & 511];
            if (!leaf) {
                p->exec_high = ((va >> shift) + 1) << shift;
                break;
            }
            if (shift == 12) {
                release_high(p, va, va + 4096);
                p->exec_high += 4096;
                break;
            }
            table = (void *)(uintptr_t)FRAME_ADDR(leaf);
        }
        if (ticks_now() - slice >= us_to_ticks(32)) return 0;
    }
    while (p->exec_dso < ELF_DSO_PAGES) {
        unsigned d = p->exec_dso++;
        int *frame = &image_frames[p->slot][p->dso_frame + d];
        free_idx(*frame);
        *frame = -1;
        shm_account(p, p->dso_source[d], 0);
        p->dso_source[d] = 0;
        if (!(d & 63)) p->dso_used[d/64] = p->dso_anonymous[d/64] = 0;
        if (ticks_now() - slice >= us_to_ticks(32)) return 0;
    }
next_range:;
    unsigned first = (p->exec_cursor - 1) * 4096u;
    unsigned last = first + 4*4096;
    memset(&image_pt[p->slot][first >> 12], 0, (last-first) >> 9);
    map_image_range(p, file, system_layout.load_bias, headers, count, first, last, &p->exec_frames);
    map_image_range(p, wap_interp_start, ELF_INTERP_BASE, wap_interp_start + system_layout.interp_phoff,
                    system_layout.interp_count, first, last, &p->exec_frames);
    for (unsigned at = first; at < last; at += 4096) {
        unsigned va = ELF_BASE + at;
        if (va < ELF_STACK_TOP - ELF_STACK_PAGES*4096 || va >= ELF_BRK_BASE + ELF_BRK_PAGES*4096) continue;
        uint64_t pa = p->pa[1];
        if (va == base) memcpy((void *)(uintptr_t)pa, page, 4096);
        else {
            pa = (uintptr_t)pool[image_frames[p->slot][p->exec_frames++]];
            memset((void *)(uintptr_t)pa, 0, 4096);
        }
        image_pt[p->slot][at >> 12] = data_pte(pa);
    }
    if (last < ELF_WINDOW) {
        p->exec_cursor += 4;
#ifdef QUOTA_WAP_LOADER_TRACE
        if (!(last & 0xffffffu)) printf("[wap] exec mapping offset=%x\n", last);
#endif
        if (ticks_now() - slice < us_to_ticks(32)) goto next_range;
        f->elr = CODE_VA;
        return 0;
    }
    memset(p->reservations, 0, sizeof p->reservations);
    p->exec_cursor = 0;
    sp = p->exec_stack;
    p->mapped_dso_pages = 0;
    p->linux_brk = ELF_BRK_BASE;
    p->clear_child_tid = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (p->signal_actions[i].handler != 1) memset(&p->signal_actions[i], 0, sizeof p->signal_actions[i]);
    for (unsigned i = 0; i < WAP_FDS; ++i) {
        if (!p->files[i].cloexec) continue;
        if (p->files[i].position) --p->files[i].position->refs;
        if (p->files[i].poll) --p->files[i].poll->refs;
        if (p->files[i].socket) packet_put(p->files[i].socket);
        pipe_put(p->files[i].event_flags, p->files[i].pipe_id);
        memset(&p->files[i], 0, sizeof p->files[i]);
    }
    unsigned nnp = p->threads[p->active_thread].no_new_privs;
    memset(p->threads, 0, sizeof p->threads);
    p->active_thread = 0;
    p->threads[0].tid = p->pid_namespace == (unsigned)p->pid ? 1 : p->pid;
    p->threads[0].no_new_privs = nnp;
    wap_fp_restore(p->threads[0].fp);
    asm volatile("msr tpidr_el0,xzr");
    tlb_asid(p->asid);
    memset(f->r, 0, sizeof f->r);
    f->spsr = 0;
    f->elr = system_layout.interp_entry;
    f->pad = base + sp;
#ifdef QUOTA_WAP_LOADER_TRACE
    p->exec_resumes = 8;
    p->mapping_traces = p->linux_traces = 0;
#endif
    printf("[wap] exec electron pid=%d argc=%lu entry=%lx sp=%lx\n", p->pid, *(uint64_t *)(page + sp), f->elr, f->pad);
    return 0;
}
#endif

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
        if (image->role == WAP_BACKEND || image->role == WAP_NODE_ONLY) {
            p->private_page_limit -= 4096;
            man.mem_min_kb -= 4096 * 4;
        }
        if (image->role == WAP_FRONTEND || image->role == WAP_PAGE_ONLY) {
#ifdef QUOTA_NODE_WEB
            /* Blink's component build consumes over 52 MiB of private DSO
             * pages before Oilpan creates its first normal allocation page. */
            const unsigned extra_pages = 12288; /* 96 MiB total private cap */
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
#ifdef QUOTA_NODE_WEB
    p->page_executable = image && (image->role == WAP_FRONTEND || image->role == WAP_PAGE_ONLY)
                         ? blink_executable() : NULL;
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
    p->image_pages = 0;
    build_pt(p);
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
        map_image(p, image->file, image->load_bias, headers, header_count);
        if (image->interp_entry) {
            map_image(p, wap_interp_start, ELF_INTERP_BASE,
                      wap_interp_start + image->interp_phoff, image->interp_count);
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

/* The existing single RX page process layout supports this ELF subset.
 * Linux runtime executables need a separate mapper and process ABI. */
static uint64_t elf_number(const unsigned char *bytes, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static int prepare_interpreter(struct elf_mapping *image)
{
    const unsigned char *file = wap_interp_start;
    uintptr_t start = (uintptr_t)file, end = (uintptr_t)wap_interp_end;
    if (!start || end < start || end - start < 64) return -1;
    size_t bytes = end - start;
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
    image->interp_entry = ELF_INTERP_BASE + entry;
    image->interp_phoff = off;
    image->interp_count = count;
    return 0;
}

static int64_t spawn_elf(const unsigned char *elf, unsigned bytes, const unsigned char *document, unsigned size,
                         const unsigned char *backend, unsigned backend_size, enum wap_role role)
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
    if (entry >= ELF_BASE && entry < ELF_BASE + ELF_WINDOW) {
        if (size > sizeof procs[0].package_contents[0] || backend_size > sizeof procs[0].package_contents[1] || (!backend && backend_size)) return -1;
        struct elf_mapping image = { .role = role, .file = elf, .load_bias = bias, .backend = backend, .backend_size = backend_size };
        uint64_t next_page = ELF_BASE;
        int executable_entry = 0, interpreter = 0;
        /* WAP roles open private package files; the legacy tile ABI shares
         * document storage with pixel rows in the same page. */
        if ((entry & 3) || (role == WAP_SINGLE && size > GUEST_TILE_OFFSET)) return -1;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned char *ph = elf + phoff + 56 * i;
            unsigned type = elf_number(ph, 4), flags = elf_number(ph + 4, 4);
            if (type == 3) {
                static const char name[] = "/lib/ld-linux-aarch64.so.1";
                uint64_t pos = elf_number(ph + 8, 8);
                if (interpreter || pos > bytes || sizeof name > bytes - pos ||
                    elf_number(ph + 32, 8) != sizeof name ||
                    elf[pos + sizeof name - 1] || strcmp((const char *)elf + pos, name)) return -1;
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
            image.pages += ((next_page - (va & ~4095ull)) >> 12) - shared_image_pages(elf, flags, off, va, len);
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
        return spawn_image(&image, NULL, 0, document, size, elf + phoff, count, entry,
                           document ? CONSOLE_PAGE : CONSOLE_LINUX_BOOTSTRAP, (role == WAP_BACKEND || role == WAP_NODE_ONLY) ? "wap-backend" : "elf-page",
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
    }
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

int64_t proc_spawn_bootstrap_elf(const unsigned char *elf, unsigned bytes)
{
    return spawn_elf(elf, bytes, NULL, 0, NULL, 0, WAP_SINGLE);
}

int64_t proc_spawn_page_elf(const unsigned char *elf, unsigned bytes,
                            const unsigned char *document, unsigned size)
{
    return document ? spawn_elf(elf, bytes, document, size, NULL, 0, WAP_SINGLE) : -1;
}

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
    free_frames = EL0_POOL;
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
