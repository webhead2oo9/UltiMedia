#pragma once

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>

static wchar_t *path_utf8_to_wide_alloc(const char *path) {
    if (!path) return NULL;

    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (length <= 0) return NULL;

    wchar_t *wide = malloc((size_t)length * sizeof(*wide));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif

static FILE *path_fopen_read(const char *path) {
#ifdef _WIN32
    wchar_t *wide = path_utf8_to_wide_alloc(path);
    if (wide) {
        FILE *file = _wfopen(wide, L"rb");
        free(wide);
        if (file) return file;
    }
#endif
    return fopen(path, "rb");
}
