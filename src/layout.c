#include "layout.h"
#include "config.h"
#include "video.h"

Layout layout;

static int clamp_i(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int min_i(int a, int b) {
    return (a < b) ? a : b;
}

static Rect zero_rect(void) {
    Rect r = {0, 0, 0, 0};
    return r;
}

void layout_compute(void) {
    const int top_pct = clamp_i(cfg.ui_top, 0, 100);
    const int left_pct = clamp_i(cfg.ui_left, 0, 100);
    int bottom_pct = clamp_i(cfg.ui_bottom, 0, 100);
    int right_pct = clamp_i(cfg.ui_right, 0, 100);

    if (bottom_pct <= top_pct) bottom_pct = clamp_i(top_pct + 1, 1, 100);
    if (right_pct <= left_pct) right_pct = clamp_i(left_pct + 1, 1, 100);

    int x0 = FB_WIDTH * left_pct / 100;
    int x1 = FB_WIDTH * right_pct / 100;
    int y0 = FB_HEIGHT * top_pct / 100;
    int y1 = FB_HEIGHT * bottom_pct / 100;

    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (x1 > FB_WIDTH) x1 = FB_WIDTH;
    if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;

    layout.area_x = x0;
    layout.area_y = y0;
    layout.area_w = x1 - x0;
    layout.area_h = y1 - y0;
    layout.is_wide = (layout.area_w * 3) > (layout.area_h * 4);
    layout.art_reserves_space = cfg.show_art;

    layout.art = zero_rect();
    layout.icons = zero_rect();
    layout.text = zero_rect();
    layout.viz = zero_rect();
    layout.bar = zero_rect();
    layout.time = zero_rect();

    layout.content_x = layout.area_x;
    layout.content_y = layout.area_y;
    layout.content_w = layout.area_w;
    layout.content_h = layout.area_h;

    if (layout.area_w <= 0 || layout.area_h <= 0) {
        layout.viz_start_x = 0;
        layout.viz_bar_width = 1;
        layout.viz_spacing = 1;
        layout.viz_max_h = 1;
        layout.viz_meter_w = 1;
        layout.icon_shuffle_x = 0;
        layout.icon_seek_x = 0;
        layout.icon_pause_x = 0;
        return;
    }

    const int gap_after_art = 4;
    if (cfg.show_art) {
        int art_side = min_i(layout.area_w, layout.area_h) * 40 / 100;
        art_side = clamp_i(art_side, 32, 120);

        if (layout.is_wide) {
            int max_side = layout.area_w - gap_after_art - 48;
            if (max_side < 0) max_side = 0;
            if (art_side > max_side) art_side = max_side;
            if (art_side > layout.area_h) art_side = layout.area_h;
            if (art_side > 0) {
                layout.art.x = layout.area_x;
                layout.art.y = layout.area_y + (layout.area_h - art_side) / 2;
                layout.art.w = art_side;
                layout.art.h = art_side;
            }
        } else {
            int max_side = layout.area_h - gap_after_art - 56;
            if (max_side < 0) max_side = 0;
            if (art_side > max_side) art_side = max_side;
            if (art_side > layout.area_w) art_side = layout.area_w;
            if (art_side > 0) {
                layout.art.x = layout.area_x + (layout.area_w - art_side) / 2;
                layout.art.y = layout.area_y;
                layout.art.w = art_side;
                layout.art.h = art_side;
            }
        }
    }

    if (cfg.show_art && layout.art.w > 0 && layout.art.h > 0) {
        if (layout.is_wide) {
            layout.content_x = layout.art.x + layout.art.w + gap_after_art;
            layout.content_y = layout.area_y;
            layout.content_w = (layout.area_x + layout.area_w) - layout.content_x;
            layout.content_h = layout.area_h;
        } else {
            layout.content_x = layout.area_x;
            layout.content_y = layout.art.y + layout.art.h + gap_after_art;
            layout.content_w = layout.area_w;
            layout.content_h = (layout.area_y + layout.area_h) - layout.content_y;
        }
    }

    if (layout.content_w < 1) layout.content_w = 1;
    if (layout.content_h < 1) layout.content_h = 1;

    const int icons_h = 8;
    const int text_h = 8;
    const int bar_h = 1;
    const int time_h = 8;
    const int viz_min_h = 16;

    const int use_icons = cfg.show_ico ? 1 : 0;
    const int use_viz = cfg.show_viz ? 1 : 0;
    const int use_text = cfg.show_txt ? 1 : 0;
    const int use_bar = cfg.show_bar ? 1 : 0;
    const int use_time = cfg.show_tim ? 1 : 0;
    const int blocks = use_icons + use_viz + use_text + use_bar + use_time;

    int fixed_h = 0;
    if (use_icons) fixed_h += icons_h;
    if (use_viz) fixed_h += viz_min_h;
    if (use_text) fixed_h += text_h;
    if (use_bar) fixed_h += bar_h;
    if (use_time) fixed_h += time_h;

    int viz_h = use_viz ? viz_min_h : 0;
    if (use_viz && fixed_h > layout.content_h) {
        viz_h -= (fixed_h - layout.content_h);
        if (viz_h < 4) viz_h = 4;
        fixed_h = (use_icons ? icons_h : 0) + (use_text ? text_h : 0) +
                  (use_bar ? bar_h : 0) + (use_time ? time_h : 0) + viz_h;
    }

    int gap = 0;
    const int gap_count = (blocks > 1) ? (blocks - 1) : 0;
    if (gap_count > 0 && layout.content_h > fixed_h) {
        gap = (layout.content_h - fixed_h) / gap_count;
        if (gap > 8) gap = 8;
    }

    int used_h = fixed_h + (gap * gap_count);
    int surplus = layout.content_h - used_h;
    if (use_viz) viz_h += surplus;
    if (use_viz && viz_h < 1) viz_h = 1;

    int y = layout.content_y;
    if (!use_viz && surplus > 0) y += surplus / 2;

    if (use_icons) {
        layout.icons.x = layout.content_x;
        layout.icons.y = y;
        layout.icons.w = layout.content_w;
        layout.icons.h = icons_h;
        y += icons_h + gap;
    }

    if (use_viz) {
        layout.viz.x = layout.content_x;
        layout.viz.y = y;
        layout.viz.w = layout.content_w;
        layout.viz.h = viz_h;
        y += viz_h + gap;
    }

    if (use_text) {
        layout.text.x = layout.content_x;
        layout.text.y = y;
        layout.text.w = layout.content_w;
        layout.text.h = text_h;
        y += text_h + gap;
    }

    if (use_bar) {
        int bar_w = layout.content_w * 80 / 100;
        if (bar_w < 40) bar_w = layout.content_w;
        layout.bar.w = bar_w;
        layout.bar.h = bar_h;
        layout.bar.x = layout.content_x + (layout.content_w - bar_w) / 2;
        layout.bar.y = y;
        y += bar_h + gap;
    }

    if (use_time) {
        layout.time.x = layout.content_x;
        layout.time.y = y;
        layout.time.w = layout.content_w;
        layout.time.h = time_h;
    }

    // Hide elements that overflow the content boundary
    int bottom = layout.content_y + layout.content_h;
    if (layout.time.h > 0 && layout.time.y + layout.time.h > bottom) layout.time = zero_rect();
    if (layout.bar.h > 0 && layout.bar.y + layout.bar.h > bottom) layout.bar = zero_rect();
    if (layout.text.h > 0 && layout.text.y + layout.text.h > bottom) layout.text = zero_rect();
    if (layout.viz.h > 0 && layout.viz.y + layout.viz.h > bottom) layout.viz = zero_rect();
    if (layout.icons.h > 0 && layout.icons.y + layout.icons.h > bottom) layout.icons = zero_rect();

    if (layout.icons.w > 0) {
        layout.icon_shuffle_x = layout.icons.x;
        layout.icon_pause_x = layout.icons.x + layout.icons.w - 16;
        if (layout.icon_pause_x < layout.icons.x) layout.icon_pause_x = layout.icons.x;

        int seek_x = layout.icons.x + layout.icons.w / 3;
        if (seek_x < layout.icons.x + 40) seek_x = layout.icons.x + 40;
        if (seek_x > layout.icon_pause_x - 16) seek_x = layout.icon_pause_x - 16;
        if (seek_x < layout.icons.x) seek_x = layout.icons.x;
        layout.icon_seek_x = seek_x;
    } else {
        layout.icon_shuffle_x = layout.content_x;
        layout.icon_seek_x = layout.content_x;
        layout.icon_pause_x = layout.content_x;
    }

    if (layout.viz.w > 0 && layout.viz.h > 0) {
        layout.viz_max_h = layout.viz.h;

        int bands = (cfg.viz_bands > 0) ? cfg.viz_bands : 40;
        int spacing = layout.viz.w / bands;
        if (spacing < 1) spacing = 1;
        layout.viz_spacing = spacing;

        int bars_w = spacing * bands;
        layout.viz_start_x = layout.viz.x + (layout.viz.w - bars_w) / 2;

        int bar_width = spacing - 1;
        if (bar_width < 1) bar_width = 1;
        if (bar_width > 4) bar_width = 4;
        layout.viz_bar_width = bar_width;

        layout.viz_meter_w = layout.viz.w - 16;
        if (layout.viz_meter_w < 8) layout.viz_meter_w = layout.viz.w;
        if (layout.viz_meter_w < 1) layout.viz_meter_w = 1;
    } else {
        layout.viz_max_h = 0;
        layout.viz_spacing = 0;
        layout.viz_start_x = layout.content_x;
        layout.viz_bar_width = 0;
        layout.viz_meter_w = 0;
    }
}
