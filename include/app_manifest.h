#pragma once

#define APP_ENTRY_CAPACITY 128

struct directory_app_manifest {
    char ui[APP_ENTRY_CAPACITY];
    char backend[APP_ENTRY_CAPACITY];
};

/* Reads one UTF-8 JSON object with ASCII relative entry names. */
int app_manifest_parse(const char *json, unsigned length, struct directory_app_manifest *result);
