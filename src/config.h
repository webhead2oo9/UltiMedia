#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "libretro.h"

typedef enum {
    SHOW_FILENAME_WITH_EXT = 0,
    SHOW_FILENAME_WITHOUT_EXT = 1,
    SHOW_ID = 2
} TrackTextMode;

// Configuration structure for all UI and display options
typedef struct {
    uint16_t bg_rgb, fg_rgb;
    int art_y, txt_y, viz_y, bar_y, tim_y, ico_y;
    int ui_top, ui_bottom, ui_left, ui_right;
    bool show_art, show_txt, show_viz, show_bar, show_tim, show_ico;
    bool responsive, debug_layout;
    int viz_bands, viz_mode, viz_peak_hold;
    bool viz_gradient;
    TrackTextMode track_text_mode;
} Config;

// Global configuration instance
extern Config cfg;

// Parse LibRetro core options into cfg struct
void config_update(retro_environment_t environ_cb);

// Declare core option variables to LibRetro
void config_declare_variables(retro_environment_t cb);
