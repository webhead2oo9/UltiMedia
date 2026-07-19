// Screen composition for the Hi-Fi Deck look: background, framed art, sunken
// visualizer panel, marquee title, progress bar, and transport icons.
#include "ui.h"
#include "video.h"
#include "config.h"
#include "layout.h"
#include "metadata.h"
#include "visualizer.h"
#include <stdio.h>
#include <string.h>

#define MARQUEE_HOLD_SEC 1.5f
#define MARQUEE_SPEED_PXS 60.0f
#define ICON_SIZE 10

static float ui_dt = 1.0f / 60.0f;
static float marquee_hold = MARQUEE_HOLD_SEC;
static float marquee_frac = 0.0f;

void ui_set_frame_dt(float dt) {
    ui_dt = dt;
}

// Resolution scale: chrome is authored at 1x (320x240) and multiplied so the
// composition is identical at 640x480, just crisper where it counts.
static int ui_s(void) {
    return (cfg.ui_scale > 0) ? cfg.ui_scale : 1;
}

// Every chrome color is derived from the user's bg/fg pair so the core
// options keep controlling the theme.
typedef struct {
    uint16_t fg, fg_dim, fg_faint;
    uint16_t bg_top, bg_bottom;
    uint16_t panel, frame_face, bevel_light, bevel_dark, shadow, handle;
} UiPalette;

static UiPalette ui_palette(void) {
    UiPalette p;
    uint16_t bg = cfg.bg_rgb;
    uint16_t fg = cfg.fg_rgb;
    p.fg = fg;
    p.fg_dim = mix565(fg, bg, 110);
    p.fg_faint = mix565(fg, bg, 185);
    p.bg_top = mix565(bg, 0xFFFF, 14);
    p.bg_bottom = mix565(bg, 0x0000, 96);
    p.panel = mix565(bg, 0x0000, 150);
    p.frame_face = mix565(bg, fg, 40);
    p.bevel_light = mix565(bg, 0xFFFF, 90);
    p.bevel_dark = mix565(bg, 0x0000, 190);
    p.shadow = mix565(bg, 0x0000, 210);
    p.handle = mix565(fg, 0xFFFF, 110);
    return p;
}

void ui_reset_marquee(void) {
    marquee_hold = MARQUEE_HOLD_SEC;
    marquee_frac = 0.0f;
}

static void format_time(char *out, size_t out_sz, uint64_t frames, uint32_t rate) {
    uint64_t sec = rate ? frames / rate : 0;
    snprintf(out, out_sz, "%02llu:%02llu",
             (unsigned long long)(sec / 60),
             (unsigned long long)(sec % 60));
}

static int progress_width(const UiFrame *f, int width) {
    if (width <= 0 || f->total_frames == 0 || f->cur_frame == 0) return 0;
    if (f->cur_frame >= f->total_frames) return width;
    return (int)(((double)f->cur_frame / (double)f->total_frames) * (double)width);
}

// ---- Pixel transport icons (10x10 unit grid, each unit an s x s block) -----

static void icon_px(int x, int y, int ux, int uy, uint16_t c) {
    int s = ui_s();
    draw_rect_fill(x + ux * s, y + uy * s, s, s, c);
}

static void icon_play(int x, int y, uint16_t c) {
    for (int r = 0; r < ICON_SIZE; r++) {
        int d = (r < ICON_SIZE / 2) ? r : ICON_SIZE - 1 - r;
        int w = 2 + d * 2;
        for (int i = 0; i < w; i++) icon_px(x, y, i, r, c);
    }
}

static void icon_pause(int x, int y, uint16_t c) {
    int s = ui_s();
    draw_rect_fill(x, y, 3 * s, ICON_SIZE * s, c);
    draw_rect_fill(x + 7 * s, y, 3 * s, ICON_SIZE * s, c);
}

static void icon_prev(int x, int y, uint16_t c) {
    int s = ui_s();
    draw_rect_fill(x, y, 2 * s, ICON_SIZE * s, c);
    for (int r = 0; r < ICON_SIZE; r++) {
        int d = (r < ICON_SIZE / 2) ? r : ICON_SIZE - 1 - r;
        int w = 2 + d * 2;
        if (w > 8) w = 8;
        for (int i = 0; i < w; i++) icon_px(x, y, ICON_SIZE - 1 - i, r, c);
    }
}

static void icon_next(int x, int y, uint16_t c) {
    int s = ui_s();
    for (int r = 0; r < ICON_SIZE; r++) {
        int d = (r < ICON_SIZE / 2) ? r : ICON_SIZE - 1 - r;
        int w = 2 + d * 2;
        if (w > 8) w = 8;
        for (int i = 0; i < w; i++) icon_px(x, y, i, r, c);
    }
    draw_rect_fill(x + 8 * s, y, 2 * s, ICON_SIZE * s, c);
}

static void icon_shuffle(int x, int y, uint16_t c) {
    // Two opposing arrows: top points right, bottom points left.
    for (int i = 0; i < 7; i++) icon_px(x, y, i, 2, c);
    for (int r = 0; r < 5; r++) {
        int d = (r < 3) ? r : 4 - r;
        for (int i = 0; i <= d; i++) icon_px(x, y, 6 + i, r, c);
    }
    for (int i = 3; i < 10; i++) icon_px(x, y, i, 7, c);
    for (int r = 0; r < 5; r++) {
        int d = (r < 3) ? r : 4 - r;
        for (int i = 0; i <= d; i++) icon_px(x, y, 3 - i, 5 + r, c);
    }
}

// ---- Shared building blocks ------------------------------------------------

static void ui_blit_art(int x, int y, int w, int h) {
    if (!art_buffer || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int src_y = j * art_h_src / h;
        for (int i = 0; i < w; i++) {
            int src_x = i * art_w_src / w;
            draw_pixel(x + i, y + j, art_buffer[src_y * art_w_src + src_x]);
        }
    }
}

static void ui_draw_art_framed(int x, int y, int w, int h, const UiPalette *pal) {
    int s = ui_s();
    if (!art_buffer || w <= 0 || h <= 0) return;
    draw_rect_fill(x, y + h + 2 * s, w + 4 * s, 2 * s, pal->shadow);
    draw_rect_fill(x + w + 2 * s, y, 2 * s, h + 2 * s, pal->shadow);
    for (int k = 0; k < s; k++)
        draw_rect_outline(x - s + k, y - s + k, w + 2 * s - 2 * k, h + 2 * s - 2 * k, pal->frame_face);
    for (int k = 0; k < s; k++)
        draw_rect_bevel(x - 2 * s + k, y - 2 * s + k, w + 4 * s - 2 * k, h + 4 * s - 2 * k,
                        pal->bevel_light, pal->bevel_dark);
    ui_blit_art(x, y, w, h);
}

// Marquee: hold at the start, crawl left until the tail is visible, hold,
// snap back. Static text stays pinned to the region's left edge. Speed and
// holds are in real time so playback rate stays correct on non-60Hz hosts.
static void ui_update_marquee(int *scroll_x, int text_w, int region_x, int region_w) {
    if (text_w <= region_w) {
        *scroll_x = region_x;
        return;
    }

    int min_x = region_x + region_w - text_w;
    if (*scroll_x > region_x || *scroll_x < min_x) {
        *scroll_x = region_x;
        marquee_hold = MARQUEE_HOLD_SEC;
        marquee_frac = 0.0f;
        return;
    }
    if (marquee_hold > 0.0f) {
        marquee_hold -= ui_dt;
        return;
    }
    if (*scroll_x == min_x) {
        *scroll_x = region_x;
        marquee_hold = MARQUEE_HOLD_SEC;
        marquee_frac = 0.0f;
        return;
    }
    marquee_frac += MARQUEE_SPEED_PXS * (float)ui_s() * ui_dt;
    int move = (int)marquee_frac;
    if (move > 0) {
        marquee_frac -= (float)move;
        *scroll_x -= move;
        if (*scroll_x <= min_x) {
            *scroll_x = min_x;
            marquee_hold = MARQUEE_HOLD_SEC;
        }
    }
}

// Outline 1px outside the rect so the overlay never overwrites the pixels
// it is annotating (an on-rect outline eats the top row of 8x8 glyphs).
static void debug_box(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    draw_rect_outline(x - 1, y - 1, w + 2, h + 2, color);
}

static void ui_draw_debug_overlay(void) {
    debug_box(layout.area_x, layout.area_y, layout.area_w, layout.area_h, 0x07FF);
    debug_box(layout.content_x, layout.content_y, layout.content_w, layout.content_h, 0x07E0);
    debug_box(layout.art.x, layout.art.y, layout.art.w, layout.art.h, 0xFFE0);
    debug_box(layout.icons.x, layout.icons.y, layout.icons.w, layout.icons.h, 0xFD20);
    debug_box(layout.text.x, layout.text.y, layout.text.w, layout.text.h, 0xF81F);
    debug_box(layout.viz.x, layout.viz.y, layout.viz.w, layout.viz.h, 0xF800);
    debug_box(layout.bar.x, layout.bar.y, layout.bar.w, layout.bar.h, 0xFFFF);
    debug_box(layout.time.x, layout.time.y, layout.time.w, layout.time.h, 0x001F);
}

static void ui_draw_transport(const UiFrame *f, const UiPalette *pal) {
    int s = ui_s();
    Rect r = layout.icons;
    int y = r.y + (r.h - ICON_SIZE * s) / 2;
    int play_x = layout.icon_pause_x;
    bool trio_fits = r.w >= 58 * s;

    if (trio_fits)
        icon_prev(layout.icon_seek_x, y, (f->seek_dir < 0) ? pal->fg : pal->fg_faint);
    if (f->paused) icon_pause(play_x, y, pal->fg);
    else icon_play(play_x, y, pal->fg);
    if (trio_fits)
        icon_next(play_x + 24 * s, y, (f->seek_dir > 0) ? pal->fg : pal->fg_faint);

    if (trio_fits && layout.icon_shuffle_x >= play_x + 24 * s + (ICON_SIZE + 4) * s)
        icon_shuffle(layout.icon_shuffle_x, y, f->shuffle ? pal->fg : pal->fg_faint);

    if (f->track_count > 1) {
        char counter[24];
        snprintf(counter, sizeof(counter), "%d/%d", f->track_index + 1, f->track_count);
        int cw = (int)strlen(counter) * GLYPH_WIDTH * s;
        int cx = r.x + r.w - cw;
        if (cx >= layout.icon_shuffle_x + (ICON_SIZE + 6) * s)
            draw_text_scaled_clipped(cx, r.y + (r.h - 8 * s) / 2, counter, pal->fg_faint, s, 0, fb_width);
    }
}

static void ui_draw_screen(UiFrame *f, const UiPalette *pal) {
    int s = ui_s();

    if (cfg.show_art && layout.art.w > 0 && layout.art.h > 0)
        ui_draw_art_framed(layout.art.x, layout.art.y, layout.art.w, layout.art.h, pal);

    if (cfg.show_viz && layout.viz.w > 0 && layout.viz.h > 0) {
        draw_rect_fill(layout.viz.x, layout.viz.y, layout.viz.w, layout.viz.h, pal->panel);
        for (int k = 0; k < s; k++)
            draw_rect_bevel(layout.viz.x + k, layout.viz.y + k,
                            layout.viz.w - 2 * k, layout.viz.h - 2 * k,
                            pal->bevel_dark, pal->bevel_light);
        viz_draw();
    }

    if (cfg.show_txt && layout.text.w > 0) {
        int scale = ((layout.text_scale > 0) ? layout.text_scale : 1) * s;
        int text_w = (int)strlen(display_str) * GLYPH_WIDTH * scale;
        ui_update_marquee(f->scroll_x, text_w, layout.text.x, layout.text.w);
        draw_text_scaled_clipped(*f->scroll_x, layout.text.y, display_str, pal->fg,
                                 scale, layout.text.x, layout.text.w);
    }

    if (cfg.show_bar && f->total_frames > 0 && layout.bar.w > 2 * s && layout.bar.h > 2 * s) {
        for (int k = 0; k < s; k++)
            draw_rect_outline(layout.bar.x + k, layout.bar.y + k,
                              layout.bar.w - 2 * k, layout.bar.h - 2 * k, pal->fg_dim);
        draw_rect_fill(layout.bar.x + s, layout.bar.y + s,
                       layout.bar.w - 2 * s, layout.bar.h - 2 * s, pal->panel);
        int fill = progress_width(f, layout.bar.w - 2 * s);
        if (fill > 0)
            draw_rect_fill(layout.bar.x + s, layout.bar.y + s, fill, layout.bar.h - 2 * s, pal->fg_dim);
        int handle_x = layout.bar.x + s + ((fill > 2 * s) ? fill - 2 * s : 0);
        draw_rect_fill(handle_x, layout.bar.y + s, 2 * s, layout.bar.h - 2 * s, pal->handle);
    }

    if (cfg.show_tim && layout.time.w > 0) {
        char elapsed[24];
        format_time(elapsed, sizeof(elapsed), f->cur_frame, f->source_rate);
        int x0 = (layout.bar.w > 0) ? layout.bar.x : layout.time.x;
        int x1 = (layout.bar.w > 0) ? layout.bar.x + layout.bar.w
                                    : layout.time.x + layout.time.w;
        draw_text_scaled_clipped(x0, layout.time.y, elapsed, pal->fg_dim, s, 0, fb_width);
        if (f->total_frames > 0) {
            char total[24];
            format_time(total, sizeof(total), f->total_frames, f->source_rate);
            int total_w = (int)strlen(total) * GLYPH_WIDTH * s;
            int elapsed_w = (int)strlen(elapsed) * GLYPH_WIDTH * s;
            // Skip the total when the row is too narrow for both labels.
            if (x0 + elapsed_w + GLYPH_WIDTH * s + total_w <= x1)
                draw_text_scaled_clipped(x1 - total_w, layout.time.y, total, pal->fg_dim, s, 0, fb_width);
        }
    }

    if (cfg.show_ico && layout.icons.w > 0 && layout.icons.h > 0)
        ui_draw_transport(f, pal);

    if (cfg.debug_layout) ui_draw_debug_overlay();
}

void ui_draw(UiFrame *frame) {
    UiPalette pal = ui_palette();
    video_fill_vgradient(pal.bg_top, pal.bg_bottom);
    ui_draw_screen(frame, &pal);
}
