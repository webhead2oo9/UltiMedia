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

// Frame duration from the frontend (60fps fallback); paces the trail/ghost/
// history cadences and the VU integration in real time.
static float viz_dt = 1.0f / 60.0f;
#define VIZ_CADENCE_SEC (1.0f / 30.0f)

void viz_set_frame_dt(float dt) {
    viz_dt = dt;
}

// Dots-mode phosphor trail: previous dot heights, most recent first.
static int dot_trail[MAX_VIZ_BANDS][2] = {{0}};
static float dot_trail_acc = 0.0f;

// Horizon mode: ring of recent spectrum rows forming a 3D landscape; the
// newest ridge stands at the front and history recedes into fog.
#define HORIZON_ROWS 24
static float horizon_hist[HORIZON_ROWS][MAX_VIZ_BANDS] = {{0}};
static int horizon_head = 0;
static float horizon_acc = 0.0f;

// Scope mode: raw mono sample ring plus per-column afterglow traces.
#define SCOPE_RING 2048
#define SCOPE_SPAN 800            // ~17ms window at 48kHz
#define SCOPE_TRIGGER_SEARCH 480  // rising-zero-crossing hunt range
static int16_t scope_ring[SCOPE_RING] = {0};
static int scope_ring_pos = 0;
static int16_t scope_ghost_lo[2][FB_WIDTH] = {{0}};
static int16_t scope_ghost_hi[2][FB_WIDTH] = {{0}};
static float scope_ghost_acc = 0.0f;

// 2048 points at 48kHz gives ~23Hz bins so the low log-spaced bands stop
// sharing bins; the analysis window grows to ~43ms, fine for a visualizer.
#define FFT_SIZE 2048
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
    // Clamp: out-of-range input would overflow the uint8_t channel math.
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
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

    static float mono_samples[FFT_SIZE];  // 8KB; keep off the frontend's stack
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

    // Feed the Horizon landscape history at a fixed real-time cadence.
    horizon_acc += viz_dt;
    if (horizon_acc >= VIZ_CADENCE_SEC) {
        horizon_acc -= VIZ_CADENCE_SEC;
        if (horizon_acc > VIZ_CADENCE_SEC) horizon_acc = 0.0f;
        horizon_head = (horizon_head + 1) % HORIZON_ROWS;
        for (int i = 0; i < MAX_VIZ_BANDS; i++)
            horizon_hist[horizon_head][i] = (i < band_count) ? viz_levels[i] : 0.0f;
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

    // ~150ms integration in both directions for the analog needle feel,
    // converted to real time so the ballistics match on non-60Hz hosts.
    float k = 1.0f - powf(0.88f, viz_dt * 60.0f);
    viz_levels[0] += (left_target - viz_levels[0]) * k;
    viz_levels[1] += (right_target - viz_levels[1]) * k;

    viz_update_peak(0);
    viz_update_peak(1);

    for (int i = 2; i < band_count; i++) {
        viz_decay_band(i);
    }
}

// Always fed so switching into Scope mode shows a live trace immediately.
static void scope_feed(const int16_t *audio_buf, int samples_per_frame) {
    if (!audio_buf || samples_per_frame <= 0) return;
    for (int i = 0; i < samples_per_frame; i++) {
        int32_t mono = ((int32_t)audio_buf[i * 2] + (int32_t)audio_buf[i * 2 + 1]) / 2;
        scope_ring[scope_ring_pos] = (int16_t)mono;
        scope_ring_pos = (scope_ring_pos + 1) & (SCOPE_RING - 1);
    }
}

void viz_update_levels(const int16_t *audio_buf, int samples_per_frame) {
    int band_count = cfg.viz_bands;
    if (band_count < 1) band_count = 1;
    if (band_count > MAX_VIZ_BANDS) band_count = MAX_VIZ_BANDS;

    scope_feed(audio_buf, samples_per_frame);

    if (cfg.viz_mode == VIZ_MODE_VU) {
        vu_update_levels(audio_buf, samples_per_frame, band_count);
        return;
    }
    if (cfg.viz_mode == VIZ_MODE_SCOPE) {
        viz_decay_levels(band_count);
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

    // Unlit segments sit just above the sunken panel shade (ui_palette mixes
    // the panel at bg->black 150), not the brighter screen background.
    uint16_t unlit_color = mix565(mix565(cfg.bg_rgb, 0x0000, 150), cfg.fg_rgb, 16);

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h > max_h) h = max_h;
        int x_base = viz_band_x(i, band_count, draw_bar_width);

        // LED ladder: 2px lit segments with 1px gaps; segments above the
        // level stay faintly visible so the ladder reads as hardware.
        for (int v = 0; v < max_h; v++) {
            if ((v % 3) == 2) continue;
            uint16_t color;
            if (v < h) color = cfg.viz_gradient ? get_gradient_color((float)v / (float)max_h) : cfg.fg_rgb;
            else color = unlit_color;
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

static void draw_dot(int x, int y, uint16_t color) {
    draw_pixel(x, y, color);
    draw_pixel(x + 1, y, color);
    draw_pixel(x, y - 1, color);
    draw_pixel(x + 1, y - 1, color);
}

static void draw_dots_mode(int band_count) {
    int max_h = layout.viz_max_h;
    int base_y = layout.viz_inner.y + layout.viz_inner.h - 1;
    if (max_h < 1) max_h = 1;

    // Record ghost positions at a fixed real-time cadence so the trail lag
    // reads the same on any host refresh rate.
    dot_trail_acc += viz_dt;
    int record = 0;
    if (dot_trail_acc >= VIZ_CADENCE_SEC) {
        record = 1;
        dot_trail_acc -= VIZ_CADENCE_SEC;
        if (dot_trail_acc > VIZ_CADENCE_SEC) dot_trail_acc = 0.0f;
    }

    for (int i = 0; i < band_count; i++) {
        int h = (int)(viz_levels[i] * max_h);
        if (h >= max_h) h = max_h - 1;
        int x = viz_band_x(i, band_count, 2);
        uint16_t color = cfg.viz_gradient ? get_gradient_color(viz_levels[i]) : cfg.fg_rgb;

        // Phosphor trail: older ghost first, both dimmed toward the bg.
        // Heights are clamped because a layout shrink can leave stale trail
        // values above the new panel height for a few frames.
        for (int g = 1; g >= 0; g--) {
            int gh = dot_trail[i][g];
            if (gh > max_h - 1) gh = max_h - 1;
            if (gh < 0) gh = 0;
            uint16_t gcolor = cfg.viz_gradient ? get_gradient_color((float)gh / (float)max_h) : cfg.fg_rgb;
            gcolor = mix565(gcolor, cfg.bg_rgb, (g == 0) ? 140 : 195);
            draw_dot(x, base_y - gh, gcolor);
        }

        if (record) {
            dot_trail[i][1] = dot_trail[i][0];
            dot_trail[i][0] = h;
        }

        draw_dot(x, base_y - h, color);

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

static int16_t scope_at(int idx) {
    return scope_ring[idx & (SCOPE_RING - 1)];
}

// Triggered oscilloscope with a car-deck face: dot graticule, per-column
// min/max envelope, amplitude-gradient trace with a glow edge, and two
// afterglow ghost traces.
static void draw_scope_mode(void) {
    int x0 = layout.viz_inner.x;
    int w = layout.viz_inner.w;
    if (w > FB_WIDTH) w = FB_WIDTH;
    int half = (layout.viz_inner.h - 2) / 2;
    if (half < 1) half = 1;
    int cy = layout.viz_inner.y + layout.viz_inner.h / 2;

    uint16_t panel = mix565(cfg.bg_rgb, 0x0000, 150);
    uint16_t grid = mix565(panel, cfg.fg_rgb, 26);
    uint16_t glow = mix565(panel, cfg.fg_rgb, 70);
    uint16_t ghost0 = mix565(cfg.fg_rgb, panel, 150);
    uint16_t ghost1 = mix565(cfg.fg_rgb, panel, 200);

    // Graticule: center hairline plus dot ticks along the top and bottom rails.
    for (int x = 0; x < w; x++) draw_pixel(x0 + x, cy, grid);
    for (int x = 0; x < w; x += 10) {
        draw_pixel(x0 + x, cy - half, grid);
        draw_pixel(x0 + x, cy + half, grid);
    }

    // Trigger: align the window to the first rising zero crossing so the
    // trace holds still instead of jittering.
    int idx0 = scope_ring_pos + SCOPE_RING - SCOPE_SPAN - SCOPE_TRIGGER_SEARCH;
    int win = idx0;
    for (int k = 0; k < SCOPE_TRIGGER_SEARCH - 1; k++) {
        if (scope_at(idx0 + k) < 0 && scope_at(idx0 + k + 1) >= 0) {
            win = idx0 + k + 1;
            break;
        }
    }

    // Afterglow ghosts, oldest first, then the live trace over them.
    for (int g = 1; g >= 0; g--) {
        uint16_t gc = (g == 0) ? ghost0 : ghost1;
        for (int x = 0; x < w; x++) {
            int lo = scope_ghost_lo[g][x], hi = scope_ghost_hi[g][x];
            if (lo < -half) lo = -half;
            if (hi > half) hi = half;
            for (int yy = lo; yy <= hi; yy++) draw_pixel(x0 + x, cy - yy, gc);
        }
    }

    scope_ghost_acc += viz_dt;
    int record = 0;
    if (scope_ghost_acc >= VIZ_CADENCE_SEC) {
        record = 1;
        scope_ghost_acc -= VIZ_CADENCE_SEC;
        if (scope_ghost_acc > VIZ_CADENCE_SEC) scope_ghost_acc = 0.0f;
    }

    for (int x = 0; x < w; x++) {
        int s0 = win + (x * SCOPE_SPAN) / w;
        int s1 = win + ((x + 1) * SCOPE_SPAN) / w;
        if (s1 <= s0) s1 = s0 + 1;
        int mn = 32767, mx = -32768;
        for (int s = s0; s < s1; s++) {
            int v = scope_at(s);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int ylo = (mn * half) / 32768;
        int yhi = (mx * half) / 32768;

        for (int yy = ylo; yy <= yhi; yy++) {
            int dist = (yy < 0) ? -yy : yy;
            uint16_t c = cfg.viz_gradient ? get_gradient_color((float)dist / (float)half) : cfg.fg_rgb;
            draw_pixel(x0 + x, cy - yy, c);
        }
        if (yhi + 1 <= half) draw_pixel(x0 + x, cy - (yhi + 1), glow);
        if (ylo - 1 >= -half) draw_pixel(x0 + x, cy - (ylo - 1), glow);

        if (record) {
            scope_ghost_lo[1][x] = scope_ghost_lo[0][x];
            scope_ghost_hi[1][x] = scope_ghost_hi[0][x];
            scope_ghost_lo[0][x] = (int16_t)ylo;
            scope_ghost_hi[0][x] = (int16_t)yhi;
        }
    }
}

// Kenwood-style mirrored spectrum: bands fan out from a center emblem with an
// outward shear, LED-segmented, over a horizon line with a dimmed reflection.
static void draw_mirror_mode(int band_count) {
    Rect in = layout.viz_inner;
    int cx = in.x + in.w / 2;

    uint16_t panel = mix565(cfg.bg_rgb, 0x0000, 150);
    uint16_t unlit = mix565(panel, cfg.fg_rgb, 16);
    uint16_t horizon = mix565(panel, cfg.fg_rgb, 40);

    // Bars rise from a horizon line; the reflection lives below it.
    int up_h = (in.h * 5) / 8;
    if (up_h < 6) up_h = 6;
    if (up_h > in.h - 1) up_h = in.h - 1;
    int base_y = in.y + up_h;
    int refl_h = in.y + in.h - base_y - 1;
    if (refl_h < 0) refl_h = 0;
    int max_h = up_h - 1;
    if (max_h < 1) max_h = 1;
    int max_shear = max_h / 6;

    int emblem_w = 30;
    if (emblem_w > in.w / 3) emblem_w = in.w / 3;
    int half_avail = (in.w - emblem_w) / 2 - 2 - max_shear;
    if (half_avail < 1) half_avail = 1;
    // Narrow panels decimate evenly across the spectrum instead of silently
    // truncating the treble bands away.
    int draw_bands = (band_count > half_avail) ? half_avail : band_count;
    int spacing = half_avail / draw_bands;
    if (spacing < 1) spacing = 1;
    int bar_w = spacing - 1;
    if (bar_w < 1) bar_w = 1;
    if (bar_w > 4) bar_w = 4;

    for (int x = 0; x < in.w; x++) draw_pixel(in.x + x, base_y, horizon);

    for (int i = 0; i < draw_bands; i++) {
        int src = i * band_count / draw_bands;
        int h = (int)(viz_levels[src] * max_h);
        if (h > max_h) h = max_h;
        int off0 = emblem_w / 2 + 2 + i * spacing;

        for (int side = 0; side < 2; side++) {
            int dir = side ? -1 : 1;

            // LED ladder leaning outward: shear grows with height.
            for (int v = 0; v < max_h; v++) {
                if ((v % 3) == 2) continue;
                int bx = cx + dir * (off0 + v / 6);
                uint16_t color = (v < h)
                    ? (cfg.viz_gradient ? get_gradient_color((float)v / (float)max_h) : cfg.fg_rgb)
                    : unlit;
                for (int w = 0; w < bar_w; w++)
                    draw_pixel(bx + dir * w, base_y - 1 - v, color);
            }

            // Reflection: lit segments only, half height, dimmed toward panel.
            int rh = h / 2;
            if (rh > refl_h) rh = refl_h;
            for (int v = 0; v < rh; v++) {
                if ((v % 3) == 2) continue;
                int bx = cx + dir * (off0 + (v * 2) / 6);
                uint16_t src = cfg.viz_gradient ? get_gradient_color((float)(v * 2) / (float)max_h) : cfg.fg_rgb;
                uint16_t color = mix565(src, panel, 150);
                for (int w = 0; w < bar_w; w++)
                    draw_pixel(bx + dir * w, base_y + 1 + v, color);
            }

            if (cfg.viz_peak_hold > 0 && viz_peak_timers[src] > 0) {
                int ph = (int)(viz_peaks[src] * max_h);
                if (ph >= max_h) ph = max_h - 1;
                int bx = cx + dir * (off0 + ph / 6);
                uint16_t pc = cfg.viz_gradient ? 0xF800 : cfg.fg_rgb;
                for (int w = 0; w < bar_w; w++)
                    draw_pixel(bx + dir * w, base_y - 1 - ph, pc);
            }
        }
    }

    // Center emblem: a bass-pulsing diamond crest — a dim outline ring with a
    // solid core that grows and heats up with the low bands.
    float bass = (viz_levels[0] + viz_levels[1] + viz_levels[2]) / 3.0f;
    int es = 18 + (int)(bass * 8.0f);
    if (es > emblem_w - 2) es = emblem_w - 2;
    if (es > up_h - 2) es = up_h - 2;
    if (es >= 6) {
        int r = es / 2;
        int my = in.y + (up_h - es) / 2 + r;
        uint16_t ring = mix565(panel, cfg.fg_rgb, 80);
        uint16_t core = cfg.viz_gradient ? get_gradient_color(bass) : cfg.fg_rgb;

        for (int dy = -r; dy <= r; dy++) {
            int wl = r - ((dy < 0) ? -dy : dy);
            draw_pixel(cx - wl, my + dy, ring);
            draw_pixel(cx + wl, my + dy, ring);
        }
        int rc = r - 3;
        if (rc < 1) rc = 1;
        for (int dy = -rc; dy <= rc; dy++) {
            int wl = rc - ((dy < 0) ? -dy : dy);
            for (int dx = -wl; dx <= wl; dx++) draw_pixel(cx + dx, my + dy, core);
        }
    }
}

// 3D spectrum landscape, drawn back to front (painter's algorithm). Each row
// is a ridge: a bright line over a dark body fill that occludes rows behind
// it, with perspective shrink and fog toward the horizon.
static void draw_horizon_mode(int band_count) {
    Rect in = layout.viz_inner;
    if (band_count < 2) band_count = 2;

    uint16_t panel = mix565(cfg.bg_rgb, 0x0000, 150);

    int cx = in.x + in.w / 2;
    int front_y = in.y + in.h - 2;
    int back_y = in.y + (in.h * 2) / 5;
    if (back_y >= front_y) back_y = front_y - 1;
    int ridge_max = (in.h * 11) / 20;
    if (ridge_max < 2) ridge_max = 2;

    const float persp_back = 1.0f / (1.0f + 2.4f);

    for (int a = HORIZON_ROWS - 1; a >= 0; a--) {
        const float *row = horizon_hist[(horizon_head - a + HORIZON_ROWS) % HORIZON_ROWS];
        float t = (float)a / (float)(HORIZON_ROWS - 1);
        float persp = 1.0f / (1.0f + 2.4f * t);
        int baseline = front_y - (int)((float)(front_y - back_y) * (1.0f - persp) / (1.0f - persp_back));
        int hw = (int)((float)(in.w / 2 - 2) * (0.35f + 0.65f * persp));
        if (hw < 4) hw = 4;
        int fog = (int)(t * 185.0f);
        float rise = (float)ridge_max * persp;

        int prev_x = cx - hw;
        int prev_y = baseline - (int)(row[0] * rise);
        for (int i = 1; i <= band_count; i++) {
            int px, py;
            if (i < band_count) {
                px = cx - hw + (2 * hw * i) / (band_count - 1);
                py = baseline - (int)(row[i] * rise);
            } else {
                px = prev_x + 1;   // close out the rightmost column
                py = prev_y;
            }
            int dx = px - prev_x;
            if (dx < 1) dx = 1;
            for (int step = 0; step < dx; step++) {
                int col = prev_x + step;
                int y0 = prev_y + ((py - prev_y) * step) / dx;
                int y1 = prev_y + ((py - prev_y) * (step + 1)) / dx;
                int lo = (y0 < y1) ? y0 : y1;
                int hi = (y0 > y1) ? y0 : y1;
                float lvl = (float)(baseline - lo) / (rise + 0.001f);
                if (lvl < 0.0f) lvl = 0.0f;
                if (lvl > 1.0f) lvl = 1.0f;
                uint16_t line_c = cfg.viz_gradient ? get_gradient_color(lvl) : cfg.fg_rgb;
                line_c = mix565(line_c, panel, fog);
                uint16_t body_c = mix565(panel, line_c, 70);
                for (int yy = hi + 1; yy <= baseline; yy++) draw_pixel(col, yy, body_c);
                for (int yy = lo; yy <= hi; yy++) draw_pixel(col, yy, line_c);
            }
            prev_x = px;
            prev_y = py;
        }
    }
}

void viz_reset_state(void) {
    memset(viz_levels, 0, sizeof(viz_levels));
    memset(viz_peaks, 0, sizeof(viz_peaks));
    memset(viz_peak_timers, 0, sizeof(viz_peak_timers));
    memset(dot_trail, 0, sizeof(dot_trail));
    dot_trail_acc = 0.0f;
    memset(horizon_hist, 0, sizeof(horizon_hist));
    horizon_head = 0;
    horizon_acc = 0.0f;
    memset(scope_ring, 0, sizeof(scope_ring));
    scope_ring_pos = 0;
    memset(scope_ghost_lo, 0, sizeof(scope_ghost_lo));
    memset(scope_ghost_hi, 0, sizeof(scope_ghost_hi));
    scope_ghost_acc = 0.0f;
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
    } else if (cfg.viz_mode == VIZ_MODE_SCOPE) {
        draw_scope_mode();
    } else if (cfg.viz_mode == VIZ_MODE_MIRROR) {
        draw_mirror_mode(band_count);
    } else if (cfg.viz_mode == VIZ_MODE_HORIZON) {
        draw_horizon_mode(band_count);
    }
}
