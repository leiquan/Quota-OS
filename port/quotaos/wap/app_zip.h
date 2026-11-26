#pragma once
#include "app_manifest.h"

#define APP_ZIP_MAX_FILES 16

struct app_zip_entry {
    const char *name;
    const unsigned char *data;
    unsigned size;
};

struct app_zip {
    struct app_zip_entry files[APP_ZIP_MAX_FILES];
    char names[APP_ZIP_MAX_FILES][APP_ENTRY_CAPACITY];
    unsigned count;
};

/* Standard ZIP with stored files. Entries borrow bytes from the input. */
int app_zip_read(const unsigned char *input, unsigned size, struct app_zip *archive);
int app_zip_write(const struct app_zip_entry *files, unsigned count, unsigned char *output, unsigned capacity);
// Entry views borrow the archive and package storage.
int app_zip_launch_entries(const struct app_zip *archive,
                           const struct app_zip_entry **ui,
                           const struct app_zip_entry **backend);
