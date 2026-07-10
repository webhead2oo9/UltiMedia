#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dr_flac.h"
#include "stb_image.h"
#include "stb_vorbis_compat.h"
#include "path_io.h"

uint16_t *art_buffer = NULL;
int art_w_src = 0, art_h_src = 0;
char display_str[256];

#define ART_STORED_MAX_DIMENSION 120

typedef struct {
    char *artist;
    char *title;
    char *album;
    int maxlen;
} FlacMetaContext;

static int art_dimensions_valid(int width, int height) {
    return width > 0 && height > 0 &&
           width <= ART_MAX_DIMENSION && height <= ART_MAX_DIMENSION;
}

// Oversized images are rejected by stb_image itself: image_codecs.c builds
// it with STBI_MAX_DIMENSIONS = ART_MAX_DIMENSION, so no pre-validation
// pass is needed here.
static unsigned char *load_art_file(const char *path, int *width, int *height) {
    if (!path || !width || !height) return NULL;
    // path_fopen_read tries the wide Windows path and falls back to the
    // narrow one; stb_image's own path handling would lose that fallback.
    FILE *file = path_fopen_read(path);
    if (!file) return NULL;
    unsigned char *data = stbi_load_from_file(file, width, height, NULL, 3);
    fclose(file);
    return data;
}

static unsigned char *load_art_memory(const unsigned char *data, size_t size, int *width, int *height) {
    if (!data || size == 0 || size > INT32_MAX || !width || !height) return NULL;
    return stbi_load_from_memory(data, (int)size, width, height, NULL, 3);
}

static void store_art_rgb565(const unsigned char *img_data, int source_w, int source_h) {
    if (!img_data || !art_dimensions_valid(source_w, source_h)) return;

    int stored_w = source_w;
    int stored_h = source_h;
    if (stored_w > ART_STORED_MAX_DIMENSION) stored_w = ART_STORED_MAX_DIMENSION;
    if (stored_h > ART_STORED_MAX_DIMENSION) stored_h = ART_STORED_MAX_DIMENSION;

    size_t pixel_count = (size_t)stored_w * (size_t)stored_h;
    uint16_t *stored = malloc(pixel_count * sizeof(*stored));
    if (!stored) return;

    for (int y = 0; y < stored_h; y++) {
        int source_y = (int)(((size_t)y * (size_t)source_h) / (size_t)stored_h);
        for (int x = 0; x < stored_w; x++) {
            int source_x = (int)(((size_t)x * (size_t)source_w) / (size_t)stored_w);
            const unsigned char *pixel = img_data +
                (((size_t)source_y * (size_t)source_w + (size_t)source_x) * 3u);
            uint8_t r = pixel[0], g = pixel[1], b = pixel[2];
            stored[(size_t)y * (size_t)stored_w + (size_t)x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }

    art_buffer = stored;
    art_w_src = stored_w;
    art_h_src = stored_h;
}

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

static void clean_meta_text(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (unsigned char)s[len - 1] < 32) {
        s[len - 1] = '\0';
        len--;
    }
}

static const char *basename_ptr(const char *path) {
    const char *b = strrchr(path, '/');
    if (!b) b = strrchr(path, '\\');
    return b ? b + 1 : path;
}

static void set_display_from_filename(const char *track_path, int strip_ext) {
    const char *base = basename_ptr(track_path);
    size_t len = strlen(base);
    if (strip_ext) {
        const char *dot = strrchr(base, '.');
        if (dot && dot > base) len = (size_t)(dot - base);
    }
    if (len > 250) len = 250;
    memcpy(display_str, base, len);
    display_str[len] = '\0';
    strncat(display_str, "   ", sizeof(display_str) - strlen(display_str) - 1);
}

static void copy_value(char *dest, int maxlen, const char *value) {
    if (!dest || !value || maxlen <= 0) return;
    int len = (int)strlen(value);
    if (len >= maxlen) len = maxlen - 1;
    if (len < 0) len = 0;
    memcpy(dest, value, (size_t)len);
    dest[len] = '\0';
}

static void maybe_store_tag(const char *entry, char *artist, char *title, char *album, int maxlen) {
    const char *eq = strchr(entry, '=');
    if (!eq || eq == entry) return;
    size_t key_len = (size_t)(eq - entry);
    const char *val = eq + 1;

    if (key_len == 5 && strncasecmp_simple(entry, "TITLE", 5) == 0 && !title[0]) {
        copy_value(title, maxlen, val);
    } else if (key_len == 6 && strncasecmp_simple(entry, "ARTIST", 6) == 0 && !artist[0]) {
        copy_value(artist, maxlen, val);
    } else if (key_len == 5 && strncasecmp_simple(entry, "ALBUM", 5) == 0 && !album[0]) {
        copy_value(album, maxlen, val);
    }
}

static int parse_ogg_vorbis_tags(const char *path, char *artist, char *title, char *album, int maxlen) {
    int err = 0;
    FILE *file = path_fopen_read(path);
    if (!file) return 0;
    stb_vorbis *ogg = stb_vorbis_open_file(file, 1, &err, NULL);
    if (!ogg) return 0;

    stb_vorbis_comment comments = stb_vorbis_get_comment(ogg);
    for (int i = 0; i < comments.comment_list_length; i++) {
        const char *entry = comments.comment_list[i];
        if (entry) maybe_store_tag(entry, artist, title, album, maxlen);
    }

    stb_vorbis_close(ogg);
    return (title[0] || artist[0]) ? 1 : 0;
}

static void flac_meta_proc(void *pUserData, drflac_metadata *pMetadata) {
    if (!pUserData || !pMetadata) return;
    if (pMetadata->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;

    FlacMetaContext *ctx = (FlacMetaContext*)pUserData;
    drflac_vorbis_comment_iterator iter;
    drflac_init_vorbis_comment_iterator(&iter,
                                        pMetadata->data.vorbis_comment.commentCount,
                                        pMetadata->data.vorbis_comment.pComments);

    drflac_uint32 comment_len = 0;
    const char *comment = NULL;
    while ((comment = drflac_next_vorbis_comment(&iter, &comment_len)) != NULL) {
        char entry[512];
        drflac_uint32 copy_len = comment_len;
        if (copy_len >= sizeof(entry)) copy_len = (drflac_uint32)sizeof(entry) - 1;
        memcpy(entry, comment, copy_len);
        entry[copy_len] = '\0';
        maybe_store_tag(entry, ctx->artist, ctx->title, ctx->album, ctx->maxlen);
    }
}

static int parse_flac_vorbis_tags(const char *path, char *artist, char *title, char *album, int maxlen) {
    FlacMetaContext ctx;
    ctx.artist = artist;
    ctx.title = title;
    ctx.album = album;
    ctx.maxlen = maxlen;

    drflac *flac = NULL;
#ifdef _WIN32
    wchar_t *wide = path_utf8_to_wide_alloc(path);
    if (wide) {
        flac = drflac_open_file_with_metadata_w(wide, flac_meta_proc, &ctx, NULL);
        free(wide);
    }
#endif
    if (!flac) flac = drflac_open_file_with_metadata(path, flac_meta_proc, &ctx, NULL);
    if (!flac) return 0;
    drflac_close(flac);
    return (title[0] || artist[0]) ? 1 : 0;
}

int parse_id3v2(const char* path, char* artist, char* title, char* album, int maxlen) {
    FILE* f = path_fopen_read(path);
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
        if (bytes_read < 4) { free(data); return 0; }
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
        if (ext_size >= bytes_read) { free(data); return 0; }  // >= rejects malformed headers
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

        if (frame_size == 0 || frame_size > bytes_read - pos - header_size) break;

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

void metadata_free_art(void) {
    if (art_buffer) {
        free(art_buffer);
        art_buffer = NULL;
    }
    art_w_src = 0;
    art_h_src = 0;
}

static void metadata_build_display(const char *track_path, TrackTextMode track_text_mode, char *album_out, size_t album_out_size) {
    char meta_title[64] = {0};
    char meta_artist[64] = {0};
    char cur_album[64] = {0};

    if (track_text_mode == SHOW_ID) {
        const char *ext = strrchr(track_path, '.');
        int found = parse_id3v2(track_path, meta_artist, meta_title, cur_album, (int)sizeof(meta_title));
        if (!found && ext && strcasecmp_simple(ext, ".ogg") == 0)
            found = parse_ogg_vorbis_tags(track_path, meta_artist, meta_title, cur_album, (int)sizeof(meta_title));
        if (!found && ext && strcasecmp_simple(ext, ".flac") == 0)
            found = parse_flac_vorbis_tags(track_path, meta_artist, meta_title, cur_album, (int)sizeof(meta_title));

        if (!found && ext && strcasecmp_simple(ext, ".mp3") == 0) {
            // Fall back to ID3v1 for MP3s.
            FILE* f = path_fopen_read(track_path);
            if (f) {
                if (fseek(f, -128, SEEK_END) == 0) {
                    char tag[3];
                    size_t r = fread(tag, 1, 3, f);
                    if (r == 3 && strncmp(tag, "TAG", 3) == 0) {
                        unsigned char id3v1_data[125] = {0};
                        char id3v1_title[31] = {0};
                        char id3v1_artist[31] = {0};
                        char id3v1_album[31] = {0};

                        size_t data_read = fread(id3v1_data, 1, sizeof(id3v1_data), f);

                        if (data_read >= 90) {
                            memcpy(id3v1_title, id3v1_data, 30);
                            memcpy(id3v1_artist, id3v1_data + 30, 30);
                            memcpy(id3v1_album, id3v1_data + 60, 30);
                            memcpy(meta_title, id3v1_title, sizeof(id3v1_title));
                            memcpy(meta_artist, id3v1_artist, sizeof(id3v1_artist));
                            memcpy(cur_album, id3v1_album, sizeof(id3v1_album));
                        }
                    }
                }
                fclose(f);
            }
        }
    }

    clean_meta_text(meta_title);
    clean_meta_text(meta_artist);
    clean_meta_text(cur_album);

    if (track_text_mode == SHOW_FILENAME_WITH_EXT) {
        set_display_from_filename(track_path, 0);
    } else if (track_text_mode == SHOW_FILENAME_WITHOUT_EXT) {
        set_display_from_filename(track_path, 1);
    } else if (meta_title[0] != 0 && meta_artist[0] != 0) {
        snprintf(display_str, sizeof(display_str), "%s - %s   ", meta_artist, meta_title);
    } else if (meta_title[0] != 0) {
        snprintf(display_str, sizeof(display_str), "%s   ", meta_title);
    } else {
        set_display_from_filename(track_path, 0);
    }

    if (album_out && album_out_size > 0) {
        strncpy(album_out, cur_album, album_out_size - 1);
        album_out[album_out_size - 1] = '\0';
    }
}

void metadata_refresh_display(const char *track_path, TrackTextMode track_text_mode) {
    metadata_build_display(track_path, track_text_mode, NULL, 0);
}

void metadata_load(const char *track_path, const char *m3u_base_path, TrackTextMode track_text_mode) {
    char cur_album[64] = {0};
    metadata_build_display(track_path, track_text_mode, cur_album, sizeof(cur_album));

    // --- Load Artwork (The 5 Location Search) ---
    metadata_free_art();
    unsigned char* img_data = NULL;
    int img_w = 0, img_h = 0;
    char path_buf[1024];
    const char* exts[] = { ".jpg", ".jpeg", ".png", ".bmp" };

    // A. Setup Directory Strings for the Music File
    char music_dir[1024] = {0}, parent_name[256] = {0};
    const char* last_s = strrchr(track_path, '/');
    if (!last_s) last_s = strrchr(track_path, '\\');
    if (last_s) {
        size_t dir_len = (size_t)(last_s - track_path);
        if (dir_len >= sizeof(music_dir)) dir_len = sizeof(music_dir) - 1;
        memcpy(music_dir, track_path, dir_len);
        music_dir[dir_len] = '\0';
        const char* p_slash = strrchr(music_dir, '/');
        if (!p_slash) p_slash = strrchr(music_dir, '\\');
        const char *parent = p_slash ? p_slash + 1 : music_dir;
        size_t parent_len = strlen(parent);
        if (parent_len >= sizeof(parent_name)) parent_len = sizeof(parent_name) - 1;
        memcpy(parent_name, parent, parent_len);
        parent_name[parent_len] = '\0';
    }

    // B. Main Search Loop
    for (int i = 0; i < 4 && !img_data; i++) {
        // 1. Same name as MP3 (e.g., C:/Music/Song.jpg)
        const char* dot = strrchr(track_path, '.');
        if (dot) {
            int base_len = (int)(dot - track_path);
            if (base_len < 0) base_len = 0;
            snprintf(path_buf, sizeof(path_buf), "%.*s%s", base_len, track_path, exts[i]);
        } else {
            snprintf(path_buf, sizeof(path_buf), "%s%s", track_path, exts[i]);
        }
        img_data = load_art_file(path_buf, &img_w, &img_h);
        if (img_data) break;

        if (music_dir[0]) {
            // 2. Name of Parent Folder (e.g., C:/Music/AlbumName/AlbumName.jpg)
            int written = snprintf(path_buf, sizeof(path_buf), "%s/%s%s", music_dir, parent_name, exts[i]);
            if (written > 0 && written < (int)sizeof(path_buf)) {
                img_data = load_art_file(path_buf, &img_w, &img_h);
                if (img_data) break;
            }

            // 3. Album Name from Metadata (e.g., C:/Music/AlbumName/MetadataAlbum.jpg)
            if (cur_album[0]) {
                written = snprintf(path_buf, sizeof(path_buf), "%s/%s%s", music_dir, cur_album, exts[i]);
                if (written > 0 && written < (int)sizeof(path_buf)) {
                    img_data = load_art_file(path_buf, &img_w, &img_h);
                    if (img_data) break;
                }
            }
        }

        // 4. Same name as M3U file (e.g., if playlist is Playlist.m3u, looks for Playlist.jpg)
        if (m3u_base_path && m3u_base_path[0]) {
            const char* m3u_dot = strrchr(m3u_base_path, '.');
            if (m3u_dot) {
                int base_len = (int)(m3u_dot - m3u_base_path);
                if (base_len < 0) base_len = 0;
                snprintf(path_buf, sizeof(path_buf), "%.*s%s", base_len, m3u_base_path, exts[i]);
            } else {
                snprintf(path_buf, sizeof(path_buf), "%s%s", m3u_base_path, exts[i]);
            }
            img_data = load_art_file(path_buf, &img_w, &img_h);
            if (img_data) break;
        }
    }

    // 5. Files Metadata (Aggressive APIC/PIC Scan)
    if (!img_data) {
        FILE* f_art = path_fopen_read(track_path);
        if (f_art) {
            // Scan 1MB: embedded art is often large and offset deep in the header
            size_t scan_size = 1024 * 1024;
            unsigned char* head = malloc(scan_size);
            if (head) {
                size_t bytes_read = fread(head, 1, scan_size, f_art);

                // Scan for embedded JPEG/PNG by magic bytes
                for (size_t i = 0; i + 10 < bytes_read; i++) {
                    // Check for JPEG (FF D8 FF)
                    if (head[i] == 0xFF && head[i+1] == 0xD8 && head[i+2] == 0xFF) {
                        img_data = load_art_memory(head + i, bytes_read - i, &img_w, &img_h);
                        if (img_data) break;
                    }
                    // Check for PNG (89 50 4E 47)
                    if (head[i] == 0x89 && head[i+1] == 0x50 && head[i+2] == 0x4E && head[i+3] == 0x47) {
                        img_data = load_art_memory(head + i, bytes_read - i, &img_w, &img_h);
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
        store_art_rgb565(img_data, img_w, img_h);
        stbi_image_free(img_data);
    }
}
