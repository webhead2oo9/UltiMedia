#pragma once

#include <stdint.h>
#include <stdbool.h>

#define OUT_RATE 48000
#define SAMPLES_PER_FRAME 800
#define MAX_CHANNELS 8
#define RESAMPLE_CACHE_FRAMES 8

typedef enum { AUDIO_NONE, AUDIO_MP3, AUDIO_WAV, AUDIO_OGG, AUDIO_FLAC } AudioType;

typedef struct {
    uint32_t version;
    uint32_t current_type;
    uint32_t source_rate;
    int32_t source_channels;
    uint64_t total_frames;
    uint64_t cur_frame;
    double resample_phase;
    int32_t resample_cache_frames;
    int16_t resample_cache[RESAMPLE_CACHE_FRAMES * MAX_CHANNELS];
} AudioStateSnapshot;

// Audio state (exposed for core.c coordination)
extern AudioType current_type;
extern void *decoder;
extern uint32_t source_rate;
extern int source_channels;
extern uint64_t total_frames;
extern uint64_t cur_frame;

// Initialize audio subsystem
void audio_init(void);

// Deinitialize and free resources
void audio_deinit(void);

// Open and decode a track file, returns true on success
bool audio_open_track(const char *path);

// Read one frame of audio (resampled + downmixed to stereo)
// Returns number of samples written, 0 if end of track
int audio_read_frame(int16_t *out_buf);

// Seek to position in current track
void audio_seek(uint64_t frame);

// Close current decoder
void audio_close(void);

// Capture the current decode/resampler state
void audio_capture_state(AudioStateSnapshot *state);

// Validate a snapshot's version and fields without touching the decoder
bool audio_snapshot_valid(const AudioStateSnapshot *state);

// Reopen a track and restore decode/resampler state
bool audio_restore_state(const char *path, const AudioStateSnapshot *state);

// Clamp float to int16
int16_t clamp_i16(float v);
