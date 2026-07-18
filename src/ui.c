// Screen composition for the Hi-Fi Deck look: background, framed art, sunken
// visualizer panel, marquee title, progress bar, and transport icons. Handles
// both the responsive layout and the manual media_*_y offset fallback.
#include "ui.h"
#include "video.h"
#include "config.h"
#include "layout.h"
#include "metadata.h"
#include "visualizer.h"
#include <stdio.h>
#include <string.h>

#define MARQUEE_HOLD_FRAMES 90
#define ICON_SIZE 10

static int marquee_hold = MARQUEE_HOLD_FRAMES;

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
    marquee_hold = MARQUEE_HOLD_FRAMES;
}

static void format_time(char *out, size_t out_sz, uint64_t frames, uint32_t rate) {
    int sec = rate ? (int)(frames / rate) : 0;
    snprintf(out, out_sz, "%02d:%02d", sec / 60, sec % 60);
}

static int progress_width(const UiFrame *f, int width) {
    if (width <= 0 || f->total_frames == 0 || f->cur_frame == 0) return 0;
    if (f->cur_frame >= f->total_frames) return width;
    return (int)(((double)f->cur_frame / (double)f->total_frames) * (double)width);
}

// ---- Pixel transport icons (10x10, drawn procedurally) ---------------------

static void icon_play(int x, int y, uint16_t c) {
    for (int r = 0; r < ICON_SIZE; r++) {
        int d = (r < ICON_SIZE / 2) ? r : ICON_SIZE - 1 - r;
        int w = 2 + d * 2;
        for (int i = 0; i < w; i++) draw_pixel(x + i, y + r, c);
    }
}

static void icon_pause(int x, int y, uint16_t c) {
    draw_rect_fill(x, y, 3, ICON_SIZE, c);
    draw_rect_fill(x + 7, y, 3, ICON_SIZE, c);
}

static void icon_prev(int x, int y, uint16_t c) {
    draw_rect_fill(x, y, 2, ICON_SIZE, c);
    for (int r = 0; r < ICON_SIZE; r++) {
        int d = (r < ICON_SIZE / 2) ? r : ICON_SIZE - 1 - r;
        int w = 2 + d * 2;
        if (w > 8) w = 8;
        for (int i = 0; i < w; i++) draw_pixel(x + ICON_SIZE - 1 - i, y + r, c);
    }
}

static void icon_next(int x, int y, uint16_t c) {
    for (int r = 0; r < ICON_SIZE; r++) {
        int d = (r < ICON_SIZE / 2) ? r : ICON_SIZE - 1 - r;
        int w = 2 + d * 2;
        if (w > 8) w = 8;
        for (int i = 0; i < w; i++) draw_pixel(x + i, y + r, c);
    }
    draw_rect_fill(x + 8, y, 2, ICON_SIZE, c);
}

static void icon_shuffle(int x, int y, uint16_t c) {
    for (int i = 0; i < 9; i++) {
        draw_pixel(x + i, y + i, c);
        draw_pixel(x + i, y + 8 - i, c);
    }
    draw_pixel(x + 7, y, c);
    draw_pixel(x + 8, y + 1, c);
    draw_pixel(x + 7, y + 8, c);
    draw_pixel(x + 8, y + 7, c);
}

static void icon_seek(int x, int y, int dir, uint16_t c) {
    // Double chevron pointing in the seek direction.
    for (int r = 0; r < ICON_SIZE; r++) {
        int d = (r < ICON_SIZE / 2) ? r : ICON_SIZE - 1 - r;
        int w = 1 + d;
        for (int i = 0; i < w; i++) {
            if (dir > 0) {
                draw_pixel(x + i, y + r, c);
                draw_pixel(x + 5 + i, y + r, c);
            } else {
                draw_pixel(x + ICON_SIZE - 1 - i, y + r, c);
                draw_pixel(x + 4 - i, y + r, c);
            }
        }
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
    if (!art_buffer || w <= 0 || h <= 0) return;
    draw_rect_fill(x, y + h + 2, w + 4, 2, pal->shadow);
    draw_rect_fill(x + w + 2, y, 2, h + 2, pal->shadow);
    draw_rect_outline(x - 1, y - 1, w + 2, h + 2, pal->frame_face);
    draw_rect_bevel(x - 2, y - 2, w + 4, h + 4, pal->bevel_light, pal->bevel_dark);
    ui_blit_art(x, y, w, h);
}

// Marquee: hold at the start, crawl left until the tail is visible, hold,
// snap back. Static text stays pinned to the region's left edge.
static void ui_update_marquee(int *scroll_x, int text_w, int region_x, int region_w) {
    if (text_w <= region_w) {
        *scroll_x = region_x;
        return;
    }

    int min_x = region_x + region_w - text_w;
    if (*scroll_x > region_x || *scroll_x < min_x) {
        *scroll_x = region_x;
        marquee_hold = MARQUEE_HOLD_FRAMES;
        return;
    }
    if (marquee_hold > 0) {
        marquee_hold--;
        return;
    }
    if (*scroll_x == min_x) {
        *scroll_x = region_x;
        marquee_hold = MARQUEE_HOLD_FRAMES;
        return;
    }
    (*scroll_x)--;
    if (*scroll_x == min_x) marquee_hold = MARQUEE_HOLD_FRAMES;
}

static void ui_draw_debug_overlay(void) {
    draw_rect_outline(layout.area_x, layout.area_y, layout.area_w, layout.area_h, 0x07FF);
    draw_rect_outline(layout.content_x, layout.content_y, layout.content_w, layout.content_h, 0x07E0);
    draw_rect_outline(layout.art.x, layout.art.y, layout.art.w, layout.art.h, 0xFFE0);
    draw_rect_outline(layout.icons.x, layout.icons.y, layout.icons.w, layout.icons.h, 0xFD20);
    draw_rect_outline(layout.text.x, layout.text.y, layout.text.w, layout.text.h, 0xF81F);
    draw_rect_outline(layout.viz.x, layout.viz.y, layout.viz.w, layout.viz.h, 0xF800);
    draw_rect_outline(layout.bar.x, layout.bar.y, layout.bar.w, layout.bar.h, 0xFFFF);
    draw_rect_outline(layout.time.x, layout.time.y, layout.time.w, layout.time.h, 0x001F);
}

// ---- Responsive layout -----------------------------------------------------

static void ui_draw_transport(const UiFrame *f, const UiPalette *pal) {
    Rect r = layout.icons;
    int y = r.y + (r.h - ICON_SIZE) / 2;
    int play_x = layout.icon_pause_x;
    bool trio_fits = r.w >= 58;

    if (trio_fits)
        icon_prev(layout.icon_seek_x, y, (f->seek_dir < 0) ? pal->fg : pal->fg_faint);
    if (f->paused) icon_pause(play_x, y, pal->fg);
    else icon_play(play_x, y, pal->fg);
    if (trio_fits)
        icon_next(play_x + 24, y, (f->seek_dir > 0) ? pal->fg : pal->fg_faint);

    if (trio_fits && layout.icon_shuffle_x >= play_x + 24 + ICON_SIZE + 4)
        icon_shuffle(layout.icon_shuffle_x, y, f->shuffle ? pal->fg : pal->fg_faint);

    if (f->track_count > 1) {
        char counter[24];
        snprintf(counter, sizeof(counter), "%d/%d", f->track_index + 1, f->track_count);
        int cw = (int)strlen(counter) * GLYPH_WIDTH;
        int cx = r.x + r.w - cw;
        if (cx >= layout.icon_shuffle_x + ICON_SIZE + 6)
            draw_text(cx, r.y + (r.h - 8) / 2, counter, pal->fg_faint);
    }
}

static void ui_draw_responsive(UiFrame *f, const UiPalette *pal) {
    if (cfg.show_art && layout.art.w > 0 && layout.art.h > 0)
        ui_draw_art_framed(layout.art.x, layout.art.y, layout.art.w, layout.art.h, pal);

    if (cfg.show_viz && layout.viz.w > 0 && layout.viz.h > 0) {
        draw_rect_fill(layout.viz.x, layout.viz.y, layout.viz.w, layout.viz.h, pal->panel);
        draw_rect_bevel(layout.viz.x, layout.viz.y, layout.viz.w, layout.viz.h,
                        pal->bevel_dark, pal->bevel_light);
        viz_draw();
    }

    if (cfg.show_txt && layout.text.w > 0) {
        int scale = (layout.text_scale > 0) ? layout.text_scale : 1;
        int text_w = (int)strlen(display_str) * GLYPH_WIDTH * scale;
        ui_update_marquee(f->scroll_x, text_w, layout.text.x, layout.text.w);
        draw_text_scaled_clipped(*f->scroll_x, layout.text.y, display_str, pal->fg,
                                 scale, layout.text.x, layout.text.w);
    }

    if (cfg.show_bar && f->total_frames > 0 && layout.bar.w > 2 && layout.bar.h > 2) {
        draw_rect_outline(layout.bar.x, layout.bar.y, layout.bar.w, layout.bar.h, pal->fg_dim);
        draw_rect_fill(layout.bar.x + 1, layout.bar.y + 1,
                       layout.bar.w - 2, layout.bar.h - 2, pal->panel);
        int fill = progress_width(f, layout.bar.w - 2);
        if (fill > 0)
            draw_rect_fill(layout.bar.x + 1, layout.bar.y + 1, fill, layout.bar.h - 2, pal->fg_dim);
        int handle_x = layout.bar.x + 1 + ((fill > 2) ? fill - 2 : 0);
        draw_rect_fill(handle_x, layout.bar.y + 1, 2, layout.bar.h - 2, pal->handle);
    }

    if (cfg.show_tim && layout.time.w > 0) {
        char elapsed[16];
        format_time(elapsed, sizeof(elapsed), f->cur_frame, f->source_rate);
        int x0 = (layout.bar.w > 0) ? layout.bar.x : layout.time.x;
        int x1 = (layout.bar.w > 0) ? layout.bar.x + layout.bar.w
                                    : layout.time.x + layout.time.w;
        draw_text(x0, layout.time.y, elapsed, pal->fg_dim);
        if (f->total_frames > 0) {
            char total[16];
            format_time(total, sizeof(total), f->total_frames, f->source_rate);
            draw_text(x1 - (int)strlen(total) * GLYPH_WIDTH, layout.time.y, total, pal->fg_dim);
        }
    }

    if (cfg.show_ico && layout.icons.w > 0 && layout.icons.h > 0)
        ui_draw_transport(f, pal);

    if (cfg.debug_layout) ui_draw_debug_overlay();
}

// ---- Manual-offset fallback (media_*_y positions) --------------------------

static void ui_draw_legacy(UiFrame *f, const UiPalette *pal) {
    if (cfg.show_art && art_buffer)
        ui_draw_art_framed(120, cfg.art_y, 80, 80, pal);

    if (cfg.show_txt) {
        draw_text(*f->scroll_x, cfg.txt_y, display_str, pal->fg);
        (*f->scroll_x)--;
        if (*f->scroll_x < -((int)strlen(display_str) * GLYPH_WIDTH)) *f->scroll_x = FB_WIDTH;
    }

    if (cfg.show_viz) viz_draw();

    if (cfg.show_bar && f->total_frames > 0) {
        draw_rect_outline(60, cfg.bar_y, 200, 8, pal->fg_dim);
        draw_rect_fill(61, cfg.bar_y + 1, 198, 6, pal->panel);
        int fill = progress_width(f, 198);
        if (fill > 0) draw_rect_fill(61, cfg.bar_y + 1, fill, 6, pal->fg_dim);
        int handle_x = 61 + ((fill > 2) ? fill - 2 : 0);
        draw_rect_fill(handle_x, cfg.bar_y + 1, 2, 6, pal->handle);
    }

    if (cfg.show_tim) {
        char elapsed[16];
        format_time(elapsed, sizeof(elapsed), f->cur_frame, f->source_rate);
        if (f->total_frames > 0) {
            char total[16];
            char line[40];
            format_time(total, sizeof(total), f->total_frames, f->source_rate);
            snprintf(line, sizeof(line), "%s / %s", elapsed, total);
            draw_text((FB_WIDTH - (int)strlen(line) * GLYPH_WIDTH) / 2, cfg.tim_y, line, pal->fg);
        } else {
            draw_text(140, cfg.tim_y, elapsed, pal->fg);
        }
    }

    if (cfg.show_ico) {
        if (f->shuffle) icon_shuffle(20, cfg.ico_y, pal->fg);
        if (f->paused) icon_pause(280, cfg.ico_y, pal->fg);
        if (f->seek_dir != 0) icon_seek(60, cfg.ico_y, f->seek_dir, pal->fg);
    }
}

void ui_draw(UiFrame *frame) {
    UiPalette pal = ui_palette();
    video_fill_vgradient(pal.bg_top, pal.bg_bottom);
    if (cfg.responsive) ui_draw_responsive(frame, &pal);
    else ui_draw_legacy(frame, &pal);
}
