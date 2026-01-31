#include "visualizer.h"
#include "video.h"
#include "config.h"
#include <stdlib.h>

float viz_levels[MAX_VIZ_BANDS] = {0};
float viz_peaks[MAX_VIZ_BANDS] = {0};
int viz_peak_timers[MAX_VIZ_BANDS] = {0};

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

void viz_update_levels(const int16_t *audio_buf, int samples_per_frame) {
    int sample_stride = (cfg.viz_bands == 40) ? 20 : 40;
    int band_count = cfg.viz_bands;

    for (int i = 0; i < band_count; i++) {
        float p = abs(audio_buf[i * sample_stride]) / 32768.0f;

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
}

static void draw_bars_mode(int band_count) {
    int start_x = (band_count == 40) ? 80 : 100;
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
}

static void draw_dots_mode(int band_count) {
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
}

static void draw_line_mode(int band_count) {
    int start_x = (band_count == 40) ? 80 : 100;
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
}

static void draw_vu_meter_mode(const int16_t *audio_buf, int samples_per_frame) {
    // Calculate L/R levels from stereo mix
    float left_level = 0, right_level = 0;
    for (int i = 0; i < samples_per_frame; i += 8) {
        float l = abs(audio_buf[i*2]) / 32768.0f;
        float r = abs(audio_buf[i*2+1]) / 32768.0f;
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

// Storage for VU meter audio buffer reference
static const int16_t *vu_audio_buf = NULL;
static int vu_samples_per_frame = 0;

void viz_set_audio_for_vu(const int16_t *audio_buf, int samples_per_frame) {
    vu_audio_buf = audio_buf;
    vu_samples_per_frame = samples_per_frame;
}

void viz_draw(void) {
    int band_count = cfg.viz_bands;

    if (cfg.viz_mode == 0) {
        draw_bars_mode(band_count);
    } else if (cfg.viz_mode == 1) {
        draw_dots_mode(band_count);
    } else if (cfg.viz_mode == 2) {
        draw_line_mode(band_count);
    } else if (cfg.viz_mode == 3 && vu_audio_buf) {
        draw_vu_meter_mode(vu_audio_buf, vu_samples_per_frame);
    }
}
