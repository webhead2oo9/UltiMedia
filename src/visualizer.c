#include "visualizer.h"
#include "video.h"
#include "config.h"
#include "layout.h"
#include "audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

float viz_levels[MAX_VIZ_BANDS] = {0};
float viz_peaks[MAX_VIZ_BANDS] = {0};
int viz_peak_timers[MAX_VIZ_BANDS] = {0};

#define FFT_SIZE 1024
#define TWO_PI_F 6.28318530717958647692f
#define FFT_MIN_FREQ 35.0f
#define FFT_MAX_FREQ_RATIO 0.92f

static float fft_window[FFT_SIZE] = {0};
static bool fft_window_ready = false;
static float fft_re[FFT_SIZE] = {0};
static float fft_im[FFT_SIZE] = {0};
static float fft_band_energy[MAX_VIZ_BANDS] = {0};
static float fft_noise_floor[MAX_VIZ_BANDS] = {0};
static float fft_auto_gain = 10.0f;
static float fft_ref_level = 0.020f;
static float fft_hp_x1 = 0.0f;
static float fft_hp_y1 = 0.0f;
static float fft_ring[FFT_SIZE] = {0};
static int fft_ring_pos = 0;
static int fft_ring_count = 0;

static int viz_band_x(int band_idx, int band_count, int item_w) {
    if (item_w < 1) item_w = 1;
    if (item_w > layout.viz_inner.w) item_w = layout.viz_inner.w;
    if (layout.viz_inner.w <= item_w) return layout.viz_inner.x;
    if (band_count <= 1) return layout.viz_inner.x + (layout.viz_inner.w - item_w) / 2;

    int span = layout.viz_inner.w - item_w;
    return layout.viz_inner.x + (band_idx * span) / (band_count - 1);
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

static void viz_decay_peak(int band_idx) {
    if (viz_peak_timers[band_idx] > 0) viz_peak_timers[band_idx]--;
    else viz_peaks[band_idx] *= 0.95f;
}

static void viz_update_peak(int band_idx) {
    if (viz_levels[band_idx] > viz_peaks[band_idx]) {
        viz_peaks[band_idx] = viz_levels[band_idx];
        viz_peak_timers[band_idx] = cfg.viz_peak_hold;
    } else {
        viz_decay_peak(band_idx);
    }
}

static void viz_decay_band(int band_idx) {
    viz_levels[band_idx] *= 0.85f;
    viz_decay_peak(band_idx);
}

static void viz_decay_levels(int band_count) {
    for (int i = 0; i < band_count; i++) {
        viz_decay_band(i);
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

    for (int i = 0; i < available_frames; i++) {
        int src = i * 2;
        int32_t left = audio_buf[src];
        int32_t right = audio_buf[src + 1];
        float mono = (float)(left + right) * (0.5f / 32768.0f);
        // High-pass to reduce rumble/sub-bass bias before spectral analysis.
        float hp = mono - fft_hp_x1 + 0.995f * fft_hp_y1;
        fft_hp_x1 = mono;
        fft_hp_y1 = hp;
        fft_ring[fft_ring_pos] = hp;
        fft_ring_pos = (fft_ring_pos + 1) & (FFT_SIZE - 1);
        if (fft_ring_count < FFT_SIZE) fft_ring_count++;
    }

    if (fft_ring_count < FFT_SIZE) {
        viz_decay_levels(band_count);
        return;
    }

    float mono_samples[FFT_SIZE];
    float dc_sum = 0.0f;
    int ring_idx = fft_ring_pos;
    for (int i = 0; i < FFT_SIZE; i++) {
        float s = fft_ring[ring_idx];
        mono_samples[i] = s;
        dc_sum += s;
        ring_idx = (ring_idx + 1) & (FFT_SIZE - 1);
    }

    float dc_bias = dc_sum / (float)FFT_SIZE;
    for (int i = 0; i < FFT_SIZE; i++) {
        fft_re[i] = (mono_samples[i] - dc_bias) * fft_window[i];
        fft_im[i] = 0.0f;
    }

    fft_compute(fft_re, fft_im, FFT_SIZE);

    const int max_bin = FFT_SIZE / 2 - 1;
    const float nyquist = (float)OUT_RATE * 0.5f;
    const float max_freq = nyquist * FFT_MAX_FREQ_RATIO;
    float sum_sq = 0.0f;

    for (int i = 0; i < band_count; i++) {
        float t0 = (float)i / (float)band_count;
        float t1 = (float)(i + 1) / (float)band_count;
        float f0 = FFT_MIN_FREQ * powf(max_freq / FFT_MIN_FREQ, t0);
        float f1 = FFT_MIN_FREQ * powf(max_freq / FFT_MIN_FREQ, t1);
        int b0 = (int)floorf(f0 * (float)FFT_SIZE / (float)OUT_RATE);
        int b1 = (int)ceilf(f1 * (float)FFT_SIZE / (float)OUT_RATE);

        if (b0 < 1) b0 = 1;
        if (b1 < 1) b1 = 1;
        if (b0 > max_bin) b0 = max_bin;
        if (b1 > max_bin) b1 = max_bin;
        if (b1 < b0) b1 = b0;
        int pad = (i < band_count / 4) ? 2 : 1;
        b0 -= pad;
        b1 += pad;
        if (b0 < 1) b0 = 1;
        if (b1 > max_bin) b1 = max_bin;

        float power_sum = 0.0f;
        int bins = 0;
        for (int b = b0; b <= b1; b++) {
            float real = fft_re[b];
            float imag = fft_im[b];
            power_sum += real * real + imag * imag;
            bins++;
        }
        float energy = 0.0f;
        if (bins > 0) {
            energy = sqrtf(power_sum / (float)bins) * (2.0f / (float)FFT_SIZE);
        }

        // Whiten spectrum response so low bands do not dominate by default.
        float center_freq = sqrtf(f0 * f1);
        float whiten = powf(center_freq / 1000.0f, 0.55f);
        if (whiten < 0.18f) whiten = 0.18f;
        if (whiten > 1.60f) whiten = 1.60f;
        energy *= whiten;

        // Track a slow per-band floor and remove it to avoid pinned bars in quiet material.
        float floor = fft_noise_floor[i];
        if (floor <= 0.0f) floor = energy * 0.50f;
        if (energy < floor) floor = floor * 0.94f + energy * 0.06f;
        else floor = floor * 0.98f + energy * 0.02f;
        fft_noise_floor[i] = floor;

        float cleaned = energy - (floor * 1.02f);
        if (cleaned < 0.0f) cleaned = 0.0f;
        fft_band_energy[i] = cleaned;
        sum_sq += cleaned * cleaned;
    }

    float frame_rms = sqrtf(sum_sq / (float)band_count);
    if (frame_rms > fft_ref_level) fft_ref_level = fft_ref_level * 0.90f + frame_rms * 0.10f;
    else fft_ref_level = fft_ref_level * 0.992f + frame_rms * 0.008f;
    if (fft_ref_level < 0.0003f) fft_ref_level = 0.0003f;

    float target_gain = 0.14f / fft_ref_level;
    if (target_gain < 1.0f) target_gain = 1.0f;
    if (target_gain > 20.0f) target_gain = 20.0f;
    if (target_gain < fft_auto_gain) fft_auto_gain = fft_auto_gain * 0.65f + target_gain * 0.35f;
    else fft_auto_gain = fft_auto_gain * 0.96f + target_gain * 0.04f;

    for (int i = 0; i < band_count; i++) {
        float scaled = fft_band_energy[i] * fft_auto_gain;
        float db = 20.0f * log10f(scaled + 0.000001f);
        float p = (db + 62.0f) / 56.0f;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;

        float rise = (i < band_count / 4) ? 0.30f : 0.45f;
        float fall = (i < band_count / 4) ? 0.94f : 0.90f;
        if (p > viz_levels[i]) viz_levels[i] = viz_levels[i] * (1.0f - rise) + p * rise;
        else viz_levels[i] = viz_levels[i] * fall + p * (1.0f - fall);
        viz_update_peak(i);
    }

    for (int i = band_count; i < MAX_VIZ_BANDS; i++) {
        viz_decay_band(i);
        fft_noise_floor[i] *= 0.995f;
    }
}

// Map channel RMS onto the meter's -40..0 dB sweep.
static float vu_db_level(float rms) {
    if (rms <= 0.0001f) return 0.0f;
    float db = 20.0f * log10f(rms);
    float v = (db + 40.0f) / 40.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static void vu_update_levels(const int16_t *audio_buf, int samples_per_frame, int band_count) {
    float left_sq = 0.0f, right_sq = 0.0f;
    int level_samples = 0;

    if (audio_buf && samples_per_frame > 0) {
        for (int i = 0; i < samples_per_frame; i += 4) {
            float l = (float)audio_buf[i * 2] / 32768.0f;
            float r = (float)audio_buf[i * 2 + 1] / 32768.0f;
            left_sq += l * l;
            right_sq += r * r;
            level_samples++;
        }
    }

    float left_target = 0.0f, right_target = 0.0f;
    if (level_samples > 0) {
        left_target = vu_db_level(sqrtf(left_sq / (float)level_samples));
        right_target = vu_db_level(sqrtf(right_sq / (float)level_samples));
    }

    // ~150ms integration in both directions for the analog needle feel.
    viz_levels[0] = viz_levels[0] * 0.88f + left_target * 0.12f;
    viz_levels[1] = viz_levels[1] * 0.88f + right_target * 0.12f;

    viz_update_peak(0);
    viz_update_peak(1);

    for (int i = 2; i < band_count; i++) {
        viz_decay_band(i);
    }
}

void viz_update_levels(const int16_t *audio_buf, int samples_per_frame) {
    int band_count = cfg.viz_bands;
    if (band_count < 1) band_count = 1;
    if (band_count > MAX_VIZ_BANDS) band_count = MAX_VIZ_BANDS;

    if (cfg.viz_mode == VIZ_MODE_VU) {
        vu_update_levels(audio_buf, samples_per_frame, band_count);
        return;
    }

    fft_update_levels(audio_buf, samples_per_frame, band_count);
}

static void draw_bars_mode(int band_count) {
    int max_h = layout.viz_max_h;
    int base_y = layout.viz_inner.y + layout.viz_inner.h - 1;
    if (max_h < 1) max_h = 1;

    int draw_bar_width = layout.viz_bar_width;
    if (draw_bar_width > layout.viz_inner.w) draw_bar_width = layout.viz_inner.w;
    if (draw_bar_width < 1) draw_bar_width = 1;

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h > max_h) h = max_h;
        int x_base = viz_band_x(i, band_count, draw_bar_width);

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
    int max_h = layout.viz_max_h;
    int base_y = layout.viz_inner.y + layout.viz_inner.h - 1;
    if (max_h < 1) max_h = 1;

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h >= max_h) h = max_h - 1;
        int x = viz_band_x(i, band_count, 2);
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
    int max_h = layout.viz_max_h;
    int base_y = layout.viz_inner.y + layout.viz_inner.h - 1;
    if (max_h < 1) max_h = 1;

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h >= max_h) h = max_h - 1;
        int x = viz_band_x(i, band_count, 1);
        uint16_t color = cfg.viz_gradient ? get_gradient_color(viz_levels[i]) : cfg.fg_rgb;

        // Draw vertical line
        for (int v = 0; v <= h; v++) draw_pixel(x, base_y - v, color);

        // Connect to next band
        if (i < band_count - 1) {
            int next_h = (int)(viz_levels[i + 1] * max_h);
            if (next_h >= max_h) next_h = max_h - 1;
            int next_x = viz_band_x(i + 1, band_count, 1);
            int dx = next_x - x;
            int dy = next_h - h;

            for (int step = 0; step < dx; step++) {
                // Fill the vertical span to the next column so steep slopes
                // stay connected instead of dissolving into scattered dots.
                int y0 = h + (dy * step) / dx;
                int y1 = h + (dy * (step + 1)) / dx;
                int lo = (y0 < y1) ? y0 : y1;
                int hi = (y0 > y1) ? y0 : y1;
                for (int yy = lo; yy <= hi; yy++) {
                    uint16_t interp_color = cfg.viz_gradient ? get_gradient_color((float)yy / (float)max_h) : cfg.fg_rgb;
                    draw_pixel(x + step, base_y - yy, interp_color);
                }
            }
        }

        // Peak markers
        if (cfg.viz_peak_hold > 0 && viz_peak_timers[i] > 0) {
            int peak_h = (int)(viz_peaks[i] * max_h);
            if (peak_h >= max_h) peak_h = max_h - 1;
            uint16_t peak_color = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
            draw_pixel(x, base_y - peak_h, peak_color);
            draw_pixel(x + 1, base_y - peak_h, peak_color);
        }
    }
}

// Meter face for the -40..0 dB sweep: unlit track, ticks at -30/-20/-10/-6,
// red zone above -6 dB.
static void vu_draw_face(int meter_x, int row_y, int meter_w, int meter_h) {
    uint16_t track_color = mix565(cfg.bg_rgb, cfg.fg_rgb, 30);
    uint16_t tick_color = mix565(cfg.bg_rgb, cfg.fg_rgb, 100);
    uint16_t zone_color = mix565(cfg.bg_rgb, 0xF800, 100);
    static const float tick_pos[4] = {0.25f, 0.50f, 0.75f, 0.85f};

    draw_rect_fill(meter_x, row_y, meter_w, meter_h, track_color);
    int zone_x = (int)((float)meter_w * 0.85f);
    draw_rect_fill(meter_x + zone_x, row_y, meter_w - zone_x, meter_h, zone_color);
    for (int t = 0; t < 4; t++) {
        int tx = meter_x + (int)((float)meter_w * tick_pos[t]);
        for (int y = -1; y <= meter_h; y++) draw_pixel(tx, row_y + y, tick_color);
    }
}

static void vu_draw_meter_row(int label_x, int meter_x, int row_y, int meter_w,
                              int meter_h, int channel, const char *label) {
    draw_text(label_x, row_y, label, cfg.fg_rgb);
    vu_draw_face(meter_x, row_y, meter_w, meter_h);

    int fill_w = (int)(viz_levels[channel] * meter_w);
    if (fill_w > meter_w) fill_w = meter_w;
    for (int x = 0; x < fill_w; x++) {
        uint16_t color = cfg.viz_gradient ? get_gradient_color((float)x / (float)meter_w) : cfg.fg_rgb;
        for (int y = 0; y < meter_h; y++) draw_pixel(meter_x + x, row_y + y, color);
    }

    if (cfg.viz_peak_hold > 0 && viz_peak_timers[channel] > 0) {
        uint16_t peak_color = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
        int peak_x = (int)(viz_peaks[channel] * meter_w);
        if (peak_x >= meter_w) peak_x = meter_w - 1;
        for (int y = 0; y < meter_h; y++) draw_pixel(meter_x + peak_x, row_y + y, peak_color);
    }
}

static void draw_vu_meter_mode(void) {
    const int meter_h = 4;
    const int meter_gap = 4;
    const int label_h = 8;
    const int row_step = (meter_h + meter_gap > label_h) ? (meter_h + meter_gap) : label_h;
    const int pair_h = row_step + label_h;

    // Two meters or nothing — a lone mono meter reads as a broken channel.
    if (layout.viz_inner.h < pair_h) return;

    int meter_w = layout.viz_meter_w;
    if (meter_w < 1) meter_w = 1;
    int meter_x = layout.viz_inner.x + (layout.viz_inner.w - meter_w);
    int label_x = layout.viz_inner.x;
    int pair_top = layout.viz_inner.y + (layout.viz_inner.h - pair_h) / 2;

    vu_draw_meter_row(label_x, meter_x, pair_top, meter_w, meter_h, 0, "L");
    vu_draw_meter_row(label_x, meter_x, pair_top + row_step, meter_w, meter_h, 1, "R");
}

void viz_reset_state(void) {
    memset(viz_levels, 0, sizeof(viz_levels));
    memset(viz_peaks, 0, sizeof(viz_peaks));
    memset(viz_peak_timers, 0, sizeof(viz_peak_timers));
    memset(fft_re, 0, sizeof(fft_re));
    memset(fft_im, 0, sizeof(fft_im));
    memset(fft_band_energy, 0, sizeof(fft_band_energy));
    memset(fft_noise_floor, 0, sizeof(fft_noise_floor));
    memset(fft_ring, 0, sizeof(fft_ring));
    fft_auto_gain = 10.0f;
    fft_ref_level = 0.020f;
    fft_hp_x1 = 0.0f;
    fft_hp_y1 = 0.0f;
    fft_ring_pos = 0;
    fft_ring_count = 0;
}

void viz_draw(void) {
    int band_count = cfg.viz_bands;
    if (layout.viz_inner.w <= 0 || layout.viz_inner.h <= 0) return;

    if (cfg.viz_mode == VIZ_MODE_BARS || cfg.viz_mode == VIZ_MODE_FFT_EQ_LEGACY) {
        draw_bars_mode(band_count);
    } else if (cfg.viz_mode == VIZ_MODE_DOTS) {
        draw_dots_mode(band_count);
    } else if (cfg.viz_mode == VIZ_MODE_LINE) {
        draw_line_mode(band_count);
    } else if (cfg.viz_mode == VIZ_MODE_VU) {
        draw_vu_meter_mode();
    }
}
