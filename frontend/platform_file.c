#if !defined(_WIN32) && !defined(PSVITA_TARGET)
/* opendir/readdir under the strict -std=c11 test builds. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform_file.h"

#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(PSVITA_TARGET)
#include <dirent.h>
#define PSXE_CASE_FALLBACK 1
#endif

static psxe_platform_fopen_callback_t g_fopen_callback;
static void* g_fopen_userdata;

void psxe_platform_set_fopen_callback(psxe_platform_fopen_callback_t callback, void* userdata) {
    g_fopen_callback = callback;
    g_fopen_userdata = userdata;
}

#ifdef PSXE_CASE_FALLBACK
static int ascii_equal_nocase(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char ca = *a;
        char cb = *b;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');

        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');

        if (ca != cb)
            return 0;
    }

    return *a == *b;
}

/*
    Cue sheets are usually written on case-insensitive systems: "GAME.BIN" in the sheet, "Game.bin"
    on disk. When a read-only open fails, look for the one directory entry whose name matches
    ignoring ASCII case.
*/
static FILE* fopen_nocase(const char* path, const char* mode) {
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    const size_t dir_len = slash ? (size_t)(slash - path) : 0;
    char* dir = malloc(dir_len + 2);

    if (!dir)
        return NULL;

    if (slash) {
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';

        if (!dir_len)
            strcpy(dir, "/");
    } else {
        strcpy(dir, ".");
    }

    FILE* file = NULL;
    DIR* handle = opendir(dir);

    if (handle) {
        struct dirent* entry;

        while ((entry = readdir(handle)) != NULL) {
            if (!ascii_equal_nocase(entry->d_name, name))
                continue;

            const size_t full_len = strlen(dir) + 1 + strlen(entry->d_name) + 1;
            char* full = malloc(full_len);

            if (full) {
                snprintf(full, full_len, "%s/%s", slash ? dir : ".", entry->d_name);
                file = fopen(full, mode);
                free(full);
            }

            break;
        }

        closedir(handle);
    }

    free(dir);

    return file;
}
#endif

FILE* psxe_platform_fopen(const char* path, const char* mode) {
    FILE* file = NULL;

    if (g_fopen_callback) {
        file = g_fopen_callback(path, mode, g_fopen_userdata);

        if (!file && !(path && path[0]))
            file = fopen(path, mode);
    } else {
        file = fopen(path, mode);
    }

#ifdef PSXE_CASE_FALLBACK
    if (!file && path && path[0] && mode && mode[0] == 'r')
        file = fopen_nocase(path, mode);
#endif

    return file;
}
