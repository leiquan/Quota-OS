/* Dedicated virtio-blk note volume on QEMU virt, modern MMIO transport.
 * Accessed only by the Tier-3 desktop. One request per period, one in flight.
 * Alternate two records so a torn update leaves the prior version readable. */
#include "quotaos.h"

enum { BLOCK = 512, MAGIC = 0x514e5431, FLUSH_FEATURE = 1u << 9 };
struct record {
    uint32_t magic, generation, length, checksum;
    char text[BLOCK - 16];
};
_Static_assert(sizeof(struct record) == BLOCK, "note sector");
struct descriptor { uint64_t addr; uint32_t length; uint16_t flags, next; };
static struct descriptor desc[8] __attribute__((aligned(16)));
static struct { uint16_t flags, idx, ring[8], event; } avail __attribute__((aligned(4)));
static volatile struct { uint16_t flags, idx; struct {uint32_t id, len;} ring[8]; uint16_t event; } used __attribute__((aligned(4)));
static struct { uint32_t type, reserved; uint64_t sector; } header;
static volatile uint8_t status;
static char serial[21];
static struct record transfer, saved;
static uint64_t base, submitted, last_period;
static unsigned submitted_once, inflight;
static uint16_t completed;
static int slot;
enum phase { OFFLINE, IDENTIFY, READ_A, READ_B, READY, WRITE, FLUSH, VERIFY, FAILED };
static enum phase phase;
static int verify_result;
static void address(uint32_t offset, uintptr_t addr)
{
    mmio_wr(base + offset, (uint32_t)addr);
    mmio_wr(base + offset + 4, addr >> 32);
}

void note_disk_init(void)
{
    uint64_t nodes[32];
    int n = dtb_find_bases("virtio,mmio", nodes, 32);
    for (int i = 0; i < n; ++i) {
        if (mmio_rd(nodes[i]+8) == 2 && mmio_rd(nodes[i]+4) == 2) {
            base = nodes[i];
            break;
        }
    }
    if (!base) return;
    mmio_wr(base+0x70, 0);
    mmio_wr(base+0x70, 3);
    mmio_wr(base+0x14, 0);
    uint32_t features = mmio_rd(base+0x10);
    if (!(features & FLUSH_FEATURE) || (features & (1u << 5))) return;
    mmio_wr(base+0x24, 0);
    mmio_wr(base+0x20, FLUSH_FEATURE);
    mmio_wr(base+0x14, 1);
    if (!(mmio_rd(base+0x10) & 1)) return;
    mmio_wr(base+0x24, 1);
    mmio_wr(base+0x20, 1);
    mmio_wr(base+0x70, 11);
    if (!(mmio_rd(base+0x70) & 8)) return;
    mmio_wr(base+0x30, 0);
    if (mmio_rd(base+0x34) < 8) return;
    mmio_wr(base+0x38, 8);
    address(0x80, (uintptr_t)desc);
    address(0x90, (uintptr_t)&avail);
    address(0xa0, (uintptr_t)&used);
    avail.flags = 1;
    mmio_wr(base+0x44, 1);
    mmio_wr(base+0x70, 15);
    phase = IDENTIFY;
}

static uint32_t checksum(const struct record *r)
{
    uint32_t hash = (2166136261u ^ r->generation) * 16777619u;
    for (unsigned i = 0; i < r->length; ++i)
        hash = (hash ^ (uint8_t)r->text[i]) * 16777619u;
    return hash;
}

static void submit(void)
{
    int flush = phase == FLUSH;
    header.type = phase == IDENTIFY ? 8 : phase == WRITE ? 1 : flush ? 4 : 0;
    header.sector = phase == READ_B || (phase == WRITE && !slot);
    if (phase == VERIFY) header.sector = slot;
    if (phase == IDENTIFY || flush) header.sector = 0;
    status = 255;
    desc[0] = (struct descriptor){(uintptr_t)&header, sizeof header, 1, flush ? 2 : 1};
    desc[1] = (struct descriptor){phase == IDENTIFY ? (uintptr_t)serial : (uintptr_t)&transfer,
        phase == IDENTIFY ? 20 : BLOCK, phase == WRITE ? 1 : 3, 2};
    desc[2] = (struct descriptor){(uintptr_t)&status, 1, 2, 0};
    avail.ring[avail.idx % 8] = 0;
    asm volatile("dmb oshst" ::: "memory");
    avail.idx++;
    asm volatile("dsb sy" ::: "memory");
    mmio_wr(base+0x50, 0);
    submitted = now_us();
    inflight = 1;
}

void note_disk_poll(void)
{
    if (phase == OFFLINE || phase == READY || phase == FAILED) return;
    if (inflight) {
        if (used.idx == completed) {
            if (now_us()-submitted > 30000000) phase = FAILED;
            return;
        }
        asm volatile("dmb oshld" ::: "memory");
        completed++;
        inflight = 0;
        if (status) { phase = FAILED; return; }
        if (phase == IDENTIFY) {
            phase = !strcmp(serial, "QUOTA-NOTES") ? READ_A : OFFLINE;
        } else if (phase == READ_A || phase == READ_B) {
            if (transfer.magic == MAGIC && transfer.length < sizeof transfer.text &&
                !transfer.text[transfer.length] && transfer.checksum == checksum(&transfer) &&
                (!saved.magic || (int32_t)(transfer.generation-saved.generation)>0)) {
                saved = transfer;
                slot = phase == READ_B;
            }
            phase = phase == READ_A ? READ_B : READY;
            if (phase == READY) printf("[files] mounted bytes=%u checksum=%u\n", saved.length, saved.checksum);
        } else if (phase == VERIFY) {
            verify_result = transfer.magic == MAGIC && transfer.generation == saved.generation &&
                transfer.length == saved.length && transfer.checksum == saved.checksum &&
                checksum(&transfer) == saved.checksum && !transfer.text[transfer.length] ? 1 : -1;
            phase = READY;
        } else if (phase == WRITE) phase = FLUSH;
        else if (phase == FLUSH) {
            saved = transfer;
            slot ^= 1;
            phase = READY;
        }
        return;
    }
    uint64_t period = sched_period_now();
    if (submitted_once && period == last_period) return;
    last_period = period;
    submitted_once = 1;
    submit();
}

int note_disk_save(const char *text)
{
    if (phase != READY || sched_current()->tier != TIER3) return -1;
    unsigned n = 0;
    while (n < sizeof transfer.text && text[n]) n++;
    if (n == sizeof transfer.text) return -2;
    memset(&transfer, 0, sizeof transfer);
    transfer.magic = MAGIC;
    transfer.generation = saved.generation+1;
    transfer.length = n;
    memcpy(transfer.text, text, n);
    transfer.checksum = checksum(&transfer);
    phase = WRITE;
    return 0;
}

unsigned note_disk_bytes(void) { return saved.length; }

int note_disk_verify(void)
{
    if (phase != READY || !saved.magic) return -1;
    verify_result = 0;
    phase = VERIFY;
    return 0;
}

int note_disk_verify_result(void) { return phase == FAILED ? -1 : verify_result; }

const char *note_disk_text(void) { return saved.magic ? saved.text : 0; }
const char *note_disk_status(void)
{
    if (phase == READY) return saved.magic ? "Saved" : "Ready";
    if (phase == WRITE || phase == FLUSH) return "Saving...";
    if (phase == FAILED) return "Disk error";
    if (phase == VERIFY) return "Verifying...";
    if (phase == OFFLINE) return "No Notes disk";
    return "Opening disk...";
}
