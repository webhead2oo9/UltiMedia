#include "config.h"
#include "visualizer.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

Config cfg;

static const char *get_var_value(retro_environment_t environ_cb, const char *key) {
    struct retro_variable var = {0};
    var.key = key;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && var.value[0])
        return var.value;
    return NULL;
}

static int parse_int_strict(const char *value, int fallback, int min_value, int max_value) {
    if (!value || !value[0]) return fallback;

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value) return fallback;

    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return fallback;

    if (parsed < min_value) return min_value;
    if (parsed > max_value) return max_value;
    return (int)parsed;
}

static int get_int_var(retro_environment_t environ_cb, const char *key, int fallback, int min_value, int max_value) {
    const char *value = get_var_value(environ_cb, key);
    return parse_int_strict(value, fallback, min_value, max_value);
}

static bool parse_on_off(const char *value, bool fallback) {
    if (!value) return fallback;
    if (!strcmp(value, "On") || !strcmp(value, "on") ||
        !strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "True") ||
        !strcmp(value, "Enabled") || !strcmp(value, "enabled"))
        return true;
    if (!strcmp(value, "Off") || !strcmp(value, "off") ||
        !strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "False") ||
        !strcmp(value, "Disabled") || !strcmp(value, "disabled"))
        return false;
    return fallback;
}

static bool get_bool_var(retro_environment_t environ_cb, const char *key, bool fallback) {
    return parse_on_off(get_var_value(environ_cb, key), fallback);
}

static TrackTextMode parse_track_text_mode(const char *value) {
    if (!value) return SHOW_ID;
    if (!strcmp(value, "Show filename with extension") || !strcmp(value, "On"))
        return SHOW_FILENAME_WITH_EXT;
    if (!strcmp(value, "Show Filename without extension"))
        return SHOW_FILENAME_WITHOUT_EXT;
    return SHOW_ID;
}

void config_update(retro_environment_t environ_cb) {
    int r, g, b;

    r = get_int_var(environ_cb, "media_bg_r", 0, 0, 255);
    g = get_int_var(environ_cb, "media_bg_g", 64, 0, 255);
    b = get_int_var(environ_cb, "media_bg_b", 0, 0, 255);
    cfg.bg_rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    r = get_int_var(environ_cb, "media_fg_r", 0, 0, 255);
    g = get_int_var(environ_cb, "media_fg_g", 255, 0, 255);
    b = get_int_var(environ_cb, "media_fg_b", 0, 0, 255);
    cfg.fg_rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    // Safety fallback for malformed option state that yields invisible black-on-black UI.
    if (cfg.bg_rgb == 0 && cfg.fg_rgb == 0) {
        cfg.bg_rgb = ((0 >> 3) << 11) | ((64 >> 2) << 5) | (0 >> 3);
        cfg.fg_rgb = ((0 >> 3) << 11) | ((255 >> 2) << 5) | (0 >> 3);
    }

    cfg.show_art = get_bool_var(environ_cb, "media_show_art", true);
    cfg.show_txt = get_bool_var(environ_cb, "media_show_txt", true);
    cfg.show_viz = get_bool_var(environ_cb, "media_show_viz", true);
    cfg.show_bar = get_bool_var(environ_cb, "media_show_bar", true);
    cfg.show_tim = get_bool_var(environ_cb, "media_show_tim", true);
    cfg.show_ico = get_bool_var(environ_cb, "media_show_ico", true);
    cfg.responsive = get_bool_var(environ_cb, "media_responsive", true);
    cfg.art_y = get_int_var(environ_cb, "media_art_y", 40, -4096, 4096);
    cfg.txt_y = get_int_var(environ_cb, "media_txt_y", 150, -4096, 4096);
    cfg.viz_y = get_int_var(environ_cb, "media_viz_y", 140, -4096, 4096);
    cfg.bar_y = get_int_var(environ_cb, "media_bar_y", 180, -4096, 4096);
    cfg.tim_y = get_int_var(environ_cb, "media_tim_y", 190, -4096, 4096);
    cfg.ico_y = get_int_var(environ_cb, "media_ico_y", 20, -4096, 4096);
    cfg.ui_top = get_int_var(environ_cb, "media_ui_top", 20, 0, 100);
    cfg.ui_bottom = get_int_var(environ_cb, "media_ui_bottom", 80, 0, 100);
    cfg.ui_left = get_int_var(environ_cb, "media_ui_left", 10, 0, 100);
    cfg.ui_right = get_int_var(environ_cb, "media_ui_right", 90, 0, 100);

    cfg.viz_bands = get_int_var(environ_cb, "media_viz_bands", 40, 1, MAX_VIZ_BANDS);
    if (cfg.viz_bands < 1) cfg.viz_bands = 1;
    if (cfg.viz_bands > MAX_VIZ_BANDS) cfg.viz_bands = MAX_VIZ_BANDS;

    const char *viz_mode_value = get_var_value(environ_cb, "media_viz_mode");
    if (viz_mode_value) {
        if (!strcmp(viz_mode_value, "Bars")) cfg.viz_mode = 0;
        else if (!strcmp(viz_mode_value, "VU Meter")) cfg.viz_mode = 3;
        else if (!strcmp(viz_mode_value, "Dots")) cfg.viz_mode = 1;
        else if (!strcmp(viz_mode_value, "Line")) cfg.viz_mode = 2;
        else cfg.viz_mode = 0;
    } else {
        cfg.viz_mode = 0;
    }

    cfg.viz_gradient = get_bool_var(environ_cb, "media_viz_gradient", true);
    cfg.viz_peak_hold = get_int_var(environ_cb, "media_viz_peak_hold", 30, 0, 300);
    cfg.track_text_mode = parse_track_text_mode(get_var_value(environ_cb, "media_use_filename"));

    // Avoid a total black frame if stale options somehow disabled every drawable element.
    if (!cfg.show_art && !cfg.show_txt && !cfg.show_viz &&
        !cfg.show_bar && !cfg.show_tim && !cfg.show_ico) {
        cfg.show_viz = true;
        cfg.show_tim = true;
    }
}

void config_declare_variables(retro_environment_t cb) {
    static const struct retro_variable vars[] = {
        { "media_bg_r", "BG Red; 0|32|64|128|255" }, { "media_bg_g", "BG Green; 64|0|32|128|255" }, { "media_bg_b", "BG Blue; 0|32|64|128|255" },
        { "media_fg_r", "FG Red; 0|32|64|128|255" }, { "media_fg_g", "FG Green; 255|0|32|64|128" }, { "media_fg_b", "FG Blue; 0|32|64|128|255" },
        { "media_show_art", "Show Art; On|Off" }, { "media_show_txt", "Show Scroll Text; On|Off" },
        { "media_show_viz", "Show Visualizer; On|Off" }, { "media_show_bar", "Show Progress Bar; On|Off" },
        { "media_show_tim", "Show Time; On|Off" }, { "media_show_ico", "Show Icons; On|Off" },
        { "media_responsive", "Responsive Layout; On|Off" },
        { "media_ui_top", "UI Top %; 20|0|10|30|40|50" },
        { "media_ui_bottom", "UI Bottom %; 80|50|60|70|90|100" },
        { "media_ui_left", "UI Left %; 10|0|20|30" },
        { "media_ui_right", "UI Right %; 90|70|80|100" },
        { "media_art_y", "Art Y; 40|0|80|120" }, { "media_txt_y", "Text Y; 150|20|120|200" },
        { "media_viz_y", "Viz Y; 140|80|200" }, { "media_bar_y", "Bar Y; 180|100|210" },
        { "media_tim_y", "Time Y; 190|110|220" }, { "media_ico_y", "Icon Y; 20|50|200" },
        { "media_viz_bands", "Viz Bands; 40|20" },
        { "media_viz_mode", "Viz Mode; Bars|VU Meter|Dots|Line" },
        { "media_viz_gradient", "Viz Gradient; On|Off" },
        { "media_viz_peak_hold", "Peak Hold; 30|0|15|45|60" },
        { "media_use_filename", "Track Text Mode; Show ID|Show filename with extension|Show Filename without extension" },
        { NULL, NULL }
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}
