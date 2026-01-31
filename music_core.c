#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "libretro.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define STB_IMAGE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_vorbis.c"
#define OUT_RATE 48000
#define SAMPLES_PER_FRAME 800 


// Prototypes
void update_variables(void);
void open_track(int idx);
void draw_pixel(int x, int y, uint16_t color);
void draw_text(int x, int y, const char* txt, uint16_t color);

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
static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

typedef enum { NONE, MP3, WAV, OGG } AudioType;
static AudioType current_type = NONE;
static void *decoder = NULL;
static char *tracks[256];
static int track_count = 0, current_idx = 0;
static int cur_track = 0;
static uint32_t source_rate = 44100;
static double resample_phase = 0.0;
static char m3u_base_path[1024] = {0};
static uint16_t *framebuffer = NULL;
static unsigned fb_width = 320;
static unsigned fb_height = 240;
static uint16_t *art_buffer = NULL;
static int art_w_src = 0, art_h_src = 0;
static int scroll_x = 320, debounce = 0;
static bool is_paused = false, is_shuffle = false;
static float viz_levels[40] = {0};
static float viz_peaks[40] = {0};
static int viz_peak_timers[40] = {0};
static char display_str[256], time_str[32];
static uint64_t total_frames = 0, cur_frame = 0;
static int ff_rw_icon_timer = 0, ff_rw_dir = 0;
// High-performance static buffer for resampling
static int16_t resample_in_buf[SAMPLES_PER_FRAME * 8]; 

struct {
    uint16_t bg_rgb, fg_rgb;
    int art_y, txt_y, viz_y, bar_y, tim_y, ico_y;
    bool show_art, show_txt, show_viz, show_bar, show_tim, show_ico, lcd_on;
    int viz_bands, viz_mode, viz_peak_hold;
    bool viz_gradient, use_filename;
} cfg;

static const uint8_t font8x8[96][8] = {
    {0,0,0,0,0,0,0,0}, {0x18,0x3C,0x3C,0x18,0x18,0x0,0x18,0x0}, {0x6C,0x6C,0x6C,0x0,0x0,0x0,0x0,0x0}, {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x0},
    {0x18,0x7E,0xC0,0x7C,0x6,0xFC,0x18,0x0}, {0x0,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x0}, {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x0}, {0x18,0x18,0x30,0x0,0x0,0x0,0x0,0x0},
    {0xC,0x18,0x30,0x30,0x30,0x18,0xC,0x0}, {0x30,0x18,0xC,0xC,0xC,0x18,0x30,0x0}, {0x0,0x66,0x3C,0xFF,0x3C,0x66,0x0,0x0}, {0x0,0x18,0x18,0x7E,0x18,0x18,0x0,0x0},
    {0x0,0x0,0x0,0x0,0x0,0x18,0x18,0x30}, {0x0,0x0,0x0,0x7E,0x0,0x0,0x0,0x0}, {0x0,0x0,0x0,0x0,0x0,0x18,0x18,0x0}, {0x6,0xC,0x18,0x30,0x60,0xC0,0x80,0x0},
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x0}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x0}, {0x7C,0xC6,0x6,0x3C,0x60,0xC0,0xFE,0x0}, {0x7C,0xC6,0x6,0x3C,0x6,0xC6,0x7C,0x0},
    {0x1C,0x3C,0x6C,0xCC,0xFE,0xC,0x1E,0x0}, {0xFE,0xC0,0xF8,0x6,0x6,0xC6,0x7C,0x0}, {0x38,0x60,0xC0,0xF8,0xC6,0xC6,0x7C,0x0}, {0xFE,0xC6,0xC,0x18,0x30,0x30,0x30,0x0},
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x0}, {0x7C,0xC6,0xC6,0x7E,0x6,0xC,0x78,0x0}, {0x0,0x18,0x18,0x0,0x18,0x18,0x0,0x0}, {0x0,0x18,0x18,0x0,0x18,0x18,0x30,0x0},
    {0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x0}, {0x0,0x0,0x7E,0x0,0x7E,0x0,0x0,0x0}, {0x60,0x30,0x18,0xC,0x18,0x30,0x60,0x0}, {0x7C,0xC6,0xC,0x18,0x18,0x0,0x18,0x0},
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x0}, {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x0}, {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x0}, {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x0},
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x0}, {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x0}, {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x0}, {0x3C,0x66,0xC0,0xCE,0xC6,0x66,0x3E,0x0},
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x0}, {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x0}, {0x1E,0xC,0xC,0xC,0xC,0xCC,0x78,0x0}, {0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x0},
    {0xF0,0x60,0x60,0x60,0x60,0x62,0xFE,0x0}, {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x0}, {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x0}, {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x0},
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x0}, {0x7C,0xC6,0xC6,0xC6,0xDE,0xCC,0x76,0x0}, {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xC6,0x0}, {0x7C,0xC6,0x60,0x38,0x6,0xC6,0x7C,0x0},
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x0}, {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x0}, {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x0}, {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x0},
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x0}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x0}, {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x0}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x0},
    {0xC0,0x60,0x30,0x18,0xC,0x6,0x2,0x0}, {0x3C,0xC,0xC,0xC,0xC,0xC,0x3C,0x0}, {0x10,0x38,0x6C,0xC6,0x0,0x0,0x0,0x0}, {0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xFF},
    {0x30,0x30,0x18,0x0,0x0,0x0,0x0,0x0}, {0x0,0x0,0x78,0xC,0x7C,0xCC,0x76,0x0}, {0xE0,0x60,0x7C,0x66,0x66,0x66,0x7C,0x0}, {0x0,0x0,0x7C,0xC6,0xC0,0xC6,0x7C,0x0},
    {0x1C,0xC,0x7C,0xCC,0xCC,0xCC,0x76,0x0}, {0x0,0x0,0x7C,0xC6,0xFE,0xC0,0x7C,0x0}, {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x0}, {0x0,0x0,0x76,0xCC,0xCC,0x7C,0xC,0xF8},
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0x66,0x0}, {0x18,0x0,0x38,0x18,0x18,0x18,0x3C,0x0}, {0x6,0x0,0x6,0x6,0x6,0x66,0x3C,0x0}, {0xE0,0x60,0x66,0x6C,0x78,0x6C,0x66,0x0},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x0}, {0x0,0x0,0xEC,0xFE,0xD6,0xD6,0xD6,0x0}, {0x0,0x0,0xDC,0x66,0x66,0x66,0x66,0x0}, {0x0,0x0,0x7C,0xC6,0xC6,0xC6,0x7C,0x0},
    {0x0,0x0,0x7C,0x66,0x66,0x7C,0x60,0xF0}, {0x0,0x0,0x76,0xCC,0xCC,0x7C,0xC,0x1E}, {0x0,0x0,0xDC,0x76,0x60,0x60,0xF0,0x0}, {0x0,0x0,0x7E,0xC0,0x7C,0x6,0xFC,0x0},
    {0x30,0x30,0xFC,0x30,0x30,0x34,0x18,0x0}, {0x0,0x0,0xCC,0xCC,0xCC,0xCC,0x76,0x0}, {0x0,0x0,0xC6,0xC6,0xC6,0x6C,0x38,0x0}, {0x0,0x0,0xC6,0xD6,0xFE,0xEE,0xC6,0x0},
    {0x0,0x0,0xC6,0x6C,0x38,0x6C,0xC6,0x0}, {0x0,0x0,0xC6,0xC6,0xC6,0x7E,0x6,0xFC}, {0x0,0x0,0xFE,0x6C,0x38,0x64,0xFE,0x0}, {0xE,0x18,0x18,0x70,0x18,0x18,0xE,0x0},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x0}, {0x70,0x18,0x18,0xE,0x18,0x18,0x70,0x0}, {0x76,0xDC,0x0,0x0,0x0,0x0,0x0,0x0}, {0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0}
};

void draw_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < 320 && y >= 0 && y < 240) framebuffer[y * 320 + x] = color;
}

void draw_text(int x, int y, const char* txt, uint16_t color) {
    while (*txt) {
        uint8_t c = (*txt++) - 32;
        if (c < 96) {
            for (int gy = 0; gy < 8; gy++)
                for (int gx = 0; gx < 8; gx++)
                    if (font8x8[c][gy] & (0x80 >> gx)) draw_pixel(x + gx, y + gy, color);
        }
        x += 8;
    }
}

uint16_t get_gradient_color(float level) {
    if (level < 0.5f) {
        // Green (0,255,0) → Yellow (255,255,0)
        float t = level * 2.0f;
        uint8_t r = (uint8_t)(t * 255);
        uint8_t g = 255;
        uint8_t b = 0;
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    } else {
        // Yellow (255,255,0) → Red (255,0,0)
        float t = (level - 0.5f) * 2.0f;
        uint8_t r = 255;
        uint8_t g = (uint8_t)((1.0f - t) * 255);
        uint8_t b = 0;
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
}

// Parse ID3v2 tags from file, returns 1 if found
static int parse_id3v2(const char* path, char* artist, char* title, char* album, int maxlen) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    // 1. Read and validate header
    unsigned char hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) {
        fclose(f);
        return 0;
    }

    uint8_t version = hdr[3];  // 2, 3, or 4
    uint8_t flags = hdr[5];

    // 2. Parse syncsafe tag size
    uint32_t tag_size = ((uint32_t)hdr[6] << 21) | ((uint32_t)hdr[7] << 14) |
                        ((uint32_t)hdr[8] << 7) | hdr[9];

    // 3. Read tag data (limit to 64KB for safety)
    if (tag_size > 65536) tag_size = 65536;
    unsigned char* data = malloc(tag_size);
    if (!data) { fclose(f); return 0; }
    size_t bytes_read = fread(data, 1, tag_size, f);
    fclose(f);

    // 4. Skip extended header if present
    size_t pos = 0;
    if (flags & 0x40) {  // Extended header flag
        uint32_t ext_size;
        if (version == 4) {
            // ID3v2.4: syncsafe size
            ext_size = ((uint32_t)data[0] << 21) | ((uint32_t)data[1] << 14) |
                       ((uint32_t)data[2] << 7) | data[3];
        } else {
            // ID3v2.3: regular size
            ext_size = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                       ((uint32_t)data[2] << 8) | data[3];
        }
        pos = ext_size;
    }

    // 5. Scan frames
    while (pos + 6 < bytes_read) {
        char frame_id[5] = {0};
        uint32_t frame_size;
        int header_size;

        if (version == 2) {
            // ID3v2.2: 3-byte frame ID, 3-byte size (big-endian)
            if (data[pos] == 0) break;  // Padding
            frame_id[0] = data[pos]; frame_id[1] = data[pos+1]; frame_id[2] = data[pos+2];
            frame_size = ((uint32_t)data[pos+3] << 16) | ((uint32_t)data[pos+4] << 8) | data[pos+5];
            header_size = 6;
        } else {
            // ID3v2.3/2.4: 4-byte frame ID, 4-byte size, 2-byte flags
            if (pos + 10 > bytes_read || data[pos] == 0) break;
            frame_id[0] = data[pos]; frame_id[1] = data[pos+1];
            frame_id[2] = data[pos+2]; frame_id[3] = data[pos+3];

            if (version == 4) {
                // ID3v2.4: syncsafe frame size
                frame_size = ((uint32_t)data[pos+4] << 21) | ((uint32_t)data[pos+5] << 14) |
                             ((uint32_t)data[pos+6] << 7) | data[pos+7];
            } else {
                // ID3v2.3: regular big-endian size
                frame_size = ((uint32_t)data[pos+4] << 24) | ((uint32_t)data[pos+5] << 16) |
                             ((uint32_t)data[pos+6] << 8) | data[pos+7];
            }
            header_size = 10;
        }

        if (frame_size == 0 || pos + header_size + frame_size > bytes_read) break;

        unsigned char* content = &data[pos + header_size];
        uint8_t encoding = content[0];
        char* text = (char*)&content[1];
        int text_len = frame_size - 1;

        // 6. Match frame ID (v2.2 uses 3-char IDs, v2.3/2.4 use 4-char)
        char* dest = NULL;
        if (strcmp(frame_id, "TIT2") == 0 || strcmp(frame_id, "TT2") == 0) dest = title;
        else if (strcmp(frame_id, "TPE1") == 0 || strcmp(frame_id, "TP1") == 0) dest = artist;
        else if (strcmp(frame_id, "TALB") == 0 || strcmp(frame_id, "TAL") == 0) dest = album;

        if (dest && text_len > 0) {
            if (encoding == 0 || encoding == 3) {
                // Latin-1 or UTF-8: direct copy
                int len = (text_len < maxlen-1) ? text_len : maxlen-1;
                memcpy(dest, text, len);
                dest[len] = '\0';
            } else if (encoding == 1 || encoding == 2) {
                // UTF-16: extract ASCII chars (skip BOM for encoding 1)
                int j = 0;
                int start = (encoding == 1 && text_len >= 2) ? 2 : 0;
                for (int i = start; i < text_len - 1 && j < maxlen - 1; i += 2) {
                    // Handle both little-endian and big-endian UTF-16
                    char c = (text[i+1] == 0) ? text[i] : ((text[i] == 0) ? text[i+1] : 0);
                    if (c >= 32) dest[j++] = c;
                }
                dest[j] = '\0';
            }
        }

        pos += header_size + frame_size;
    }

    free(data);
    return (title[0] || artist[0]) ? 1 : 0;
}

void open_track(int idx) {
    if (track_count == 0) return;

    // 1. Cleanup previous decoder
    if (decoder) {
        if (current_type == MP3) drmp3_uninit((drmp3*)decoder);
        else if (current_type == WAV) drwav_uninit((drwav*)decoder);
        else if (current_type == OGG) stb_vorbis_close((stb_vorbis*)decoder);
        if (current_type != OGG) free(decoder);
        decoder = NULL;
    }
    
    // 2. Select new track
    current_idx = (idx + track_count) % track_count;
    const char *p = tracks[current_idx];
    
    // 3. Reset Metadata
    char meta_title[32] = {0};
    char meta_artist[32] = {0};
    char cur_album[32] = {0};
    
    // 4. Initialize Decoder (WITH ERROR CHECKING)
    bool load_success = false;
    const char* ext = strrchr(p, '.');
    if (ext && strcasecmp_simple(ext, ".mp3") == 0) {
        decoder = malloc(sizeof(drmp3));
        if (decoder && drmp3_init_file((drmp3*)decoder, p, NULL)) {
            current_type = MP3; source_rate = ((drmp3*)decoder)->sampleRate;
            total_frames = ((drmp3*)decoder)->totalPCMFrameCount; load_success = true;
        }
    } else if (ext && strcasecmp_simple(ext, ".ogg") == 0) {
        int err = 0; stb_vorbis* ogg = stb_vorbis_open_filename(p, &err, NULL);
        if (ogg) {
            current_type = OGG; decoder = ogg; load_success = true;
            stb_vorbis_info info = stb_vorbis_get_info(ogg);
            source_rate = info.sample_rate; total_frames = stb_vorbis_stream_length_in_samples(ogg);
        }
    } else {
        decoder = malloc(sizeof(drwav));
        if (decoder && drwav_init_file((drwav*)decoder, p, NULL)) {
            current_type = WAV; source_rate = ((drwav*)decoder)->sampleRate;
            total_frames = ((drwav*)decoder)->totalPCMFrameCount; load_success = true;
        }
    }

    // 5. Handle Failure
    if (!load_success) {
        if (decoder) { free(decoder); decoder = NULL; }
        snprintf(display_str, sizeof(display_str), "ERROR LOADING: %.230s", p); // Show error on screen
        return;
    }
resample_phase = 0.0;

    // 6. Reset Playback State
    cur_frame = 0;
    
    // 7. Load Metadata - Try ID3v2 first, fall back to ID3v1 (unless filename-only mode)
    if (!cfg.use_filename) {
        if (!parse_id3v2(p, meta_artist, meta_title, cur_album, 31)) {
            // Fall back to ID3v1
            FILE* f = fopen(p, "rb");
            if (f) {
                fseek(f, -128, SEEK_END); char tag[3]; size_t r = fread(tag, 1, 3, f);
                if (r == 3 && strncmp(tag, "TAG", 3) == 0) {
                    fread(meta_title, 1, 30, f);
                    fread(meta_artist, 1, 30, f);
                    fread(cur_album, 1, 30, f);
                }
                fclose(f);
            }
        }
    }
    
    // Clean strings
    for(int i=29; i>=0; i--) { if(meta_title[i]<32) meta_title[i]=0; else break; }
    for(int i=29; i>=0; i--) { if(meta_artist[i]<32) meta_artist[i]=0; else break; }
    for(int i=29; i>=0; i--) { if(cur_album[i]<32) cur_album[i]=0; else break; }

    // Set Display String
    if (meta_title[0] != 0 && meta_artist[0] != 0)
        sprintf(display_str, "%s - %s   ", meta_artist, meta_title);
    else if (meta_title[0] != 0)
        sprintf(display_str, "%s   ", meta_title);
    else {
        const char* b = strrchr(p, '/'); if(!b) b=strrchr(p, '\\');
        strncpy(display_str, b ? b + 1 : p, 250);
        display_str[250] = '\0';
        strcat(display_str, "   ");
    }
    scroll_x = 320;

    // --- 8. Load Artwork (The 5 Location Search) ---
    if (art_buffer) { free(art_buffer); art_buffer = NULL; }
    unsigned char* img_data = NULL;
    char path_buf[1024];
    const char* exts[] = { ".jpg", ".jpeg", ".png", ".bmp" };

    // A. Setup Directory Strings for the Music File
    char music_dir[1024] = {0}, parent_name[256] = {0};
    const char* last_s = strrchr(p, '/'); if(!last_s) last_s = strrchr(p, '\\');
    if (last_s) {
        size_t dir_len = last_s - p;
        strncpy(music_dir, p, dir_len);
        music_dir[dir_len] = '\0';
        const char* p_slash = strrchr(music_dir, '/'); if(!p_slash) p_slash = strrchr(music_dir, '\\');
        strncpy(parent_name, p_slash ? p_slash + 1 : music_dir, sizeof(parent_name) - 1);
        parent_name[sizeof(parent_name) - 1] = '\0';
    }

    // B. Main Search Loop
    for (int i = 0; i < 4 && !img_data; i++) {
        // 1. Same name as MP3 (e.g., C:/Music/Song.jpg)
        strcpy(path_buf, p); char* dot = strrchr(path_buf, '.');
        if (dot) strcpy(dot, exts[i]); else strcat(path_buf, exts[i]);
        img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
        if (img_data) break;

        if (music_dir[0]) {
            // 2. Name of Parent Folder (e.g., C:/Music/AlbumName/AlbumName.jpg)
            sprintf(path_buf, "%s/%s%s", music_dir, parent_name, exts[i]);
            img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
            if (img_data) break;
            
            // 3. Album Name from Metadata (e.g., C:/Music/AlbumName/MetadataAlbum.jpg)
            if (cur_album[0]) {
                sprintf(path_buf, "%s/%s%s", music_dir, cur_album, exts[i]);
                img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
                if (img_data) break;
            }
        }

        // 4. Same name as M3U file (e.g., if playlist is Playlist.m3u, looks for Playlist.jpg)
        if (m3u_base_path[0]) {
            strcpy(path_buf, m3u_base_path);
            char* m3u_dot = strrchr(path_buf, '.');
            if (m3u_dot) strcpy(m3u_dot, exts[i]); else strcat(path_buf, exts[i]);
            img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
            if (img_data) break;
        }
    }

    // 5. Files Metadata (Aggressive APIC/PIC Scan)
    if (!img_data) {
        FILE* f_art = fopen(p, "rb");
        if (f_art) {
            // Scan 1MB: embedded art is often large and offset deep in the header
            size_t scan_size = 1024 * 1024; 
            unsigned char* head = malloc(scan_size);
            if (head) {
                size_t bytes_read = fread(head, 1, scan_size, f_art);
                
                // First: Look for 'APIC' or 'PIC' (ID3v2 Picture Frames)
                // If we find the frame, we know an image is nearby.
                for (size_t i = 0; i + 10 < bytes_read; i++) {
                    // Check for JPEG (FF D8 FF)
                    if (head[i] == 0xFF && head[i+1] == 0xD8 && head[i+2] == 0xFF) {
                        img_data = stbi_load_from_memory(head + i, (int)(bytes_read - i), &art_w_src, &art_h_src, NULL, 3);
                        if (img_data) break;
                    }
                    // Check for PNG (89 50 4E 47)
                    if (head[i] == 0x89 && head[i+1] == 0x50 && head[i+2] == 0x4E && head[i+3] == 0x47) {
                        img_data = stbi_load_from_memory(head + i, (int)(bytes_read - i), &art_w_src, &art_h_src, NULL, 3);
                        if (img_data) break;
                    }
                }
                free(head);
            }
            fclose(f_art);
        }
    }

    // Prepare for Rendering (RGB565)
    if (img_data) {
        if (art_w_src > 0 && art_h_src > 0 && art_w_src <= 4096 && art_h_src <= 4096) {
            size_t art_size = (size_t)art_w_src * art_h_src * 2;
            art_buffer = malloc(art_size);
            if (art_buffer) {
                for (int i = 0; i < art_w_src * art_h_src; i++) {
                    uint8_t r = img_data[i*3], g = img_data[i*3+1], b = img_data[i*3+2];
                    art_buffer[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                }
            }
        }
        stbi_image_free(img_data);
    }
}
void retro_run(void) {
    update_variables();
    input_poll_cb();

    // 1. Handle Inputs
    if (decoder && !is_paused) {
        int seek_speed = source_rate * 3;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
            cur_frame = (cur_frame + seek_speed >= total_frames) ? total_frames - 1 : cur_frame + seek_speed;
            if (current_type == MP3) drmp3_seek_to_pcm_frame((drmp3*)decoder, cur_frame);
            else if (current_type == WAV) drwav_seek_to_pcm_frame((drwav*)decoder, cur_frame);
            else if (current_type == OGG) stb_vorbis_seek((stb_vorbis*)decoder, cur_frame);
            ff_rw_icon_timer = 15; ff_rw_dir = 1;
        } else if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)) {
            cur_frame = (cur_frame < (uint64_t)seek_speed) ? 0 : cur_frame - seek_speed;
            if (current_type == MP3) drmp3_seek_to_pcm_frame((drmp3*)decoder, cur_frame);
            else if (current_type == WAV) drwav_seek_to_pcm_frame((drwav*)decoder, cur_frame);
            else if (current_type == OGG) stb_vorbis_seek((stb_vorbis*)decoder, cur_frame);
            ff_rw_icon_timer = 15; ff_rw_dir = -1;
        }
    }

    if (debounce > 0) debounce--;
    else {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y)) { is_shuffle = !is_shuffle; debounce = 20; }
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)) { is_paused = !is_paused; debounce = 20; }
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R)) { open_track(is_shuffle ? rand()%track_count : current_idx + 1); debounce = 20; }
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L)) { open_track(current_idx - 1); debounce = 20; }
    }

    // 2. Audio Core
int16_t out_buf[SAMPLES_PER_FRAME * 2] = {0};

if (decoder && !is_paused) {
    double ratio = (double)source_rate / (double)OUT_RATE;
    uint32_t needed = (uint32_t)(SAMPLES_PER_FRAME * ratio) + 2;
    if (needed > SAMPLES_PER_FRAME * 8) needed = SAMPLES_PER_FRAME * 8;

    memset(resample_in_buf, 0, sizeof(resample_in_buf));
    uint64_t read = 0;
    int channels = 2;

    if (current_type == MP3) {
        read = drmp3_read_pcm_frames_s16((drmp3*)decoder, needed, resample_in_buf);
        channels = ((drmp3*)decoder)->channels;
    } else if (current_type == WAV) {
        read = drwav_read_pcm_frames_s16((drwav*)decoder, needed, resample_in_buf);
        channels = ((drwav*)decoder)->channels;
    } else if (current_type == OGG) {
        read = stb_vorbis_get_samples_short_interleaved((stb_vorbis*)decoder, 2, resample_in_buf, needed * 2);
        channels = 2;
    }

    if (read < 2 || (read < needed && cur_frame > 1000)) { 
        open_track(is_shuffle ? rand()%track_count : current_idx + 1); 
    } else {
        for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
            double src_pos = resample_phase + i * ratio;
            int i1 = (int)src_pos;
            int i2 = i1 + 1;

            // --- CLAMP INDICES ---
            if (i1 < 0) i1 = 0;
            if (i2 < 0) i2 = 0;
            if (i1 >= (int)read) i1 = (int)read - 1;
            if (i2 >= (int)read) i2 = i1;

            float frac = (float)(src_pos - i1);

            if (channels == 2) {
                out_buf[i*2]   = (int16_t)((1.0f - frac) * resample_in_buf[i1*2]   + frac * resample_in_buf[i2*2]);
                out_buf[i*2+1] = (int16_t)((1.0f - frac) * resample_in_buf[i1*2+1] + frac * resample_in_buf[i2*2+1]);
            } else {
                int16_t s = (int16_t)((1.0f - frac) * resample_in_buf[i1] + frac * resample_in_buf[i2]);
                out_buf[i*2] = out_buf[i*2+1] = s;
            }
        }

        // --- WRAP PHASE INSTEAD OF GROWING FOREVER ---
        resample_phase += SAMPLES_PER_FRAME * ratio;
        resample_phase -= (int)resample_phase;  // Keep fractional remainder only

        cur_frame += (uint64_t)(SAMPLES_PER_FRAME * ratio);
    }
}


    // 3. Visualizer & Audio Batch
    int sample_stride = (cfg.viz_bands == 40) ? 20 : 40;
    int band_count = cfg.viz_bands;
    for(int i=0; i<band_count; i++) {
        float p = abs(out_buf[i * sample_stride]) / 32768.0f;

        // Update current level with decay
        if (p > viz_levels[i]) viz_levels[i] = p;
        else viz_levels[i] *= 0.85f;

        // Update peak hold
        if (p > viz_peaks[i]) {
            viz_peaks[i] = p;
            viz_peak_timers[i] = cfg.viz_peak_hold;
        } else if (viz_peak_timers[i] > 0) {
            viz_peak_timers[i]--;
        } else {
            viz_peaks[i] *= 0.95f;
        }
    }
    audio_batch_cb(out_buf, SAMPLES_PER_FRAME);

    // 4. Rendering Section
    for (int i = 0; i < 320 * 240; i++) framebuffer[i] = cfg.bg_rgb;

    if (cfg.show_art && art_buffer) {
        for(int y=0; y<80; y++) for(int x=0; x<80; x++)
            draw_pixel(120+x, cfg.art_y+y, art_buffer[(y*art_h_src/80)*art_w_src+(x*art_w_src/80)]);
    }
    if (cfg.show_txt) {
        draw_text(scroll_x, cfg.txt_y, display_str, cfg.fg_rgb);
        scroll_x--; if (scroll_x < -((int)strlen(display_str)*8)) scroll_x = 320;
    }
    if (cfg.show_viz) {
        int band_count = cfg.viz_bands;

        if (cfg.viz_mode == 0) {  // Bars mode
            int start_x = (band_count == 40) ? 60 : 100;
            int bar_width = (band_count == 40) ? 2 : 4;
            int spacing = (band_count == 40) ? 4 : 6;

            for (int i = 0; i < band_count; i++) {
                int h = (int)(viz_levels[i] * 35);
                int x_base = start_x + (i * spacing);

                // Draw main bar
                for (int v = 0; v < h; v++) {
                    uint16_t color = cfg.viz_gradient ? get_gradient_color((float)v / 35.0f) : cfg.fg_rgb;
                    for (int w = 0; w < bar_width; w++) draw_pixel(x_base + w, cfg.viz_y - v, color);
                }

                // Draw peak hold dot
                if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
                    int peak_h = (int)(viz_peaks[i] * 35);
                    uint16_t peak_color = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
                    for (int w = 0; w < bar_width; w++) {
                        draw_pixel(x_base + w, cfg.viz_y - peak_h, peak_color);
                        if (peak_h + 1 < 35) draw_pixel(x_base + w, cfg.viz_y - peak_h - 1, peak_color);
                    }
                }
            }
        } else if (cfg.viz_mode == 1) {  // Dots mode
            int start_x = (band_count == 40) ? 100 : 130;
            int spacing = (band_count == 40) ? 3 : 4;

            for (int i = 0; i < band_count; i++) {
                int h = (int)(viz_levels[i] * 50);
                int x = start_x + (i * spacing);
                uint16_t color = cfg.viz_gradient ? get_gradient_color(viz_levels[i]) : cfg.fg_rgb;

                // Draw 2x2 dot
                draw_pixel(x, cfg.viz_y - h, color);
                draw_pixel(x + 1, cfg.viz_y - h, color);
                draw_pixel(x, cfg.viz_y - h - 1, color);
                draw_pixel(x + 1, cfg.viz_y - h - 1, color);

                // Peak dot
                if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
                    int peak_h = (int)(viz_peaks[i] * 50);
                    uint16_t peak_color = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
                    draw_pixel(x, cfg.viz_y - peak_h, peak_color);
                    draw_pixel(x + 1, cfg.viz_y - peak_h, peak_color);
                }
            }
        } else if (cfg.viz_mode == 2) {  // Line Graph mode
            int start_x = (band_count == 40) ? 60 : 100;
            int spacing = (band_count == 40) ? 4 : 6;

            for (int i = 0; i < band_count; i++) {
                int h = (int)(viz_levels[i] * 40);
                int x = start_x + (i * spacing);
                uint16_t color = cfg.viz_gradient ? get_gradient_color(viz_levels[i]) : cfg.fg_rgb;

                // Draw vertical line
                for (int v = 0; v <= h; v++) draw_pixel(x, cfg.viz_y - v, color);

                // Connect to next band
                if (i < band_count - 1) {
                    int next_h = (int)(viz_levels[i + 1] * 40);
                    int next_x = start_x + ((i + 1) * spacing);
                    int dx = next_x - x;
                    int dy = next_h - h;

                    for (int step = 0; step < dx; step++) {
                        int interp_y = h + (dy * step) / dx;
                        uint16_t interp_color = cfg.viz_gradient ? get_gradient_color((float)interp_y / 40.0f) : cfg.fg_rgb;
                        draw_pixel(x + step, cfg.viz_y - interp_y, interp_color);
                    }
                }

                // Peak markers
                if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
                    int peak_h = (int)(viz_peaks[i] * 40);
                    draw_pixel(x, cfg.viz_y - peak_h, 0xF800);
                    draw_pixel(x + 1, cfg.viz_y - peak_h, 0xF800);
                }
            }
        } else if (cfg.viz_mode == 3) {  // VU Meter mode
            // Calculate L/R levels from stereo mix
            float left_level = 0, right_level = 0;
            for (int i = 0; i < SAMPLES_PER_FRAME; i += 8) {
                float l = abs(out_buf[i*2]) / 32768.0f;
                float r = abs(out_buf[i*2+1]) / 32768.0f;
                if (l > left_level) left_level = l;
                if (r > right_level) right_level = r;
            }

            // Smooth with decay
            if (left_level > viz_levels[0]) viz_levels[0] = left_level;
            else viz_levels[0] *= 0.85f;
            if (right_level > viz_levels[1]) viz_levels[1] = right_level;
            else viz_levels[1] *= 0.85f;

            // Peak tracking for L/R
            if (viz_levels[0] > viz_peaks[0]) {
                viz_peaks[0] = viz_levels[0];
                viz_peak_timers[0] = cfg.viz_peak_hold;
            } else if (viz_peak_timers[0] > 0) {
                viz_peak_timers[0]--;
            }
            if (viz_levels[1] > viz_peaks[1]) {
                viz_peaks[1] = viz_levels[1];
                viz_peak_timers[1] = cfg.viz_peak_hold;
            } else if (viz_peak_timers[1] > 0) {
                viz_peak_timers[1]--;
            }

            // Draw Left meter
            draw_text(80, cfg.viz_y - 15, "L", cfg.fg_rgb);
            int left_w = (int)(viz_levels[0] * 180);
            for (int x = 0; x < left_w; x++) {
                uint16_t color = cfg.viz_gradient ? get_gradient_color((float)x / 180.0f) : cfg.fg_rgb;
                for (int y = 0; y < 4; y++) draw_pixel(95 + x, cfg.viz_y - 15 + y, color);
            }
            if (cfg.viz_peak_hold > 0 && viz_peak_timers[0] > 0) {
                int peak_x = (int)(viz_peaks[0] * 180);
                for (int y = 0; y < 4; y++) draw_pixel(95 + peak_x, cfg.viz_y - 15 + y, 0xF800);
            }

            // Draw Right meter
            draw_text(80, cfg.viz_y - 5, "R", cfg.fg_rgb);
            int right_w = (int)(viz_levels[1] * 180);
            for (int x = 0; x < right_w; x++) {
                uint16_t color = cfg.viz_gradient ? get_gradient_color((float)x / 180.0f) : cfg.fg_rgb;
                for (int y = 0; y < 4; y++) draw_pixel(95 + x, cfg.viz_y - 5 + y, color);
            }
            if (cfg.viz_peak_hold > 0 && viz_peak_timers[1] > 0) {
                int peak_x = (int)(viz_peaks[1] * 180);
                for (int y = 0; y < 4; y++) draw_pixel(95 + peak_x, cfg.viz_y - 5 + y, 0xF800);
            }
        }
    }
    if (cfg.show_bar && total_frames > 0) {
        float p = (float)cur_frame / total_frames;
        for (int w = 0; w < 200; w++) draw_pixel(60 + w, cfg.bar_y, cfg.bg_rgb | 0x18C3);
        for (int w = 0; w < (int)(p * 200); w++) draw_pixel(60 + w, cfg.bar_y, cfg.fg_rgb);
    }
    if (cfg.show_tim) {
        int sec = cur_frame / source_rate;
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
    if (cfg.lcd_on) {
        for(int y=0; y<240; y+=2) for(int x=0; x<320; x++) framebuffer[y*320+x] = (framebuffer[y*320+x] >> 1) & 0x7BEF;
    }

    video_cb(framebuffer, 320, 240, 640);
}
void update_variables(void) {
    struct retro_variable var = {0};
    int r, g, b;
    var.key = "media_bg_r"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) r = atoi(var.value); else r=0;
    var.key = "media_bg_g"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) g = atoi(var.value); else g=64;
    var.key = "media_bg_b"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) b = atoi(var.value); else b=0;
    cfg.bg_rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    var.key = "media_fg_r"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) r = atoi(var.value); else r=0;
    var.key = "media_fg_g"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) g = atoi(var.value); else g=255;
    var.key = "media_fg_b"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) b = atoi(var.value); else b=0;
    cfg.fg_rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    var.key = "media_show_art"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_art = !strcmp(var.value, "On"); else cfg.show_art = true;
    var.key = "media_show_txt"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_txt = !strcmp(var.value, "On"); else cfg.show_txt = true;
    var.key = "media_show_viz"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_viz = !strcmp(var.value, "On"); else cfg.show_viz = true;
    var.key = "media_show_bar"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_bar = !strcmp(var.value, "On"); else cfg.show_bar = true;
    var.key = "media_show_tim"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_tim = !strcmp(var.value, "On"); else cfg.show_tim = true;
    var.key = "media_show_ico"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_ico = !strcmp(var.value, "On"); else cfg.show_ico = true;
    var.key = "media_lcd"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.lcd_on = !strcmp(var.value, "On"); else cfg.lcd_on = true;

    var.key = "media_art_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.art_y = atoi(var.value); else cfg.art_y = 40;
    var.key = "media_txt_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.txt_y = atoi(var.value); else cfg.txt_y = 150;
    var.key = "media_viz_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_y = atoi(var.value); else cfg.viz_y = 140;
    var.key = "media_bar_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.bar_y = atoi(var.value); else cfg.bar_y = 180;
    var.key = "media_tim_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.tim_y = atoi(var.value); else cfg.tim_y = 190;
    var.key = "media_ico_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.ico_y = atoi(var.value); else cfg.ico_y = 20;

    var.key = "media_viz_bands"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_bands = atoi(var.value); else cfg.viz_bands = 40;
    var.key = "media_viz_mode"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) {
        if (!strcmp(var.value, "Bars")) cfg.viz_mode = 0;
        else if (!strcmp(var.value, "VU Meter")) cfg.viz_mode = 3;
        else if (!strcmp(var.value, "Dots")) cfg.viz_mode = 1;
        else if (!strcmp(var.value, "Line")) cfg.viz_mode = 2;
    } else cfg.viz_mode = 0;
    var.key = "media_viz_gradient"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_gradient = !strcmp(var.value, "On"); else cfg.viz_gradient = true;
    var.key = "media_viz_peak_hold"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_peak_hold = atoi(var.value); else cfg.viz_peak_hold = 30;
    var.key = "media_use_filename"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.use_filename = !strcmp(var.value, "On"); else cfg.use_filename = false;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    static const struct retro_variable vars[] = {
        { "media_bg_r", "BG Red; 0|32|64|128|255" }, { "media_bg_g", "BG Green; 64|0|32|128|255" }, { "media_bg_b", "BG Blue; 0|32|64|128|255" },
        { "media_fg_r", "FG Red; 0|32|64|128|255" }, { "media_fg_g", "FG Green; 255|0|32|64|128" }, { "media_fg_b", "FG Blue; 0|32|64|128|255" },
        { "media_show_art", "Show Art; On|Off" }, { "media_show_txt", "Show Scroll Text; On|Off" },
        { "media_show_viz", "Show Visualizer; On|Off" }, { "media_show_bar", "Show Progress Bar; On|Off" },
        { "media_show_tim", "Show Time; On|Off" }, { "media_show_ico", "Show Icons; On|Off" },
        { "media_art_y", "Art Y; 40|0|80|120" }, { "media_txt_y", "Text Y; 150|20|120|200" },
        { "media_viz_y", "Viz Y; 140|80|200" }, { "media_bar_y", "Bar Y; 180|100|210" },
        { "media_tim_y", "Time Y; 190|110|220" }, { "media_ico_y", "Icon Y; 20|50|200" },
        { "media_lcd", "LCD Effect; On|Off" },
        { "media_viz_bands", "Viz Bands; 40|20" },
        { "media_viz_mode", "Viz Mode; Bars|VU Meter|Dots|Line" },
        { "media_viz_gradient", "Viz Gradient; On|Off" },
        { "media_viz_peak_hold", "Peak Hold; 30|0|15|45|60" },
        { "media_use_filename", "Show Filename Only; Off|On" },
        { NULL, NULL }
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565; cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

bool retro_load_game(const struct retro_game_info *g) {
    if (!g || !g->path) return false;

    // 1. Capture the path as provided by EmuVR/RetroArch
   
    
    track_count = 0;

    // 2. Check for M3U extension
    const char* ext = strrchr(g->path, '.');
    if (ext && strcasecmp_simple(ext, ".m3u") == 0) {
        // Log the path to help you see what EmuVR is actually passing
        // (You'll see this in the RetroArch log now)
        fprintf(stderr, "[MusicCore] Attempting to open M3U: %s\n", g->path);

        FILE *f = fopen(g->path, "r");
        
        // If it fails, EmuVR might be using a relative path that needs resolving
        if (!f) {
            fprintf(stderr, "[MusicCore] Failed to open M3U at %s\n", g->path);
            return false;
        }
 strncpy(m3u_base_path, g->path, 1023);
        m3u_base_path[sizeof(m3u_base_path) - 1] = '\0';
        char line[1024];
        while (fgets(line, sizeof(line), f) && track_count < 256) {
            // Clean the line aggressively
            line[strcspn(line, "\r\n")] = 0;
            
            // Trim leading/trailing spaces
            char* trimmed = line;
            while(*trimmed == ' ' || *trimmed == '\t') trimmed++;
            char* end = trimmed + strlen(trimmed) - 1;
            while(end >= trimmed && (*end == ' ' || *end == '\t' || *end == '\r')) {
                *end = '\0';
                end--;
            }

            if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

            // Fix backslashes for standard C file handling
            for (int i = 0; trimmed[i]; i++) {
                if (trimmed[i] == '\\') trimmed[i] = '/';
            }

            tracks[track_count++] = strdup(trimmed);
        }
        fclose(f);
    } else {
        // Single track logic
        tracks[track_count++] = strdup(g->path);
    }

    if (track_count == 0) return false;

    cur_track = 0;
    open_track(cur_track);
    return true;
}

void retro_init(void) {
    framebuffer = malloc(fb_width * fb_height * sizeof(uint16_t));
    if (framebuffer) {
        memset(framebuffer, 0, fb_width * fb_height * sizeof(uint16_t));
    }
    srand((unsigned int)time(NULL));
}
void retro_deinit(void) {
    if (decoder) {
        if (current_type == MP3) drmp3_uninit((drmp3*)decoder);
        else if (current_type == WAV) drwav_uninit((drwav*)decoder);
        else if (current_type == OGG) stb_vorbis_close((stb_vorbis*)decoder);
        if (current_type != OGG) free(decoder);
        decoder = NULL;
    }
    free(framebuffer);
    if(art_buffer) free(art_buffer);
    for(int i=0; i<track_count; i++) free(tracks[i]);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *i) { i->library_name="UltiMedia UGC"; i->library_version="15.0"; i->valid_extensions="mp3|wav|m3u|ogg"; i->need_fullpath=true; }
void retro_get_system_av_info(struct retro_system_av_info *info) {
    // Default values until a file is loaded
    info->timing.fps = 60.0;
    info->timing.sample_rate = (double)OUT_RATE;
    info->geometry.base_width = fb_width;
    info->geometry.base_height = fb_height;
    info->geometry.max_width = fb_width;
    info->geometry.max_height = fb_height;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}
void retro_set_audio_sample(retro_audio_sample_t cb) {}
void retro_unload_game() {
    if (decoder) {
        if (current_type == MP3) drmp3_uninit((drmp3*)decoder);
        else if (current_type == WAV) drwav_uninit((drwav*)decoder);
        else if (current_type == OGG) stb_vorbis_close((stb_vorbis*)decoder);
        if (current_type != OGG) free(decoder);
        decoder = NULL;
    }
    if (art_buffer) {
        free(art_buffer);
        art_buffer = NULL;
    }
    for (int i = 0; i < track_count; i++) {
        if (tracks[i]) {
            free(tracks[i]);
            tracks[i] = NULL;
        }
    }
    track_count = 0;
}
void retro_reset() {}
size_t retro_serialize_size() { return 0; }
bool retro_serialize(void *d, size_t s) { return false; }
bool retro_unserialize(const void *d, size_t s) { return false; }
void retro_cheat_reset() {}
void retro_cheat_set(unsigned i, bool e, const char *c) {}
void retro_set_controller_port_device(unsigned p, unsigned d) {}
void* retro_get_memory_data(unsigned i) { return NULL; }
size_t retro_get_memory_size(unsigned i) { return 0; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { return false; }
unsigned retro_get_region() { return RETRO_REGION_NTSC; }