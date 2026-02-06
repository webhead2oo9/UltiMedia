#include "config.h"
#include "visualizer.h"
#include <stdlib.h>
#include <string.h>

Config cfg;

void config_update(retro_environment_t environ_cb) {
    struct retro_variable var = {0};
    int r, g, b;

    var.key = "media_bg_r"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) r = atoi(var.value); else r=0;
    var.key = "media_bg_g"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) g = atoi(var.value); else g=64;
    var.key = "media_bg_b"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) b = atoi(var.value); else b=0;
    cfg.bg_rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    var.key = "media_fg_r"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) r = atoi(var.value); else r=0;
    var.key = "media_fg_g"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) g = atoi(var.value); else g=255;
    var.key = "media_fg_b"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) b = atoi(var.value); else b=0;
    cfg.fg_rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    var.key = "media_show_art"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_art = !strcmp(var.value, "On"); else cfg.show_art = true;
    var.key = "media_show_txt"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_txt = !strcmp(var.value, "On"); else cfg.show_txt = true;
    var.key = "media_show_viz"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_viz = !strcmp(var.value, "On"); else cfg.show_viz = true;
    var.key = "media_show_bar"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_bar = !strcmp(var.value, "On"); else cfg.show_bar = true;
    var.key = "media_show_tim"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_tim = !strcmp(var.value, "On"); else cfg.show_tim = true;
    var.key = "media_show_ico"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.show_ico = !strcmp(var.value, "On"); else cfg.show_ico = true;
    var.key = "media_responsive"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.responsive = !strcmp(var.value, "On"); else cfg.responsive = false;
    var.key = "media_art_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.art_y = atoi(var.value); else cfg.art_y = 40;
    var.key = "media_txt_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.txt_y = atoi(var.value); else cfg.txt_y = 150;
    var.key = "media_viz_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_y = atoi(var.value); else cfg.viz_y = 140;
    var.key = "media_bar_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.bar_y = atoi(var.value); else cfg.bar_y = 180;
    var.key = "media_tim_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.tim_y = atoi(var.value); else cfg.tim_y = 190;
    var.key = "media_ico_y"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.ico_y = atoi(var.value); else cfg.ico_y = 20;
    var.key = "media_ui_top"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.ui_top = atoi(var.value); else cfg.ui_top = 20;
    var.key = "media_ui_bottom"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.ui_bottom = atoi(var.value); else cfg.ui_bottom = 80;
    var.key = "media_ui_left"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.ui_left = atoi(var.value); else cfg.ui_left = 0;
    var.key = "media_ui_right"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.ui_right = atoi(var.value); else cfg.ui_right = 100;

    var.key = "media_viz_bands"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_bands = atoi(var.value); else cfg.viz_bands = 40;
    if (cfg.viz_bands < 1) cfg.viz_bands = 1;
    if (cfg.viz_bands > MAX_VIZ_BANDS) cfg.viz_bands = MAX_VIZ_BANDS;
    var.key = "media_viz_mode"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) {
        if (!strcmp(var.value, "Bars")) cfg.viz_mode = 0;
        else if (!strcmp(var.value, "VU Meter")) cfg.viz_mode = 3;
        else if (!strcmp(var.value, "Dots")) cfg.viz_mode = 1;
        else if (!strcmp(var.value, "Line")) cfg.viz_mode = 2;
    } else cfg.viz_mode = 0;
    var.key = "media_viz_gradient"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_gradient = !strcmp(var.value, "On"); else cfg.viz_gradient = true;
    var.key = "media_viz_peak_hold"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.viz_peak_hold = atoi(var.value); else cfg.viz_peak_hold = 30;
    var.key = "media_use_filename"; if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) cfg.use_filename = !strcmp(var.value, "On"); else cfg.use_filename = false;
}

void config_declare_variables(retro_environment_t cb) {
    static const struct retro_variable vars[] = {
        { "media_bg_r", "BG Red; 0|32|64|128|255" }, { "media_bg_g", "BG Green; 64|0|32|128|255" }, { "media_bg_b", "BG Blue; 0|32|64|128|255" },
        { "media_fg_r", "FG Red; 0|32|64|128|255" }, { "media_fg_g", "FG Green; 255|0|32|64|128" }, { "media_fg_b", "FG Blue; 0|32|64|128|255" },
        { "media_show_art", "Show Art; On|Off" }, { "media_show_txt", "Show Scroll Text; On|Off" },
        { "media_show_viz", "Show Visualizer; On|Off" }, { "media_show_bar", "Show Progress Bar; On|Off" },
        { "media_show_tim", "Show Time; On|Off" }, { "media_show_ico", "Show Icons; On|Off" },
        { "media_responsive", "Responsive Layout; Off|On" },
        { "media_ui_top", "UI Top %; 20|0|10|30|40|50" },
        { "media_ui_bottom", "UI Bottom %; 80|50|60|70|90|100" },
        { "media_ui_left", "UI Left %; 0|10|20|30" },
        { "media_ui_right", "UI Right %; 100|70|80|90" },
        { "media_art_y", "Art Y; 40|0|80|120" }, { "media_txt_y", "Text Y; 150|20|120|200" },
        { "media_viz_y", "Viz Y; 140|80|200" }, { "media_bar_y", "Bar Y; 180|100|210" },
        { "media_tim_y", "Time Y; 190|110|220" }, { "media_ico_y", "Icon Y; 20|50|200" },
        { "media_viz_bands", "Viz Bands; 40|20" },
        { "media_viz_mode", "Viz Mode; Bars|VU Meter|Dots|Line" },
        { "media_viz_gradient", "Viz Gradient; On|Off" },
        { "media_viz_peak_hold", "Peak Hold; 30|0|15|45|60" },
        { "media_use_filename", "Show Filename Only; Off|On" },
        { NULL, NULL }
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}
