// UltiMedia - LibRetro Audio Player Core
// Main entry point and LibRetro callbacks

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <stdarg.h>
#include "libretro.h"

#include "config.h"
#include "layout.h"
#include "video.h"
#include "render_gl.h"
#include "ui.h"
#include "audio.h"
#include "metadata.h"
#include "visualizer.h"
#include "core_log.h"
#include "core_state.h"
#include "path_io.h"

#define CORE_MAX_PATH 1024
#define CORE_STATE_MAGIC 0x554D5354u
#define CORE_STATE_VERSION 2u
#define CORE_DEFAULT_SEED 0xA341316Cu
#define SEEK_ICON_FRAMES 15 // How long the >>/<< icon stays up after a seek

// LibRetro callbacks
static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

// Diagnostics: route through the frontend log interface when present,
// otherwise fall back to stderr (e.g. under the native test harness).
static void core_log_fallback(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

retro_log_printf_t core_log = core_log_fallback;

void core_log_init(retro_environment_t cb) {
    struct retro_log_callback logging;
    if (cb && cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log)
        core_log = logging.log;
}

// Playlist state
static char *tracks[CORE_MAX_TRACKS];
static int track_count = 0;
static int current_idx = 0;
static char m3u_base_path[CORE_MAX_PATH] = {0};

// UI state
static int scroll_x = 320;
static uint16_t held_buttons = 0;
static bool is_paused = false;
static bool is_shuffle = false;
// Visualizer mode chosen with the X button; survives config refreshes until
// the user changes the core option itself.
static bool viz_mode_user_override = false;
static int viz_mode_menu_value = 0;
static int ff_rw_icon_timer = 0;
static int ff_rw_dir = 0;
static int seek_repeat_cooldown = 0;
// Frame duration reported by the frontend; stays at the 60fps fallback when
// the frontend does not support the frame-time callback.
static retro_usec_t frame_time_usec = 1000000 / 60;
// A resolution change happened outside retro_run; SET_GEOMETRY is only legal
// from within retro_run, so the notification is deferred to the next frame.
static bool geometry_dirty = false;
static bool config_needs_refresh = true;
static uint32_t shuffle_seed = 0;
static uint32_t shuffle_state = 0;
static int shuffle_order[CORE_MAX_TRACKS] = {0};
static int shuffle_count = 0;
static int shuffle_pos = 0;
static int shuffle_history[CORE_MAX_TRACKS] = {0};
static int shuffle_history_count = 0;
static int shuffle_history_pos = 0;

static const struct retro_input_descriptor input_descriptors[] = {
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Pause/Play"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Cycle Visualizer"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Previous Track"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Next Track"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Seek Backward 3 Seconds"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Seek Forward 3 Seconds"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Toggle Shuffle"},
    {0},
};

// Forward declarations
static bool open_track(int idx);
static void clear_shuffle_state(void);
static void reset_runtime_state(bool preserve_shuffle_mode);
static size_t serialized_state_size(void);
static void open_next_track(void);
static void open_previous_track(void);

static void frame_time_cb(retro_usec_t usec) {
    frame_time_usec = usec;
}

// Apply the configured resolution to the framebuffer wherever config is
// re-read (run/reset/unserialize); the frontend learns about it on the next
// retro_run via SET_GEOMETRY.
static void apply_resolution_from_config(void) {
    int scale = (cfg.ui_scale > 0) ? cfg.ui_scale : 1;
    if (320 * scale == fb_width) return;
    video_set_resolution(320 * scale, 240 * scale);
    geometry_dirty = true;
}

// Edge-triggered button check: true only on the frame the button goes down.
// Joypad IDs are < 16, so one uint16_t tracks every button's held state.
static bool button_pressed(unsigned id) {
    bool down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
    uint16_t mask = (uint16_t)(1u << id);
    bool was_down = (held_buttons & mask) != 0;
    if (down) held_buttons |= mask;
    else held_buttons &= ~mask;
    return down && !was_down;
}

static int next_viz_mode(int mode) {
    if (mode == VIZ_MODE_BARS || mode == VIZ_MODE_FFT_EQ_LEGACY) return VIZ_MODE_VU; // Bars -> VU Meter
    if (mode == VIZ_MODE_VU) return VIZ_MODE_DOTS;                                     // VU Meter -> Dots
    if (mode == VIZ_MODE_DOTS) return VIZ_MODE_LINE;                                   // Dots -> Line
    if (mode == VIZ_MODE_LINE) return VIZ_MODE_SCOPE;                                  // Line -> Scope
    if (mode == VIZ_MODE_SCOPE) return VIZ_MODE_MIRROR;                                // Scope -> Mirror
    if (mode == VIZ_MODE_MIRROR) return VIZ_MODE_HORIZON;                              // Mirror -> Horizon
    return VIZ_MODE_BARS;                                                               // Horizon/unknown -> Bars
}

static int is_valid_viz_mode(int mode) {
    return mode == VIZ_MODE_BARS ||
           mode == VIZ_MODE_FFT_EQ_LEGACY ||
           mode == VIZ_MODE_VU ||
           mode == VIZ_MODE_DOTS ||
           mode == VIZ_MODE_LINE ||
           mode == VIZ_MODE_SCOPE ||
           mode == VIZ_MODE_MIRROR ||
           mode == VIZ_MODE_HORIZON;
}

static int normalize_viz_mode(int mode) {
    return (mode == VIZ_MODE_FFT_EQ_LEGACY) ? VIZ_MODE_BARS : mode;
}

// Helper function for case-insensitive string comparison
static int strcasecmp_simple(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

static int strncasecmp_simple(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c1 = s1[i];
        char c2 = s2[i];
        if (!c1 || !c2) return c1 - c2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
    }
    return 0;
}

static int is_drive_letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_localhost_host(const char *s, size_t len) {
    return len == 9 && strncasecmp_simple(s, "localhost", 9) == 0;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void percent_decode_inplace(char *s) {
    if (!s) return;
    size_t r = 0;
    size_t w = 0;
    while (s[r]) {
        if (s[r] == '%' && s[r + 1] && s[r + 2]) {
            int hi = hex_value(s[r + 1]);
            int lo = hex_value(s[r + 2]);
            if (hi >= 0 && lo >= 0) {
                s[w++] = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

static char *file_uri_path_start(char *uri, bool *file_is_unc) {
    if (!uri || !uri[0]) return uri;

    // Keep Windows-style file://C:/... URIs unchanged.
    if (is_drive_letter(uri[0]) && uri[1] == ':') return uri;

    char *sep = uri;
    while (*sep && *sep != '/' && *sep != '\\') sep++;

    // file://hostname with no path: treat as UNC host.
    if (!*sep) {
        if (!is_localhost_host(uri, strlen(uri))) *file_is_unc = true;
        return uri;
    }

    size_t host_len = (size_t)(sep - uri);
    if (host_len == 0) {
        // file:///path...
        uri = sep;
    } else if (is_localhost_host(uri, host_len)) {
        // file://localhost/path...
        uri = sep;
    } else {
        // file://server/share...
        *file_is_unc = true;
    }

    // file:///C:/... -> C:/... (Windows drive path)
    if ((uri[0] == '/' || uri[0] == '\\') && is_drive_letter(uri[1]) && uri[2] == ':')
        uri++;

    return uri;
}

static size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int read_utf16_line(FILE *f, char *out, size_t out_sz, bool le) {
    if (!out || out_sz == 0) return 0;
    size_t idx = 0;
    for (;;) {
        unsigned char b[2];
        size_t r = fread(b, 1, 2, f);
        if (r < 2) {
            if (idx == 0) return 0;
            break;
        }
        uint16_t ch = le ? (uint16_t)(b[0] | (b[1] << 8)) : (uint16_t)(b[1] | (b[0] << 8));
        if (ch == 0xFEFF) continue;
        if (ch == '\n') break;
        if (ch == '\r') {
            long pos = ftell(f);
            if (pos >= 0) {
                unsigned char nb[2];
                size_t nr = fread(nb, 1, 2, f);
                if (nr == 2) {
                    uint16_t ch2 = le ? (uint16_t)(nb[0] | (nb[1] << 8)) : (uint16_t)(nb[1] | (nb[0] << 8));
                    if (ch2 != '\n') fseek(f, pos, SEEK_SET);
                }
            }
            break;
        }
        uint32_t cp = ch;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            // High surrogate: pair it with the next code unit.
            long pos = ftell(f);
            unsigned char nb[2];
            cp = (uint32_t)'?';
            if (fread(nb, 1, 2, f) == 2) {
                uint16_t lo = le ? (uint16_t)(nb[0] | (nb[1] << 8)) : (uint16_t)(nb[1] | (nb[0] << 8));
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000u + (((uint32_t)ch - 0xD800u) << 10) + ((uint32_t)lo - 0xDC00u);
                else if (pos >= 0)
                    fseek(f, pos, SEEK_SET); // unpaired; reprocess the next unit
            }
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            cp = (uint32_t)'?'; // unpaired low surrogate
        }

        char enc[4];
        size_t enc_len = utf8_encode(cp, enc);
        if (idx + enc_len < out_sz) {
            memcpy(out + idx, enc, enc_len);
            idx += enc_len;
        }
    }
    out[idx] = '\0';
    return 1;
}

static size_t detect_m3u_encoding(FILE *f, bool *utf16_le, bool *utf16_be) {
    unsigned char buf[64];
    size_t n = fread(buf, 1, sizeof(buf), f);
    size_t skip = 0;
    *utf16_le = false;
    *utf16_be = false;

    if (n >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) {
        *utf16_le = true;
        skip = 2;
    } else if (n >= 2 && buf[0] == 0xFE && buf[1] == 0xFF) {
        *utf16_be = true;
        skip = 2;
    } else if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        skip = 3; // UTF-8 BOM
    } else if (n >= 4) {
        size_t even_zero = 0;
        size_t odd_zero = 0;
        for (size_t i = 0; i + 1 < n; i += 2) {
            if (buf[i] == 0) even_zero++;
            if (buf[i + 1] == 0) odd_zero++;
        }
        if (odd_zero > even_zero * 2 && odd_zero >= 4) *utf16_le = true;
        else if (even_zero > odd_zero * 2 && even_zero >= 4) *utf16_be = true;
    }

    fseek(f, (long)skip, SEEK_SET);
    return skip;
}

static int read_m3u_line(FILE *f, char *out, size_t out_sz, bool utf16_le, bool utf16_be) {
    if (!utf16_le && !utf16_be) return fgets(out, (int)out_sz, f) != NULL;
    return read_utf16_line(f, out, out_sz, utf16_le);
}

static int is_absolute_path(const char *p) {
    if (!p || !p[0]) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return 1;
    return 0;
}

static int copy_cstr_fixed(char *dest, size_t dest_sz, const char *src) {
    if (!dest || dest_sz == 0 || !src) return 0;
    size_t len = strlen(src);
    if (len >= dest_sz) return 0;
    memcpy(dest, src, len + 1);
    return 1;
}

static char *core_strdup(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char *copy = (char*)malloc(len);
    if (!copy) return NULL;
    memcpy(copy, src, len);
    return copy;
}

static void free_tracks(void) {
    for (int i = 0; i < track_count; i++) {
        if (tracks[i]) {
            free(tracks[i]);
            tracks[i] = NULL;
        }
    }
    track_count = 0;
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char*)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t hash_cstr(uint32_t hash, const char *s) {
    return hash_bytes(hash, s, strlen(s) + 1);
}

static uint32_t current_content_hash(void) {
    uint32_t hash = 2166136261u;
    hash = hash_bytes(hash, &track_count, sizeof(track_count));
    hash = hash_cstr(hash, m3u_base_path);
    for (int i = 0; i < track_count; i++) {
        if (!tracks[i]) continue;
        hash = hash_cstr(hash, tracks[i]);
    }
    return hash ? hash : CORE_DEFAULT_SEED;
}

static uint32_t sanitize_seed(uint32_t seed) {
    return seed ? seed : CORE_DEFAULT_SEED;
}

static uint32_t generate_shuffle_seed(void) {
    uint32_t seed = current_content_hash();
    seed ^= (uint32_t)time(NULL);
    seed ^= (uint32_t)clock();
    return sanitize_seed(seed);
}

static uint32_t next_shuffle_random(void) {
    uint32_t x = sanitize_seed(shuffle_state);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    shuffle_state = sanitize_seed(x);
    return shuffle_state;
}

static void clear_shuffle_state(void) {
    shuffle_seed = 0;
    shuffle_state = 0;
    shuffle_count = 0;
    shuffle_pos = 0;
    memset(shuffle_order, 0, sizeof(shuffle_order));
    shuffle_history_count = 0;
    shuffle_history_pos = 0;
    memset(shuffle_history, 0, sizeof(shuffle_history));
}

static int normalize_track_index(int idx) {
    if (track_count <= 0) return 0;
    return (idx + track_count) % track_count;
}

static void seed_shuffle_history(int idx) {
    if (track_count <= 0) {
        shuffle_history_count = 0;
        shuffle_history_pos = 0;
        return;
    }

    shuffle_history_count = 1;
    shuffle_history_pos = 0;
    shuffle_history[0] = normalize_track_index(idx);
}

static void ensure_shuffle_history_current(void) {
    if (!is_shuffle || track_count <= 0) return;
    if (shuffle_history_count <= 0 ||
        shuffle_history_pos < 0 ||
        shuffle_history_pos >= shuffle_history_count ||
        shuffle_history[shuffle_history_pos] != current_idx) {
        seed_shuffle_history(current_idx);
    }
}

static void append_shuffle_history(int idx) {
    if (track_count <= 0) return;

    int normalized = normalize_track_index(idx);
    ensure_shuffle_history_current();

    if (shuffle_history_count > 0 && shuffle_history_pos < shuffle_history_count - 1)
        shuffle_history_count = shuffle_history_pos + 1;

    if (shuffle_history_count >= CORE_MAX_TRACKS) {
        memmove(&shuffle_history[0],
                &shuffle_history[1],
                (size_t)(CORE_MAX_TRACKS - 1) * sizeof(shuffle_history[0]));
        shuffle_history_count = CORE_MAX_TRACKS - 1;
        if (shuffle_history_pos > 0) shuffle_history_pos--;
    }

    shuffle_history[shuffle_history_count++] = normalized;
    shuffle_history_pos = shuffle_history_count - 1;
}

static void rebuild_shuffle_order(void) {
    shuffle_count = track_count;
    shuffle_pos = 0;
    for (int i = 0; i < shuffle_count; i++) shuffle_order[i] = i;
    for (int i = shuffle_count - 1; i > 0; i--) {
        int j = (int)(next_shuffle_random() % (uint32_t)(i + 1));
        int tmp = shuffle_order[i];
        shuffle_order[i] = shuffle_order[j];
        shuffle_order[j] = tmp;
    }
}

static void start_shuffle_cycle(uint32_t seed) {
    shuffle_seed = sanitize_seed(seed);
    shuffle_state = shuffle_seed;
    rebuild_shuffle_order();
}

static void sync_shuffle_to_current_track(void) {
    if (!is_shuffle || shuffle_count <= 0) return;
    if (shuffle_count == 1) {
        shuffle_order[0] = current_idx;
        shuffle_pos = 0;
        return;
    }

    for (int attempt = 0; attempt < 4; attempt++) {
        int found = -1;
        for (int i = 0; i < shuffle_count; i++) {
            if (shuffle_order[i] == current_idx) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            start_shuffle_cycle(next_shuffle_random());
            continue;
        }
        if (found != shuffle_count - 1) {
            int current_track = shuffle_order[found];
            memmove(&shuffle_order[found],
                    &shuffle_order[found + 1],
                    (size_t)(shuffle_count - found - 1) * sizeof(shuffle_order[0]));
            shuffle_order[shuffle_count - 1] = current_track;
        }
        if (shuffle_order[shuffle_count - 1] == current_idx) {
            shuffle_pos = 0;
            return;
        }
        start_shuffle_cycle(next_shuffle_random());
    }

    shuffle_pos = 0;
}

static void ensure_shuffle_ready(void) {
    if (track_count <= 0) return;
    if (shuffle_count == track_count && shuffle_seed != 0) return;
    start_shuffle_cycle(shuffle_seed ? shuffle_seed : current_content_hash());
    sync_shuffle_to_current_track();
}

static int next_track_index(void) {
    if (track_count == 0) return 0;
    if (!is_shuffle || track_count <= 1) return current_idx + 1;

    ensure_shuffle_ready();
    for (int guard = 0; guard < CORE_MAX_TRACKS * 2; guard++) {
        if (shuffle_pos >= shuffle_count) {
            start_shuffle_cycle(next_shuffle_random());
            sync_shuffle_to_current_track();
        }
        if (shuffle_count <= 0) break;
        int next_idx = shuffle_order[shuffle_pos++];
        if (track_count == 1 || next_idx != current_idx) return next_idx;
    }

    return current_idx + 1;
}

// Both navigation helpers retry past tracks that fail to open so one bad
// file doesn't dead-end playback; they give up after one full playlist pass.
static void open_next_track(void) {
    if (track_count == 0) return;

    for (int attempt = 0; attempt < track_count; attempt++) {
        if (!is_shuffle || track_count <= 1) {
            if (open_track(current_idx + 1)) return;
            continue;
        }

        ensure_shuffle_ready();
        ensure_shuffle_history_current();

        if (shuffle_history_pos + 1 < shuffle_history_count) {
            shuffle_history_pos++;
            if (open_track(shuffle_history[shuffle_history_pos])) return;
            continue;
        }

        int next_idx = next_track_index();
        append_shuffle_history(next_idx);
        if (open_track(next_idx)) return;
    }
}

static void open_previous_track(void) {
    if (track_count == 0) return;

    for (int attempt = 0; attempt < track_count; attempt++) {
        if (!is_shuffle || track_count <= 1) {
            if (open_track(current_idx - 1)) return;
            continue;
        }

        ensure_shuffle_ready();
        ensure_shuffle_history_current();

        if (shuffle_history_pos > 0) {
            shuffle_history_pos--;
            if (open_track(shuffle_history[shuffle_history_pos])) return;
            continue;
        }

        if (open_track(current_idx - 1)) {
            ensure_shuffle_ready();
            sync_shuffle_to_current_track();
            seed_shuffle_history(current_idx);
            return;
        }
    }
}

static void reset_runtime_state(bool preserve_shuffle_mode) {
    scroll_x = fb_width;
    held_buttons = 0;
    viz_mode_user_override = false;
    is_paused = false;
    if (!preserve_shuffle_mode) is_shuffle = false;
    ff_rw_icon_timer = 0;
    ff_rw_dir = 0;
    seek_repeat_cooldown = 0;
    config_needs_refresh = true;
    clear_shuffle_state();
    viz_reset_state();
    ui_reset_marquee();
}

// Tear down the whole playback session: decoder, artwork, playlist, and
// runtime state. Shared by retro_unload_game and every retro_load_game
// bail-out so no path can leave stale session state (e.g. shuffle order)
// behind.
static void unload_session(void) {
    audio_close();
    metadata_free_art();
    free_tracks();
    render_gl_shutdown();
    current_idx = 0;
    m3u_base_path[0] = '\0';
    reset_runtime_state(false);
}

static int add_serialized_size(size_t *total, size_t add) {
    if (!total || add > SIZE_MAX - *total) return 0;
    *total += add;
    return 1;
}

static size_t serialized_state_size(void) {
    size_t total = sizeof(CoreStateSnapshot);
    if (!add_serialized_size(&total, strlen(m3u_base_path) + 1)) return 0;

    for (int i = 0; i < track_count; i++) {
        if (!tracks[i]) return 0;
        if (!add_serialized_size(&total, strlen(tracks[i]) + 1)) return 0;
    }

    return total;
}

static int validate_loaded_content(const CoreStateSnapshot *state, size_t serialized_size) {
    if (!state) return 0;
    if (state->track_count == 0 || state->track_count > CORE_MAX_TRACKS) return 0;
    if ((uint32_t)track_count != state->track_count) return 0;
    if (serialized_size < sizeof(CoreStateSnapshot)) return 0;

    const char *cursor = (const char*)(state + 1);
    const char *end = (const char*)state + serialized_size;

    if (state->m3u_base_path_len == 0) return 0;
    if ((size_t)(end - cursor) < state->m3u_base_path_len) return 0;
    if (cursor[state->m3u_base_path_len - 1] != '\0') return 0;
    if (strlen(m3u_base_path) + 1 != state->m3u_base_path_len) return 0;
    if (memcmp(m3u_base_path, cursor, state->m3u_base_path_len) != 0) return 0;
    cursor += state->m3u_base_path_len;

    for (uint32_t i = 0; i < state->track_count; i++) {
        uint32_t path_len = state->track_path_lens[i];
        if (path_len == 0) return 0;
        if ((size_t)(end - cursor) < path_len) return 0;
        if (cursor[path_len - 1] != '\0') return 0;
        if (!tracks[i]) return 0;
        if (strlen(tracks[i]) + 1 != path_len) return 0;
        if (memcmp(tracks[i], cursor, path_len) != 0) return 0;
        cursor += path_len;
    }

    return state->content_hash == current_content_hash();
}

static bool open_track(int idx) {
    if (track_count == 0) return false;

    current_idx = (idx + track_count) % track_count;
    const char *p = tracks[current_idx];

    // Open audio
    if (!audio_open_track(p)) {
        snprintf(display_str, sizeof(display_str), "ERROR LOADING: %.230s", p);
        return false;
    }

    // Check channel limit
    if (source_channels > MAX_CHANNELS) {
        audio_close();
        snprintf(display_str, sizeof(display_str), "UNSUPPORTED CHANNELS: %d", source_channels);
        return false;
    }

    viz_reset_state();

    // Load metadata and album art
    metadata_load(p, m3u_base_path, cfg.track_text_mode);
    scroll_x = layout.text.x;
    ui_reset_marquee();
    return true;
}

// Re-read core options. The X button can override the visualizer mode at
// runtime; keep that override across refreshes until the menu option itself
// changes, then adopt the menu value and drop the override.
static void apply_config_update(void) {
    int runtime_mode = cfg.viz_mode;
    config_update(environ_cb);
    int menu_mode = cfg.viz_mode;
    if (viz_mode_user_override) {
        if (menu_mode == viz_mode_menu_value)
            cfg.viz_mode = runtime_mode;
        else
            viz_mode_user_override = false;
    }
    viz_mode_menu_value = menu_mode;
}

static void refresh_config_and_layout(void) {
    TrackTextMode old_track_text_mode = cfg.track_text_mode;
    bool old_show_txt = cfg.show_txt;
    int old_ui_scale = cfg.ui_scale;
    apply_config_update();

    apply_resolution_from_config();
    layout_compute();

    if (track_count > 0 &&
        current_idx >= 0 &&
        current_idx < track_count &&
        tracks[current_idx]) {
        if (old_track_text_mode != cfg.track_text_mode)
            metadata_refresh_display(tracks[current_idx], cfg.track_text_mode);
        // Restart the marquee when the text mode changes, the title is
        // re-enabled, or the resolution scale changes, so it never resumes
        // from a stale or wrong-scale mid-scroll position.
        if (old_track_text_mode != cfg.track_text_mode ||
            (cfg.show_txt && !old_show_txt) ||
            cfg.ui_scale != old_ui_scale) {
            scroll_x = layout.text.x;
            ui_reset_marquee();
        }
    }
}

void retro_run(void) {
    float dt = (float)frame_time_usec / 1000000.0f;
    if (dt < 0.001f || dt > 0.1f) dt = 1.0f / 60.0f;  // ignore stalls/garbage
    ui_set_frame_dt(dt);
    viz_set_frame_dt(dt);

    if (config_needs_refresh) {
        refresh_config_and_layout();
        config_needs_refresh = false;
    } else {
        bool updated = false;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
            refresh_config_and_layout();
    }

    if (geometry_dirty) {
        // Within the pre-declared max, SET_GEOMETRY changes size without a
        // driver reinit (which would tear down EmuVR's shared-texture pipe).
        struct retro_game_geometry geom = {
            (unsigned)fb_width, (unsigned)fb_height,
            FB_MAX_WIDTH, FB_MAX_HEIGHT,
            4.0f / 3.0f
        };
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
        geometry_dirty = false;
    }

    input_poll_cb();

    // 1. Handle Inputs
    if (decoder && !is_paused) {
        bool seek_fwd = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) != 0;
        bool seek_back = !seek_fwd && input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) != 0;
        if (seek_repeat_cooldown > 0) seek_repeat_cooldown--;
        if (!seek_fwd && !seek_back) {
            seek_repeat_cooldown = 0; // a fresh tap seeks immediately
        } else if (seek_repeat_cooldown == 0) {
            // Decoder seeks are costly (MP3 backward seeks re-decode from the
            // start of the file), so a held button repeats at 10 Hz, not 60.
            seek_repeat_cooldown = 6;
            uint64_t seek_speed = (uint64_t)source_rate * 3u;
            if (seek_fwd) {
                uint64_t next = cur_frame + seek_speed;
                if (next < cur_frame) next = cur_frame; // overflow guard
                if (total_frames > 0 && cur_frame < total_frames && next >= total_frames)
                    next = total_frames - 1;
                if (audio_seek(next)) {
                    ff_rw_icon_timer = SEEK_ICON_FRAMES;
                    ff_rw_dir = 1;
                }
            } else {
                uint64_t next = (cur_frame < seek_speed) ? 0 : cur_frame - seek_speed;
                if (audio_seek(next)) {
                    ff_rw_icon_timer = SEEK_ICON_FRAMES;
                    ff_rw_dir = -1;
                }
            }
        }
    }

    if (button_pressed(RETRO_DEVICE_ID_JOYPAD_Y)) {
        is_shuffle = !is_shuffle;
        if (is_shuffle) {
            ensure_shuffle_ready();
            sync_shuffle_to_current_track();
            seed_shuffle_history(current_idx);
        } else {
            shuffle_history_count = 0;
            shuffle_history_pos = 0;
        }
    }
    if (button_pressed(RETRO_DEVICE_ID_JOYPAD_B)) is_paused = !is_paused;
    if (button_pressed(RETRO_DEVICE_ID_JOYPAD_X)) {
        cfg.viz_mode = next_viz_mode(cfg.viz_mode);
        viz_mode_user_override = true;
        layout_compute();
    }
    if (button_pressed(RETRO_DEVICE_ID_JOYPAD_R)) open_next_track();
    if (button_pressed(RETRO_DEVICE_ID_JOYPAD_L)) open_previous_track();
    if (ff_rw_icon_timer > 0) ff_rw_icon_timer--;

    // 2. Audio Core
    int16_t out_buf[SAMPLES_PER_FRAME * 2] = {0};

    if (decoder && !is_paused) {
        int samples = audio_read_frame(out_buf);
        if (samples == 0) {
            // End of track, go to next
            open_next_track();
        }
    }

    // 3. Visualizer & Audio Batch
    viz_update_levels(out_buf, SAMPLES_PER_FRAME);
    audio_batch_cb(out_buf, SAMPLES_PER_FRAME);

    // 4. Rendering Section
    UiFrame frame = {
        .paused = is_paused,
        .shuffle = is_shuffle,
        .seek_dir = (ff_rw_icon_timer > 0) ? ff_rw_dir : 0,
        .track_index = current_idx,
        .track_count = track_count,
        .cur_frame = cur_frame,
        .total_frames = total_frames,
        .source_rate = source_rate,
        .scroll_x = &scroll_x,
    };
    ui_draw(&frame);
    if (render_gl_negotiated()) {
        // Once SET_HW_RENDER is accepted the contract allows only a hw frame
        // or NULL (frame dupe) -- 1.7.5's gl driver ignores CPU uploads while
        // its hw FBO is active. NULL covers the frames before context_reset
        // and any GL setup failure (which context_reset already logged).
        video_cb(render_gl_frame() ? RETRO_HW_FRAME_BUFFER_VALID : NULL,
                 fb_width, fb_height, 0);
    } else {
        video_cb(framebuffer, fb_width, fb_height, fb_width * 2);
    }
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    core_log_init(cb);
    config_declare_variables(cb);
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)input_descriptors);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

bool retro_load_game(const struct retro_game_info *g) {
    if (!g || !g->path) return false;

    unload_session();
    char m3u_dir[CORE_MAX_PATH] = {0};

    // Check for M3U extension
    const char* ext = strrchr(g->path, '.');
    if (ext && strcasecmp_simple(ext, ".m3u") == 0) {
        core_log(RETRO_LOG_DEBUG, "[MusicCore] Attempting to open M3U: %s\n", g->path);

        FILE *f = path_fopen_read(g->path);
        if (!f) {
            core_log(RETRO_LOG_ERROR, "[MusicCore] Failed to open M3U at %s\n", g->path);
            return false;
        }
        bool m3u_utf16_le = false;
        bool m3u_utf16_be = false;
        detect_m3u_encoding(f, &m3u_utf16_le, &m3u_utf16_be);
        strncpy(m3u_base_path, g->path, CORE_MAX_PATH - 1);
        m3u_base_path[sizeof(m3u_base_path) - 1] = '\0';
        if (!path_dirname(g->path, m3u_dir, sizeof(m3u_dir))) {
            strncpy(m3u_dir, ".", sizeof(m3u_dir) - 1);
            m3u_dir[sizeof(m3u_dir) - 1] = '\0';
        }
        char line[CORE_MAX_PATH];
        while (read_m3u_line(f, line, sizeof(line), m3u_utf16_le, m3u_utf16_be) && track_count < CORE_MAX_TRACKS) {
            // Clean the line aggressively
            line[strcspn(line, "\r\n")] = 0;

            // Trim leading/trailing spaces
            char* trimmed = line;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
            size_t tlen = strlen(trimmed);
            if (tlen == 0) continue;
            char* end = trimmed + tlen - 1;
            while (end >= trimmed && (*end == ' ' || *end == '\t' || *end == '\r')) {
                *end = '\0';
                end--;
            }

            // Strip UTF-8 BOM if present (common in M3U files)
            if ((unsigned char)trimmed[0] == 0xEF && (unsigned char)trimmed[1] == 0xBB && (unsigned char)trimmed[2] == 0xBF)
                trimmed += 3;

            // Strip surrounding quotes
            tlen = strlen(trimmed);
            if (tlen >= 2 && ((trimmed[0] == '"' && trimmed[tlen - 1] == '"') || (trimmed[0] == '\'' && trimmed[tlen - 1] == '\''))) {
                trimmed[tlen - 1] = '\0';
                trimmed++;
            }

            if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

            // Handle file:// URIs
            bool file_is_unc = false;
            if (strncasecmp_simple(trimmed, "file://", 7) == 0) {
                trimmed = file_uri_path_start(trimmed + 7, &file_is_unc);
                percent_decode_inplace(trimmed);
            }

            // Fix backslashes for standard C file handling
            for (int i = 0; trimmed[i]; i++) {
                if (trimmed[i] == '\\') trimmed[i] = '/';
            }
            char resolved[CORE_MAX_PATH];
            int written = 0;
            if (is_absolute_path(trimmed) || file_is_unc || !m3u_dir[0]) {
                if (file_is_unc)
                    written = snprintf(resolved, sizeof(resolved), "//%s", trimmed);
                else
                    written = snprintf(resolved, sizeof(resolved), "%s", trimmed);
            } else {
                written = snprintf(resolved, sizeof(resolved), "%s/%s", m3u_dir, trimmed);
            }
            if (written <= 0 || written >= (int)sizeof(resolved)) continue;
            for (int i = 0; resolved[i]; i++) {
                if (resolved[i] == '\\') resolved[i] = '/';
            }
            tracks[track_count] = core_strdup(resolved);
            if (!tracks[track_count]) {
                fclose(f);
                unload_session();
                return false;
            }
            track_count++;
        }
        fclose(f);
    } else {
        // Single track logic
        tracks[track_count] = core_strdup(g->path);
        if (!tracks[track_count]) return false;
        track_count++;
    }

    if (track_count == 0) {
        unload_session();
        return false;
    }

    struct retro_frame_time_callback ftcb = { frame_time_cb, 1000000 / 60 };
    environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &ftcb);

    start_shuffle_cycle(generate_shuffle_seed());
    apply_config_update();
    // SET_HW_RENDER is only legal during load; when the frontend refuses,
    // render_gl_frame() stays false and presentation continues in software.
    if (cfg.renderer == RENDERER_OPENGL)
        render_gl_request_context(environ_cb);
    // The frontend queries retro_get_system_av_info after load, so the base
    // size is declared there; no SET_GEOMETRY needed for this change.
    video_set_resolution(320 * cfg.ui_scale, 240 * cfg.ui_scale);
    geometry_dirty = false;
    layout_compute();

    bool opened = open_track(0);
    if (!opened) {
        // Skip past unreadable leading tracks so the playlist still starts.
        for (int i = 1; i < track_count; i++) {
            if (open_track(i)) {
                opened = true;
                break;
            }
        }
    }
    if (!opened) {
        unload_session();
        return false;
    }
    return true;
}

void retro_init(void) {
    video_init();
    audio_init();
    current_idx = 0;
    m3u_base_path[0] = '\0';
    reset_runtime_state(false);
}

void retro_deinit(void) {
    audio_deinit();
    video_deinit();
    metadata_free_art();
    free_tracks();
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *i) {
    // Frontends key per-core settings, remaps, and save-state folders off
    // this identity -- renaming it orphans existing users' data.
    i->library_name = "UltiMedia UGC";
    i->library_version = "17.0";
    i->valid_extensions = "mp3|wav|m3u|ogg|flac";
    i->need_fullpath = true;
}
void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = 60.0;
    info->timing.sample_rate = (double)OUT_RATE;
    info->geometry.base_width = fb_width;
    info->geometry.base_height = fb_height;
    info->geometry.max_width = FB_MAX_WIDTH;
    info->geometry.max_height = FB_MAX_HEIGHT;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_unload_game(void) {
    unload_session();
}
void retro_reset(void) {
    if (track_count == 0 || current_idx < 0 || current_idx >= track_count || !tracks[current_idx]) {
        reset_runtime_state(false);
        return;
    }

    bool preserve_shuffle = is_shuffle;
    uint32_t session_seed = shuffle_seed ? shuffle_seed : current_content_hash();
    int restore_idx = current_idx;

    reset_runtime_state(preserve_shuffle);
    current_idx = restore_idx;
    refresh_config_and_layout();
    config_needs_refresh = false;

    open_track(current_idx);
    if (preserve_shuffle) {
        start_shuffle_cycle(session_seed);
        sync_shuffle_to_current_track();
        seed_shuffle_history(current_idx);
    }
}

size_t retro_serialize_size(void) { return serialized_state_size(); }

bool retro_serialize(void *d, size_t s) {
    size_t required_size = serialized_state_size();
    if (!d || required_size == 0 || s < required_size || track_count <= 0 || current_idx < 0 || current_idx >= track_count)
        return false;

    CoreStateSnapshot state;
    memset(&state, 0, sizeof(state));
    state.magic = CORE_STATE_MAGIC;
    state.version = CORE_STATE_VERSION;
    state.content_hash = current_content_hash();
    state.track_count = (uint32_t)track_count;
    state.current_idx = current_idx;
    state.viz_mode = normalize_viz_mode(cfg.viz_mode);
    state.scroll_x = scroll_x;
    state.ff_rw_icon_timer = ff_rw_icon_timer;
    state.ff_rw_dir = ff_rw_dir;
    state.is_paused = is_paused ? 1u : 0u;
    state.is_shuffle = is_shuffle ? 1u : 0u;
    state.shuffle_seed = shuffle_seed ? shuffle_seed : current_content_hash();
    state.shuffle_state = shuffle_state ? shuffle_state : state.shuffle_seed;
    if (shuffle_count < 0 || shuffle_count > track_count) return false;
    if (shuffle_pos < 0 || shuffle_pos > shuffle_count) return false;
    if (shuffle_history_count < 0 || shuffle_history_count > CORE_MAX_TRACKS) return false;
    if ((shuffle_history_count == 0 && shuffle_history_pos != 0) ||
        (shuffle_history_count > 0 && (shuffle_history_pos < 0 || shuffle_history_pos >= shuffle_history_count)))
        return false;
    state.shuffle_count = (uint32_t)shuffle_count;
    state.shuffle_pos = (uint32_t)shuffle_pos;
    state.shuffle_history_count = (uint32_t)shuffle_history_count;
    state.shuffle_history_pos = (uint32_t)shuffle_history_pos;
    audio_capture_state(&state.audio);
    state.m3u_base_path_len = (uint32_t)(strlen(m3u_base_path) + 1);

    for (int i = 0; i < track_count; i++) {
        if (!tracks[i]) return false;
        state.track_path_lens[i] = (uint32_t)(strlen(tracks[i]) + 1);
    }
    for (int i = 0; i < shuffle_count; i++) state.shuffle_order[i] = shuffle_order[i];
    for (int i = 0; i < shuffle_history_count; i++) state.shuffle_history[i] = shuffle_history[i];

    memcpy(d, &state, sizeof(state));
    char *cursor = (char*)d + sizeof(state);
    memcpy(cursor, m3u_base_path, state.m3u_base_path_len);
    cursor += state.m3u_base_path_len;
    for (int i = 0; i < track_count; i++) {
        memcpy(cursor, tracks[i], state.track_path_lens[i]);
        cursor += state.track_path_lens[i];
    }

    return true;
}

bool retro_unserialize(const void *d, size_t s) {
    if (!d || s < sizeof(CoreStateSnapshot)) return false;

    const CoreStateSnapshot *state = (const CoreStateSnapshot*)d;
    if (state->magic != CORE_STATE_MAGIC || state->version != CORE_STATE_VERSION) return false;
    if (state->track_count == 0 || state->track_count > CORE_MAX_TRACKS) return false;
    if (state->current_idx < 0 || state->current_idx >= (int32_t)state->track_count) return false;
    if (!is_valid_viz_mode(state->viz_mode)) return false;
    if (state->shuffle_count > state->track_count || state->shuffle_pos > state->shuffle_count) return false;
    if (state->shuffle_history_count > CORE_MAX_TRACKS) return false;
    if ((state->shuffle_history_count == 0 && state->shuffle_history_pos != 0) ||
        (state->shuffle_history_count > 0 && state->shuffle_history_pos >= state->shuffle_history_count))
        return false;
    if (!validate_loaded_content(state, s)) return false;
    for (uint32_t i = 0; i < state->shuffle_count; i++) {
        if (state->shuffle_order[i] < 0 || state->shuffle_order[i] >= (int32_t)state->track_count)
            return false;
    }
    for (uint32_t i = 0; i < state->shuffle_history_count; i++) {
        if (state->shuffle_history[i] < 0 || state->shuffle_history[i] >= (int32_t)state->track_count)
            return false;
    }

    // Reject invalid audio snapshots before touching the live decoder:
    // audio_restore_state would fail on pure validation, and tearing
    // playback down for a state that was never going to load would silence
    // a healthy session.
    if (!audio_snapshot_valid(&state->audio)) return false;

    AudioStateSnapshot previous_audio;
    const char *previous_track = NULL;
    bool had_current_track = current_idx >= 0 && current_idx < track_count && tracks[current_idx];
    if (had_current_track) previous_track = tracks[current_idx];
    audio_capture_state(&previous_audio);

    // The incoming snapshot validated above, so a restore failure here
    // means the live decoder really was disturbed; roll back, and close
    // only if the rollback fails too.
    if (!audio_restore_state(tracks[state->current_idx], &state->audio)) {
        if (!audio_restore_state(previous_track, &previous_audio))
            audio_close();
        return false;
    }

    apply_config_update();
    cfg.viz_mode = normalize_viz_mode(state->viz_mode);
    apply_resolution_from_config();
    layout_compute();

    reset_runtime_state(state->is_shuffle != 0);
    viz_mode_user_override = (cfg.viz_mode != viz_mode_menu_value);
    current_idx = state->current_idx;
    const char *serialized_m3u_base_path = (const char*)(state + 1);
    if (!copy_cstr_fixed(m3u_base_path, sizeof(m3u_base_path), serialized_m3u_base_path)) return false;

    metadata_load(tracks[current_idx], m3u_base_path, cfg.track_text_mode);
    is_paused = state->is_paused != 0;
    is_shuffle = state->is_shuffle != 0;
    int restored_scale = ((layout.text_scale > 0) ? layout.text_scale : 1)
                       * ((cfg.ui_scale > 0) ? cfg.ui_scale : 1);
    int min_scroll_x = -((int)strlen(display_str) * GLYPH_WIDTH * restored_scale);
    scroll_x = state->scroll_x;
    if (scroll_x < min_scroll_x || scroll_x > fb_width) scroll_x = fb_width;
    ff_rw_icon_timer = state->ff_rw_icon_timer;
    if (ff_rw_icon_timer < 0) ff_rw_icon_timer = 0;
    if (ff_rw_icon_timer > SEEK_ICON_FRAMES) ff_rw_icon_timer = SEEK_ICON_FRAMES;
    ff_rw_dir = (state->ff_rw_dir > 0) ? 1 : ((state->ff_rw_dir < 0) ? -1 : 0);
    shuffle_seed = sanitize_seed(state->shuffle_seed ? state->shuffle_seed : current_content_hash());
    shuffle_state = sanitize_seed(state->shuffle_state ? state->shuffle_state : shuffle_seed);
    shuffle_count = (int)state->shuffle_count;
    shuffle_pos = (int)state->shuffle_pos;
    shuffle_history_count = (int)state->shuffle_history_count;
    shuffle_history_pos = (int)state->shuffle_history_pos;
    memset(shuffle_order, 0, sizeof(shuffle_order));
    for (int i = 0; i < shuffle_count; i++) shuffle_order[i] = state->shuffle_order[i];
    memset(shuffle_history, 0, sizeof(shuffle_history));
    for (int i = 0; i < shuffle_history_count; i++) shuffle_history[i] = state->shuffle_history[i];
    config_needs_refresh = false;
    return true;
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
void* retro_get_memory_data(unsigned i) { (void)i; return NULL; }
size_t retro_get_memory_size(unsigned i) { (void)i; return 0; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { (void)t; (void)g; (void)n; return false; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

int core_debug_get_track_count(void) { return track_count; }
int core_debug_get_current_index(void) { return current_idx; }
bool core_debug_is_shuffle_enabled(void) { return is_shuffle; }
bool core_debug_is_paused(void) { return is_paused; }
const char *core_debug_get_current_track_path(void) {
    if (current_idx < 0 || current_idx >= track_count) return NULL;
    return tracks[current_idx];
}
