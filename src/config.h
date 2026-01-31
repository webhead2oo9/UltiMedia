#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "libretro.h"

// Configuration structure for all UI and display options
typedef struct {
    uint16_t bg_rgb, fg_rgb;
    int art_y, txt_y, viz_y, bar_y, tim_y, ico_y;
    bool show_art, show_txt, show_viz, show_bar, show_tim, show_ico, lcd_on;
    int viz_bands, viz_mode, viz_peak_hold;
    bool viz_gradient, use_filename;
} Config;

// Global configuration instance
extern Config cfg;

// Parse LibRetro core options into cfg struct
void config_update(retro_environment_t environ_cb);

// Declare core option variables to LibRetro
void config_declare_variables(retro_environment_t cb);
