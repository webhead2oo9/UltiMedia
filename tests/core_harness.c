#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "core_debug.h"
#include "libretro.h"
#include "metadata.h"
#include "visualizer.h"

#define MAX_ENV_OPTIONS 32
#define MAX_JOYPAD_IDS 32
#define MAX_PATH_LEN 1024

typedef struct {
    const char *key;
    const char *value;
} EnvOption;

typedef struct {
    const char *fixtures_dir;
} TestContext;

typedef bool (*TestFn)(TestContext *ctx);

typedef struct {
    const char *name;
    TestFn fn;
} TestCase;

static EnvOption g_env_options[MAX_ENV_OPTIONS];
static int g_env_option_count = 0;
static bool g_pressed[MAX_JOYPAD_IDS] = {0};
static size_t g_audio_batch_frames = 0;
static size_t g_video_frame_count = 0;

extern void retro_init(void);
extern void retro_deinit(void);
extern void retro_run(void);
extern void retro_reset(void);
extern void retro_unload_game(void);
extern bool retro_load_game(const struct retro_game_info *info);
extern size_t retro_serialize_size(void);
extern bool retro_serialize(void *data, size_t size);
extern bool retro_unserialize(const void *data, size_t size);
extern void retro_set_environment(retro_environment_t cb);
extern void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
extern void retro_set_video_refresh(retro_video_refresh_t cb);
extern void retro_set_input_poll(retro_input_poll_t cb);
extern void retro_set_input_state(retro_input_state_t cb);

static void clear_env_options(void) {
    g_env_option_count = 0;
}

static void set_env_option(const char *key, const char *value) {
    if (!key || !value || g_env_option_count >= MAX_ENV_OPTIONS) return;
    g_env_options[g_env_option_count].key = key;
    g_env_options[g_env_option_count].value = value;
    g_env_option_count++;
}

static void reset_callback_counters(void) {
    g_audio_batch_frames = 0;
    g_video_frame_count = 0;
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            bool *updated = (bool*)data;
            if (updated) *updated = false;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable*)data;
            if (!var || !var->key) return false;
            for (int i = 0; i < g_env_option_count; i++) {
                if (strcmp(var->key, g_env_options[i].key) == 0) {
                    var->value = g_env_options[i].value;
                    return true;
                }
            }
            var->value = NULL;
            return false;
        }
        default:
            return false;
    }
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    (void)data;
    g_audio_batch_frames += frames;
    return frames;
}

static void video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    (void)data;
    (void)width;
    (void)height;
    (void)pitch;
    g_video_frame_count++;
}

static void input_poll_cb_impl(void) {}

static int16_t input_state_cb_impl(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port;
    (void)index;
    if (device != RETRO_DEVICE_JOYPAD) return 0;
    if (id >= MAX_JOYPAD_IDS) return 0;
    return g_pressed[id] ? 1 : 0;
}

static bool failf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    return false;
}

static bool require_true(bool condition, const char *fmt, ...) {
    if (condition) return true;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    return false;
}

static void run_frames(int count) {
    for (int i = 0; i < count; i++) retro_run();
}

static void run_until_track_changes(int initial_index, int max_frames) {
    for (int i = 0; i < max_frames && core_debug_get_current_index() == initial_index; i++)
        retro_run();
}

static void clear_inputs(void) {
    memset(g_pressed, 0, sizeof(g_pressed));
}

static void tap_button(unsigned id) {
    if (id >= MAX_JOYPAD_IDS) return;
    g_pressed[id] = true;
    retro_run();
    g_pressed[id] = false;
    run_frames(21);
}

static void build_path(char *out, size_t out_size, const char *dir, const char *name) {
    snprintf(out, out_size, "%s/%s", dir, name);
}

static bool load_game_path(const char *path) {
    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = path;
    return retro_load_game(&info);
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = slash;
    if (!base || (backslash && backslash > base)) base = backslash;
    return base ? base + 1 : path;
}

static bool current_track_is(const char *expected_name) {
    const char *path = core_debug_get_current_track_path();
    if (!path) return false;
    return strcmp(path_basename(path), expected_name) == 0;
}

static void prepare_test(void) {
    clear_env_options();
    clear_inputs();
    reset_callback_counters();
    retro_unload_game();
}

static bool test_basic_load_play(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "track_a.wav");

    if (!load_game_path(path))
        return failf("basic_load_play: failed to load %s", path);

    if (!require_true(core_debug_get_current_index() == 0, "basic_load_play: expected track index 0"))
        return false;
    if (!require_true(cur_frame == 0, "basic_load_play: expected initial cur_frame 0, got %llu", (unsigned long long)cur_frame))
        return false;

    run_frames(5);

    if (!require_true(cur_frame > 0, "basic_load_play: playback did not advance"))
        return false;
    if (!require_true(g_audio_batch_frames > 0, "basic_load_play: no audio frames were emitted"))
        return false;
    if (!require_true(g_video_frame_count >= 5, "basic_load_play: expected video callback to run each frame"))
        return false;
    return true;
}

static bool test_playlist_navigation(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    set_env_option("media_use_filename", "Show filename with extension");
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("playlist_navigation: failed to load %s", path);

    if (!require_true(core_debug_get_track_count() == 4, "playlist_navigation: expected 4 tracks"))
        return false;
    if (!require_true(current_track_is("track_a.wav"), "playlist_navigation: expected track_a.wav first"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    if (!require_true(core_debug_get_current_index() == 1, "playlist_navigation: expected current index 1 after next"))
        return false;
    if (!require_true(current_track_is("track_b.wav"), "playlist_navigation: expected track_b.wav after next"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    if (!require_true(core_debug_get_current_index() == 2, "playlist_navigation: expected current index 2 after second next"))
        return false;
    if (!require_true(current_track_is("track_c.wav"), "playlist_navigation: expected track_c.wav after second next"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_L);
    if (!require_true(core_debug_get_current_index() == 1, "playlist_navigation: expected current index 1 after previous"))
        return false;
    if (!require_true(current_track_is("track_b.wav"), "playlist_navigation: expected track_b.wav after previous"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_L);
    if (!require_true(core_debug_get_current_index() == 0, "playlist_navigation: expected current index 0 after previous"))
        return false;
    if (!require_true(current_track_is("track_a.wav"), "playlist_navigation: expected track_a.wav after previous"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_L);
    if (!require_true(core_debug_get_current_index() == 3, "playlist_navigation: expected wrap to current index 3"))
        return false;
    if (!require_true(current_track_is("track_d.wav"), "playlist_navigation: expected track_d.wav after wrap"))
        return false;
    return true;
}

static bool test_save_state_restore_track_and_position(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    size_t state_size = 0;
    void *state_data = NULL;
    int saved_index = 0;
    uint64_t saved_frame = 0;
    bool ok = false;

    prepare_test();
    set_env_option("media_use_filename", "Show filename with extension");
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("save_state_restore: failed to load %s", path);

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    run_frames(4);

    saved_index = core_debug_get_current_index();
    saved_frame = cur_frame;
    state_size = retro_serialize_size();
    if (!require_true(saved_index == 1, "save_state_restore: expected to save on track index 1"))
        return false;
    if (!require_true(state_size > 0, "save_state_restore: serialize size was zero"))
        return false;

    state_data = malloc(state_size);
    if (!state_data) return failf("save_state_restore: failed to allocate %zu bytes", state_size);
    if (!require_true(retro_serialize(state_data, state_size), "save_state_restore: serialize failed"))
        goto done;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    run_frames(6);

    if (!require_true(core_debug_get_current_index() == 2, "save_state_restore: expected current index 2 after mutation"))
        goto done;
    if (!require_true(cur_frame != saved_frame, "save_state_restore: expected playback position to change before restore"))
        goto done;

    if (!require_true(retro_unserialize(state_data, state_size), "save_state_restore: unserialize failed"))
        goto done;
    if (!require_true(core_debug_get_current_index() == saved_index, "save_state_restore: current index was not restored"))
        goto done;
    if (!require_true(cur_frame == saved_frame, "save_state_restore: cur_frame was not restored"))
        goto done;
    if (!require_true(current_track_is("track_b.wav"), "save_state_restore: expected current track to restore to track_b.wav"))
        goto done;

    run_frames(1);
    if (!require_true(cur_frame > saved_frame, "save_state_restore: playback did not resume after restore"))
        goto done;

    ok = true;
done:
    free(state_data);
    return ok;
}

static bool test_pause_state_restore(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    size_t state_size = 0;
    void *state_data = NULL;
    uint64_t paused_frame = 0;
    bool ok = false;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "track_a.wav");

    if (!load_game_path(path))
        return failf("pause_state_restore: failed to load %s", path);

    run_frames(5);
    tap_button(RETRO_DEVICE_ID_JOYPAD_B);
    if (!require_true(core_debug_is_paused(), "pause_state_restore: expected core to be paused"))
        return false;

    paused_frame = cur_frame;
    run_frames(4);
    if (!require_true(cur_frame == paused_frame, "pause_state_restore: playback advanced while paused"))
        return false;

    state_size = retro_serialize_size();
    if (!require_true(state_size > 0, "pause_state_restore: serialize size was zero"))
        return false;
    state_data = malloc(state_size);
    if (!state_data) return failf("pause_state_restore: failed to allocate %zu bytes", state_size);
    if (!require_true(retro_serialize(state_data, state_size), "pause_state_restore: serialize failed"))
        goto done;

    tap_button(RETRO_DEVICE_ID_JOYPAD_B);
    if (!require_true(!core_debug_is_paused(), "pause_state_restore: expected core to unpause"))
        goto done;
    run_frames(4);
    if (!require_true(cur_frame > paused_frame, "pause_state_restore: playback did not advance after unpausing"))
        goto done;

    if (!require_true(retro_unserialize(state_data, state_size), "pause_state_restore: unserialize failed"))
        goto done;
    if (!require_true(core_debug_is_paused(), "pause_state_restore: pause state was not restored"))
        goto done;
    if (!require_true(cur_frame == paused_frame, "pause_state_restore: paused frame was not restored"))
        goto done;

    run_frames(3);
    if (!require_true(cur_frame == paused_frame, "pause_state_restore: playback advanced after restoring paused state"))
        goto done;

    tap_button(RETRO_DEVICE_ID_JOYPAD_B);
    run_frames(2);
    if (!require_true(cur_frame > paused_frame, "pause_state_restore: playback did not advance after unpausing restored state"))
        goto done;

    ok = true;
done:
    free(state_data);
    return ok;
}

static bool test_reset_preserves_shuffle_and_restarts_current_track(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    int reset_index = 0;
    uint64_t before_reset = 0;

    prepare_test();
    set_env_option("media_use_filename", "Show filename with extension");
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("reset_shuffle: failed to load %s", path);

    tap_button(RETRO_DEVICE_ID_JOYPAD_Y);
    if (!require_true(core_debug_is_shuffle_enabled(), "reset_shuffle: expected shuffle to be enabled"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    run_frames(3);

    reset_index = core_debug_get_current_index();
    before_reset = cur_frame;
    if (!require_true(before_reset > 0, "reset_shuffle: expected playback to advance before reset"))
        return false;

    retro_reset();

    if (!require_true(core_debug_is_shuffle_enabled(), "reset_shuffle: shuffle state was not preserved"))
        return false;
    if (!require_true(core_debug_get_current_index() == reset_index, "reset_shuffle: current track index changed across reset"))
        return false;
    if (!require_true(cur_frame == 0, "reset_shuffle: expected cur_frame 0 immediately after reset, got %llu", (unsigned long long)cur_frame))
        return false;

    run_frames(1);
    if (!require_true(cur_frame > 0, "reset_shuffle: playback did not restart after reset"))
        return false;
    return true;
}

static bool test_shuffle_cycle_no_repeats(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    bool seen[8] = {0};
    int track_count = 0;
    int last_index = 0;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("shuffle_cycle: failed to load %s", path);

    tap_button(RETRO_DEVICE_ID_JOYPAD_Y);
    if (!require_true(core_debug_is_shuffle_enabled(), "shuffle_cycle: expected shuffle to be enabled"))
        return false;

    track_count = core_debug_get_track_count();
    if (!require_true(track_count == 4, "shuffle_cycle: expected 4 tracks"))
        return false;

    seen[core_debug_get_current_index()] = true;
    for (int i = 1; i < track_count; i++) {
        tap_button(RETRO_DEVICE_ID_JOYPAD_R);
        last_index = core_debug_get_current_index();
        if (!require_true(last_index >= 0 && last_index < track_count, "shuffle_cycle: invalid track index %d", last_index))
            return false;
        if (!require_true(!seen[last_index], "shuffle_cycle: repeated track index %d before cycle completion", last_index))
            return false;
        seen[last_index] = true;
    }

    for (int i = 0; i < track_count; i++) {
        if (!require_true(seen[i], "shuffle_cycle: track index %d was never visited", i))
            return false;
    }

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    if (!require_true(core_debug_get_current_index() != last_index, "shuffle_cycle: reshuffle repeated the previous track"))
        return false;
    return true;
}

static bool test_shuffle_previous_returns_to_history(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    int start_index = 0;
    int first_shuffle_index = 0;
    int second_shuffle_index = 0;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("shuffle_previous_history: failed to load %s", path);

    start_index = core_debug_get_current_index();
    tap_button(RETRO_DEVICE_ID_JOYPAD_Y);
    if (!require_true(core_debug_is_shuffle_enabled(), "shuffle_previous_history: expected shuffle to be enabled"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    first_shuffle_index = core_debug_get_current_index();
    if (!require_true(first_shuffle_index != start_index, "shuffle_previous_history: expected first shuffle step to change track"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    second_shuffle_index = core_debug_get_current_index();
    if (!require_true(second_shuffle_index != first_shuffle_index, "shuffle_previous_history: expected second shuffle step to change track"))
        return false;
    if (!require_true(second_shuffle_index != start_index, "shuffle_previous_history: expected second shuffle step to avoid the start track"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_L);
    if (!require_true(core_debug_get_current_index() == first_shuffle_index, "shuffle_previous_history: expected previous to return to first shuffled track"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_L);
    if (!require_true(core_debug_get_current_index() == start_index, "shuffle_previous_history: expected second previous to return to start track"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    if (!require_true(core_debug_get_current_index() == first_shuffle_index, "shuffle_previous_history: expected next after rewind to replay recorded shuffle history"))
        return false;

    return true;
}

static bool test_shuffle_previous_after_eof_returns_finished_track(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    int start_index = 0;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("shuffle_previous_eof: failed to load %s", path);

    start_index = core_debug_get_current_index();
    tap_button(RETRO_DEVICE_ID_JOYPAD_Y);
    if (!require_true(core_debug_is_shuffle_enabled(), "shuffle_previous_eof: expected shuffle to be enabled"))
        return false;

    run_until_track_changes(start_index, (int)(total_frames / SAMPLES_PER_FRAME) + 10);
    if (!require_true(core_debug_get_current_index() != start_index, "shuffle_previous_eof: expected EOF to advance to a different shuffled track"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_L);
    if (!require_true(core_debug_get_current_index() == start_index, "shuffle_previous_eof: expected previous after EOF advance to return to the finished track"))
        return false;

    return true;
}

static bool test_shuffle_save_state_restores_previous_history(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    size_t state_size = 0;
    void *state_data = NULL;
    int start_index = 0;
    int shuffled_index = 0;
    bool ok = false;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("shuffle_history_restore: failed to load %s", path);

    start_index = core_debug_get_current_index();
    tap_button(RETRO_DEVICE_ID_JOYPAD_Y);
    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    shuffled_index = core_debug_get_current_index();
    if (!require_true(shuffled_index != start_index, "shuffle_history_restore: expected shuffle next to change tracks"))
        return false;

    state_size = retro_serialize_size();
    if (!require_true(state_size > 0, "shuffle_history_restore: serialize size was zero"))
        return false;

    state_data = malloc(state_size);
    if (!state_data) return failf("shuffle_history_restore: failed to allocate %zu bytes", state_size);
    if (!require_true(retro_serialize(state_data, state_size), "shuffle_history_restore: serialize failed"))
        goto done;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    if (!require_true(core_debug_get_current_index() != shuffled_index, "shuffle_history_restore: expected mutation after save"))
        goto done;

    if (!require_true(retro_unserialize(state_data, state_size), "shuffle_history_restore: unserialize failed"))
        goto done;
    if (!require_true(core_debug_get_current_index() == shuffled_index, "shuffle_history_restore: expected shuffled track to restore"))
        goto done;

    tap_button(RETRO_DEVICE_ID_JOYPAD_L);
    if (!require_true(core_debug_get_current_index() == start_index, "shuffle_history_restore: expected previous after restore to return to the saved prior track"))
        goto done;

    ok = true;
done:
    free(state_data);
    return ok;
}

static bool test_negative_restore_keeps_playback_usable(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    char alt_path[MAX_PATH_LEN];
    size_t state_size = 0;
    void *state_data = NULL;
    uint64_t before_frames = 0;
    bool ok = false;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");
    build_path(alt_path, sizeof(alt_path), ctx->fixtures_dir, "playlist_alt.m3u");

    if (!load_game_path(path))
        return failf("negative_restore: failed to load %s", path);

    run_frames(4);
    state_size = retro_serialize_size();
    if (!require_true(state_size > 0, "negative_restore: serialize size was zero"))
        return false;
    state_data = malloc(state_size);
    if (!state_data) return failf("negative_restore: failed to allocate %zu bytes", state_size);
    if (!require_true(retro_serialize(state_data, state_size), "negative_restore: serialize failed"))
        goto done;

    retro_unload_game();

    if (!load_game_path(alt_path))
        goto done;
    if (!require_true(current_track_is("alt_1.wav"), "negative_restore: expected alt_1.wav to be loaded"))
        goto done;

    if (!require_true(!retro_unserialize(state_data, state_size), "negative_restore: expected restore against different content to fail"))
        goto done;

    if (!require_true(current_track_is("alt_1.wav"), "negative_restore: current track changed after failed restore"))
        goto done;

    before_frames = cur_frame;
    run_frames(3);
    if (!require_true(cur_frame > before_frames, "negative_restore: playback did not remain usable after failed restore"))
        goto done;

    ok = true;
done:
    free(state_data);
    return ok;
}

static bool test_config_layout_smoke(TestContext *ctx) {
    char path[MAX_PATH_LEN];

    prepare_test();
    set_env_option("media_responsive", "Off");
    set_env_option("media_show_art", "Off");
    set_env_option("media_viz_mode", "Line");
    set_env_option("media_viz_bands", "20");
    set_env_option("media_viz_gradient", "Off");
    set_env_option("media_use_filename", "Show filename with extension");
    build_path(path, sizeof(path), ctx->fixtures_dir, "track_b.wav");

    if (!load_game_path(path))
        return failf("config_layout_smoke: failed to load %s", path);

    run_frames(6);

    if (!require_true(cfg.responsive == false, "config_layout_smoke: expected responsive layout to be off"))
        return false;
    if (!require_true(cfg.viz_mode == VIZ_MODE_LINE, "config_layout_smoke: expected line visualizer mode"))
        return false;
    if (!require_true(cfg.viz_bands == 20, "config_layout_smoke: expected 20 visualizer bands"))
        return false;
    if (!require_true(g_audio_batch_frames > 0, "config_layout_smoke: no audio frames were emitted"))
        return false;
    if (!require_true(g_video_frame_count >= 6, "config_layout_smoke: expected video callback to run"))
        return false;
    return true;
}

static bool test_vu_mode_updates_once_per_frame(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    int16_t loud_buf[16 * 2] = {0};
    int16_t silent_buf[16 * 2] = {0};
    float left_before_draw = 0.0f;
    float right_before_draw = 0.0f;
    float left_peak_before_hold_expiry = 0.0f;
    float right_peak_before_hold_expiry = 0.0f;
    int left_timer_before_draw = 0;
    int right_timer_before_draw = 0;

    prepare_test();
    set_env_option("media_responsive", "Off");
    set_env_option("media_show_art", "Off");
    set_env_option("media_viz_mode", "VU Meter");
    set_env_option("media_viz_peak_hold", "3");
    build_path(path, sizeof(path), ctx->fixtures_dir, "track_a.wav");

    if (!load_game_path(path))
        return failf("vu_mode_updates_once_per_frame: failed to load %s", path);
    if (!require_true(cfg.viz_mode == VIZ_MODE_VU, "vu_mode_updates_once_per_frame: expected VU mode"))
        return false;

    viz_reset_state();
    for (int i = 0; i < 16; i++) {
        loud_buf[i * 2] = 16000;
        loud_buf[i * 2 + 1] = 12000;
    }

    viz_update_levels(loud_buf, 16);
    if (!require_true(viz_levels[0] > 0.0f, "vu_mode_updates_once_per_frame: expected left VU level to rise"))
        return false;
    if (!require_true(viz_levels[1] > 0.0f, "vu_mode_updates_once_per_frame: expected right VU level to rise"))
        return false;
    if (!require_true(viz_peak_timers[0] == 3, "vu_mode_updates_once_per_frame: expected left peak timer 3, got %d", viz_peak_timers[0]))
        return false;
    if (!require_true(viz_peak_timers[1] == 3, "vu_mode_updates_once_per_frame: expected right peak timer 3, got %d", viz_peak_timers[1]))
        return false;

    left_before_draw = viz_levels[0];
    right_before_draw = viz_levels[1];
    left_timer_before_draw = viz_peak_timers[0];
    right_timer_before_draw = viz_peak_timers[1];

    viz_draw();
    if (!require_true(fabsf(viz_levels[0] - left_before_draw) < 0.0001f, "vu_mode_updates_once_per_frame: draw changed left level"))
        return false;
    if (!require_true(fabsf(viz_levels[1] - right_before_draw) < 0.0001f, "vu_mode_updates_once_per_frame: draw changed right level"))
        return false;
    if (!require_true(viz_peak_timers[0] == left_timer_before_draw, "vu_mode_updates_once_per_frame: draw changed left peak timer"))
        return false;
    if (!require_true(viz_peak_timers[1] == right_timer_before_draw, "vu_mode_updates_once_per_frame: draw changed right peak timer"))
        return false;

    viz_update_levels(silent_buf, 16);
    if (!require_true(viz_peak_timers[0] == 2, "vu_mode_updates_once_per_frame: expected left peak timer to decay once to 2, got %d", viz_peak_timers[0]))
        return false;
    if (!require_true(viz_peak_timers[1] == 2, "vu_mode_updates_once_per_frame: expected right peak timer to decay once to 2, got %d", viz_peak_timers[1]))
        return false;
    if (!require_true(viz_levels[0] < left_before_draw, "vu_mode_updates_once_per_frame: expected left level to decay on silence"))
        return false;
    if (!require_true(viz_levels[1] < right_before_draw, "vu_mode_updates_once_per_frame: expected right level to decay on silence"))
        return false;

    left_peak_before_hold_expiry = viz_peaks[0];
    right_peak_before_hold_expiry = viz_peaks[1];

    viz_update_levels(silent_buf, 16);
    viz_update_levels(silent_buf, 16);
    if (!require_true(viz_peak_timers[0] == 0, "vu_mode_updates_once_per_frame: expected left peak timer to reach 0, got %d", viz_peak_timers[0]))
        return false;
    if (!require_true(viz_peak_timers[1] == 0, "vu_mode_updates_once_per_frame: expected right peak timer to reach 0, got %d", viz_peak_timers[1]))
        return false;
    if (!require_true(fabsf(viz_peaks[0] - left_peak_before_hold_expiry) < 0.0001f, "vu_mode_updates_once_per_frame: left peak decayed before hold expired"))
        return false;
    if (!require_true(fabsf(viz_peaks[1] - right_peak_before_hold_expiry) < 0.0001f, "vu_mode_updates_once_per_frame: right peak decayed before hold expired"))
        return false;

    viz_update_levels(silent_buf, 16);
    if (!require_true(viz_peaks[0] < left_peak_before_hold_expiry, "vu_mode_updates_once_per_frame: expected left peak to decay after hold expiry"))
        return false;
    if (!require_true(viz_peaks[1] < right_peak_before_hold_expiry, "vu_mode_updates_once_per_frame: expected right peak to decay after hold expiry"))
        return false;
    return true;
}

static bool test_track_change_resets_fft_visualizer_state(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    int16_t loud_buf[1024 * 2] = {0};
    bool saw_level = false;

    prepare_test();
    set_env_option("media_responsive", "Off");
    set_env_option("media_show_art", "Off");
    set_env_option("media_viz_mode", "Bars");
    set_env_option("media_viz_bands", "20");
    set_env_option("media_viz_peak_hold", "30");
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_main.m3u");

    if (!load_game_path(path))
        return failf("track_change_resets_fft_visualizer_state: failed to load %s", path);
    if (!require_true(cfg.viz_mode == VIZ_MODE_BARS, "track_change_resets_fft_visualizer_state: expected bars mode"))
        return false;

    viz_reset_state();
    for (int i = 0; i < 1024; i++) {
        loud_buf[i * 2] = 16000;
        loud_buf[i * 2 + 1] = 12000;
    }

    viz_update_levels(loud_buf, 1024);
    for (int i = 0; i < cfg.viz_bands; i++) {
        if (viz_levels[i] > 0.0f || viz_peaks[i] > 0.0f || viz_peak_timers[i] > 0) {
            saw_level = true;
            break;
        }
    }
    if (!require_true(saw_level, "track_change_resets_fft_visualizer_state: expected seeded FFT state before track change"))
        return false;

    g_pressed[RETRO_DEVICE_ID_JOYPAD_R] = true;
    retro_run();
    g_pressed[RETRO_DEVICE_ID_JOYPAD_R] = false;
    if (!require_true(core_debug_get_current_index() == 1, "track_change_resets_fft_visualizer_state: expected next track to load"))
        return false;

    for (int i = 0; i < cfg.viz_bands; i++) {
        if (!require_true(viz_levels[i] == 0.0f, "track_change_resets_fft_visualizer_state: expected level %d to reset, got %f", i, viz_levels[i]))
            return false;
        if (!require_true(viz_peaks[i] == 0.0f, "track_change_resets_fft_visualizer_state: expected peak %d to reset, got %f", i, viz_peaks[i]))
            return false;
        if (!require_true(viz_peak_timers[i] == 0, "track_change_resets_fft_visualizer_state: expected timer %d to reset, got %d", i, viz_peak_timers[i]))
            return false;
    }

    return true;
}

static const TestCase kTests[] = {
    { "basic_load_play", test_basic_load_play },
    { "playlist_navigation", test_playlist_navigation },
    { "save_state_restore_track_and_position", test_save_state_restore_track_and_position },
    { "pause_state_restore", test_pause_state_restore },
    { "reset_preserves_shuffle_and_restarts_current_track", test_reset_preserves_shuffle_and_restarts_current_track },
    { "shuffle_cycle_no_repeats", test_shuffle_cycle_no_repeats },
    { "shuffle_previous_returns_to_history", test_shuffle_previous_returns_to_history },
    { "shuffle_previous_after_eof_returns_finished_track", test_shuffle_previous_after_eof_returns_finished_track },
    { "shuffle_save_state_restores_previous_history", test_shuffle_save_state_restores_previous_history },
    { "negative_restore_keeps_playback_usable", test_negative_restore_keeps_playback_usable },
    { "config_layout_smoke", test_config_layout_smoke },
    { "vu_mode_updates_once_per_frame", test_vu_mode_updates_once_per_frame },
    { "track_change_resets_fft_visualizer_state", test_track_change_resets_fft_visualizer_state },
};

int main(int argc, char **argv) {
    TestContext ctx;
    size_t passed = 0;
    size_t total = sizeof(kTests) / sizeof(kTests[0]);

    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
        return 2;
    }

    ctx.fixtures_dir = argv[1];
    retro_set_environment(env_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_video_refresh(video_refresh_cb);
    retro_set_input_poll(input_poll_cb_impl);
    retro_set_input_state(input_state_cb_impl);
    retro_init();

    for (size_t i = 0; i < total; i++) {
        bool ok = kTests[i].fn(&ctx);
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", kTests[i].name);
        if (!ok) {
            retro_unload_game();
            retro_deinit();
            return 1;
        }
        passed++;
    }

    retro_unload_game();
    retro_deinit();
    printf("passed %zu/%zu tests\n", passed, total);
    return 0;
}
