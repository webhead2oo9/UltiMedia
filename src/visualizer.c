#include "visualizer.h"
#include "video.h"
#include "config.h"
#include "layout.h"
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
        // Manual abs to avoid undefined behavior with INT16_MIN (-32768)
        int32_t sample = audio_buf[i * sample_stride];
        float p = (sample < 0 ? -sample : sample) / 32768.0f;

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
    int start_x;
    int bar_width;
    int spacing;
    int max_h;
    int base_y;

    if (cfg.responsive) {
        start_x = layout.viz_start_x;
        bar_width = layout.viz_bar_width;
        spacing = layout.viz_spacing;
        max_h = layout.viz_max_h;
        base_y = layout.viz.y + layout.viz.h - 1;
    } else {
        start_x = (band_count == 40) ? 80 : 100;
        bar_width = (band_count == 40) ? 2 : 4;
        spacing = (band_count == 40) ? 4 : 6;
        max_h = 35;
        base_y = cfg.viz_y;
    }
    if (max_h < 1) max_h = 1;

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h > max_h) h = max_h;
        int x_base = start_x + (i * spacing);

        // Draw main bar
        for (int v = 0; v < h; v++) {
            uint16_t color = cfg.viz_gradient ? get_gradient_color((float)v / (float)max_h) : cfg.fg_rgb;
            for (int w = 0; w < bar_width; w++) draw_pixel(x_base + w, base_y - v, color);
        }

        // Draw peak hold dot
        if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
            int peak_h = (int)(viz_peaks[i] * max_h);
            if (peak_h >= max_h) peak_h = max_h - 1;
            uint16_t peak_color = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
            for (int w = 0; w < bar_width; w++) {
                draw_pixel(x_base + w, base_y - peak_h, peak_color);
                if (peak_h + 1 < max_h) draw_pixel(x_base + w, base_y - peak_h - 1, peak_color);
            }
        }
    }
}

static void draw_dots_mode(int band_count) {
    int start_x;
    int spacing;
    int max_h;
    int base_y;

    if (cfg.responsive) {
        start_x = layout.viz_start_x;
        spacing = layout.viz_spacing;
        max_h = layout.viz_max_h;
        base_y = layout.viz.y + layout.viz.h - 1;
        if (spacing < 2) spacing = 2;

        // Keep 2x2 dots inside the responsive visualizer bounds.
        if (band_count > 1) {
            int dots_w = spacing * (band_count - 1) + 2;
            if (dots_w > layout.viz.w) {
                spacing = (layout.viz.w - 2) / (band_count - 1);
                if (spacing < 1) spacing = 1;
                dots_w = spacing * (band_count - 1) + 2;
            }
            start_x = layout.viz.x + (layout.viz.w - dots_w) / 2;
        } else {
            start_x = layout.viz.x + (layout.viz.w - 2) / 2;
        }
    } else {
        start_x = (band_count == 40) ? 100 : 130;
        spacing = (band_count == 40) ? 3 : 4;
        max_h = 50;
        base_y = cfg.viz_y;
    }
    if (max_h < 1) max_h = 1;

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h >= max_h) h = max_h - 1;
        int x = start_x + (i * spacing);
        uint16_t color = cfg.viz_gradient ? get_gradient_color(viz_levels[i]) : cfg.fg_rgb;

        // Draw 2x2 dot
        draw_pixel(x, base_y - h, color);
        draw_pixel(x + 1, base_y - h, color);
        draw_pixel(x, base_y - h - 1, color);
        draw_pixel(x + 1, base_y - h - 1, color);

        // Peak dot
        if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
            int peak_h = (int)(viz_peaks[i] * max_h);
            if (peak_h >= max_h) peak_h = max_h - 1;
            uint16_t peak_color = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
            draw_pixel(x, base_y - peak_h, peak_color);
            draw_pixel(x + 1, base_y - peak_h, peak_color);
        }
    }
}

static void draw_line_mode(int band_count) {
    int start_x;
    int spacing;
    int max_h;
    int base_y;

    if (cfg.responsive) {
        start_x = layout.viz_start_x;
        spacing = layout.viz_spacing;
        max_h = layout.viz_max_h;
        base_y = layout.viz.y + layout.viz.h - 1;
    } else {
        start_x = (band_count == 40) ? 80 : 100;
        spacing = (band_count == 40) ? 4 : 6;
        max_h = 40;
        base_y = cfg.viz_y;
    }
    if (max_h < 1) max_h = 1;

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h >= max_h) h = max_h - 1;
        int x = start_x + (i * spacing);
        uint16_t color = cfg.viz_gradient ? get_gradient_color(viz_levels[i]) : cfg.fg_rgb;

        // Draw vertical line
        for (int v = 0; v <= h; v++) draw_pixel(x, base_y - v, color);

        // Connect to next band
        if (i < band_count - 1) {
            int next_h = (int)(viz_levels[i + 1] * max_h);
            if (next_h >= max_h) next_h = max_h - 1;
            int next_x = start_x + ((i + 1) * spacing);
            int dx = next_x - x;
            int dy = next_h - h;

            for (int step = 0; step < dx; step++) {
                int interp_y = h + (dy * step) / dx;
                uint16_t interp_color = cfg.viz_gradient ? get_gradient_color((float)interp_y / (float)max_h) : cfg.fg_rgb;
                draw_pixel(x + step, base_y - interp_y, interp_color);
            }
        }

        // Peak markers
        if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
            int peak_h = (int)(viz_peaks[i] * max_h);
            if (peak_h >= max_h) peak_h = max_h - 1;
            draw_pixel(x, base_y - peak_h, 0xF800);
            draw_pixel(x + 1, base_y - peak_h, 0xF800);
        }
    }
}

static void draw_vu_meter_mode(const int16_t *audio_buf, int samples_per_frame) {
    // Calculate L/R levels from stereo mix
    float left_level = 0, right_level = 0;
    for (int i = 0; i < samples_per_frame; i += 8) {
        int32_t ls = audio_buf[i*2], rs = audio_buf[i*2+1];
        float l = (ls < 0 ? -ls : ls) / 32768.0f;
        float r = (rs < 0 ? -rs : rs) / 32768.0f;
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

    int label_x = 80;
    int meter_x = 95;
    int meter_w = 180;
    const int meter_h = 4;
    const int meter_gap = 3;
    const int pair_h = meter_h * 2 + meter_gap;
    int left_y = cfg.viz_y - 15;
    int right_y = left_y + meter_h + meter_gap;

    if (cfg.responsive) {
        meter_w = layout.viz_meter_w;
        if (meter_w < 1) meter_w = 1;
        meter_x = layout.viz.x + (layout.viz.w - meter_w);
        label_x = layout.viz.x;

        if (layout.viz.h >= pair_h) {
            int pair_top = layout.viz.y + (layout.viz.h - pair_h) / 2;
            left_y = pair_top;
            right_y = pair_top + meter_h + meter_gap;
        } else {
            // Too short for two meters, show only left as a mono meter
            left_y = layout.viz.y + (layout.viz.h - meter_h) / 2;
            if (left_y < layout.viz.y) left_y = layout.viz.y;
            right_y = -1;
        }
    }

    // Draw Left meter
    draw_text(label_x, left_y, "L", cfg.fg_rgb);
    int left_w = (int)(viz_levels[0] * meter_w);
    if (left_w > meter_w) left_w = meter_w;
    for (int x = 0; x < left_w; x++) {
        uint16_t color = cfg.viz_gradient ? get_gradient_color((float)x / (float)meter_w) : cfg.fg_rgb;
        for (int y = 0; y < meter_h; y++) draw_pixel(meter_x + x, left_y + y, color);
    }
    if (cfg.viz_peak_hold > 0 && viz_peak_timers[0] > 0) {
        int peak_x = (int)(viz_peaks[0] * meter_w);
        if (peak_x >= meter_w) peak_x = meter_w - 1;
        for (int y = 0; y < meter_h; y++) draw_pixel(meter_x + peak_x, left_y + y, 0xF800);
    }

    // Draw Right meter (skip if too short for two meters)
    if (right_y >= 0) {
        draw_text(label_x, right_y, "R", cfg.fg_rgb);
        int right_w = (int)(viz_levels[1] * meter_w);
        if (right_w > meter_w) right_w = meter_w;
        for (int x = 0; x < right_w; x++) {
            uint16_t color = cfg.viz_gradient ? get_gradient_color((float)x / (float)meter_w) : cfg.fg_rgb;
            for (int y = 0; y < meter_h; y++) draw_pixel(meter_x + x, right_y + y, color);
        }
        if (cfg.viz_peak_hold > 0 && viz_peak_timers[1] > 0) {
            int peak_x = (int)(viz_peaks[1] * meter_w);
            if (peak_x >= meter_w) peak_x = meter_w - 1;
            for (int y = 0; y < meter_h; y++) draw_pixel(meter_x + peak_x, right_y + y, 0xF800);
        }
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
    if (cfg.responsive && (layout.viz.w <= 0 || layout.viz.h <= 0)) return;

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
