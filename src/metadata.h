#pragma once

#include <stdint.h>
#include "config.h"

// Album art buffer (RGB565)
extern uint16_t *art_buffer;
extern int art_w_src, art_h_src;

// Display metadata
extern char display_str[256];

// Parse ID3v2 tags, returns 1 if found
int parse_id3v2(const char* path, char* artist, char* title, char* album, int maxlen);

// Load metadata and album art for a track
// Sets display_str and loads art_buffer
void metadata_load(const char *track_path, const char *m3u_base_path, TrackTextMode track_text_mode);

// Free album art buffer
void metadata_free_art(void);
