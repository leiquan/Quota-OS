#include "root_disk.h"
#include "auth.h"
/* Dedicated virtio-blk note volume on QEMU virt, modern MMIO transport.
 * Accessed only by the Tier-3 desktop. One request per period, one in flight.
 * Alternate two records so a torn update leaves the prior version readable. */
#include "quotaos.h"

enum { BLOCK = 512, MAGIC = 0x514e5431, FLUSH_FEATURE = 1u << 9, SOURCE_MAGIC = 0x514e5432, SOURCE_NAME = 32, PACKAGE_MAGIC = 0x51415031, NOTE_DATA_MAGIC = 0x514e4431, NOTE_DATA_SLOT = 3, PACKAGE_EXT = 4, HELLO_SLOT = 5, DOCUMENT_COUNT = 6, LEGACY_BYTES = BLOCK-16, PACKAGE_BANK_BYTES = 4*BLOCK };
struct record {
    uint32_t magic, generation, length, checksum;
    char text[NOTE_DISK_PACKAGE_CAPACITY];
};
_Static_assert(sizeof(struct record) == PACKAGE_BANK_BYTES, "package bank");
struct descriptor { uint64_t addr; uint32_t length; uint16_t flags, next; };
static struct descriptor desc[8] __attribute__((aligned(16)));
static struct { uint16_t flags, idx, ring[8], event; } avail __attribute__((aligned(4)));
static volatile struct { uint16_t flags, idx; struct {uint32_t id, len;} ring[8]; uint16_t event; } used __attribute__((aligned(4)));
static struct { uint32_t type, reserved; uint64_t sector; } header;
static volatile uint8_t status;
static char serial[21];
static struct record transfer, documents[DOCUMENT_COUNT];
static unsigned document, document_count = PACKAGE_EXT;
#define saved (documents[document])
static uint64_t base, submitted, last_period;
static unsigned submitted_once, inflight;
static uint16_t completed;
static int slots[DOCUMENT_COUNT];
#define slot (slots[document])
enum phase { OFFLINE, IDENTIFY, AUTH_READ, AUTH_WRITE, AUTH_FLUSH, AUTH_CHECK, READ_A, READ_B, READY, WRITE, FLUSH, VERIFY, SCRUB, FAILED };
static enum phase phase;
static uint8_t credential_sector[512];
int auth_disk_store(const uint8_t data[512])
{
    if (phase != READY || inflight) return -1;
    memcpy(credential_sector, data, BLOCK);
    phase = AUTH_WRITE;
    return 0;
}
static int verify_result;
static unsigned scrub_enabled, scrub_sector, scrub_submitted, scrub_completed;

int note_disk_scrub_start(void)
{
    if (phase != READY || inflight || scrub_enabled || sched_current()->tier != TIER3)
        return -1;
    scrub_enabled = 1;
    scrub_sector = scrub_submitted = scrub_completed = 0;
    return 0;
}

unsigned note_disk_scrub_stop(unsigned *completed_requests)
{
    scrub_enabled = 0;
    *completed_requests = scrub_completed;
    return scrub_submitted;
}
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
        if (nodes[i] == ROOT_RW_MMIO) continue;
        if (mmio_rd(nodes[i]+8) == 2 && mmio_rd(nodes[i]+4) == 2) {
#ifdef QUOTA_RUNTIME_VOLUME_BOOT
            /* The read-only runtime disk has its own Tier 3 driver. */
            mmio_wr(nodes[i] + 0x14, 0);
            if (mmio_rd(nodes[i] + 0x10) & (1u << 5))
                continue;
#endif
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
    if (mmio_rd(base + 0x104) || mmio_rd(base + 0x100) >= 16)
        document_count = (mmio_rd(base + 0x104) || mmio_rd(base + 0x100) >= 18) ? DOCUMENT_COUNT : HELLO_SLOT;
    phase = IDENTIFY;
}

static uint32_t checksum(const struct record *r)
{
    uint32_t hash = (2166136261u ^ r->generation) * 16777619u;
    for (unsigned i = 0; i < r->length; ++i)
        hash = (hash ^ (uint8_t)r->text[i]) * 16777619u;
    if (r->magic == SOURCE_MAGIC)
        for (unsigned i = LEGACY_BYTES - SOURCE_NAME; i < LEGACY_BYTES; i++)
            hash = (hash ^ (uint8_t)r->text[i]) * 16777619u;
    return hash;
}

static int valid_source_name(const char *name)
{
    unsigned n = 0;
    for (; n < SOURCE_NAME && name[n]; n++) {
        char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return n > 2 && n < SOURCE_NAME && name[0] != '.' &&
           name[n - 2] == '.' && name[n - 1] == 'c';
}

static void submit(void)
{
    int flush = phase == FLUSH || phase == AUTH_FLUSH;
    int writing = phase == WRITE || phase == AUTH_WRITE;
    int credential = phase >= AUTH_READ && phase <= AUTH_CHECK;
    header.type = phase == IDENTIFY ? 8 : writing ? 1 : flush ? 4 : 0;
    header.sector = phase == READ_B || (phase == WRITE && !slot);
    if (phase == VERIFY) header.sector = slot;
    if (document == PACKAGE_EXT) header.sector = 8 + 4 * header.sector;
    else if (document == HELLO_SLOT) header.sector += 16;
    else header.sector += document * 2;
    if (phase == IDENTIFY || flush) header.sector = 0;
    if (phase == SCRUB) header.sector = scrub_sector;
    if (credential && !flush) header.sector = 63;
    status = 255;
    desc[0] = (struct descriptor){(uintptr_t)&header, sizeof header, 1, flush ? 2 : 1};
    desc[1] = (struct descriptor){phase == IDENTIFY ? (uintptr_t)serial : credential ? (uintptr_t)credential_sector : (uintptr_t)&transfer,
        phase == IDENTIFY ? 20 : credential ? BLOCK : document == PACKAGE_EXT ? PACKAGE_BANK_BYTES : BLOCK, writing ? 1 : 3, 2};
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
    if (sched_current()->tier != TIER3 || phase == OFFLINE || phase == FAILED ||
        (phase == READY && !scrub_enabled))
        return;
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
            phase = !strcmp(serial, "QUOTA-NOTES") ? AUTH_READ : OFFLINE;
        } else if (phase == AUTH_READ) {
            phase = READ_A;
        } else if (phase == AUTH_WRITE) phase = AUTH_FLUSH;
        else if (phase == AUTH_FLUSH) phase = AUTH_CHECK;
        else if (phase == AUTH_CHECK) {
            auth_disk_verified(credential_sector);
            phase = READY;
        } else if (phase == READ_A || phase == READ_B) {
            int valid = document == NOTE_DATA_SLOT
                ? transfer.magic == NOTE_DATA_MAGIC && transfer.length <= LEGACY_BYTES
                : (document == 2 || document == PACKAGE_EXT)
                ? transfer.magic == PACKAGE_MAGIC && transfer.length && transfer.length <= (document == PACKAGE_EXT ? sizeof transfer.text : LEGACY_BYTES)
                : (transfer.magic == MAGIC || (document == 1 && transfer.magic == SOURCE_MAGIC &&
                    transfer.length < LEGACY_BYTES - SOURCE_NAME &&
                    valid_source_name(transfer.text + LEGACY_BYTES - SOURCE_NAME))) &&
                    transfer.length < LEGACY_BYTES && !transfer.text[transfer.length];
            if (valid && transfer.checksum == checksum(&transfer) &&
                (!saved.magic || (int32_t)(transfer.generation-saved.generation)>0)) {
                saved = transfer;
                slot = phase == READ_B;
            }
            phase = phase == READ_A ? READ_B : READY;
            if (phase == READY && document + 1 < document_count) {
                document++;
                phase = READ_A;
            } else if (phase == READY) {
                document = 0;
                auth_disk_loaded(credential_sector);
                printf("[files] mounted bytes=%u checksum=%u\n", saved.length, saved.checksum);
            }
        } else if (phase == SCRUB) {
            scrub_completed++;
            scrub_sector = (scrub_sector + 1) % 4;
            phase = READY;
        } else if (phase == VERIFY) {
            verify_result = transfer.magic == MAGIC && transfer.generation == saved.generation &&
                transfer.length == saved.length && transfer.checksum == saved.checksum &&
                checksum(&transfer) == saved.checksum && !transfer.text[transfer.length] ? 1 : -1;
            phase = READY;
        } else if (phase == WRITE) phase = FLUSH;
        else if (phase == FLUSH) {
            saved = transfer;
            slot ^= 1;
            document = 0;
            phase = READY;
        }
        return;
    }
    uint64_t period = sched_period_now();
    if (submitted_once && period == last_period) return;
    last_period = period;
    submitted_once = 1;
    if (phase == READY) {
        /* The Notes sectors are read-only here. Each request scans one sector. */
        phase = SCRUB;
        scrub_submitted++;
    }
    submit();
}

static int save_document(const char *text, unsigned index, const char *name)
{
    if (phase != READY || sched_current()->tier != TIER3) return -1;
    if (name && !valid_source_name(name)) return -3;
    unsigned limit = LEGACY_BYTES - (name ? SOURCE_NAME : 0);
    unsigned n = 0;
    while (n < limit && text[n]) n++;
    if (n == limit) return -2;
    document = index;
    memset(&transfer, 0, sizeof transfer);
    transfer.magic = name ? SOURCE_MAGIC : MAGIC;
    transfer.generation = saved.generation+1;
    transfer.length = n;
    memcpy(transfer.text, text, n);
    if (name) str_copy(transfer.text + limit, name, SOURCE_NAME);
    transfer.checksum = checksum(&transfer);
    phase = WRITE;
    return 0;
}

static int save_binary(unsigned index, uint32_t magic, const unsigned char *bytes, unsigned length)
{
    if (phase != READY || sched_current()->tier != TIER3 || !bytes ||
        length > (index == PACKAGE_EXT ? sizeof transfer.text : LEGACY_BYTES))
        return -1;
    document = index;
    memset(&transfer, 0, sizeof transfer);
    transfer.magic = magic;
    transfer.generation = saved.generation + 1;
    transfer.length = length;
    memcpy(transfer.text, bytes, length);
    transfer.checksum = checksum(&transfer);
    phase = WRITE;
    return 0;
}

int note_disk_save_package(const unsigned char *bytes, unsigned length)
{
    return length ? save_binary(document_count > PACKAGE_EXT ? PACKAGE_EXT : 2, PACKAGE_MAGIC, bytes, length) : -1;
}

int note_disk_save_wap_note(const unsigned char *bytes, unsigned length, uint32_t *generation)
{
    if (!generation || save_binary(NOTE_DATA_SLOT, NOTE_DATA_MAGIC, bytes, length))
        return -1;
    *generation = transfer.generation;
    return 0;
}

/* A write is acknowledged only once virtio-blk completed its flush. */
int note_disk_wap_note_committed(uint32_t generation)
{
    const struct record *record = &documents[NOTE_DATA_SLOT];
    if (record->magic == NOTE_DATA_MAGIC && record->generation == generation)
        return 1;
    if (document == NOTE_DATA_SLOT && (phase == WRITE || phase == FLUSH) &&
        transfer.generation == generation)
        return 0;
    return -1;
}

const unsigned char *note_disk_wap_note(unsigned *length)
{
    *length = documents[NOTE_DATA_SLOT].length;
    return documents[NOTE_DATA_SLOT].magic ? (const unsigned char *)documents[NOTE_DATA_SLOT].text : NULL;
}

const unsigned char *note_disk_package(unsigned *length)
{
    const struct record *r = documents[PACKAGE_EXT].magic ? &documents[PACKAGE_EXT] : &documents[2];
    *length = r->length;
    return r->magic ? (const unsigned char *)r->text : NULL;
}

int note_disk_save(const char *text) { return save_document(text, 0, NULL); }
int note_disk_save_source(const char *text, const char *name) { return save_document(text, 1, name); }
const char *note_disk_source_name(void)
{
    const struct record *source = &documents[1];
    if (!source->magic) return NULL;
    return source->magic == SOURCE_MAGIC ? source->text + LEGACY_BYTES - SOURCE_NAME : "hello.c";
}
const char *note_disk_source(void) { return documents[1].magic ? documents[1].text : 0; }

unsigned note_disk_bytes(void) { return documents[0].length; }

int note_disk_verify(void)
{
    if (phase != READY || !documents[0].magic) return -1;
    verify_result = 0;
    phase = VERIFY;
    return 0;
}

int note_disk_verify_result(void) { return phase == FAILED ? -1 : verify_result; }

const char *note_disk_text(void) { return documents[0].magic ? documents[0].text : 0; }
const char *note_disk_status(void)
{
    if (phase == READY || phase == SCRUB) return documents[0].magic || documents[1].magic || documents[2].magic || documents[PACKAGE_EXT].magic ? "Saved" : "Ready";
    if (phase == WRITE || phase == FLUSH) return "Saving...";
    if (phase == FAILED) return "Disk error";
    if (phase == VERIFY) return "Verifying...";
    if (phase == OFFLINE) return "No Notes disk";
    return "Opening disk...";
}

int note_disk_power_ready(void)
{
    if (phase == FAILED) return -1;
    return (phase == READY && !scrub_enabled) || phase == OFFLINE;
}

const char *note_disk_hello(void) { return documents[HELLO_SLOT].magic ? documents[HELLO_SLOT].text : NULL; }
int note_disk_save_hello(const char *text) { return document_count > HELLO_SLOT ? save_document(text, HELLO_SLOT, NULL) : -1; }
