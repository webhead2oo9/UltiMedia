#include "visualizer.h"
#include "video.h"
#include "config.h"
#include "layout.h"
#include "audio.h"
#include <math.h>
#include <stdlib.h>

float viz_levels[MAX_VIZ_BANDS] = {0};
float viz_peaks[MAX_VIZ_BANDS] = {0};
int viz_peak_timers[MAX_VIZ_BANDS] = {0};

#define FFT_SIZE 512
#define TWO_PI_F 6.28318530717958647692f
#define FFT_MIN_FREQ 35.0f
#define FFT_MAX_FREQ_RATIO 0.92f

static float fft_window[FFT_SIZE] = {0};
static bool fft_window_ready = false;
static float fft_re[FFT_SIZE] = {0};
static float fft_im[FFT_SIZE] = {0};
static float fft_band_energy[MAX_VIZ_BANDS] = {0};
static float fft_auto_gain = 24.0f;

static int viz_band_x(int band_idx, int band_count, int item_w, int start_x, int spacing) {
    if (!cfg.responsive) return start_x + (band_idx * spacing);

    if (item_w < 1) item_w = 1;
    if (item_w > layout.viz.w) item_w = layout.viz.w;
    if (layout.viz.w <= item_w) return layout.viz.x;
    if (band_count <= 1) return layout.viz.x + (layout.viz.w - item_w) / 2;

    int span = layout.viz.w - item_w;
    return layout.viz.x + (band_idx * span) / (band_count - 1);
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

static void viz_decay_levels(int band_count) {
    for (int i = 0; i < band_count; i++) {
        viz_levels[i] *= 0.85f;
        if (viz_peak_timers[i] > 0) viz_peak_timers[i]--;
        else viz_peaks[i] *= 0.95f;
    }
}

static void fft_prepare_window(void) {
    if (fft_window_ready) return;
    for (int i = 0; i < FFT_SIZE; i++) {
        fft_window[i] = 0.5f - 0.5f * cosf((TWO_PI_F * (float)i) / (float)(FFT_SIZE - 1));
    }
    fft_window_ready = true;
}

static void fft_compute(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            float tr = re[i];
            re[i] = re[j];
            re[j] = tr;
            float ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        float ang = -TWO_PI_F / (float)len;
        float wlen_cos = cosf(ang);
        float wlen_sin = sinf(ang);

        for (int i = 0; i < n; i += len) {
            float w_cos = 1.0f;
            float w_sin = 0.0f;
            for (int j = 0; j < half; j++) {
                int u = i + j;
                int v = i + j + half;

                float vr = re[v] * w_cos - im[v] * w_sin;
                float vi = re[v] * w_sin + im[v] * w_cos;
                float ur = re[u];
                float ui = im[u];

                re[u] = ur + vr;
                im[u] = ui + vi;
                re[v] = ur - vr;
                im[v] = ui - vi;

                float next_cos = w_cos * wlen_cos - w_sin * wlen_sin;
                w_sin = w_cos * wlen_sin + w_sin * wlen_cos;
                w_cos = next_cos;
            }
        }
    }
}

static void fft_update_levels(const int16_t *audio_buf, int samples_per_frame, int band_count) {
    if (!audio_buf || samples_per_frame <= 0) {
        viz_decay_levels(band_count);
        return;
    }

    fft_prepare_window();

    int available_frames = samples_per_frame;
    if (available_frames < 1) {
        viz_decay_levels(band_count);
        return;
    }

    for (int i = 0; i < FFT_SIZE; i++) {
        int src_frame = (i < available_frames) ? i : (available_frames - 1);
        int src = src_frame * 2;
        int32_t left = audio_buf[src];
        int32_t right = audio_buf[src + 1];
        float mono = (float)(left + right) * (0.5f / 32768.0f);
        fft_re[i] = mono * fft_window[i];
        fft_im[i] = 0.0f;
    }

    fft_compute(fft_re, fft_im, FFT_SIZE);

    const int max_bin = FFT_SIZE / 2 - 1;
    const float nyquist = (float)OUT_RATE * 0.5f;
    const float max_freq = nyquist * FFT_MAX_FREQ_RATIO;
    float frame_max = 0.0f;

    for (int i = 0; i < band_count; i++) {
        float t0 = (float)i / (float)band_count;
        float t1 = (float)(i + 1) / (float)band_count;
        float f0 = FFT_MIN_FREQ * powf(max_freq / FFT_MIN_FREQ, t0);
        float f1 = FFT_MIN_FREQ * powf(max_freq / FFT_MIN_FREQ, t1);
        int b0 = (int)(f0 * (float)FFT_SIZE / (float)OUT_RATE);
        int b1 = (int)(f1 * (float)FFT_SIZE / (float)OUT_RATE);

        if (b0 < 1) b0 = 1;
        if (b1 < 1) b1 = 1;
        if (b0 > max_bin) b0 = max_bin;
        if (b1 > max_bin) b1 = max_bin;
        if (b1 < b0) b1 = b0;

        float energy = 0.0f;
        int bins = 0;
        for (int b = b0; b <= b1; b++) {
            float real = fft_re[b];
            float imag = fft_im[b];
            float mag = sqrtf(real * real + imag * imag) * (2.0f / (float)FFT_SIZE);
            energy += mag;
            bins++;
        }
        if (bins > 0) energy /= (float)bins;

        // Slightly emphasize lower bands for a classic graphic-EQ look.
        float low_boost = 1.15f - (0.35f * t0);
        if (low_boost < 0.75f) low_boost = 0.75f;
        energy *= low_boost;

        fft_band_energy[i] = energy;
        if (energy > frame_max) frame_max = energy;
    }

    if (frame_max < 0.000001f) frame_max = 0.000001f;
    float target_gain = 0.90f / frame_max;
    if (target_gain < 1.0f) target_gain = 1.0f;
    if (target_gain > 140.0f) target_gain = 140.0f;
    fft_auto_gain = fft_auto_gain * 0.95f + target_gain * 0.05f;

    for (int i = 0; i < band_count; i++) {
        float p = fft_band_energy[i] * fft_auto_gain;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        p = powf(p, 0.62f);

        if (p > viz_levels[i]) viz_levels[i] = viz_levels[i] * 0.40f + p * 0.60f;
        else viz_levels[i] = viz_levels[i] * 0.88f + p * 0.12f;

        if (p > viz_peaks[i]) {
            viz_peaks[i] = p;
            viz_peak_timers[i] = cfg.viz_peak_hold;
        } else if (viz_peak_timers[i] > 0) {
            viz_peak_timers[i]--;
        } else {
            viz_peaks[i] *= 0.95f;
        }
    }

    for (int i = band_count; i < MAX_VIZ_BANDS; i++) {
        viz_levels[i] *= 0.85f;
        if (viz_peak_timers[i] > 0) viz_peak_timers[i]--;
        else viz_peaks[i] *= 0.95f;
    }
}

void viz_update_levels(const int16_t *audio_buf, int samples_per_frame) {
    int band_count = cfg.viz_bands;
    if (band_count < 1) band_count = 1;
    if (band_count > MAX_VIZ_BANDS) band_count = MAX_VIZ_BANDS;

    if (cfg.viz_mode == VIZ_MODE_FFT_EQ) {
        fft_update_levels(audio_buf, samples_per_frame, band_count);
        return;
    }

    int sample_stride = (band_count == 40) ? 20 : 40;
    int total_samples = samples_per_frame * 2; // Interleaved stereo buffer

    if (!audio_buf || total_samples <= 0) {
        viz_decay_levels(band_count);
        return;
    }

    for (int i = 0; i < band_count; i++) {
        int sample_idx = i * sample_stride;
        if (sample_idx >= total_samples) sample_idx = total_samples - 1;

        // Manual abs to avoid undefined behavior with INT16_MIN (-32768)
        int32_t sample = audio_buf[sample_idx];
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

    int draw_bar_width = bar_width;
    if (cfg.responsive && draw_bar_width > layout.viz.w) draw_bar_width = layout.viz.w;

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h > max_h) h = max_h;
        int x_base = viz_band_x(i, band_count, draw_bar_width, start_x, spacing);

        // Draw main bar
        for (int v = 0; v < h; v++) {
            uint16_t color = cfg.viz_gradient ? get_gradient_color((float)v / (float)max_h) : cfg.fg_rgb;
            for (int w = 0; w < draw_bar_width; w++) draw_pixel(x_base + w, base_y - v, color);
        }

        // Draw peak hold dot
        if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
            int peak_h = (int)(viz_peaks[i] * max_h);
            if (peak_h >= max_h) peak_h = max_h - 1;
            uint16_t peak_color = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
            for (int w = 0; w < draw_bar_width; w++) {
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
        int x = viz_band_x(i, band_count, 2, start_x, spacing);
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
        int x = viz_band_x(i, band_count, 1, start_x, spacing);
        uint16_t color = cfg.viz_gradient ? get_gradient_color(viz_levels[i]) : cfg.fg_rgb;

        // Draw vertical line
        for (int v = 0; v <= h; v++) draw_pixel(x, base_y - v, color);

        // Connect to next band
        if (i < band_count - 1) {
            int next_h = (int)(viz_levels[i + 1] * max_h);
            if (next_h >= max_h) next_h = max_h - 1;
            int next_x = viz_band_x(i + 1, band_count, 1, start_x, spacing);
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
    float left_level = 0.0f, right_level = 0.0f;
    float left_sum = 0.0f, right_sum = 0.0f;
    float left_peak = 0.0f, right_peak = 0.0f;
    int level_samples = 0;

    for (int i = 0; i < samples_per_frame; i += 4) {
        int32_t ls = audio_buf[i*2], rs = audio_buf[i*2+1];
        float l = (ls < 0 ? -ls : ls) / 32768.0f;
        float r = (rs < 0 ? -rs : rs) / 32768.0f;
        left_sum += l;
        right_sum += r;
        if (l > left_peak) left_peak = l;
        if (r > right_peak) right_peak = r;
        level_samples++;
    }

    if (level_samples > 0) {
        float left_avg = left_sum / (float)level_samples;
        float right_avg = right_sum / (float)level_samples;

        // Blend average + peak so channels stay responsive but don't collapse to identical values.
        left_level = left_avg * 0.75f + left_peak * 0.25f;
        right_level = right_avg * 0.75f + right_peak * 0.25f;
        if (left_level > 1.0f) left_level = 1.0f;
        if (right_level > 1.0f) right_level = 1.0f;
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
    const int meter_gap = 4;
    const int label_h = 8;
    const int row_step = (meter_h + meter_gap > label_h) ? (meter_h + meter_gap) : label_h;
    const int pair_h = row_step + label_h;
    int left_y = cfg.viz_y - 15;
    int right_y = left_y + row_step;

    if (cfg.responsive) {
        meter_w = layout.viz_meter_w;
        if (meter_w < 1) meter_w = 1;
        meter_x = layout.viz.x + (layout.viz.w - meter_w);
        label_x = layout.viz.x;

        if (layout.viz.h >= pair_h) {
            int pair_top = layout.viz.y + (layout.viz.h - pair_h) / 2;
            left_y = pair_top;
            right_y = pair_top + row_step;
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

    if (cfg.viz_mode == VIZ_MODE_BARS || cfg.viz_mode == VIZ_MODE_FFT_EQ) {
        draw_bars_mode(band_count);
    } else if (cfg.viz_mode == VIZ_MODE_DOTS) {
        draw_dots_mode(band_count);
    } else if (cfg.viz_mode == VIZ_MODE_LINE) {
        draw_line_mode(band_count);
    } else if (cfg.viz_mode == VIZ_MODE_VU && vu_audio_buf) {
        draw_vu_meter_mode(vu_audio_buf, vu_samples_per_frame);
    }
}
