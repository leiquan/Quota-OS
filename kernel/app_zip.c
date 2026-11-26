#include "app_zip.h"
#include <stdint.h>

static unsigned get16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t get32(const unsigned char *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static void put16(unsigned char *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
static void put32(unsigned char *p, uint32_t n) { put16(p, n); put16(p + 2, n >> 16); }
static void copy(unsigned char *to, const unsigned char *from, unsigned n)
{
    while (n--) *to++ = *from++;
}
static void clear(unsigned char *p, unsigned n)
{
    while (n--) *p++ = 0;
}
static uint32_t crc(const unsigned char *p, unsigned n)
{
    uint32_t sum = UINT32_MAX;
    while (n--) {
        sum ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            sum = (sum >> 1) ^ ((sum & 1) ? 0xedb88320u : 0);
    }
    return ~sum;
}
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int conflicts(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (!*a && (!*b || *b == '/')) || (!*b && *a == '/');
}
static int valid_name(const unsigned char *p, unsigned n)
{
    if (!n || n >= APP_ENTRY_CAPACITY) return 0;
    unsigned start = 0;
    for (unsigned i = 0; i <= n; i++) {
        unsigned char c = i < n ? p[i] : '/';
        if (c == '/') {
            unsigned length = i - start;
            if (!length || (p[start] == '.' &&
                (length == 1 || (length == 2 && p[start + 1] == '.'))))
                return 0;
            start = i + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int app_zip_read(const unsigned char *input, unsigned size, struct app_zip *archive)
{
    if (!archive) return -1;
    archive->count = 0;
    if (!input || size < 22) return -1;
    const unsigned char *end = input + size - 22;
    unsigned count = get16(end + 10);
    unsigned central = get32(end + 16), limit = size - 22;
    if (get32(end) != 0x06054b50 || get32(end + 4) || get16(end + 8) != count ||
        !count || count > APP_ZIP_MAX_FILES || get16(end + 20) ||
        central > limit || get32(end + 12) != limit - central)
        return -1;

    unsigned at = central, expected = 0;
    for (unsigned i = 0; i < count; i++) {
        if (limit - at < 46) return -1;
        const unsigned char *record = input + at;
        unsigned name = get16(record + 28), bytes = get32(record + 24);
        uint32_t checksum = get32(record + 16);
        if (get32(record) != 0x02014b50 || get32(record + 8) ||
            get32(record + 20) != bytes || name > limit - at - 46 ||
            get16(record + 30) || get16(record + 32) || get16(record + 34) ||
            (get32(record + 38) >> 16 & 0170000u) == 0120000u ||
            get32(record + 42) != expected || !valid_name(record + 46, name))
            return -1;
        if (central - expected < 30 || name > central - expected - 30 ||
            bytes > central - expected - 30 - name)
            return -1;
        const unsigned char *local = input + expected;
        if (get32(local) != 0x04034b50 || get32(local + 6) ||
            get32(local + 14) != checksum || get32(local + 18) != bytes ||
            get32(local + 22) != bytes || get16(local + 26) != name || get16(local + 28))
            return -1;
        for (unsigned j = 0; j < name; j++)
            if (local[30 + j] != record[46 + j]) return -1;
        char *path = archive->names[i];
        copy((unsigned char *)path, record + 46, name);
        path[name] = 0;
        for (unsigned j = 0; j < i; j++)
            if (conflicts(path, archive->files[j].name)) return -1;
        const unsigned char *data = local + 30 + name;
        if (crc(data, bytes) != checksum) return -1;
        archive->files[i] = (struct app_zip_entry){path, data, bytes};
        expected += 30 + name + bytes;
        at += 46 + name;
    }
    if (expected != central || at != limit) return -1;
    archive->count = count;
    return 0;
}

int app_zip_write(const struct app_zip_entry *files, unsigned count, unsigned char *output, unsigned capacity)
{
    if (!files || !output || !count || count > APP_ZIP_MAX_FILES || capacity > INT32_MAX)
        return -1;
    unsigned names[APP_ZIP_MAX_FILES], offsets[APP_ZIP_MAX_FILES];
    unsigned total = 22;
    for (unsigned i = 0; i < count; i++) {
        const struct app_zip_entry *file = &files[i];
        if (!file->name || (!file->data && file->size)) return -1;
        unsigned name = 0;
        while (name < APP_ENTRY_CAPACITY && file->name[name]) name++;
        if (!valid_name((const unsigned char *)file->name, name) ||
            total > capacity || 76 + 2 * name > capacity - total ||
            file->size > capacity - total - 76 - 2 * name) return -1;
        for (unsigned j = 0; j < i; j++)
            if (conflicts(file->name, files[j].name)) return -1;
        names[i] = name;
        total += 76 + 2 * name + file->size;
    }
    unsigned at = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned char *local = output + at;
        offsets[i] = at;
        clear(local, 30);
        put32(local, 0x04034b50); put16(local + 4, 20);
        put32(local + 14, crc(files[i].data, files[i].size));
        put32(local + 18, files[i].size); put32(local + 22, files[i].size);
        put16(local + 26, names[i]);
        copy(local + 30, (const unsigned char *)files[i].name, names[i]);
        copy(local + 30 + names[i], files[i].data, files[i].size);
        at += 30 + names[i] + files[i].size;
    }
    unsigned central = at;
    for (unsigned i = 0; i < count; i++) {
        unsigned char *record = output + at;
        clear(record, 46);
        put32(record, 0x02014b50); put16(record + 4, 20); put16(record + 6, 20);
        put32(record + 16, get32(output + offsets[i] + 14));
        put32(record + 20, files[i].size); put32(record + 24, files[i].size);
        put16(record + 28, names[i]); put32(record + 42, offsets[i]);
        copy(record + 46, (const unsigned char *)files[i].name, names[i]);
        at += 46 + names[i];
    }
    unsigned char *end = output + at;
    clear(end, 22);
    put32(end, 0x06054b50); put16(end + 8, count); put16(end + 10, count);
    put32(end + 12, at - central); put32(end + 16, central);
    return (int)total;
}

static const struct app_zip_entry *find(const struct app_zip *archive, const char *name)
{
    for (unsigned i = 0; i < archive->count; i++)
        if (same(name, archive->files[i].name)) return &archive->files[i];
    return 0;
}

int app_zip_launch_entries(const struct app_zip *archive,
                           const struct app_zip_entry **ui,
                           const struct app_zip_entry **backend)
{
    if (!archive || !ui || !backend || archive->count > APP_ZIP_MAX_FILES)
        return -1;
    const struct app_zip_entry *entry = find(archive, "app.json");
    struct directory_app_manifest manifest;
    if (!entry || app_manifest_parse((const char *)entry->data, entry->size, &manifest))
        return -1;

    const struct app_zip_entry *page = find(archive, manifest.ui[0] ? manifest.ui : "app.html");
    const struct app_zip_entry *script = find(archive, manifest.backend[0] ? manifest.backend : "app.js");
    if (!page || (manifest.backend[0] && !script))
        return -1;
    *ui = page;
    *backend = script;
    return 0;
}
