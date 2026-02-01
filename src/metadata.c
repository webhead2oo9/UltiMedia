#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

uint16_t *art_buffer = NULL;
int art_w_src = 0, art_h_src = 0;
char display_str[256];

int parse_id3v2(const char* path, char* artist, char* title, char* album, int maxlen) {
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
}

void metadata_load(const char *track_path, const char *m3u_base_path, int use_filename) {
    char meta_title[32] = {0};
    char meta_artist[32] = {0};
    char cur_album[32] = {0};

    // Load Metadata - Try ID3v2 first, fall back to ID3v1 (unless filename-only mode)
    if (!use_filename) {
        if (!parse_id3v2(track_path, meta_artist, meta_title, cur_album, 31)) {
            // Fall back to ID3v1
            FILE* f = fopen(track_path, "rb");
            if (f) {
                fseek(f, -128, SEEK_END);
                char tag[3];
                size_t r = fread(tag, 1, 3, f);
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
    for (int i = 29; i >= 0; i--) { if (meta_title[i] < 32) meta_title[i] = 0; else break; }
    for (int i = 29; i >= 0; i--) { if (meta_artist[i] < 32) meta_artist[i] = 0; else break; }
    for (int i = 29; i >= 0; i--) { if (cur_album[i] < 32) cur_album[i] = 0; else break; }

    // Set Display String
    if (meta_title[0] != 0 && meta_artist[0] != 0)
        sprintf(display_str, "%s - %s   ", meta_artist, meta_title);
    else if (meta_title[0] != 0)
        sprintf(display_str, "%s   ", meta_title);
    else {
        const char* b = strrchr(track_path, '/');
        if (!b) b = strrchr(track_path, '\\');
        strncpy(display_str, b ? b + 1 : track_path, 250);
        display_str[250] = '\0';
        strcat(display_str, "   ");
    }

    // --- Load Artwork (The 5 Location Search) ---
    metadata_free_art();
    unsigned char* img_data = NULL;
    char path_buf[1024];
    const char* exts[] = { ".jpg", ".jpeg", ".png", ".bmp" };

    // A. Setup Directory Strings for the Music File
    char music_dir[1024] = {0}, parent_name[256] = {0};
    const char* last_s = strrchr(track_path, '/');
    if (!last_s) last_s = strrchr(track_path, '\\');
    if (last_s) {
        size_t dir_len = last_s - track_path;
        strncpy(music_dir, track_path, dir_len);
        music_dir[dir_len] = '\0';
        const char* p_slash = strrchr(music_dir, '/');
        if (!p_slash) p_slash = strrchr(music_dir, '\\');
        strncpy(parent_name, p_slash ? p_slash + 1 : music_dir, sizeof(parent_name) - 1);
        parent_name[sizeof(parent_name) - 1] = '\0';
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
        img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
        if (img_data) break;

        if (music_dir[0]) {
            // 2. Name of Parent Folder (e.g., C:/Music/AlbumName/AlbumName.jpg)
            snprintf(path_buf, sizeof(path_buf), "%s/%s%s", music_dir, parent_name, exts[i]);
            img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
            if (img_data) break;

            // 3. Album Name from Metadata (e.g., C:/Music/AlbumName/MetadataAlbum.jpg)
            if (cur_album[0]) {
                snprintf(path_buf, sizeof(path_buf), "%s/%s%s", music_dir, cur_album, exts[i]);
                img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
                if (img_data) break;
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
            img_data = stbi_load(path_buf, &art_w_src, &art_h_src, NULL, 3);
            if (img_data) break;
        }
    }

    // 5. Files Metadata (Aggressive APIC/PIC Scan)
    if (!img_data) {
        FILE* f_art = fopen(track_path, "rb");
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
