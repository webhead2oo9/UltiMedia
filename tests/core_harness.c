#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "core_debug.h"
#include "core_state.h"
#include "libretro.h"
#include "metadata.h"
#include "visualizer.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define MAX_ENV_OPTIONS 32
#define MAX_JOYPAD_IDS 32
#define MAX_PATH_LEN 1024
#define EXPECTED_LIBRARY_NAME "UltiMedia UGC"
#define STUB_GL_PIXEL_UNPACK_BUFFER 0x88EC

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
static int16_t g_last_audio_batch[SAMPLES_PER_FRAME * 2];
static size_t g_last_audio_batch_frames = 0;
static size_t g_video_frame_count = 0;
// Fake-GL frontend state for the hw_render lifecycle test.
static bool g_accept_hw_render = false;
static bool g_hw_render_requested = false;
static unsigned g_hw_render_context_type = 0;
static struct retro_hw_render_callback g_hw_render_cb;
static bool g_gl_stub_break_loader = false;
static bool g_gl_stub_break_glow = false;      // glow entry points unresolvable
static bool g_gl_stub_fbo_incomplete = false;  // glow FBOs never complete
static unsigned int g_gl_stub_next_id = 1;     // unique names from the Gen stubs
static int g_gl_stub_draw_calls = 0;
static unsigned int g_gl_stub_pixel_unpack_buffer = 0;
static int g_gl_stub_tex_image_calls = 0;
static int g_gl_stub_tex_sub_image_calls = 0;
static int g_gl_stub_tex_image_with_pbo = 0;
static int g_gl_stub_tex_sub_image_with_pbo = 0;
static char g_video_frame_unset_sentinel;
static const void *g_last_video_frame = NULL;

// Stub GL implementation handed to the core through get_proc_address.
// Signatures match src/render_gl.c's RGL_PROC_LIST exactly so the calls stay
// well-defined; behavior is the minimum for resource setup to "succeed".
#if defined(_WIN32) && !defined(_WIN64)
#define STUB_GLCALL __stdcall
#else
#define STUB_GLCALL
#endif

static unsigned int STUB_GLCALL stub_glGetError(void) { return 0; }
static void STUB_GLCALL stub_glEnable(unsigned int cap) { (void)cap; }
static void STUB_GLCALL stub_glDisable(unsigned int cap) { (void)cap; }
static void STUB_GLCALL stub_glViewport(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static void STUB_GLCALL stub_glClearColor(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
static void STUB_GLCALL stub_glClear(unsigned int mask) { (void)mask; }
static void STUB_GLCALL stub_glPixelStorei(unsigned int pname, int param) { (void)pname; (void)param; }
static void STUB_GLCALL stub_glGenTextures(int n, unsigned int *textures) { for (int i = 0; i < n; i++) textures[i] = g_gl_stub_next_id++; }
static void STUB_GLCALL stub_glDeleteTextures(int n, const unsigned int *textures) { (void)n; (void)textures; }
static void STUB_GLCALL stub_glBindTexture(unsigned int target, unsigned int texture) { (void)target; (void)texture; }
static void STUB_GLCALL stub_glTexParameteri(unsigned int target, unsigned int pname, int param) { (void)target; (void)pname; (void)param; }
static void STUB_GLCALL stub_glTexImage2D(unsigned int target, int level, int internalformat, int w, int h, int border, unsigned int format, unsigned int type, const void *pixels) {
    (void)target; (void)level; (void)internalformat; (void)w; (void)h;
    (void)border; (void)format; (void)type; (void)pixels;
    g_gl_stub_tex_image_calls++;
    if (g_gl_stub_pixel_unpack_buffer) g_gl_stub_tex_image_with_pbo++;
}
static void STUB_GLCALL stub_glTexSubImage2D(unsigned int target, int level, int x, int y, int w, int h, unsigned int format, unsigned int type, const void *pixels) {
    (void)target; (void)level; (void)x; (void)y; (void)w; (void)h;
    (void)format; (void)type; (void)pixels;
    g_gl_stub_tex_sub_image_calls++;
    if (g_gl_stub_pixel_unpack_buffer) g_gl_stub_tex_sub_image_with_pbo++;
}
static void STUB_GLCALL stub_glActiveTexture(unsigned int texture) { (void)texture; }
static unsigned int STUB_GLCALL stub_glCreateShader(unsigned int type) { (void)type; return 1; }
static void STUB_GLCALL stub_glShaderSource(unsigned int shader, int count, const char **string, const int *length) { (void)shader; (void)count; (void)string; (void)length; }
static void STUB_GLCALL stub_glCompileShader(unsigned int shader) { (void)shader; }
static void STUB_GLCALL stub_glGetShaderiv(unsigned int shader, unsigned int pname, int *params) { (void)shader; (void)pname; if (params) *params = 1; }
static void STUB_GLCALL stub_glGetShaderInfoLog(unsigned int shader, int buf_size, int *length, char *info_log) { (void)shader; (void)buf_size; if (length) *length = 0; if (info_log) info_log[0] = '\0'; }
static void STUB_GLCALL stub_glDeleteShader(unsigned int shader) { (void)shader; }
static unsigned int STUB_GLCALL stub_glCreateProgram(void) { return 5; }
static void STUB_GLCALL stub_glAttachShader(unsigned int program, unsigned int shader) { (void)program; (void)shader; }
static void STUB_GLCALL stub_glLinkProgram(unsigned int program) { (void)program; }
static void STUB_GLCALL stub_glGetProgramiv(unsigned int program, unsigned int pname, int *params) { (void)program; (void)pname; if (params) *params = 1; }
static void STUB_GLCALL stub_glGetProgramInfoLog(unsigned int program, int buf_size, int *length, char *info_log) { (void)program; (void)buf_size; if (length) *length = 0; if (info_log) info_log[0] = '\0'; }
static void STUB_GLCALL stub_glUseProgram(unsigned int program) { (void)program; }
static void STUB_GLCALL stub_glDeleteProgram(unsigned int program) { (void)program; }
static int STUB_GLCALL stub_glGetAttribLocation(unsigned int program, const char *name) {
    (void)program;
    if (name && strcmp(name, "a_pos") == 0) return 0;
    if (name && strcmp(name, "a_uv") == 0) return 1;
    return -1;
}
static int STUB_GLCALL stub_glGetUniformLocation(unsigned int program, const char *name) { (void)program; (void)name; return 0; }
static void STUB_GLCALL stub_glUniform1i(int location, int v0) { (void)location; (void)v0; }
static void STUB_GLCALL stub_glGenBuffers(int n, unsigned int *buffers) { for (int i = 0; i < n; i++) buffers[i] = g_gl_stub_next_id++; }
static void STUB_GLCALL stub_glDeleteBuffers(int n, const unsigned int *buffers) { (void)n; (void)buffers; }
static void STUB_GLCALL stub_glBindBuffer(unsigned int target, unsigned int buffer) {
    if (target == STUB_GL_PIXEL_UNPACK_BUFFER)
        g_gl_stub_pixel_unpack_buffer = buffer;
}
static void STUB_GLCALL stub_glBufferData(unsigned int target, ptrdiff_t size, const void *data, unsigned int usage) { (void)target; (void)size; (void)data; (void)usage; }
static void STUB_GLCALL stub_glEnableVertexAttribArray(unsigned int index) { (void)index; }
static void STUB_GLCALL stub_glDisableVertexAttribArray(unsigned int index) { (void)index; }
static void STUB_GLCALL stub_glVertexAttribPointer(unsigned int index, int size, unsigned int type, unsigned char normalized, int stride, const void *pointer) { (void)index; (void)size; (void)type; (void)normalized; (void)stride; (void)pointer; }
static void STUB_GLCALL stub_glDrawArrays(unsigned int mode, int first, int count) { (void)mode; (void)first; (void)count; g_gl_stub_draw_calls++; }
static void STUB_GLCALL stub_glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { (void)r; (void)g; (void)b; (void)a; }
static void STUB_GLCALL stub_glBindFramebuffer(unsigned int target, unsigned int framebuffer) { (void)target; (void)framebuffer; }
static void STUB_GLCALL stub_glGenFramebuffers(int n, unsigned int *framebuffers) { for (int i = 0; i < n; i++) framebuffers[i] = g_gl_stub_next_id++; }
static void STUB_GLCALL stub_glDeleteFramebuffers(int n, const unsigned int *framebuffers) { (void)n; (void)framebuffers; }
static void STUB_GLCALL stub_glFramebufferTexture2D(unsigned int target, unsigned int attachment, unsigned int textarget, unsigned int texture, int level) { (void)target; (void)attachment; (void)textarget; (void)texture; (void)level; }
static unsigned int STUB_GLCALL stub_glCheckFramebufferStatus(unsigned int target) { (void)target; return g_gl_stub_fbo_incomplete ? 0 : 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */ }
static void STUB_GLCALL stub_glBlendFunc(unsigned int sfactor, unsigned int dfactor) { (void)sfactor; (void)dfactor; }
static void STUB_GLCALL stub_glBlendEquation(unsigned int mode) { (void)mode; }
static void STUB_GLCALL stub_glUniform1f(int location, float v0) { (void)location; (void)v0; }
static void STUB_GLCALL stub_glUniform2f(int location, float v0, float v1) { (void)location; (void)v0; (void)v1; }

typedef struct {
    const char *name;
    retro_proc_address_t fn;
} GlStubEntry;

#define STUB_ENTRY(name) { #name, (retro_proc_address_t)stub_##name }
static const GlStubEntry kGlStubs[] = {
    STUB_ENTRY(glGetError), STUB_ENTRY(glEnable), STUB_ENTRY(glDisable),
    STUB_ENTRY(glViewport), STUB_ENTRY(glClearColor), STUB_ENTRY(glClear),
    STUB_ENTRY(glPixelStorei), STUB_ENTRY(glGenTextures), STUB_ENTRY(glDeleteTextures),
    STUB_ENTRY(glBindTexture), STUB_ENTRY(glTexParameteri), STUB_ENTRY(glTexImage2D),
    STUB_ENTRY(glTexSubImage2D), STUB_ENTRY(glActiveTexture), STUB_ENTRY(glCreateShader),
    STUB_ENTRY(glShaderSource), STUB_ENTRY(glCompileShader), STUB_ENTRY(glGetShaderiv),
    STUB_ENTRY(glGetShaderInfoLog), STUB_ENTRY(glDeleteShader), STUB_ENTRY(glCreateProgram),
    STUB_ENTRY(glAttachShader), STUB_ENTRY(glLinkProgram), STUB_ENTRY(glGetProgramiv),
    STUB_ENTRY(glGetProgramInfoLog), STUB_ENTRY(glUseProgram), STUB_ENTRY(glDeleteProgram),
    STUB_ENTRY(glGetAttribLocation), STUB_ENTRY(glGetUniformLocation), STUB_ENTRY(glUniform1i),
    STUB_ENTRY(glGenBuffers), STUB_ENTRY(glDeleteBuffers), STUB_ENTRY(glBindBuffer),
    STUB_ENTRY(glBufferData), STUB_ENTRY(glEnableVertexAttribArray), STUB_ENTRY(glDisableVertexAttribArray),
    STUB_ENTRY(glVertexAttribPointer), STUB_ENTRY(glDrawArrays), STUB_ENTRY(glColorMask),
    STUB_ENTRY(glBindFramebuffer),
    STUB_ENTRY(glGenFramebuffers), STUB_ENTRY(glDeleteFramebuffers),
    STUB_ENTRY(glFramebufferTexture2D), STUB_ENTRY(glCheckFramebufferStatus),
    STUB_ENTRY(glBlendFunc), STUB_ENTRY(glBlendEquation),
    STUB_ENTRY(glUniform1f), STUB_ENTRY(glUniform2f),
};
#undef STUB_ENTRY

static uintptr_t RETRO_CALLCONV stub_get_current_framebuffer(void) { return 0; }

static retro_proc_address_t RETRO_CALLCONV stub_get_proc_address(const char *sym) {
    if (!sym) return NULL;
    // Simulates a driver missing one symbol so setup must fail cleanly.
    if (g_gl_stub_break_loader && strcmp(sym, "glTexSubImage2D") == 0) return NULL;
    // Simulates a driver without FBO entry points (either name) so only the
    // optional glow stage fails.
    if (g_gl_stub_break_glow &&
        (strcmp(sym, "glGenFramebuffers") == 0 || strcmp(sym, "glGenFramebuffersEXT") == 0))
        return NULL;
    for (size_t i = 0; i < sizeof(kGlStubs) / sizeof(kGlStubs[0]); i++) {
        if (strcmp(sym, kGlStubs[i].name) == 0) return kGlStubs[i].fn;
    }
    return NULL;
}

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
extern void retro_get_system_info(struct retro_system_info *info);

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
    g_last_audio_batch_frames = 0;
    memset(g_last_audio_batch, 0, sizeof(g_last_audio_batch));
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
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            struct retro_hw_render_callback *hw = (struct retro_hw_render_callback*)data;
            if (!hw) return false;
            g_hw_render_requested = true;
            g_hw_render_context_type = (unsigned)hw->context_type;
            if (!g_accept_hw_render) return false;
            hw->get_current_framebuffer = stub_get_current_framebuffer;
            hw->get_proc_address = stub_get_proc_address;
            g_hw_render_cb = *hw;
            return true;
        }
        default:
            return false;
    }
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    g_audio_batch_frames += frames;
    g_last_audio_batch_frames = 0;
    if (data && frames <= SAMPLES_PER_FRAME) {
        memcpy(g_last_audio_batch, data, frames * 2 * sizeof(*data));
        g_last_audio_batch_frames = frames;
    }
    return frames;
}

static void video_refresh_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    (void)width;
    (void)height;
    (void)pitch;
    g_last_video_frame = data;
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

static bool test_utf16_unicode_playlists(TestContext *ctx) {
    const char *playlist_names[] = { "playlist_utf16le.m3u", "playlist_utf16be.m3u" };
    char path[MAX_PATH_LEN];

    for (size_t i = 0; i < sizeof(playlist_names) / sizeof(playlist_names[0]); i++) {
        prepare_test();
        build_path(path, sizeof(path), ctx->fixtures_dir, playlist_names[i]);

        if (!load_game_path(path))
            return failf("utf16_unicode_playlists: failed to load %s", path);
        if (!require_true(core_debug_get_track_count() == 1,
                          "utf16_unicode_playlists: expected one track for %s", playlist_names[i]))
            return false;
        if (!require_true(current_track_is("unicode_音.wav"),
                          "utf16_unicode_playlists: Unicode track did not open for %s", playlist_names[i]))
            return false;

        run_frames(2);
        if (!require_true(cur_frame > 0,
                          "utf16_unicode_playlists: playback did not advance for %s", playlist_names[i]))
            return false;
    }

    return true;
}

static bool test_bad_track_skipping(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_missing_middle.m3u");

    if (!load_game_path(path))
        return failf("bad_track_skipping: failed to load %s", path);
    if (!require_true(current_track_is("track_a.wav"), "bad_track_skipping: expected track_a.wav first"))
        return false;

    tap_button(RETRO_DEVICE_ID_JOYPAD_R);
    if (!require_true(core_debug_get_current_index() == 2,
                      "bad_track_skipping: expected missing index 1 to be skipped"))
        return false;
    if (!require_true(current_track_is("track_b.wav"), "bad_track_skipping: expected track_b.wav after skip"))
        return false;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "playlist_all_bad.m3u");
    if (!require_true(!load_game_path(path), "bad_track_skipping: all-bad playlist was accepted"))
        return false;
    if (!require_true(core_debug_get_track_count() == 0 && decoder == NULL,
                      "bad_track_skipping: failed load retained playback state"))
        return false;
    return true;
}

static bool test_unsupported_sample_rate_is_rejected(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "unsupported_rate.wav");

    if (!require_true(!load_game_path(path),
                      "unsupported_sample_rate_is_rejected: 768 kHz source was accepted"))
        return false;
    if (!require_true(core_debug_get_track_count() == 0 && decoder == NULL,
                      "unsupported_sample_rate_is_rejected: failed load retained state"))
        return false;
    return true;
}

static bool test_seek_tap_and_repeat(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    uint64_t before_seek;
    uint64_t after_first_seek;
    uint64_t before_repeat;
    uint64_t before_backward;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "seek_long.wav");
    if (!load_game_path(path))
        return failf("seek_tap_and_repeat: failed to load %s", path);

    run_frames(5);
    before_seek = cur_frame;
    g_pressed[RETRO_DEVICE_ID_JOYPAD_RIGHT] = true;
    retro_run();
    after_first_seek = cur_frame;
    if (!require_true(after_first_seek > before_seek + (uint64_t)source_rate * 2u,
                      "seek_tap_and_repeat: forward tap did not seek about three seconds"))
        return false;

    before_repeat = cur_frame;
    run_frames(5);
    if (!require_true(cur_frame < before_repeat + (uint64_t)source_rate,
                      "seek_tap_and_repeat: held seek repeated before cooldown expired"))
        return false;
    retro_run();
    if (!require_true(cur_frame > before_repeat + (uint64_t)source_rate * 2u,
                      "seek_tap_and_repeat: held seek did not repeat after cooldown"))
        return false;

    g_pressed[RETRO_DEVICE_ID_JOYPAD_RIGHT] = false;
    retro_run();
    before_backward = cur_frame;
    g_pressed[RETRO_DEVICE_ID_JOYPAD_LEFT] = true;
    retro_run();
    g_pressed[RETRO_DEVICE_ID_JOYPAD_LEFT] = false;
    if (!require_true(cur_frame + (uint64_t)source_rate * 2u < before_backward,
                      "seek_tap_and_repeat: backward tap did not seek about three seconds"))
        return false;
    return true;
}

static bool test_short_track_position_is_bounded(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "short.wav");

    if (!load_game_path(path))
        return failf("short_track_position_is_bounded: failed to load %s", path);
    if (!require_true(total_frames > 0 && total_frames < SAMPLES_PER_FRAME,
                      "short_track_position_is_bounded: fixture was not shorter than one output frame"))
        return false;

    retro_run();
    if (!require_true(cur_frame <= total_frames,
                      "short_track_position_is_bounded: cur_frame exceeded total_frames"))
        return false;
    return true;
}

static bool test_artwork_is_downscaled(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "art_track.wav");

    if (!load_game_path(path))
        return failf("artwork_is_downscaled: failed to load %s", path);
    if (!require_true(art_buffer != NULL, "artwork_is_downscaled: expected sidecar artwork to load"))
        return false;
    if (!require_true(art_w_src == 120 && art_h_src == 120,
                      "artwork_is_downscaled: expected 120x120 stored art, got %dx%d", art_w_src, art_h_src))
        return false;
    run_frames(2);
    return true;
}

// Read a `key = "value"` entry from the core's .info file. run_tests.py
// runs the harness from the repo root, so the file is reachable by name.
static bool read_info_string(const char *key, char *out, size_t out_size) {
    FILE *f = fopen("music_playlist_libretro.info", "r");
    char line[512];
    bool found = false;

    if (!f || out_size == 0) {
        if (f) fclose(f);
        return false;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        const char *p = line;
        size_t key_len = strlen(key);
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, key_len) != 0) continue;
        p += key_len;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        const char *open_quote = strchr(p + 1, '"');
        if (!open_quote) continue;
        const char *close_quote = strchr(open_quote + 1, '"');
        if (!close_quote) continue;
        size_t len = (size_t)(close_quote - open_quote - 1);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, open_quote + 1, len);
        out[len] = '\0';
        found = true;
    }
    fclose(f);
    return found;
}

static bool test_core_identity_matches_info(TestContext *ctx) {
    struct retro_system_info info;
    char info_name[128];
    char info_version[64];
    (void)ctx;
    memset(&info, 0, sizeof(info));
    retro_get_system_info(&info);

    if (!require_true(info.library_name && strcmp(info.library_name, EXPECTED_LIBRARY_NAME) == 0,
                      "core_identity_matches_info: stable library_name changed from '%s'",
                      EXPECTED_LIBRARY_NAME))
        return false;
    if (!require_true(read_info_string("display_name", info_name, sizeof(info_name)),
                      "core_identity_matches_info: could not read display_name from music_playlist_libretro.info"))
        return false;
    if (!require_true(read_info_string("display_version", info_version, sizeof(info_version)),
                      "core_identity_matches_info: could not read display_version from music_playlist_libretro.info"))
        return false;

    if (!require_true(info.library_name && strcmp(info.library_name, info_name) == 0,
                      "core_identity_matches_info: library_name '%s' does not match .info display_name '%s'",
                      info.library_name ? info.library_name : "(null)", info_name))
        return false;
    if (!require_true(info.library_version && strcmp(info.library_version, info_version) == 0,
                      "core_identity_matches_info: library_version '%s' does not match .info display_version '%s'",
                      info.library_version ? info.library_version : "(null)", info_version))
        return false;
    return true;
}

static bool next_audio_matches_after_restore(const char *test_name) {
    size_t state_size = retro_serialize_size();
    void *state_data = NULL;
    int16_t expected[SAMPLES_PER_FRAME * 2];
    size_t mismatches = 0;
    bool ok = false;

    if (!require_true(state_size > 0, "%s: serialize size was zero", test_name))
        return false;
    state_data = malloc(state_size);
    if (!state_data) return failf("%s: failed to allocate %zu bytes", test_name, state_size);
    if (!require_true(retro_serialize(state_data, state_size), "%s: serialize failed", test_name))
        goto done;

    retro_run();
    if (!require_true(g_last_audio_batch_frames == SAMPLES_PER_FRAME,
                      "%s: expected a full audio batch before restore", test_name))
        goto done;
    memcpy(expected, g_last_audio_batch, sizeof(expected));

    if (!require_true(retro_unserialize(state_data, state_size), "%s: unserialize failed", test_name))
        goto done;
    retro_run();
    if (!require_true(g_last_audio_batch_frames == SAMPLES_PER_FRAME,
                      "%s: expected a full audio batch after restore", test_name))
        goto done;

    for (size_t i = 0; i < SAMPLES_PER_FRAME * 2; i++) {
        if (expected[i] != g_last_audio_batch[i]) mismatches++;
    }
    if (!require_true(mismatches == 0,
                      "%s: restored audio differed at %zu/%d samples",
                      test_name, mismatches, SAMPLES_PER_FRAME * 2))
        goto done;

    ok = true;
done:
    free(state_data);
    return ok;
}

static bool test_unknown_length_flac_restore(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "unknown_length.flac");

    if (!load_game_path(path))
        return failf("unknown_length_flac_restore: failed to load %s", path);
    run_frames(20);
    if (!require_true(current_type == AUDIO_FLAC && total_frames == 0 && cur_frame > 0,
                      "unknown_length_flac_restore: fixture did not expose an unknown FLAC length"))
        return false;
    return next_audio_matches_after_restore("unknown_length_flac_restore");
}

static bool test_underreported_ogg_restore(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "underreported.ogg");

    if (!load_game_path(path))
        return failf("underreported_ogg_restore: failed to load %s", path);
    run_frames(20);
    if (!require_true(current_type == AUDIO_OGG && total_frames == 800 && cur_frame > total_frames,
                      "underreported_ogg_restore: playback cursor did not pass the reported length"))
        return false;
    return next_audio_matches_after_restore("underreported_ogg_restore");
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

static bool test_malformed_save_states_are_rejected(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    size_t state_size;
    void *state_data = NULL;
    CoreStateSnapshot *snapshot;
    uint64_t before_frames;
    uint32_t saved_magic;
    uint32_t saved_audio_type;
    uint64_t saved_cur_frame;
    int32_t saved_cache_frames;
    double saved_phase;
    bool ok = false;

    prepare_test();
    build_path(path, sizeof(path), ctx->fixtures_dir, "track_a.wav");
    if (!load_game_path(path))
        return failf("malformed_save_states: failed to load %s", path);
    run_frames(4);

    state_size = retro_serialize_size();
    if (!require_true(state_size > sizeof(CoreStateSnapshot),
                      "malformed_save_states: state was smaller than expected snapshot"))
        return false;
    state_data = malloc(state_size);
    if (!state_data) return failf("malformed_save_states: failed to allocate %zu bytes", state_size);
    if (!require_true(retro_serialize(state_data, state_size), "malformed_save_states: serialize failed"))
        goto done;

    snapshot = (CoreStateSnapshot*)state_data;
    before_frames = cur_frame;

    if (!require_true(!retro_unserialize(state_data, sizeof(CoreStateSnapshot) - 1),
                      "malformed_save_states: truncated state was accepted"))
        goto done;

    saved_magic = snapshot->magic;
    snapshot->magic ^= 0xFFFFFFFFu;
    if (!require_true(!retro_unserialize(state_data, state_size),
                      "malformed_save_states: corrupt magic was accepted"))
        goto done;
    snapshot->magic = saved_magic;

    saved_audio_type = snapshot->audio.current_type;
    snapshot->audio.current_type = AUDIO_NONE;
    if (!require_true(!retro_unserialize(state_data, state_size),
                      "malformed_save_states: inconsistent AUDIO_NONE state was accepted"))
        goto done;
    snapshot->audio.current_type = saved_audio_type;

    saved_phase = snapshot->audio.resample_phase;
    snapshot->audio.resample_phase = NAN;
    if (!require_true(!retro_unserialize(state_data, state_size),
                      "malformed_save_states: non-finite resampler phase was accepted"))
        goto done;
    snapshot->audio.resample_phase = saved_phase;

    saved_cur_frame = snapshot->audio.cur_frame;
    saved_cache_frames = snapshot->audio.resample_cache_frames;
    snapshot->audio.cur_frame = UINT64_MAX;
    snapshot->audio.resample_cache_frames = 1;
    if (!require_true(!retro_unserialize(state_data, state_size),
                      "malformed_save_states: overflowing decoder position was accepted"))
        goto done;
    snapshot->audio.cur_frame = saved_cur_frame;
    snapshot->audio.resample_cache_frames = saved_cache_frames;

    run_frames(2);
    if (!require_true(cur_frame > before_frames,
                      "malformed_save_states: playback did not remain usable after rejected states"))
        goto done;

    ok = true;
done:
    free(state_data);
    return ok;
}

static bool test_config_layout_smoke(TestContext *ctx) {
    char path[MAX_PATH_LEN];

    prepare_test();
    set_env_option("media_show_art", "Off");
    set_env_option("media_viz_mode", "Line");
    set_env_option("media_viz_bands", "20");
    set_env_option("media_viz_gradient", "Off");
    set_env_option("media_use_filename", "Show filename with extension");
    build_path(path, sizeof(path), ctx->fixtures_dir, "track_b.wav");

    if (!load_game_path(path))
        return failf("config_layout_smoke: failed to load %s", path);

    run_frames(6);

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

    // Two updates fill the 2048-sample FFT analysis ring.
    viz_update_levels(loud_buf, 1024);
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

// Drives the SET_HW_RENDER contract end-to-end with the stub GL frontend:
// once negotiation succeeds, only RETRO_HW_FRAME_BUFFER_VALID or NULL may
// reach the video callback -- a raw CPU framebuffer would be ignored by the
// 1.7.5 gl driver and show stale FBO contents.
static bool test_gl_negotiation_lifecycle_and_fallback(TestContext *ctx) {
    char path[MAX_PATH_LEN];
    build_path(path, sizeof(path), ctx->fixtures_dir, "track_a.wav");

    // 1) Frontend refuses the GL context: software frames keep flowing.
    prepare_test();
    set_env_option("media_renderer", "OpenGL");
    g_accept_hw_render = false;
    g_hw_render_requested = false;
    if (!load_game_path(path))
        return failf("gl_lifecycle: failed to load %s", path);
    if (!require_true(g_hw_render_requested, "gl_lifecycle: expected a SET_HW_RENDER request"))
        return false;
    if (!require_true(g_hw_render_context_type == RETRO_HW_CONTEXT_OPENGL,
                      "gl_lifecycle: expected an OpenGL context request, got %u", g_hw_render_context_type))
        return false;
    g_last_video_frame = &g_video_frame_unset_sentinel;
    run_frames(1);
    if (!require_true(g_last_video_frame != NULL &&
                      g_last_video_frame != RETRO_HW_FRAME_BUFFER_VALID &&
                      g_last_video_frame != &g_video_frame_unset_sentinel,
                      "gl_lifecycle: refused negotiation must present the software framebuffer"))
        return false;

    // 2) Negotiation accepted but no context yet: NULL dupes only.
    prepare_test();
    set_env_option("media_renderer", "OpenGL");
    g_accept_hw_render = true;
    g_hw_render_requested = false;
    memset(&g_hw_render_cb, 0, sizeof(g_hw_render_cb));
    if (!load_game_path(path))
        return failf("gl_lifecycle: failed to reload %s", path);
    if (!require_true(g_hw_render_requested && g_hw_render_cb.context_reset && g_hw_render_cb.context_destroy,
                      "gl_lifecycle: expected hw render callbacks to be registered"))
        return false;
    g_last_video_frame = &g_video_frame_unset_sentinel;
    run_frames(1);
    if (!require_true(g_last_video_frame == NULL,
                      "gl_lifecycle: negotiated GL without a context must dupe (NULL), not submit a CPU frame"))
        return false;

    // 3) context_reset arrives with inherited shared-context PBO state.
    // Texture allocation and the per-frame CPU upload must both unbind it.
    g_gl_stub_pixel_unpack_buffer = 41;
    g_gl_stub_tex_image_calls = 0;
    g_gl_stub_tex_sub_image_calls = 0;
    g_gl_stub_tex_image_with_pbo = 0;
    g_gl_stub_tex_sub_image_with_pbo = 0;
    // Full bloom: bright-pass + two blur passes + scene blit + glow overlay.
    g_hw_render_cb.context_reset();
    if (!require_true(g_gl_stub_tex_image_calls > 0 && g_gl_stub_tex_image_with_pbo == 0,
                      "gl_lifecycle: texture allocation must unbind inherited pixel-unpack buffers"))
        return false;
    g_gl_stub_pixel_unpack_buffer = 42;
    g_last_video_frame = &g_video_frame_unset_sentinel;
    g_gl_stub_draw_calls = 0;
    run_frames(1);
    if (!require_true(g_gl_stub_tex_sub_image_calls > 0 && g_gl_stub_tex_sub_image_with_pbo == 0,
                      "gl_lifecycle: CPU uploads must unbind inherited pixel-unpack buffers"))
        return false;
    if (!require_true(g_last_video_frame == RETRO_HW_FRAME_BUFFER_VALID,
                      "gl_lifecycle: expected RETRO_HW_FRAME_BUFFER_VALID after context_reset"))
        return false;
    if (!require_true(g_gl_stub_draw_calls == 5,
                      "gl_lifecycle: expected 5 draws with glow enabled, got %d", g_gl_stub_draw_calls))
        return false;

    // 4) context_destroy: back to NULL dupes, still never a CPU frame.
    g_hw_render_cb.context_destroy();
    g_last_video_frame = &g_video_frame_unset_sentinel;
    run_frames(1);
    if (!require_true(g_last_video_frame == NULL,
                      "gl_lifecycle: expected NULL dupes after context_destroy"))
        return false;

    // 5) GL setup failure inside context_reset (driver missing a symbol):
    // the core must degrade to dupes, never to a CPU frame.
    g_gl_stub_break_loader = true;
    g_hw_render_cb.context_reset();
    g_last_video_frame = &g_video_frame_unset_sentinel;
    run_frames(1);
    g_gl_stub_break_loader = false;
    if (!require_true(g_last_video_frame == NULL,
                      "gl_lifecycle: failed GL setup must dupe (NULL), not submit a CPU frame"))
        return false;
    g_hw_render_cb.context_destroy();

    // 6) Glow-only failure (no FBO entry points): the picture survives as a
    // plain passthrough hw frame (scene blit only = 1 draw).
    g_gl_stub_break_glow = true;
    g_hw_render_cb.context_reset();
    g_last_video_frame = &g_video_frame_unset_sentinel;
    g_gl_stub_draw_calls = 0;
    run_frames(1);
    g_gl_stub_break_glow = false;
    if (!require_true(g_last_video_frame == RETRO_HW_FRAME_BUFFER_VALID,
                      "gl_lifecycle: glow loader failure must not cost the hw frame"))
        return false;
    if (!require_true(g_gl_stub_draw_calls == 1,
                      "gl_lifecycle: expected passthrough-only (1 draw) without glow, got %d", g_gl_stub_draw_calls))
        return false;
    g_hw_render_cb.context_destroy();

    // 7) Glow FBOs never complete: same survival guarantee via the other
    // failure exit.
    g_gl_stub_fbo_incomplete = true;
    g_hw_render_cb.context_reset();
    g_last_video_frame = &g_video_frame_unset_sentinel;
    g_gl_stub_draw_calls = 0;
    run_frames(1);
    g_gl_stub_fbo_incomplete = false;
    if (!require_true(g_last_video_frame == RETRO_HW_FRAME_BUFFER_VALID,
                      "gl_lifecycle: incomplete glow FBOs must not cost the hw frame"))
        return false;
    if (!require_true(g_gl_stub_draw_calls == 1,
                      "gl_lifecycle: expected passthrough-only (1 draw) with incomplete FBOs, got %d", g_gl_stub_draw_calls))
        return false;
    g_hw_render_cb.context_destroy();

    g_accept_hw_render = false;
    return true;
}

static const TestCase kTests[] = {
    { "basic_load_play", test_basic_load_play },
    { "playlist_navigation", test_playlist_navigation },
    { "utf16_unicode_playlists", test_utf16_unicode_playlists },
    { "bad_track_skipping", test_bad_track_skipping },
    { "unsupported_sample_rate_is_rejected", test_unsupported_sample_rate_is_rejected },
    { "seek_tap_and_repeat", test_seek_tap_and_repeat },
    { "short_track_position_is_bounded", test_short_track_position_is_bounded },
    { "artwork_is_downscaled", test_artwork_is_downscaled },
    { "core_identity_matches_info", test_core_identity_matches_info },
    { "unknown_length_flac_restore", test_unknown_length_flac_restore },
    { "underreported_ogg_restore", test_underreported_ogg_restore },
    { "save_state_restore_track_and_position", test_save_state_restore_track_and_position },
    { "pause_state_restore", test_pause_state_restore },
    { "reset_preserves_shuffle_and_restarts_current_track", test_reset_preserves_shuffle_and_restarts_current_track },
    { "shuffle_cycle_no_repeats", test_shuffle_cycle_no_repeats },
    { "shuffle_previous_returns_to_history", test_shuffle_previous_returns_to_history },
    { "shuffle_previous_after_eof_returns_finished_track", test_shuffle_previous_after_eof_returns_finished_track },
    { "shuffle_save_state_restores_previous_history", test_shuffle_save_state_restores_previous_history },
    { "negative_restore_keeps_playback_usable", test_negative_restore_keeps_playback_usable },
    { "malformed_save_states_are_rejected", test_malformed_save_states_are_rejected },
    { "config_layout_smoke", test_config_layout_smoke },
    { "vu_mode_updates_once_per_frame", test_vu_mode_updates_once_per_frame },
    { "track_change_resets_fft_visualizer_state", test_track_change_resets_fft_visualizer_state },
    { "gl_negotiation_lifecycle_and_fallback", test_gl_negotiation_lifecycle_and_fallback },
};

#ifdef _WIN32
// Narrow argv arrives in the ANSI code page, but the core treats paths as
// UTF-8 (UTF-16 playlist entries are decoded to UTF-8 before path
// concatenation), so a fixtures dir containing non-ASCII characters must
// be re-encoded to keep the combined paths openable.
static const char *argv_path_to_utf8(const char *arg) {
    static char utf8_buf[MAX_PATH_LEN * 4];
    wchar_t wide_buf[MAX_PATH_LEN];
    if (MultiByteToWideChar(CP_ACP, 0, arg, -1, wide_buf, MAX_PATH_LEN) <= 0)
        return arg;
    if (WideCharToMultiByte(CP_UTF8, 0, wide_buf, -1, utf8_buf, (int)sizeof(utf8_buf), NULL, NULL) <= 0)
        return arg;
    return utf8_buf;
}
#endif

int main(int argc, char **argv) {
    TestContext ctx;
    size_t passed = 0;
    size_t total = sizeof(kTests) / sizeof(kTests[0]);

    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixtures_dir>\n", argv[0]);
        return 2;
    }

    ctx.fixtures_dir = argv[1];
#ifdef _WIN32
    ctx.fixtures_dir = argv_path_to_utf8(argv[1]);
#endif
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
