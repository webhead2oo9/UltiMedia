// Optional OpenGL presentation path (Phase A of docs/gpu-renderer-plan.md).
// The GL types, constants, and function pointers are declared locally and
// resolved through the frontend's get_proc_address so the file compiles on
// every CI platform without any GL SDK installed and links nothing new.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "render_gl.h"
#include "video.h"
#include "core_log.h"

// GL entry points use the stdcall convention on 32-bit Windows only.
#if defined(_WIN32) && !defined(_WIN64)
#define RGL_APIENTRY __stdcall
#else
#define RGL_APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef float GLfloat;
typedef ptrdiff_t GLsizeiptr;

#define GL_NO_ERROR 0
#define GL_TRIANGLE_STRIP 0x0005
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_2D 0x0DE1
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40

#define RGL_PROC_LIST(X) \
    X(glGetError, GLenum, (void)) \
    X(glEnable, void, (GLenum cap)) \
    X(glDisable, void, (GLenum cap)) \
    X(glViewport, void, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(glClearColor, void, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(glClear, void, (GLbitfield mask)) \
    X(glPixelStorei, void, (GLenum pname, GLint param)) \
    X(glGenTextures, void, (GLsizei n, GLuint *textures)) \
    X(glDeleteTextures, void, (GLsizei n, const GLuint *textures)) \
    X(glBindTexture, void, (GLenum target, GLuint texture)) \
    X(glTexParameteri, void, (GLenum target, GLenum pname, GLint param)) \
    X(glTexImage2D, void, (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(glTexSubImage2D, void, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)) \
    X(glActiveTexture, void, (GLenum texture)) \
    X(glCreateShader, GLuint, (GLenum type)) \
    X(glShaderSource, void, (GLuint shader, GLsizei count, const GLchar **string, const GLint *length)) \
    X(glCompileShader, void, (GLuint shader)) \
    X(glGetShaderiv, void, (GLuint shader, GLenum pname, GLint *params)) \
    X(glGetShaderInfoLog, void, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(glDeleteShader, void, (GLuint shader)) \
    X(glCreateProgram, GLuint, (void)) \
    X(glAttachShader, void, (GLuint program, GLuint shader)) \
    X(glLinkProgram, void, (GLuint program)) \
    X(glGetProgramiv, void, (GLuint program, GLenum pname, GLint *params)) \
    X(glGetProgramInfoLog, void, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(glUseProgram, void, (GLuint program)) \
    X(glDeleteProgram, void, (GLuint program)) \
    X(glGetAttribLocation, GLint, (GLuint program, const GLchar *name)) \
    X(glGetUniformLocation, GLint, (GLuint program, const GLchar *name)) \
    X(glUniform1i, void, (GLint location, GLint v0)) \
    X(glGenBuffers, void, (GLsizei n, GLuint *buffers)) \
    X(glDeleteBuffers, void, (GLsizei n, const GLuint *buffers)) \
    X(glBindBuffer, void, (GLenum target, GLuint buffer)) \
    X(glBufferData, void, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(glEnableVertexAttribArray, void, (GLuint index)) \
    X(glDisableVertexAttribArray, void, (GLuint index)) \
    X(glVertexAttribPointer, void, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(glDrawArrays, void, (GLenum mode, GLint first, GLsizei count)) \
    X(glBindFramebuffer, void, (GLenum target, GLuint framebuffer))

#define RGL_DECLARE(name, ret, args) \
    typedef ret (RGL_APIENTRY *pfn_##name) args; \
    static pfn_##name name##_;
RGL_PROC_LIST(RGL_DECLARE)
#undef RGL_DECLARE

static struct retro_hw_render_callback hw_cb;
static bool gl_negotiated = false;      // SET_HW_RENDER accepted this load
static bool gl_context_alive = false;   // between context_reset and destroy
static bool gl_resources_ready = false; // texture/program/VBO built and valid
static GLuint gl_texture = 0;
static GLuint gl_program = 0;
static GLuint gl_vbo = 0;
static GLint gl_attr_pos = -1;
static GLint gl_attr_uv = -1;
static GLint gl_uniform_tex = -1;
// Framebuffer size the VBO's UVs were computed for; the quad only needs a
// re-upload when media_resolution changes the active size.
static int gl_quad_w = 0;
static int gl_quad_h = 0;

// GLSL 1.10 so the 2.x compatibility context EmuVR's gl driver hands out
// accepts it everywhere.
static const char *vertex_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_src =
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static bool load_gl_functions(void) {
    if (!hw_cb.get_proc_address) return false;
    bool ok = true;
#define RGL_LOAD(name, ret, args) \
    name##_ = (pfn_##name)hw_cb.get_proc_address(#name); \
    if (!name##_) ok = false;
    RGL_PROC_LIST(RGL_LOAD)
#undef RGL_LOAD
    // Pre-3.0 drivers expose FBO binding only under the EXT name.
    if (!glBindFramebuffer_)
        glBindFramebuffer_ = (pfn_glBindFramebuffer)hw_cb.get_proc_address("glBindFramebufferEXT");
    if (!glBindFramebuffer_) ok = false;
    if (!ok)
        core_log(RETRO_LOG_ERROR,
                 "[MusicCore] GL renderer: required GL functions are missing; using software frames.\n");
    return ok;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader_(type);
    if (!shader) return 0;
    glShaderSource_(shader, 1, &src, NULL);
    glCompileShader_(shader);
    GLint status = 0;
    glGetShaderiv_(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char info[512] = {0};
        glGetShaderInfoLog_(shader, sizeof(info) - 1, NULL, info);
        core_log(RETRO_LOG_ERROR, "[MusicCore] GL renderer: shader compile failed: %s\n", info);
        glDeleteShader_(shader);
        return 0;
    }
    return shader;
}

static bool build_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
    if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
    if (!fs) {
        glDeleteShader_(vs);
        return false;
    }

    gl_program = glCreateProgram_();
    glAttachShader_(gl_program, vs);
    glAttachShader_(gl_program, fs);
    glLinkProgram_(gl_program);
    glDeleteShader_(vs);
    glDeleteShader_(fs);

    GLint status = 0;
    glGetProgramiv_(gl_program, GL_LINK_STATUS, &status);
    if (!status) {
        char info[512] = {0};
        glGetProgramInfoLog_(gl_program, sizeof(info) - 1, NULL, info);
        core_log(RETRO_LOG_ERROR, "[MusicCore] GL renderer: program link failed: %s\n", info);
        glDeleteProgram_(gl_program);
        gl_program = 0;
        return false;
    }

    gl_attr_pos = glGetAttribLocation_(gl_program, "a_pos");
    gl_attr_uv = glGetAttribLocation_(gl_program, "a_uv");
    gl_uniform_tex = glGetUniformLocation_(gl_program, "u_tex");
    return gl_attr_pos >= 0 && gl_attr_uv >= 0 && gl_uniform_tex >= 0;
}

// Interleaved x,y,u,v quad. The FBO is bottom-left origin while the CPU
// framebuffer is top-left, so V is flipped here in the UVs (never in the
// upload). Only the active fb_width x fb_height corner of the max-size
// texture is sampled.
static void upload_quad(int w, int h) {
    GLfloat umax = (GLfloat)w / (GLfloat)FB_MAX_WIDTH;
    GLfloat vmax = (GLfloat)h / (GLfloat)FB_MAX_HEIGHT;
    GLfloat verts[16] = {
        -1.0f, -1.0f, 0.0f, vmax,
         1.0f, -1.0f, umax, vmax,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, umax, 0.0f,
    };
    glBindBuffer_(GL_ARRAY_BUFFER, gl_vbo);
    glBufferData_(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_DYNAMIC_DRAW);
    gl_quad_w = w;
    gl_quad_h = h;
}

static bool build_resources(void) {
    // Allocate the texture at the declared max once; per-frame uploads then
    // touch only the active region, and resolution changes need no realloc.
    glGenTextures_(1, &gl_texture);
    glBindTexture_(GL_TEXTURE_2D, gl_texture);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGB, FB_MAX_WIDTH, FB_MAX_HEIGHT, 0,
                  GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);

    if (!build_program()) return false;

    glGenBuffers_(1, &gl_vbo);
    upload_quad(fb_width, fb_height);

    return glGetError_() == GL_NO_ERROR;
}

static void context_reset_cb(void) {
    // Per the libretro contract, everything GPU-side is invalid here; new
    // names are created without freeing the old ones.
    gl_context_alive = true;
    gl_resources_ready = false;
    gl_texture = 0;
    gl_program = 0;
    gl_vbo = 0;

    if (!load_gl_functions()) return;
    if (!build_resources()) {
        core_log(RETRO_LOG_ERROR,
                 "[MusicCore] GL renderer: resource setup failed; using software frames.\n");
        return;
    }
    gl_resources_ready = true;
    core_log(RETRO_LOG_INFO, "[MusicCore] GL renderer: context ready.\n");
}

static void context_destroy_cb(void) {
    if (gl_resources_ready) {
        if (gl_texture) glDeleteTextures_(1, &gl_texture);
        if (gl_program) glDeleteProgram_(gl_program);
        if (gl_vbo) glDeleteBuffers_(1, &gl_vbo);
    }
    gl_texture = 0;
    gl_program = 0;
    gl_vbo = 0;
    gl_resources_ready = false;
    gl_context_alive = false;
}

bool render_gl_request_context(retro_environment_t environ_cb) {
    if (!environ_cb) return false;

    memset(&hw_cb, 0, sizeof(hw_cb));
    hw_cb.context_type = RETRO_HW_CONTEXT_OPENGL;
    hw_cb.context_reset = context_reset_cb;
    hw_cb.context_destroy = context_destroy_cb;
    hw_cb.bottom_left_origin = true;
    hw_cb.cache_context = true; // EmuVR's Spout pipe dislikes context churn

    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_cb)) {
        core_log(RETRO_LOG_INFO,
                 "[MusicCore] GL renderer: frontend refused the OpenGL context; using the software renderer.\n");
        gl_negotiated = false;
        return false;
    }
    gl_negotiated = true;
    core_log(RETRO_LOG_INFO, "[MusicCore] GL renderer: OpenGL context negotiated.\n");
    return true;
}

bool render_gl_frame(void) {
    if (!gl_negotiated || !gl_context_alive || !gl_resources_ready) return false;
    if (!hw_cb.get_current_framebuffer) return false;

    glBindFramebuffer_(GL_FRAMEBUFFER, (GLuint)hw_cb.get_current_framebuffer());
    glViewport_(0, 0, fb_width, fb_height);
    glDisable_(GL_DEPTH_TEST);
    glDisable_(GL_CULL_FACE);
    glDisable_(GL_BLEND);
    glDisable_(GL_SCISSOR_TEST);
    glClearColor_(0.0f, 0.0f, 0.0f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);

    // EmuVR runs with a shared GL context, so pixel-store state can be
    // anything the frontend left behind; set what the upload depends on.
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture_(GL_TEXTURE_2D, gl_texture);
    glPixelStorei_(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei_(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, fb_width, fb_height,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, framebuffer);

    if (gl_quad_w != fb_width || gl_quad_h != fb_height)
        upload_quad(fb_width, fb_height);

    glUseProgram_(gl_program);
    glUniform1i_(gl_uniform_tex, 0);
    glBindBuffer_(GL_ARRAY_BUFFER, gl_vbo);
    glEnableVertexAttribArray_((GLuint)gl_attr_pos);
    glEnableVertexAttribArray_((GLuint)gl_attr_uv);
    glVertexAttribPointer_((GLuint)gl_attr_pos, 2, GL_FLOAT, 0,
                           4 * (GLsizei)sizeof(GLfloat), (const void*)0);
    glVertexAttribPointer_((GLuint)gl_attr_uv, 2, GL_FLOAT, 0,
                           4 * (GLsizei)sizeof(GLfloat), (const void*)(2 * sizeof(GLfloat)));
    glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray_((GLuint)gl_attr_pos);
    glDisableVertexAttribArray_((GLuint)gl_attr_uv);
    glBindBuffer_(GL_ARRAY_BUFFER, 0);
    glUseProgram_(0);
    return true;
}

void render_gl_shutdown(void) {
    gl_negotiated = false;
}
