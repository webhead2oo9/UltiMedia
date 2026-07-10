#pragma once

#include <stdint.h>
#include "audio.h"

#define CORE_MAX_TRACKS 256

// Serialized save-state layout. Shared with tests/core_harness.c so the
// harness's field offsets can never drift from the real layout; bump
// CORE_STATE_VERSION in core.c when changing this struct.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t content_hash;
    uint32_t track_count;
    int32_t current_idx;
    int32_t viz_mode;
    int32_t scroll_x;
    int32_t legacy_debounce; // unused since edge-triggered input; keeps layout stable
    int32_t ff_rw_icon_timer;
    int32_t ff_rw_dir;
    uint8_t is_paused;
    uint8_t is_shuffle;
    uint8_t reserved[2];
    uint32_t shuffle_seed;
    uint32_t shuffle_state;
    uint32_t shuffle_count;
    uint32_t shuffle_pos;
    uint32_t shuffle_history_count;
    uint32_t shuffle_history_pos;
    AudioStateSnapshot audio;
    int32_t shuffle_order[CORE_MAX_TRACKS];
    int32_t shuffle_history[CORE_MAX_TRACKS];
    uint32_t m3u_base_path_len;
    uint32_t track_path_lens[CORE_MAX_TRACKS];
} CoreStateSnapshot;
