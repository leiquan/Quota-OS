#include "app_manifest.h"

struct input {
    const char *next, *end;
};

static void skip_space(struct input *input)
{
    while (input->next != input->end) {
        char c = *input->next;
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
            break;
        input->next++;
    }
}

static int take(struct input *input, char c)
{
    skip_space(input);
    if (input->next == input->end || *input->next != c)
        return 0;
    input->next++;
    return 1;
}

static int string(struct input *input, char *output, unsigned capacity)
{
    if (!take(input, '"'))
        return 0;
    unsigned size = 0;
    while (input->next != input->end && *input->next != '"') {
        unsigned char c = *input->next++;
        if (c < 32 || c >= 127 || c == '\\' || size + 1 == capacity)
            return 0;
        output[size++] = c;
    }
    if (input->next == input->end)
        return 0;
    input->next++;
    output[size] = 0;
    return 1;
}

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int valid_entry(const char *path, int backend)
{
    const char *component = path;
    const char *extension = 0;
    for (const char *p = path;; p++) {
        char c = *p;
        if (!c || c == '/') {
            if (p == component || (p - component == 1 && *component == '.') ||
                (p - component == 2 && component[0] == '.' && component[1] == '.'))
                return 0;
            if (!c)
                break;
            component = p + 1;
            extension = 0;
        } else if (c == '.') {
            extension = p;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    if (!extension || extension == component)
        return 0;
    if (backend)
        return equal(extension, ".js") || equal(extension, ".cjs") || equal(extension, ".mjs");
    return equal(extension, ".html") || equal(extension, ".htm");
}

int app_manifest_parse(const char *json, unsigned length, struct directory_app_manifest *result)
{
    if (!json || !result || !length || length > 16384)
        return -1;
    struct input input = {json, json + length};
    struct directory_app_manifest parsed = {{0}, {0}};
    unsigned seen = 0;
    if (!take(&input, '{'))
        return -1;
    do {
        char key[8];
        if (!string(&input, key, sizeof key) || !take(&input, ':'))
            return -1;
        if (equal(key, "tier")) {
            if ((seen & 1) || !take(&input, '3'))
                return -1;
            seen |= 1;
        } else if (equal(key, "ui")) {
            if ((seen & 2) || !string(&input, parsed.ui, sizeof parsed.ui) || !valid_entry(parsed.ui, 0))
                return -1;
            seen |= 2;
        } else if (equal(key, "backend")) {
            if ((seen & 4) || !string(&input, parsed.backend, sizeof parsed.backend) || !valid_entry(parsed.backend, 1))
                return -1;
            seen |= 4;
        } else {
            return -1;
        }
    } while (take(&input, ','));
    if (!take(&input, '}'))
        return -1;
    skip_space(&input);
    if (!(seen & 1) || input.next != input.end)
        return -1;
    *result = parsed;
    return 0;
}
