#include "layout.h"
#include "config.h"
#include "video.h"

Layout layout;

// Sunken panel border + padding around the visualizer's drawing region.
#define VIZ_PANEL_PAD 4

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

static void reset_viz_geometry(void) {
    layout.viz_max_h = 0;
    layout.viz_spacing = 0;
    layout.viz_start_x = layout.content_x;
    layout.viz_bar_width = 0;
    layout.viz_meter_w = 0;
}

// Bar/meter geometry is derived from the panel interior, not the panel rect.
static void compute_viz_geometry(void) {
    if (layout.viz_inner.w <= 0 || layout.viz_inner.h <= 0) {
        reset_viz_geometry();
        return;
    }

    layout.viz_max_h = layout.viz_inner.h;

    int bands = (cfg.viz_bands > 0) ? cfg.viz_bands : 40;
    int spacing = layout.viz_inner.w / bands;
    if (spacing < 1) spacing = 1;
    layout.viz_spacing = spacing;

    int bars_w = spacing * bands;
    layout.viz_start_x = layout.viz_inner.x + (layout.viz_inner.w - bars_w) / 2;

    int bar_width = spacing - 1;
    if (bar_width < 1) bar_width = 1;
    if (bar_width > 4) bar_width = 4;
    layout.viz_bar_width = bar_width;

    layout.viz_meter_w = layout.viz_inner.w - 16;
    if (layout.viz_meter_w < 8) layout.viz_meter_w = layout.viz_inner.w;
    if (layout.viz_meter_w < 1) layout.viz_meter_w = 1;
}

static void compute_viz_inner(void) {
    layout.viz_inner = zero_rect();
    if (layout.viz.w <= 0 || layout.viz.h <= 0) return;

    int pad = VIZ_PANEL_PAD;
    if (layout.viz.w <= pad * 2 + 2 || layout.viz.h <= pad * 2 + 2) pad = 1;
    layout.viz_inner.x = layout.viz.x + pad;
    layout.viz_inner.y = layout.viz.y + pad;
    layout.viz_inner.w = layout.viz.w - pad * 2;
    layout.viz_inner.h = layout.viz.h - pad * 2;
    if (layout.viz_inner.w < 1 || layout.viz_inner.h < 1) layout.viz_inner = zero_rect();
}

static void place_art(void) {
    const int gap_after_art = 8;   // room for the frame, drop shadow, and breathing space
    const int frame_inset = 2;     // bevel frame drawn just outside the art rect
    if (!cfg.show_art) return;

    int art_side = min_i(layout.area_w, layout.area_h) * 40 / 100;
    art_side = clamp_i(art_side, 32, 120);

    if (layout.is_wide) {
        int max_side = layout.area_w - frame_inset - gap_after_art - 48;
        if (max_side < 0) max_side = 0;
        if (art_side > max_side) art_side = max_side;
        // Reserve the frame inset above plus frame + drop shadow (4px) below.
        if (art_side > layout.area_h - frame_inset - 4) art_side = layout.area_h - frame_inset - 4;
        if (art_side > 0) {
            // Top-aligned so the art frame's top edge lines up with the
            // visualizer panel's top edge instead of floating mid-column.
            layout.art.x = layout.area_x + frame_inset;
            layout.art.y = layout.area_y + frame_inset;
            layout.art.w = art_side;
            layout.art.h = art_side;
        }
    } else {
        int max_side = layout.area_h - frame_inset - gap_after_art - 56;
        if (max_side < 0) max_side = 0;
        if (art_side > max_side) art_side = max_side;
        // Frame on both sides plus the drop shadow's 2px to the right.
        if (art_side > layout.area_w - frame_inset * 2 - 2) art_side = layout.area_w - frame_inset * 2 - 2;
        if (art_side > 0) {
            layout.art.x = layout.area_x + (layout.area_w - art_side) / 2;
            layout.art.y = layout.area_y + frame_inset;
            layout.art.w = art_side;
            layout.art.h = art_side;
        }
    }

    if (layout.art.w > 0 && layout.art.h > 0) {
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

    layout.art = zero_rect();
    layout.icons = zero_rect();
    layout.text = zero_rect();
    layout.viz = zero_rect();
    layout.bar = zero_rect();
    layout.time = zero_rect();
    layout.viz_inner = zero_rect();
    layout.text_scale = 1;

    layout.content_x = layout.area_x;
    layout.content_y = layout.area_y;
    layout.content_w = layout.area_w;
    layout.content_h = layout.area_h;

    if (layout.area_w <= 0 || layout.area_h <= 0) {
        reset_viz_geometry();
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

    place_art();

    if (layout.content_w < 1) layout.content_w = 1;
    if (layout.content_h < 1) layout.content_h = 1;

    const int transport_h = 12;
    const int bar_h = 8;
    const int time_h = 8;
    const int cluster_gap = 2;
    const int viz_min_h = 20;                       // 12px of drawing inside the panel
    const int vu_compact_h = 16 + 2 * VIZ_PANEL_PAD;

    const int use_viz = cfg.show_viz ? 1 : 0;
    const int use_text = cfg.show_txt ? 1 : 0;
    const int use_bar = cfg.show_bar ? 1 : 0;
    const int use_time = cfg.show_tim ? 1 : 0;
    int use_transport = cfg.show_ico ? 1 : 0;

    int text_scale = use_text ? 2 : 1;
    // VU mode needs a 16px interior (two meter rows + labels); the FFT modes
    // start from the smaller floor and absorb surplus height later.
    int viz_h = use_viz ? ((cfg.viz_mode == VIZ_MODE_VU) ? vu_compact_h : viz_min_h) : 0;
    int cluster_h = 0;
    int fixed_h = 0;

    // Fit pass. On short screens the transport row vanishes first, then the
    // large title falls back to 1x, then the visualizer panel gives up height.
    for (;;) {
        int text_h = use_text ? 8 * text_scale : 0;
        int members = use_text + use_bar + use_time;
        cluster_h = text_h + (use_bar ? bar_h : 0) + (use_time ? time_h : 0);
        if (members > 1) cluster_h += (members - 1) * cluster_gap;
        fixed_h = viz_h + cluster_h + (use_transport ? transport_h : 0);
        if (fixed_h <= layout.content_h) break;
        if (use_transport) { use_transport = 0; continue; }
        if (text_scale == 2) { text_scale = 1; continue; }
        break;
    }

    if (use_viz && fixed_h > layout.content_h) {
        // VU never shrinks below two meter rows (no mono fallback); the
        // overflow guards drop other elements instead.
        int viz_floor = (cfg.viz_mode == VIZ_MODE_VU) ? vu_compact_h : 12;
        viz_h -= (fixed_h - layout.content_h);
        if (viz_h < viz_floor) viz_h = viz_floor;
        fixed_h = viz_h + cluster_h;
    }

    const int has_cluster = (use_text + use_bar + use_time) > 0 ? 1 : 0;
    const int groups = use_viz + has_cluster + use_transport;
    const int gap_count = (groups > 1) ? (groups - 1) : 0;
    int group_gap = 0;
    if (gap_count > 0 && layout.content_h > fixed_h) {
        group_gap = (layout.content_h - fixed_h) / gap_count;
        if (group_gap > 8) group_gap = 8;
    }

    int used_h = fixed_h + (group_gap * gap_count);
    int surplus = layout.content_h - used_h;
    int y = layout.content_y;
    if (use_viz) {
        if (cfg.viz_mode == VIZ_MODE_VU) {
            if (surplus > 0) y += surplus / 2;
        } else if (surplus > 0) {
            // Never add negative surplus: that would drag the panel below the
            // 12px floor; the overflow guards drop elements instead.
            viz_h += surplus;
        }
    } else if (surplus > 0) {
        y += surplus / 2;
    }
    if (use_viz && viz_h < 1) viz_h = 1;

    if (use_viz) {
        layout.viz.x = layout.content_x;
        layout.viz.y = y;
        layout.viz.w = layout.content_w;
        layout.viz.h = viz_h;
        y += viz_h;
        if (has_cluster || use_transport) y += group_gap;
    }

    if (use_text) {
        layout.text.x = layout.content_x;
        layout.text.y = y;
        layout.text.w = layout.content_w;
        layout.text.h = 8 * text_scale;
        layout.text_scale = text_scale;
        y += layout.text.h;
        if (use_bar || use_time) y += cluster_gap;
    }

    if (use_bar) {
        // Full content width so the bar's left edge lines up with the panel
        // and title instead of floating centered.
        layout.bar.w = layout.content_w;
        layout.bar.h = bar_h;
        layout.bar.x = layout.content_x;
        layout.bar.y = y;
        y += bar_h;
        if (use_time) y += cluster_gap;
    }

    if (use_time) {
        layout.time.x = layout.content_x;
        layout.time.y = y;
        layout.time.w = layout.content_w;
        layout.time.h = time_h;
        y += time_h;
    }

    if (use_transport) {
        if (has_cluster) y += group_gap;
        layout.icons.x = layout.content_x;
        layout.icons.y = y;
        layout.icons.w = layout.content_w;
        layout.icons.h = transport_h;
    }

    // Hide elements that overflow the content boundary, bottom-most first.
    int bottom = layout.content_y + layout.content_h;
    if (layout.icons.h > 0 && layout.icons.y + layout.icons.h > bottom) layout.icons = zero_rect();
    if (layout.time.h > 0 && layout.time.y + layout.time.h > bottom) layout.time = zero_rect();
    if (layout.bar.h > 0 && layout.bar.y + layout.bar.h > bottom) layout.bar = zero_rect();
    if (layout.text.h > 0 && layout.text.y + layout.text.h > bottom) layout.text = zero_rect();
    if (layout.viz.h > 0 && layout.viz.y + layout.viz.h > bottom) layout.viz = zero_rect();

    if (layout.icons.w > 0) {
        // Transport anchors: prev / play-pause / next trio around the center,
        // shuffle to the right of the trio.
        int center = layout.icons.x + layout.icons.w / 2;
        layout.icon_pause_x = center - 5;
        layout.icon_seek_x = layout.icon_pause_x - 24;
        layout.icon_shuffle_x = layout.icon_pause_x + 48;
        if (layout.icon_seek_x < layout.icons.x) layout.icon_seek_x = layout.icons.x;
        if (layout.icon_shuffle_x + 10 > layout.icons.x + layout.icons.w)
            layout.icon_shuffle_x = layout.icons.x + layout.icons.w - 10;
    } else {
        layout.icon_shuffle_x = layout.content_x;
        layout.icon_seek_x = layout.content_x;
        layout.icon_pause_x = layout.content_x;
    }

    compute_viz_inner();
    compute_viz_geometry();
}
