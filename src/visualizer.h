#pragma once

#include <stdint.h>

#define MAX_VIZ_BANDS 40

// Visualizer state
extern float viz_levels[MAX_VIZ_BANDS];
extern float viz_peaks[MAX_VIZ_BANDS];
extern int viz_peak_timers[MAX_VIZ_BANDS];

// Update visualizer levels from audio buffer
void viz_update_levels(const int16_t *audio_buf, int samples_per_frame);

// Draw current visualizer mode
void viz_draw(void);

// Get gradient color based on level (0.0 - 1.0)
uint16_t get_gradient_color(float level);

// Set audio buffer for VU meter mode (called before viz_draw)
void viz_set_audio_for_vu(const int16_t *audio_buf, int samples_per_frame);
