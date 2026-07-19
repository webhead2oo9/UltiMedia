#pragma once

#include <stdint.h>
#include <stdbool.h>

// Per-frame playback state handed from core.c to the renderer. scroll_x is
// owned (and serialized) by core.c; ui_draw advances it for the marquee.
typedef struct {
    bool paused;
    bool shuffle;
    int seek_dir;          // -1 seeking back, 1 seeking forward, 0 idle
    int track_index;       // 0-based
    int track_count;
    uint64_t cur_frame;
    uint64_t total_frames;
    uint32_t source_rate;
    int *scroll_x;
} UiFrame;

// Compose one full frame: background, art, visualizer, text, bar, transport.
void ui_draw(UiFrame *frame);

// Restart the marquee hold after a track change or state restore.
void ui_reset_marquee(void);

// Report the frontend's frame duration so animations advance in real time.
void ui_set_frame_dt(float dt);
