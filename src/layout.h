#pragma once

#include <stdbool.h>

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    // Usable region from boundary percentages.
    int area_x, area_y, area_w, area_h;
    bool is_wide;

    // Album art region. Zero-sized when art does not reserve space.
    Rect art;

    // Remaining content area after art placement.
    int content_x, content_y, content_w, content_h;

    // Element bounds. icons is the transport row (bottom of the stack); it is
    // zero-sized when the user hides it or when the content area is too short.
    Rect icons, text, viz, bar, time;

    // Region inside the sunken visualizer panel where modes actually draw.
    Rect viz_inner;

    // Title glyph scale (2 when there is room for a large title, else 1).
    int text_scale;

    // Icon anchors.
    int icon_shuffle_x;
    int icon_seek_x;
    int icon_pause_x;

    // Visualizer geometry.
    int viz_start_x;
    int viz_bar_width;
    int viz_spacing;
    int viz_max_h;
    int viz_meter_w;
} Layout;

extern Layout layout;

void layout_compute(void);
