#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/*
 * Open a file through its wide-character API variant on Windows, falling
 * back to the narrow variant when UTF-8 conversion or the wide open fails;
 * elsewhere only the narrow variant runs. `wide_expr` may reference the
 * local `wide` (wchar_t *). Both expressions must evaluate to `result`'s
 * type, with `fail_value` marking failure.
 */
#ifdef _WIN32
#define PATH_OPEN_WIDE_THEN_NARROW(result, path, wide_expr, narrow_expr, fail_value) \
    do { \
        wchar_t *wide = path_utf8_to_wide_alloc(path); \
        (result) = (fail_value); \
        if (wide) { \
            (result) = (wide_expr); \
            free(wide); \
        } \
        if ((result) == (fail_value)) (result) = (narrow_expr); \
    } while (0)
#else
#define PATH_OPEN_WIDE_THEN_NARROW(result, path, wide_expr, narrow_expr, fail_value) \
    do { (result) = (narrow_expr); } while (0)
#endif

static FILE *path_fopen_read(const char *path) {
    FILE *file;
    PATH_OPEN_WIDE_THEN_NARROW(file, path,
                               _wfopen(wide, L"rb"),
                               fopen(path, "rb"),
                               (FILE*)NULL);
    return file;
}

// Copy the directory portion of `path` (the bytes before the last '/' or
// '\\') into `out`, truncating to fit. Returns 1 when a separator was
// found, 0 otherwise (out is emptied).
static int path_dirname(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!path) return 0;

    const char *last = strrchr(path, '/');
    const char *last_bslash = strrchr(path, '\\');
    if (!last || (last_bslash && last_bslash > last)) last = last_bslash;
    if (!last) return 0;

    size_t dir_len = (size_t)(last - path);
    if (dir_len >= out_size) dir_len = out_size - 1;
    memcpy(out, path, dir_len);
    out[dir_len] = '\0';
    return 1;
}

// The component after the last '/' or '\\', or `path` itself.
static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *base = slash;
    if (!base || (bslash && bslash > base)) base = bslash;
    return base ? base + 1 : path;
}
