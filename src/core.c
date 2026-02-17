// UltiMedia - LibRetro Audio Player Core
// Main entry point and LibRetro callbacks

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "libretro.h"

#include "config.h"
#include "layout.h"
#include "video.h"
#include "audio.h"
#include "metadata.h"
#include "visualizer.h"

// LibRetro callbacks
static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static bool use_xrgb8888 = false;

// Playlist state
static char *tracks[256];
static int track_count = 0;
static int current_idx = 0;
static char m3u_base_path[1024] = {0};

// UI state
static int scroll_x = 320;
static int debounce = 0;
static bool is_paused = false;
static bool is_shuffle = false;
static char time_str[32];
static int ff_rw_icon_timer = 0;
static int ff_rw_dir = 0;

// Forward declarations
static void open_track(int idx);

static int next_viz_mode(int mode) {
    if (mode == 0) return 3; // Bars -> VU Meter
    if (mode == 3) return 1; // VU Meter -> Dots
    if (mode == 1) return 2; // Dots -> Line
    return 0; // Line/unknown -> Bars
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
        if (idx + 1 < out_sz) out[idx++] = (ch < 0x80) ? (char)ch : '?';
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

static void open_track(int idx) {
    if (track_count == 0) return;

    current_idx = (idx + track_count) % track_count;
    const char *p = tracks[current_idx];

    // Open audio
    if (!audio_open_track(p)) {
        snprintf(display_str, sizeof(display_str), "ERROR LOADING: %.230s", p);
        return;
    }

    // Check channel limit
    if (source_channels > MAX_CHANNELS) {
        audio_close();
        snprintf(display_str, sizeof(display_str), "UNSUPPORTED CHANNELS: %d", source_channels);
        return;
    }

    // Load metadata and album art
    metadata_load(p, m3u_base_path, cfg.track_text_mode);
    scroll_x = cfg.responsive ? (layout.content_x + layout.content_w) : FB_WIDTH;
}

static void refresh_config_and_layout(void) {
    TrackTextMode old_track_text_mode = cfg.track_text_mode;
    config_update(environ_cb);
    if (cfg.responsive)
        layout_compute();

    if (old_track_text_mode != cfg.track_text_mode &&
        track_count > 0 &&
        current_idx >= 0 &&
        current_idx < track_count &&
        tracks[current_idx]) {
        metadata_refresh_display(tracks[current_idx], cfg.track_text_mode);
        scroll_x = cfg.responsive ? (layout.content_x + layout.content_w) : FB_WIDTH;
    }
}

// External declaration for viz_set_audio_for_vu
void viz_set_audio_for_vu(const int16_t *audio_buf, int samples_per_frame);

void retro_run(void) {
    static bool first_run = true;
    if (first_run) {
        refresh_config_and_layout();
        first_run = false;
    } else {
        bool updated = false;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
            refresh_config_and_layout();
    }
    input_poll_cb();

    // 1. Handle Inputs
    if (decoder && !is_paused) {
        int seek_speed = source_rate * 3;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
            uint64_t next = cur_frame + (uint64_t)seek_speed;
            if (next < cur_frame) next = cur_frame; // overflow guard
            if (total_frames > 0 && next >= total_frames) next = total_frames - 1;
            audio_seek(next);
            ff_rw_icon_timer = 15; ff_rw_dir = 1;
        } else if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)) {
            uint64_t next = (cur_frame < (uint64_t)seek_speed) ? 0 : cur_frame - (uint64_t)seek_speed;
            audio_seek(next);
            ff_rw_icon_timer = 15; ff_rw_dir = -1;
        }
    }

    if (debounce > 0) debounce--;
    else {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y)) { is_shuffle = !is_shuffle; debounce = 20; }
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B)) { is_paused = !is_paused; debounce = 20; }
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X)) {
            cfg.viz_mode = next_viz_mode(cfg.viz_mode);
            if (cfg.responsive) layout_compute();
            debounce = 20;
        }
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R)) { open_track(is_shuffle && track_count > 0 ? rand()%track_count : current_idx + 1); debounce = 20; }
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L)) { open_track(current_idx - 1); debounce = 20; }
    }

    // 2. Audio Core
    int16_t out_buf[SAMPLES_PER_FRAME * 2] = {0};

    if (decoder && !is_paused) {
        int samples = audio_read_frame(out_buf);
        if (samples == 0) {
            // End of track, go to next
            open_track(is_shuffle && track_count > 0 ? rand()%track_count : current_idx + 1);
        }
    }

    // 3. Visualizer & Audio Batch
    viz_update_levels(out_buf, SAMPLES_PER_FRAME);
    viz_set_audio_for_vu(out_buf, SAMPLES_PER_FRAME);
    audio_batch_cb(out_buf, SAMPLES_PER_FRAME);

    // 4. Rendering Section
    video_clear(cfg.bg_rgb);

    if (cfg.responsive) {
        if (cfg.show_art && art_buffer && layout.art.w > 0 && layout.art.h > 0) {
            for (int y = 0; y < layout.art.h; y++) {
                int src_y = y * art_h_src / layout.art.h;
                for (int x = 0; x < layout.art.w; x++) {
                    int src_x = x * art_w_src / layout.art.w;
                    draw_pixel(layout.art.x + x, layout.art.y + y, art_buffer[src_y * art_w_src + src_x]);
                }
            }
        }

        if (cfg.show_txt && layout.text.w > 0) {
            int right_edge = layout.content_x + layout.content_w;
            int text_w = (int)strlen(display_str) * 8;
            int left_bound = layout.content_x - text_w;
            if (scroll_x > right_edge || scroll_x < left_bound)
                scroll_x = right_edge;
            draw_text(scroll_x, layout.text.y, display_str, cfg.fg_rgb);
            scroll_x--;
        }

        if (cfg.show_viz) {
            viz_draw();
        }

        if (cfg.show_bar && total_frames > 0 && layout.bar.w > 0) {
            float p = (float)cur_frame / total_frames;
            for (int w = 0; w < layout.bar.w; w++) draw_pixel(layout.bar.x + w, layout.bar.y, cfg.bg_rgb | 0x18C3);
            for (int w = 0; w < (int)(p * layout.bar.w); w++) draw_pixel(layout.bar.x + w, layout.bar.y, cfg.fg_rgb);
        }

        if (cfg.show_tim && layout.time.w > 0) {
            int sec = source_rate ? (int)(cur_frame / source_rate) : 0;
            sprintf(time_str, "%02d:%02d", sec / 60, sec % 60);
            int time_x = layout.time.x + (layout.time.w - ((int)strlen(time_str) * 8)) / 2;
            draw_text(time_x, layout.time.y, time_str, cfg.fg_rgb);
        }

        if (cfg.show_ico && layout.icons.w > 0 && layout.icons.h > 0) {
            if (is_shuffle) draw_text(layout.icon_shuffle_x, layout.icons.y, "SHUF", cfg.fg_rgb);
            if (is_paused) draw_text(layout.icon_pause_x, layout.icons.y, "||", cfg.fg_rgb);
            if (ff_rw_icon_timer > 0) {
                draw_text(layout.icon_seek_x, layout.icons.y, (ff_rw_dir > 0) ? ">>" : "<<", cfg.fg_rgb);
                ff_rw_icon_timer--;
            }
        }
    } else {
        if (cfg.show_art && art_buffer) {
            for (int y = 0; y < 80; y++) {
                for (int x = 0; x < 80; x++) {
                    draw_pixel(120 + x, cfg.art_y + y, art_buffer[(y * art_h_src / 80) * art_w_src + (x * art_w_src / 80)]);
                }
            }
        }
        if (cfg.show_txt) {
            draw_text(scroll_x, cfg.txt_y, display_str, cfg.fg_rgb);
            scroll_x--;
            if (scroll_x < -((int)strlen(display_str) * 8)) scroll_x = FB_WIDTH;
        }
        if (cfg.show_viz) {
            viz_draw();
        }
        if (cfg.show_bar && total_frames > 0) {
            float p = (float)cur_frame / (float)total_frames;
            for (int w = 0; w < 200; w++) draw_pixel(60 + w, cfg.bar_y, cfg.bg_rgb | 0x18C3);
            for (int w = 0; w < (int)(p * 200); w++) draw_pixel(60 + w, cfg.bar_y, cfg.fg_rgb);
        }
        if (cfg.show_tim) {
            int sec = source_rate ? (int)(cur_frame / source_rate) : 0;
            sprintf(time_str, "%02d:%02d", sec / 60, sec % 60);
            draw_text(140, cfg.tim_y, time_str, cfg.fg_rgb);
        }
        if (cfg.show_ico) {
            if (is_shuffle) draw_text(20, cfg.ico_y, "SHUF", cfg.fg_rgb);
            if (is_paused) draw_text(280, cfg.ico_y, "||", cfg.fg_rgb);
            if (ff_rw_icon_timer > 0) {
                draw_text(60, cfg.ico_y, (ff_rw_dir > 0) ? ">>" : "<<", cfg.fg_rgb);
                ff_rw_icon_timer--;
            }
        }
    }
    if (video_cb) {
        if (use_xrgb8888) {
            static uint32_t framebuffer_xrgb8888[FB_WIDTH * FB_HEIGHT];
            for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
                uint16_t c = framebuffer[i];
                uint32_t r5 = (c >> 11) & 0x1F;
                uint32_t g6 = (c >> 5) & 0x3F;
                uint32_t b5 = c & 0x1F;

                // Expand 5/6-bit channels to 8-bit with bit replication.
                uint32_t r8 = (r5 << 3) | (r5 >> 2);
                uint32_t g8 = (g6 << 2) | (g6 >> 4);
                uint32_t b8 = (b5 << 3) | (b5 >> 2);
                framebuffer_xrgb8888[i] = (r8 << 16) | (g8 << 8) | b8;
            }
            video_cb(framebuffer_xrgb8888, FB_WIDTH, FB_HEIGHT, FB_WIDTH * 4);
        } else {
            video_cb(framebuffer, FB_WIDTH, FB_HEIGHT, FB_WIDTH * 2);
        }
    }
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    config_declare_variables(cb);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    use_xrgb8888 = false;
    if (!cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        if (cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
            use_xrgb8888 = true;
    }
}

bool retro_load_game(const struct retro_game_info *g) {
    if (!g || !g->path) return false;

    // Free existing tracks before loading new ones
    for (int i = 0; i < track_count; i++) {
        if (tracks[i]) {
            free(tracks[i]);
            tracks[i] = NULL;
        }
    }
    track_count = 0;
    m3u_base_path[0] = '\0';
    char m3u_dir[1024] = {0};

    // Check for M3U extension
    const char* ext = strrchr(g->path, '.');
    if (ext && strcasecmp_simple(ext, ".m3u") == 0) {
        fprintf(stderr, "[MusicCore] Attempting to open M3U: %s\n", g->path);

        FILE *f = fopen(g->path, "rb");
        if (!f) {
            fprintf(stderr, "[MusicCore] Failed to open M3U at %s\n", g->path);
            return false;
        }
        bool m3u_utf16_le = false;
        bool m3u_utf16_be = false;
        detect_m3u_encoding(f, &m3u_utf16_le, &m3u_utf16_be);
        strncpy(m3u_base_path, g->path, 1023);
        m3u_base_path[sizeof(m3u_base_path) - 1] = '\0';
        const char* last = strrchr(g->path, '/');
        if (!last) last = strrchr(g->path, '\\');
        if (last) {
            size_t dir_len = (size_t)(last - g->path);
            if (dir_len >= sizeof(m3u_dir)) dir_len = sizeof(m3u_dir) - 1;
            memcpy(m3u_dir, g->path, dir_len);
            m3u_dir[dir_len] = '\0';
        } else {
            strncpy(m3u_dir, ".", sizeof(m3u_dir) - 1);
            m3u_dir[sizeof(m3u_dir) - 1] = '\0';
        }
        char line[1024];
        while (read_m3u_line(f, line, sizeof(line), m3u_utf16_le, m3u_utf16_be) && track_count < 256) {
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
                trimmed += 7;
                if (strncasecmp_simple(trimmed, "localhost/", 10) == 0 || strncasecmp_simple(trimmed, "localhost\\", 10) == 0)
                    trimmed += 10;
                if (trimmed[0] == '/' && is_drive_letter(trimmed[1]) && trimmed[2] == ':')
                    trimmed++;
                else if (trimmed[0] != '/' && !is_drive_letter(trimmed[0]))
                    file_is_unc = true;
            }

            // Fix backslashes for standard C file handling
            for (int i = 0; trimmed[i]; i++) {
                if (trimmed[i] == '\\') trimmed[i] = '/';
            }
            char resolved[1024];
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
            tracks[track_count++] = strdup(resolved);
        }
        fclose(f);
    } else {
        // Single track logic
        tracks[track_count++] = strdup(g->path);
    }

    if (track_count == 0) return false;

    config_update(environ_cb);
    if (cfg.responsive)
        layout_compute();

    open_track(0);
    return true;
}

void retro_init(void) {
    video_init();
    audio_init();
    srand((unsigned int)time(NULL));
}

void retro_deinit(void) {
    audio_deinit();
    video_deinit();
    metadata_free_art();
    for (int i = 0; i < track_count; i++) free(tracks[i]);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *i) {
    i->library_name = "UltiMedia UGC";
    i->library_version = "17.0";
    i->valid_extensions = "mp3|wav|m3u|ogg|flac";
    i->need_fullpath = true;
}
void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = 60.0;
    info->timing.sample_rate = (double)OUT_RATE;
    info->geometry.base_width = FB_WIDTH;
    info->geometry.base_height = FB_HEIGHT;
    info->geometry.max_width = FB_WIDTH;
    info->geometry.max_height = FB_HEIGHT;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_unload_game(void) {
    audio_close();
    metadata_free_art();
    for (int i = 0; i < track_count; i++) {
        if (tracks[i]) {
            free(tracks[i]);
            tracks[i] = NULL;
        }
    }
    track_count = 0;
}
void retro_reset(void) {}
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
void* retro_get_memory_data(unsigned i) { (void)i; return NULL; }
size_t retro_get_memory_size(unsigned i) { (void)i; return 0; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { (void)t; (void)g; (void)n; return false; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
